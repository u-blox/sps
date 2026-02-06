# u-blox SPS Python Client Implementation

This document provides a complete **SPS Client** implementation in Python using the [Bleak](https://bleak.readthedocs.io/) library.

**Related Documentation:**
- [SPS Protocol Specification](SPS_PROTOCOL.md)
- [Python Server Implementation](SPS_SERVER_PYTHON.md)
- [C Client Implementation](SPS_CLIENT_C.md)

---

## Overview

An **SPS Client**:
- Scans for and connects to an SPS Server
- Writes data TO the server (via FIFO characteristic)
- Receives data FROM the server (via FIFO notifications)
- Manages flow control credits in both directions

## Requirements

```bash
pip install bleak
```

- Python 3.7 or later
- Bluetooth adapter with BLE support
- Works on Windows, macOS, and Linux

## Quick Start

```python
import asyncio
from bleak import BleakClient

SPS_FIFO_UUID = "2456e1b9-26e2-8f83-e744-f34f01e9d703"

async def main():
    async with BleakClient("AA:BB:CC:DD:EE:FF") as client:
        await client.start_notify(SPS_FIFO_UUID, lambda s, d: print(f"RX: {d}"))
        await client.write_gatt_char(SPS_FIFO_UUID, b"Hello!")

asyncio.run(main())
```

---

## Complete Implementation

```python
"""
u-blox SPS CLIENT Implementation
================================
This code runs on YOUR device (PC, Raspberry Pi, etc.)
It connects to a u-blox module (or any device) running as SPS SERVER.

Role: CLIENT (initiates connection, writes to server, receives notifications)

Requirements:
    pip install bleak
    
Usage:
    python sps_client.py
"""
import asyncio
from bleak import BleakClient, BleakScanner

# =============================================================================
# SPS UUIDs - defined by u-blox, same on all devices
# =============================================================================
SPS_SERVICE_UUID = "2456e1b9-26e2-8f83-e744-f34f01e9d701"   # SPS Service
SPS_FIFO_UUID = "2456e1b9-26e2-8f83-e744-f34f01e9d703"      # Data transfer
SPS_CREDITS_UUID = "2456e1b9-26e2-8f83-e744-f34f01e9d704"   # Flow control


class SPSClient:
    """
    SPS Client implementation with full flow control support.
    
    This is the CLIENT side - it:
    - Scans for and connects to an SPS server
    - Writes data TO the server (via FIFO characteristic)
    - Receives data FROM the server (via FIFO notifications)
    - Manages flow control credits in both directions
    """
    
    def __init__(self):
        self.client = None
        
        # Flow control: how many packets we can SEND to the server
        self.tx_credits = 0
        
        # Flow control: how many packets we've RECEIVED (need to grant back)
        self.rx_credits_pending = 0
        
        # Buffer for received data
        self.rx_buffer = bytearray()
        
        # Event for waiting on credits
        self._credits_event = asyncio.Event()
        
        # Callback for received data (user can override)
        self.on_data_received = None
    
    def handle_fifo_notification(self, sender, data):
        """
        Called when SERVER sends data TO us.
        
        This is a NOTIFICATION from the server's FIFO characteristic.
        The server can only send this after we enabled notifications (CCCD).
        """
        self.rx_buffer.extend(data)
        print(f"RX from server: {data.hex()} ({len(data)} bytes)")
        
        # Call user callback if set
        if self.on_data_received:
            self.on_data_received(data)
        
        # We received a packet - track it for flow control
        self.rx_credits_pending += 1
        
        # Grant credits back to server periodically (every 4 packets)
        # This tells the server: "I processed these, you can send more"
        if self.rx_credits_pending >= 4:
            asyncio.create_task(self.grant_credits(self.rx_credits_pending))
            self.rx_credits_pending = 0
    
    def handle_credits_notification(self, sender, data):
        """
        Called when SERVER grants us credits.
        
        This is a NOTIFICATION from the server's Credits characteristic.
        Each credit allows us to send one packet to the server.
        """
        new_credits = data[0]
        self.tx_credits += new_credits
        print(f"Server granted {new_credits} credits, total: {self.tx_credits}")
        
        # Signal that credits are available
        self._credits_event.set()
    
    async def scan(self, timeout: float = 5.0) -> list:
        """
        Scan for SPS servers.
        
        Returns a list of devices advertising the SPS service.
        """
        print(f"Scanning for SPS devices ({timeout}s)...")
        devices = await BleakScanner.discover(timeout=timeout)
        
        sps_devices = []
        for device in devices:
            # Check if device advertises SPS service
            # Note: Not all devices include service UUIDs in advertisements
            print(f"Found: {device.name or 'Unknown'} - {device.address}")
            sps_devices.append(device)
        
        return sps_devices
    
    async def connect(self, address: str):
        """
        Connect to an SPS server.
        
        Steps performed:
        1. Establish BLE connection
        2. Enable notifications on FIFO (to receive data)
        3. Enable notifications on Credits (to receive flow control)
        4. Wait for server to grant initial credits
        
        Args:
            address: Bluetooth address of the server (e.g., "AA:BB:CC:DD:EE:FF")
        """
        self.client = BleakClient(address)
        await self.client.connect()
        print(f"Connected to SPS server: {address}")
        
        # Enable notifications - CRITICAL!
        # Without this, we won't receive any data or credits from the server
        await self.client.start_notify(SPS_FIFO_UUID, self.handle_fifo_notification)
        await self.client.start_notify(SPS_CREDITS_UUID, self.handle_credits_notification)
        print("Notifications enabled, waiting for initial credits...")
        
        # Wait for server to send initial credits (with timeout)
        try:
            await asyncio.wait_for(self._credits_event.wait(), timeout=2.0)
        except asyncio.TimeoutError:
            print("Warning: No initial credits received from server")
        
        print(f"Ready! Have {self.tx_credits} credits to send")
    
    async def disconnect(self):
        """Disconnect from the SPS server."""
        if self.client and self.client.is_connected:
            await self.client.disconnect()
            print("Disconnected from SPS server")
    
    async def write(self, data: bytes):
        """
        Send data TO the SPS server.
        
        This WRITES to the server's FIFO characteristic.
        Each write costs one credit. Will wait for credits if none available.
        
        Args:
            data: Bytes to send (max 244 bytes with MTU 247)
        """
        # Wait for credits if we don't have any
        while self.tx_credits <= 0:
            print("Waiting for credits...")
            self._credits_event.clear()
            await self._credits_event.wait()
        
        await self.client.write_gatt_char(SPS_FIFO_UUID, data, response=False)
        self.tx_credits -= 1
        print(f"TX to server: {data.hex()}, credits remaining: {self.tx_credits}")
    
    async def write_string(self, text: str):
        """Send a string to the SPS server (UTF-8 encoded)."""
        await self.write(text.encode('utf-8'))
    
    async def grant_credits(self, count: int = 1):
        """
        Grant credits TO the server.
        
        This tells the server: "I can receive <count> more packets"
        The server will use these credits when sending data to us.
        
        Args:
            count: Number of credits to grant (1-255)
        """
        await self.client.write_gatt_char(SPS_CREDITS_UUID, bytes([count]), response=False)
        print(f"Granted {count} credits to server")
    
    def read_buffer(self) -> bytes:
        """Read and clear the receive buffer."""
        data = bytes(self.rx_buffer)
        self.rx_buffer.clear()
        return data


# =============================================================================
# Example Usage
# =============================================================================
async def main():
    """Example: Connect to SPS server and exchange data."""
    
    sps = SPSClient()
    
    # Optional: Set callback for received data
    sps.on_data_received = lambda data: print(f"Callback received: {data}")
    
    # Step 1: Scan for SPS servers
    devices = await sps.scan(timeout=5.0)
    
    if not devices:
        print("No devices found!")
        return
    
    # Step 2: Connect to first device (or specify address)
    # Replace with your device's address
    target_address = "AA:BB:CC:DD:EE:FF"
    
    # Or use first discovered device:
    # target_address = devices[0].address
    
    try:
        await sps.connect(target_address)
        
        # Step 3: Send some data
        await sps.write_string("Hello from SPS client!")
        
        # Step 4: Wait for responses
        print("Waiting for data from server...")
        await asyncio.sleep(5)
        
        # Check received data
        received = sps.read_buffer()
        if received:
            print(f"Total received: {received}")
        
    finally:
        await sps.disconnect()


async def echo_example():
    """Example: Simple echo test - send data and print responses."""
    
    sps = SPSClient()
    
    # Print all received data
    sps.on_data_received = lambda data: print(f"Echo: {data.decode('utf-8', errors='replace')}")
    
    await sps.connect("AA:BB:CC:DD:EE:FF")  # Replace with actual address
    
    try:
        # Send test messages
        for i in range(5):
            await sps.write_string(f"Test message {i+1}")
            await asyncio.sleep(1)
    finally:
        await sps.disconnect()


if __name__ == "__main__":
    # Run the main example
    asyncio.run(main())
    
    # Or run the echo example:
    # asyncio.run(echo_example())
```

---

## API Reference

### SPSClient Class

#### Constructor

```python
sps = SPSClient()
```

Creates a new SPS client instance.

#### Properties

| Property | Type | Description |
|----------|------|-------------|
| `tx_credits` | int | Number of credits available to send |
| `rx_buffer` | bytearray | Buffer containing received data |
| `on_data_received` | callable | Callback function for received data |

#### Methods

| Method | Description |
|--------|-------------|
| `await scan(timeout)` | Scan for BLE devices, returns list |
| `await connect(address)` | Connect to SPS server |
| `await disconnect()` | Disconnect from server |
| `await write(data)` | Send bytes to server |
| `await write_string(text)` | Send string to server |
| `await grant_credits(count)` | Grant credits to server |
| `read_buffer()` | Read and clear receive buffer |

---

## Flow Control Details

### Sending Data

```python
# Each write costs 1 credit
await sps.write(b"data")  # tx_credits decremented

# If no credits, write() will wait automatically
# Or check manually:
if sps.tx_credits > 0:
    await sps.write(b"data")
```

### Receiving Data

Credits are granted automatically every 4 packets. To customize:

```python
# Grant credits manually
await sps.grant_credits(8)  # Allow server to send 8 more packets
```

---

## Troubleshooting

### "No initial credits received"

The server didn't send credits after connection. Check:
- Server has SPS enabled (`AT+USPS=1`)
- Server is advertising (`AT+UBTAL`)
- You're connecting to the correct device

### "No credits available"

The `tx_credits` counter is zero. Wait for server to grant more credits or ensure the server is processing data.

### Connection fails

- Verify the Bluetooth address is correct
- Ensure the device is advertising
- Check that no other device is connected to the server

### No data received

- Verify notifications are enabled (done automatically in `connect()`)
- Check that the server is sending data
- Ensure flow control is working (server has credits to send)

---

## Platform Notes

### Windows

Works out of the box with Windows 10/11 built-in Bluetooth.

### macOS

Works with built-in Bluetooth. May require Bluetooth permissions in System Preferences.

### Linux

Requires BlueZ 5.43+. May need to run with sudo or configure D-Bus permissions:

```bash
sudo python sps_client.py
```

---

## See Also

- [SPS Protocol Specification](SPS_PROTOCOL.md) - Understand the protocol
- [Python Server Implementation](SPS_SERVER_PYTHON.md) - Build an SPS server
- [u-blox Module Configuration](SPS_UBLOX_CONFIG.md) - Configure u-blox modules
