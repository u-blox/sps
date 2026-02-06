# u-blox SPS Python Server Implementation

This document provides a complete **SPS Server** implementation in Python using the [bless](https://github.com/kevincar/bless) library.

**Related Documentation:**
- [SPS Protocol Specification](SPS_PROTOCOL.md)
- [Python Client Implementation](SPS_CLIENT_PYTHON.md)
- [C Server Implementation](SPS_SERVER_C.md)

---

## Overview

An **SPS Server**:
- Advertises the SPS service so clients can find it
- Accepts connections from SPS Clients
- Receives data FROM the client (via FIFO writes)
- Sends data TO the client (via FIFO notifications)
- Manages flow control credits in both directions

## Requirements

```bash
pip install bless
```

- Python 3.7 or later
- Bluetooth adapter with BLE support
- Platform support:
  - **Linux**: BlueZ 5.43+ (best support)
  - **macOS**: CoreBluetooth
  - **Windows**: Limited support

> **Note**: Python's Bleak library is for GATT *clients* only. To create a GATT *server*, you need `bless` or a platform-specific library.

---

## Complete Implementation

```python
"""
u-blox SPS SERVER Implementation
================================
This code runs on YOUR device (PC, Raspberry Pi with BlueZ, etc.)
It acts as the SPS SERVER - clients connect to it.

Role: SERVER (advertises, accepts connections, hosts GATT service)

Requirements:
    pip install bless
    
Platform Support:
    - Linux (BlueZ) - Best support
    - macOS (CoreBluetooth) - Good support
    - Windows - Limited support

Usage:
    python sps_server.py
"""
import asyncio
from bless import (
    BlessServer,
    BlessGATTCharacteristic,
    GATTCharacteristicProperties,
    GATTAttributePermissions
)

# =============================================================================
# SPS UUIDs - defined by u-blox, same on all devices
# =============================================================================
SPS_SERVICE_UUID = "2456e1b9-26e2-8f83-e744-f34f01e9d701"   # SPS Service
SPS_FIFO_UUID = "2456e1b9-26e2-8f83-e744-f34f01e9d703"      # Data transfer
SPS_CREDITS_UUID = "2456e1b9-26e2-8f83-e744-f34f01e9d704"   # Flow control


class SPSServer:
    """
    SPS Server implementation with full flow control support.
    
    This is the SERVER side - it:
    - Advertises the SPS service
    - Accepts connections from SPS clients
    - Receives data FROM the client (via FIFO writes from client)
    - Sends data TO the client (via FIFO notifications)
    - Manages flow control credits in both directions
    """
    
    def __init__(self):
        self.server = None
        
        # Flow control: how many packets we can SEND to the client
        self.tx_credits = 0
        
        # Flow control: how many packets we've RECEIVED (need to grant back)
        self.rx_credits_pending = 0
        
        # Buffer for received data
        self.rx_buffer = bytearray()
        
        # Buffer for data to send
        self.tx_buffer = bytearray()
        
        # Configuration
        self.INITIAL_CREDITS = 8      # Credits to grant on connection
        self.CREDIT_THRESHOLD = 4     # Grant credits after this many packets
        self.MAX_PACKET_SIZE = 244    # MTU 247 - 3 = 244 bytes
        
        # Connection state
        self.is_connected = False
        
        # Callback for received data (user can override)
        self.on_data_received = None
    
    def on_fifo_write(self, characteristic: BlessGATTCharacteristic, value: bytes):
        """
        Called when CLIENT writes data TO us.
        
        This is a WRITE from the client to our FIFO characteristic.
        The client is sending data to us.
        """
        self.rx_buffer.extend(value)
        print(f"RX from client: {value.hex()} ({len(value)} bytes)")
        
        # Call user callback if set
        if self.on_data_received:
            self.on_data_received(value)
        
        # Track received packet for flow control
        self.rx_credits_pending += 1
        
        # Grant credits back to client periodically
        if self.rx_credits_pending >= self.CREDIT_THRESHOLD:
            asyncio.create_task(self.grant_credits(self.rx_credits_pending))
            self.rx_credits_pending = 0
    
    def on_credits_write(self, characteristic: BlessGATTCharacteristic, value: bytes):
        """
        Called when CLIENT grants us credits.
        
        This is a WRITE from the client to our Credits characteristic.
        Each credit allows us to send one packet to the client.
        """
        if len(value) > 0:
            new_credits = value[0]
            self.tx_credits += new_credits
            print(f"Client granted {new_credits} credits, total: {self.tx_credits}")
            
            # Try to send pending data
            asyncio.create_task(self.flush_tx_buffer())
    
    async def grant_credits(self, count: int):
        """
        Grant credits TO the client.
        
        This tells the client: "I can receive <count> more packets from you"
        We send this as a NOTIFICATION on the Credits characteristic.
        
        Args:
            count: Number of credits to grant (1-255)
        """
        if self.server and self.is_connected:
            try:
                await self.server.update_value(SPS_SERVICE_UUID, SPS_CREDITS_UUID, bytes([count]))
                print(f"Granted {count} credits to client")
            except Exception as e:
                print(f"Failed to grant credits: {e}")
    
    async def send_data(self, data: bytes):
        """
        Queue data to send TO the client.
        
        We NOTIFY the client via the FIFO characteristic.
        Each notification costs one credit.
        
        Args:
            data: Bytes to send
        """
        self.tx_buffer.extend(data)
        await self.flush_tx_buffer()
    
    async def send_string(self, text: str):
        """Send a string to the client (UTF-8 encoded)."""
        await self.send_data(text.encode('utf-8'))
    
    async def flush_tx_buffer(self):
        """
        Send pending data from TX buffer if we have credits.
        
        Data is sent as NOTIFICATIONs on the FIFO characteristic.
        Each notification costs one credit.
        """
        if not self.server or not self.is_connected:
            return
        
        while self.tx_credits > 0 and len(self.tx_buffer) > 0:
            # Send up to MAX_PACKET_SIZE bytes per packet
            chunk_size = min(self.MAX_PACKET_SIZE, len(self.tx_buffer))
            chunk = bytes(self.tx_buffer[:chunk_size])
            del self.tx_buffer[:chunk_size]
            
            try:
                await self.server.update_value(SPS_SERVICE_UUID, SPS_FIFO_UUID, chunk)
                self.tx_credits -= 1
                print(f"TX to client: {chunk.hex()}, credits remaining: {self.tx_credits}")
            except Exception as e:
                print(f"Failed to send data: {e}")
                # Put data back in buffer
                self.tx_buffer = bytearray(chunk) + self.tx_buffer
                break
    
    def handle_write_request(self, characteristic: BlessGATTCharacteristic, value: bytes):
        """
        Handle write requests from clients.
        Route to appropriate handler based on characteristic UUID.
        """
        char_uuid = str(characteristic.uuid).lower()
        
        if SPS_FIFO_UUID.lower() in char_uuid:
            self.on_fifo_write(characteristic, value)
        elif SPS_CREDITS_UUID.lower() in char_uuid:
            self.on_credits_write(characteristic, value)
        else:
            print(f"Unknown characteristic write: {char_uuid}")
    
    def handle_read_request(self, characteristic: BlessGATTCharacteristic) -> bytes:
        """
        Handle read requests from clients.
        SPS doesn't typically use reads, but we implement for completeness.
        """
        return bytes([0])
    
    def handle_subscribe(self, characteristic: BlessGATTCharacteristic, notify: bool):
        """
        Called when client enables/disables notifications (CCCD write).
        """
        char_uuid = str(characteristic.uuid).lower()
        action = "enabled" if notify else "disabled"
        
        if SPS_FIFO_UUID.lower() in char_uuid:
            print(f"FIFO notifications {action}")
            if notify:
                self.is_connected = True
                # Grant initial credits when client subscribes
                asyncio.create_task(self._delayed_initial_credits())
        elif SPS_CREDITS_UUID.lower() in char_uuid:
            print(f"Credits notifications {action}")
    
    async def _delayed_initial_credits(self):
        """Grant initial credits after a short delay."""
        await asyncio.sleep(0.5)
        await self.grant_credits(self.INITIAL_CREDITS)
    
    async def start(self, device_name: str = "SPS-Server"):
        """
        Start the SPS server.
        
        Steps:
        1. Create GATT server with SPS service
        2. Add FIFO and Credits characteristics
        3. Start advertising
        4. Wait for connections
        
        Args:
            device_name: Name to advertise (visible to scanning clients)
        """
        self.server = BlessServer(name=device_name)
        
        # Set up callbacks
        self.server.write_request_func = self.handle_write_request
        self.server.read_request_func = self.handle_read_request
        
        # Note: bless handles subscription internally, but we can detect it
        # through the write_request_func when CCCD is written
        
        # Define the SPS service with its characteristics
        await self.server.add_new_service(SPS_SERVICE_UUID)
        
        # Add FIFO characteristic (data transfer)
        # Properties: Write Without Response (client→server), Notify (server→client)
        fifo_props = (
            GATTCharacteristicProperties.write_without_response |
            GATTCharacteristicProperties.write |
            GATTCharacteristicProperties.notify
        )
        await self.server.add_new_characteristic(
            SPS_SERVICE_UUID,
            SPS_FIFO_UUID,
            fifo_props,
            bytes([0]),  # Initial value
            GATTAttributePermissions.readable | GATTAttributePermissions.writeable
        )
        
        # Add Credits characteristic (flow control)
        # Properties: Write Without Response (client→server), Notify (server→client)
        credits_props = (
            GATTCharacteristicProperties.write_without_response |
            GATTCharacteristicProperties.write |
            GATTCharacteristicProperties.notify
        )
        await self.server.add_new_characteristic(
            SPS_SERVICE_UUID,
            SPS_CREDITS_UUID,
            credits_props,
            bytes([0]),  # Initial value
            GATTAttributePermissions.readable | GATTAttributePermissions.writeable
        )
        
        # Start advertising and accepting connections
        await self.server.start()
        print(f"SPS Server '{device_name}' started, waiting for connections...")
        print(f"Service UUID: {SPS_SERVICE_UUID}")
    
    async def stop(self):
        """Stop the SPS server."""
        if self.server:
            await self.server.stop()
            print("SPS Server stopped")
    
    def read_buffer(self) -> bytes:
        """Read and clear the receive buffer."""
        data = bytes(self.rx_buffer)
        self.rx_buffer.clear()
        return data


# =============================================================================
# Example Usage
# =============================================================================
async def main():
    """Example: Start SPS server and periodically send data."""
    
    server = SPSServer()
    
    # Optional: Set callback for received data
    def on_data(data):
        print(f"Application received: {data.decode('utf-8', errors='replace')}")
        # Echo back to client
        asyncio.create_task(server.send_data(b"Echo: " + data))
    
    server.on_data_received = on_data
    
    await server.start("My-SPS-Server")
    
    print("Server running. Press Ctrl+C to stop.")
    print("Connect with a BLE client and send data to test.")
    
    try:
        # Keep server running and periodically send data
        counter = 0
        while True:
            await asyncio.sleep(10)
            if server.is_connected:
                counter += 1
                await server.send_string(f"Server heartbeat #{counter}")
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        await server.stop()


async def echo_server():
    """Example: Simple echo server - reflects all received data."""
    
    server = SPSServer()
    
    def echo_handler(data):
        print(f"Echoing: {data}")
        asyncio.create_task(server.send_data(data))
    
    server.on_data_received = echo_handler
    
    await server.start("SPS-Echo")
    
    try:
        while True:
            await asyncio.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        await server.stop()


if __name__ == "__main__":
    # Run the main example
    asyncio.run(main())
    
    # Or run the echo server:
    # asyncio.run(echo_server())
```

---

## API Reference

### SPSServer Class

#### Constructor

```python
server = SPSServer()
```

Creates a new SPS server instance.

#### Properties

| Property | Type | Description |
|----------|------|-------------|
| `tx_credits` | int | Credits available to send to client |
| `rx_buffer` | bytearray | Buffer containing received data |
| `is_connected` | bool | Whether a client is connected |
| `on_data_received` | callable | Callback for received data |

#### Configuration

| Property | Default | Description |
|----------|---------|-------------|
| `INITIAL_CREDITS` | 8 | Credits granted on connection |
| `CREDIT_THRESHOLD` | 4 | Grant credits after N packets |
| `MAX_PACKET_SIZE` | 20 | Max bytes per notification |

#### Methods

| Method | Description |
|--------|-------------|
| `await start(device_name)` | Start server and advertising |
| `await stop()` | Stop server |
| `await send_data(data)` | Queue data to send to client |
| `await send_string(text)` | Queue string to send |
| `await grant_credits(count)` | Grant credits to client |
| `read_buffer()` | Read and clear receive buffer |

---

## Flow Control Details

### Receiving Data from Client

When the client writes to FIFO, we receive the data and track credits:

```python
# Automatic: credits granted every CREDIT_THRESHOLD packets
# Manual: call grant_credits() directly
await server.grant_credits(8)
```

### Sending Data to Client

```python
# Queue data to send
await server.send_data(b"Hello client!")

# Data is sent when:
# 1. We have credits (client granted them)
# 2. flush_tx_buffer() is called (automatic after grant)
```

---

## Platform-Specific Notes

### Linux (BlueZ)

Best platform support. May need to run with sudo:

```bash
sudo python sps_server.py
```

Or configure D-Bus permissions. You may also need to enable experimental features:

```bash
sudo btmgmt power off
sudo btmgmt bredr off
sudo btmgmt power on
```

### macOS

Works with CoreBluetooth. The device name may appear differently in scanner apps.

### Windows

Limited support through `bless`. Consider using native Windows BLE APIs for production.

---

## Troubleshooting

### "Server not visible in scan"

- Ensure Bluetooth is enabled and adapter supports BLE advertising
- On Linux, check BlueZ version (`bluetoothctl --version`)
- Try running with sudo

### "Client connects but no data"

- Verify client enables notifications on FIFO and Credits CCCDs
- Check that initial credits are being sent

### "tx_credits stays at 0"

- Client is not granting credits back
- Check client implementation grants credits after receiving data

### "Notifications fail to send"

- Verify `is_connected` is True
- Ensure client has enabled notifications (CCCD)
- Check that you have `tx_credits > 0`

---

## See Also

- [SPS Protocol Specification](SPS_PROTOCOL.md) - Understand the protocol
- [Python Client Implementation](SPS_CLIENT_PYTHON.md) - Build an SPS client
- [u-blox Module Configuration](SPS_UBLOX_CONFIG.md) - Configure u-blox modules
