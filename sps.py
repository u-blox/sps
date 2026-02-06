#!/usr/bin/env python3
"""
u-blox SPS (Serial Port Service) - BLE Communication Library
=============================================================

A comprehensive library and CLI tool for SPS communication over Bluetooth Low Energy.
SPS is u-blox's proprietary protocol for serial port emulation over BLE.

ROLES:
    CLIENT (Central)     - Connects to an SPS peripheral/server
    PERIPHERAL (Server)  - Accepts connections from SPS clients

ARCHITECTURE:
    ┌─────────────┐                    ┌─────────────┐
    │  SPS Client │  ◄── BLE Link ──►  │ SPS Server  │
    │  (Central)  │                    │ (Peripheral)│
    └─────────────┘                    └─────────────┘
         │                                    │
         │ Bleak library                      │ Bless library
         ▼                                    ▼
    ┌─────────────┐                    ┌─────────────┐
    │   Your PC   │                    │  NORA-W36   │
    │   RPi, etc  │                    │  or any SPS │
    └─────────────┘                    └─────────────┘

SPS PROTOCOL:
    Service UUID:  2456e1b9-26e2-8f83-e744-f34f01e9d701
    FIFO UUID:     2456e1b9-26e2-8f83-e744-f34f01e9d703  (data transfer)
    Credits UUID:  2456e1b9-26e2-8f83-e744-f34f01e9d704  (flow control)

    Flow Control:
    - Credits-based flow control prevents buffer overflow
    - Each side grants credits to allow the other side to send
    - One credit = one packet (up to MTU-3 bytes, typically 244)
    - Initial credits granted after connection establishment

REQUIREMENTS:
    pip install bleak bless

USAGE:
    # As a library
    from sps import SPSClient, SPSPeripheral
    
    # As CLI tool
    python sps.py scan                           # Scan for BLE devices
    python sps.py client -a AA:BB:CC:DD:EE:FF    # Connect as client
    python sps.py server -n "My-Device"          # Run as peripheral/server

API DOCUMENTATION:
    See SPSClient and SPSPeripheral class docstrings below.

Author: u-blox Application Engineering
License: Apache 2.0
"""

import sys
sys.dont_write_bytecode = True  # Prevent __pycache__ creation

import asyncio
import argparse
from typing import Optional, Callable, List, Tuple
from dataclasses import dataclass
from enum import IntEnum

# =============================================================================
# SPS Protocol Constants
# =============================================================================

# Service and Characteristic UUIDs (u-blox proprietary)
SPS_SERVICE_UUID = "2456e1b9-26e2-8f83-e744-f34f01e9d701"
SPS_FIFO_UUID = "2456e1b9-26e2-8f83-e744-f34f01e9d703"      # Data transfer
SPS_CREDITS_UUID = "2456e1b9-26e2-8f83-e744-f34f01e9d704"   # Flow control

# Default configuration
DEFAULT_MTU = 247           # BLE 5.x default MTU
DEFAULT_PACKET_SIZE = 244   # MTU - 3 (ATT header)
INITIAL_CREDITS = 8         # Credits granted on connection
CREDIT_THRESHOLD = 4        # Grant more credits when this many consumed


class SPSState(IntEnum):
    """SPS connection state."""
    DISCONNECTED = 0
    CONNECTING = 1
    CONNECTED = 2
    READY = 3  # Connected and credits exchanged


@dataclass
class SPSStats:
    """Statistics for SPS connection."""
    bytes_sent: int = 0
    bytes_received: int = 0
    packets_sent: int = 0
    packets_received: int = 0
    credits_sent: int = 0
    credits_received: int = 0


# =============================================================================
# SPS Client (Central Role)
# =============================================================================

class SPSClient:
    """
    SPS Client - connects to an SPS peripheral and exchanges data.
    
    Use this when your device acts as the BLE Central (initiator).
    Typical use case: PC/Raspberry Pi connecting to a u-blox module.
    
    Example:
        ```python
        import asyncio
        from sps import SPSClient
        
        async def main():
            client = SPSClient()
            
            # Set callback for received data
            client.on_data = lambda data: print(f"Received: {data}")
            
            # Connect to peripheral
            await client.connect("AA:BB:CC:DD:EE:FF")
            
            # Send data
            await client.send(b"Hello, SPS!")
            await client.send_line("AT")
            
            # Keep connection alive
            await asyncio.sleep(10)
            
            # Disconnect
            await client.disconnect()
        
        asyncio.run(main())
        ```
    
    Attributes:
        state (SPSState): Current connection state
        tx_credits (int): Available credits for sending
        stats (SPSStats): Connection statistics
        on_data (Callable): Callback for received data
        on_state_change (Callable): Callback for state changes
    """
    
    def __init__(self, packet_size: int = DEFAULT_PACKET_SIZE):
        """
        Initialize SPS client.
        
        Args:
            packet_size: Maximum packet size (default: 244 bytes)
        """
        self._client = None
        self._state = SPSState.DISCONNECTED
        self._packet_size = packet_size
        
        # Flow control
        self.tx_credits = 0
        self._rx_credits_pending = 0
        self._credits_event = asyncio.Event()
        
        # Buffers
        self._rx_buffer = bytearray()
        
        # Statistics
        self.stats = SPSStats()
        
        # Callbacks
        self.on_data: Optional[Callable[[bytes], None]] = None
        self.on_state_change: Optional[Callable[[SPSState], None]] = None
    
    @property
    def state(self) -> SPSState:
        """Get current connection state."""
        return self._state
    
    @state.setter
    def state(self, value: SPSState):
        """Set state and trigger callback."""
        self._state = value
        if self.on_state_change:
            self.on_state_change(value)
    
    @property
    def is_connected(self) -> bool:
        """Check if connected and ready to send data."""
        return self._state >= SPSState.CONNECTED and self._client and self._client.is_connected
    
    @property
    def is_ready(self) -> bool:
        """Check if ready (connected and have credits)."""
        return self._state == SPSState.READY and self.tx_credits > 0
    
    def _handle_fifo_notification(self, sender, data: bytes):
        """Handle data received from peripheral."""
        self._rx_buffer.extend(data)
        self.stats.bytes_received += len(data)
        self.stats.packets_received += 1
        
        # Trigger callback
        if self.on_data:
            self.on_data(bytes(data))
        
        # Grant credits back
        self._rx_credits_pending += 1
        if self._rx_credits_pending >= CREDIT_THRESHOLD:
            asyncio.create_task(self._grant_credits(self._rx_credits_pending))
            self._rx_credits_pending = 0
    
    def _handle_credits_notification(self, sender, data: bytes):
        """Handle credits granted by peripheral."""
        if len(data) > 0:
            credits = data[0]
            self.tx_credits += credits
            self.stats.credits_received += credits
            self._credits_event.set()
            
            # Transition to READY state on first credits
            if self._state == SPSState.CONNECTED:
                self.state = SPSState.READY
    
    async def connect(self, address: str, timeout: float = 10.0) -> bool:
        """
        Connect to an SPS peripheral.
        
        Args:
            address: Bluetooth address (e.g., "AA:BB:CC:DD:EE:FF")
            timeout: Connection timeout in seconds
            
        Returns:
            True if connected successfully
            
        Raises:
            ConnectionError: If connection fails
        """
        from bleak import BleakClient
        
        self.state = SPSState.CONNECTING
        
        try:
            self._client = BleakClient(address)
            await self._client.connect(timeout=timeout)
            self.state = SPSState.CONNECTED
            
            # Enable notifications
            await self._client.start_notify(SPS_FIFO_UUID, self._handle_fifo_notification)
            await self._client.start_notify(SPS_CREDITS_UUID, self._handle_credits_notification)
            
            # Wait for initial credits
            try:
                await asyncio.wait_for(self._credits_event.wait(), timeout=3.0)
            except asyncio.TimeoutError:
                pass  # Some peripherals don't send initial credits
            
            return True
            
        except Exception as e:
            self.state = SPSState.DISCONNECTED
            raise ConnectionError(f"Failed to connect: {e}")
    
    async def disconnect(self):
        """Disconnect from peripheral."""
        if self._client:
            try:
                await self._client.disconnect()
            except:
                pass
        self._client = None
        self.state = SPSState.DISCONNECTED
        self.tx_credits = 0
    
    async def _grant_credits(self, count: int):
        """Grant credits to peripheral (internal)."""
        if self._client and self._client.is_connected:
            await self._client.write_gatt_char(SPS_CREDITS_UUID, bytes([count]), response=False)
            self.stats.credits_sent += count
    
    async def send(self, data: bytes, timeout: float = 5.0) -> int:
        """
        Send data to peripheral.
        
        Data is automatically chunked to fit packet size.
        Waits for credits if none available.
        
        Args:
            data: Data to send
            timeout: Timeout waiting for credits
            
        Returns:
            Number of bytes sent
            
        Raises:
            TimeoutError: If no credits available within timeout
        """
        if not self.is_connected:
            raise ConnectionError("Not connected")
        
        bytes_sent = 0
        offset = 0
        
        while offset < len(data):
            # Wait for credits
            if self.tx_credits <= 0:
                self._credits_event.clear()
                try:
                    await asyncio.wait_for(self._credits_event.wait(), timeout)
                except asyncio.TimeoutError:
                    raise TimeoutError("No credits available")
            
            # Send chunk
            chunk_size = min(self._packet_size, len(data) - offset)
            chunk = data[offset:offset + chunk_size]
            
            await self._client.write_gatt_char(SPS_FIFO_UUID, chunk, response=False)
            self.tx_credits -= 1
            self.stats.bytes_sent += len(chunk)
            self.stats.packets_sent += 1
            
            bytes_sent += len(chunk)
            offset += chunk_size
        
        return bytes_sent
    
    async def send_line(self, text: str, newline: str = "\r\n") -> int:
        """
        Send text line with newline.
        
        Args:
            text: Text to send
            newline: Newline characters (default: CRLF)
            
        Returns:
            Number of bytes sent
        """
        return await self.send((text + newline).encode('utf-8'))
    
    def read(self, clear: bool = True) -> bytes:
        """
        Read received data from buffer.
        
        Args:
            clear: Clear buffer after reading (default: True)
            
        Returns:
            Received data
        """
        data = bytes(self._rx_buffer)
        if clear:
            self._rx_buffer.clear()
        return data
    
    def read_line(self, clear: bool = True) -> Optional[str]:
        """
        Read a complete line from buffer.
        
        Args:
            clear: Remove line from buffer after reading
            
        Returns:
            Line without newline, or None if no complete line
        """
        # Look for newline
        for i, b in enumerate(self._rx_buffer):
            if b == ord('\n'):
                line = bytes(self._rx_buffer[:i]).decode('utf-8').rstrip('\r')
                if clear:
                    del self._rx_buffer[:i+1]
                return line
        return None


# =============================================================================
# SPS Peripheral (Server Role)
# =============================================================================

class SPSPeripheral:
    """
    SPS Peripheral - accepts connections from SPS clients.
    
    Use this when your device acts as the BLE Peripheral (advertiser).
    Typical use case: Creating an SPS bridge on Raspberry Pi.
    
    Example:
        ```python
        import asyncio
        from sps import SPSPeripheral
        
        async def main():
            peripheral = SPSPeripheral()
            
            # Set callback for received data
            peripheral.on_data = lambda data: print(f"Received: {data}")
            
            # Set callback for connection events
            peripheral.on_connect = lambda: print("Client connected!")
            peripheral.on_disconnect = lambda: print("Client disconnected!")
            
            # Start advertising
            await peripheral.start("My-SPS-Device")
            
            # Wait for connection and send data
            while peripheral.state != SPSState.READY:
                await asyncio.sleep(0.1)
            
            await peripheral.send(b"Welcome!")
            
            # Keep running
            try:
                while True:
                    await asyncio.sleep(1)
            except KeyboardInterrupt:
                pass
            
            await peripheral.stop()
        
        asyncio.run(main())
        ```
    
    Attributes:
        state (SPSState): Current connection state
        tx_credits (int): Available credits for sending
        stats (SPSStats): Connection statistics
        on_data (Callable): Callback for received data
        on_connect (Callable): Callback for client connection
        on_disconnect (Callable): Callback for client disconnection
    """
    
    def __init__(self, packet_size: int = DEFAULT_PACKET_SIZE):
        """
        Initialize SPS peripheral.
        
        Args:
            packet_size: Maximum packet size (default: 244 bytes)
        """
        self._server = None
        self._state = SPSState.DISCONNECTED
        self._packet_size = packet_size
        
        # Flow control
        self.tx_credits = 0
        self._rx_credits_pending = 0
        
        # Buffers
        self._rx_buffer = bytearray()
        self._tx_buffer = bytearray()
        
        # Statistics
        self.stats = SPSStats()
        
        # Callbacks
        self.on_data: Optional[Callable[[bytes], None]] = None
        self.on_connect: Optional[Callable[[], None]] = None
        self.on_disconnect: Optional[Callable[[], None]] = None
    
    @property
    def state(self) -> SPSState:
        """Get current connection state."""
        return self._state
    
    @property
    def is_connected(self) -> bool:
        """Check if a client is connected."""
        return self._state >= SPSState.CONNECTED
    
    @property
    def is_ready(self) -> bool:
        """Check if ready to send data."""
        return self._state == SPSState.READY and self.tx_credits > 0
    
    def _on_fifo_write(self, characteristic, value: bytes):
        """Handle data received from client."""
        self._rx_buffer.extend(value)
        self.stats.bytes_received += len(value)
        self.stats.packets_received += 1
        
        if self.on_data:
            self.on_data(bytes(value))
        
        # Grant credits back
        self._rx_credits_pending += 1
        if self._rx_credits_pending >= CREDIT_THRESHOLD:
            asyncio.create_task(self._grant_credits(self._rx_credits_pending))
            self._rx_credits_pending = 0
    
    def _on_credits_write(self, characteristic, value: bytes):
        """Handle credits granted by client."""
        if len(value) > 0:
            credits = value[0]
            self.tx_credits += credits
            self.stats.credits_received += credits
            
            # Transition to READY and flush buffer
            if self._state == SPSState.CONNECTED:
                self._state = SPSState.READY
            
            asyncio.create_task(self._flush_tx_buffer())
    
    def _handle_write(self, characteristic, value: bytes):
        """Route write requests."""
        char_uuid = str(characteristic.uuid).lower()
        if SPS_FIFO_UUID.lower() in char_uuid:
            self._on_fifo_write(characteristic, value)
        elif SPS_CREDITS_UUID.lower() in char_uuid:
            self._on_credits_write(characteristic, value)
    
    def _handle_read(self, characteristic) -> bytes:
        """Handle read requests."""
        return bytes([0])
    
    async def _grant_credits(self, count: int):
        """Grant credits to client."""
        if self._server and self._state >= SPSState.CONNECTED:
            try:
                await self._server.update_value(SPS_SERVICE_UUID, SPS_CREDITS_UUID, bytes([count]))
                self.stats.credits_sent += count
            except Exception:
                pass
    
    async def _delayed_initial_credits(self):
        """Grant initial credits after connection."""
        await asyncio.sleep(0.5)
        await self._grant_credits(INITIAL_CREDITS)
    
    async def _flush_tx_buffer(self):
        """Send pending data if credits available."""
        if not self._server or self._state < SPSState.READY:
            return
        
        while self.tx_credits > 0 and len(self._tx_buffer) > 0:
            chunk_size = min(self._packet_size, len(self._tx_buffer))
            chunk = bytes(self._tx_buffer[:chunk_size])
            del self._tx_buffer[:chunk_size]
            
            try:
                await self._server.update_value(SPS_SERVICE_UUID, SPS_FIFO_UUID, chunk)
                self.tx_credits -= 1
                self.stats.bytes_sent += len(chunk)
                self.stats.packets_sent += 1
            except Exception:
                # Put data back on failure
                self._tx_buffer = bytearray(chunk) + self._tx_buffer
                break
    
    async def start(self, name: str = "SPS-Peripheral"):
        """
        Start advertising as SPS peripheral.
        
        Args:
            name: Device name to advertise
        """
        from bless import (
            BlessServer,
            GATTCharacteristicProperties,
            GATTAttributePermissions
        )
        
        self._server = BlessServer(name=name)
        self._server.write_request_func = self._handle_write
        self._server.read_request_func = self._handle_read
        
        # Create SPS service
        await self._server.add_new_service(SPS_SERVICE_UUID)
        
        # FIFO characteristic (data transfer)
        fifo_props = (
            GATTCharacteristicProperties.write_without_response |
            GATTCharacteristicProperties.write |
            GATTCharacteristicProperties.notify
        )
        await self._server.add_new_characteristic(
            SPS_SERVICE_UUID, SPS_FIFO_UUID, fifo_props, bytes([0]),
            GATTAttributePermissions.readable | GATTAttributePermissions.writeable
        )
        
        # Credits characteristic (flow control)
        credits_props = (
            GATTCharacteristicProperties.write_without_response |
            GATTCharacteristicProperties.write |
            GATTCharacteristicProperties.notify
        )
        await self._server.add_new_characteristic(
            SPS_SERVICE_UUID, SPS_CREDITS_UUID, credits_props, bytes([0]),
            GATTAttributePermissions.readable | GATTAttributePermissions.writeable
        )
        
        await self._server.start()
        self._state = SPSState.CONNECTED
        
        # Grant initial credits
        asyncio.create_task(self._delayed_initial_credits())
        
        if self.on_connect:
            self.on_connect()
    
    async def stop(self):
        """Stop advertising and disconnect."""
        if self._server:
            await self._server.stop()
        self._server = None
        self._state = SPSState.DISCONNECTED
        self.tx_credits = 0
        
        if self.on_disconnect:
            self.on_disconnect()
    
    async def send(self, data: bytes) -> int:
        """
        Send data to connected client.
        
        Data is queued and sent when credits are available.
        
        Args:
            data: Data to send
            
        Returns:
            Number of bytes queued
        """
        self._tx_buffer.extend(data)
        await self._flush_tx_buffer()
        return len(data)
    
    async def send_line(self, text: str, newline: str = "\r\n") -> int:
        """
        Send text line with newline.
        
        Args:
            text: Text to send
            newline: Newline characters (default: CRLF)
            
        Returns:
            Number of bytes queued
        """
        return await self.send((text + newline).encode('utf-8'))
    
    def read(self, clear: bool = True) -> bytes:
        """
        Read received data from buffer.
        
        Args:
            clear: Clear buffer after reading (default: True)
            
        Returns:
            Received data
        """
        data = bytes(self._rx_buffer)
        if clear:
            self._rx_buffer.clear()
        return data


# =============================================================================
# BLE Scanner
# =============================================================================

@dataclass
class BLEDevice:
    """Information about a discovered BLE device."""
    address: str
    name: str
    rssi: int
    has_sps: bool


async def scan_devices(timeout: float = 5.0, sps_only: bool = False) -> List[BLEDevice]:
    """
    Scan for BLE devices.
    
    Args:
        timeout: Scan duration in seconds
        sps_only: Only return devices advertising SPS service
        
    Returns:
        List of discovered devices
    """
    from bleak import BleakScanner
    
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    
    result = []
    for address, (device, adv_data) in devices.items():
        # Check for SPS service
        service_uuids = [str(u).lower() for u in (adv_data.service_uuids or [])]
        has_sps = SPS_SERVICE_UUID.lower() in service_uuids
        
        if sps_only and not has_sps:
            continue
        
        result.append(BLEDevice(
            address=address,
            name=device.name or "(unknown)",
            rssi=adv_data.rssi or -100,
            has_sps=has_sps
        ))
    
    return sorted(result, key=lambda d: d.rssi, reverse=True)


# =============================================================================
# Interactive Console
# =============================================================================

async def interactive_console(sps, role: str):
    """
    Interactive console for SPS communication.
    
    Commands:
        <text>     - Send text (adds CRLF)
        /raw XX    - Send raw text (no newline)
        /hex XX    - Send hex bytes
        /at CMD    - Send AT command
        /credits   - Show credit status
        /stats     - Show statistics
        /buffer    - Show receive buffer
        /clear     - Clear receive buffer
        /quit      - Exit
    """
    print("\n" + "=" * 60)
    print(f"SPS {role.upper()} Interactive Console")
    print("=" * 60)
    print("Commands:")
    print("  <text>      Send text with CRLF")
    print("  /raw XX     Send raw text (no newline)")
    print("  /hex XX     Send hex bytes (e.g., /hex 48454C4C4F)")
    print("  /at CMD     Send AT command with CR")
    print("  /credits    Show credit status")
    print("  /stats      Show statistics")
    print("  /buffer     Show and clear receive buffer")
    print("  /clear      Clear receive buffer")
    print("  /quit       Exit")
    print("=" * 60 + "\n")
    
    # Data received callback
    def on_rx(data: bytes):
        try:
            text = data.decode('utf-8')
            if text.strip():
                print(f"\n<< {text.rstrip()}")
        except UnicodeDecodeError:
            print(f"\n<< (hex) {data.hex()}")
        print("> ", end="", flush=True)
    
    sps.on_data = on_rx
    
    # Input loop
    loop = asyncio.get_event_loop()
    while True:
        try:
            line = await loop.run_in_executor(None, input, "> ")
            line = line.strip()
            
            if not line:
                continue
            
            if line.lower() == "/quit":
                break
            elif line.lower() == "/credits":
                print(f"TX credits: {sps.tx_credits}")
            elif line.lower() == "/stats":
                s = sps.stats
                print(f"Bytes: sent={s.bytes_sent}, received={s.bytes_received}")
                print(f"Packets: sent={s.packets_sent}, received={s.packets_received}")
                print(f"Credits: sent={s.credits_sent}, received={s.credits_received}")
            elif line.lower() == "/buffer":
                data = sps.read()
                if data:
                    try:
                        print(f"Buffer ({len(data)} bytes): {data.decode('utf-8')}")
                    except UnicodeDecodeError:
                        print(f"Buffer ({len(data)} bytes, hex): {data.hex()}")
                else:
                    print("Buffer empty")
            elif line.lower() == "/clear":
                sps.read()
                print("Buffer cleared")
            elif line.lower().startswith("/hex "):
                hex_str = line[5:].replace(" ", "")
                try:
                    data = bytes.fromhex(hex_str)
                    await sps.send(data)
                    print(f">> (hex) {data.hex()}")
                except ValueError:
                    print("Invalid hex string")
            elif line.lower().startswith("/raw "):
                text = line[5:]
                await sps.send(text.encode('utf-8'))
                print(f">> {text}")
            elif line.lower().startswith("/at "):
                cmd = line[4:]
                await sps.send((cmd + "\r").encode('utf-8'))
                print(f">> AT: {cmd}")
            else:
                await sps.send_line(line)
                print(f">> {line}")
                
        except EOFError:
            break
        except Exception as e:
            print(f"Error: {e}")


# =============================================================================
# CLI Entry Points
# =============================================================================

async def cmd_scan(args):
    """Scan for BLE devices."""
    print(f"Scanning for BLE devices ({args.timeout}s)...")
    devices = await scan_devices(args.timeout, args.sps_only)
    
    print(f"\nFound {len(devices)} device(s):\n")
    print(f"{'Address':<20} {'Name':<25} {'RSSI':<6} {'SPS'}")
    print("-" * 60)
    
    for dev in devices:
        sps_mark = "✓" if dev.has_sps else ""
        print(f"{dev.address:<20} {dev.name:<25} {dev.rssi:<6} {sps_mark}")


async def cmd_client(args):
    """Run as SPS client."""
    client = SPSClient()
    
    print(f"Connecting to {args.address}...")
    try:
        await client.connect(args.address, timeout=args.timeout)
        print(f"Connected! State: {client.state.name}, Credits: {client.tx_credits}")
        
        if args.interactive:
            await interactive_console(client, "client")
        else:
            # Simple test
            await client.send_line("Hello from SPS client!")
            await asyncio.sleep(2)
            data = client.read()
            if data:
                print(f"Received: {data}")
    finally:
        await client.disconnect()
        print("Disconnected")


async def cmd_server(args):
    """Run as SPS peripheral/server."""
    peripheral = SPSPeripheral()
    
    peripheral.on_connect = lambda: print("Client connected!")
    
    print(f"Starting SPS peripheral '{args.name}'...")
    try:
        await peripheral.start(args.name)
        print(f"Advertising as '{args.name}'")
        print(f"Service UUID: {SPS_SERVICE_UUID}")
        
        if args.interactive:
            await interactive_console(peripheral, "peripheral")
        else:
            print("Press Ctrl+C to stop...")
            while True:
                await asyncio.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        await peripheral.stop()
        print("Stopped")


def main():
    parser = argparse.ArgumentParser(
        description="u-blox SPS (Serial Port Service) - BLE Communication Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s scan                          Scan for BLE devices
  %(prog)s scan --sps-only               Scan for SPS devices only
  %(prog)s client -a AA:BB:CC:DD:EE:FF   Connect to device as client
  %(prog)s server -n "My-SPS-Bridge"     Run as SPS peripheral

SPS UUIDs:
  Service:  2456e1b9-26e2-8f83-e744-f34f01e9d701
  FIFO:     2456e1b9-26e2-8f83-e744-f34f01e9d703
  Credits:  2456e1b9-26e2-8f83-e744-f34f01e9d704
        """
    )
    
    subparsers = parser.add_subparsers(dest="command", help="Command")
    
    # Scan
    scan_p = subparsers.add_parser("scan", help="Scan for BLE devices")
    scan_p.add_argument("-t", "--timeout", type=float, default=5.0,
                        help="Scan timeout (default: 5s)")
    scan_p.add_argument("--sps-only", action="store_true",
                        help="Only show devices with SPS service")
    
    # Client
    client_p = subparsers.add_parser("client", help="Connect as SPS client (central)")
    client_p.add_argument("-a", "--address", required=True,
                          help="Bluetooth address (e.g., AA:BB:CC:DD:EE:FF)")
    client_p.add_argument("-t", "--timeout", type=float, default=10.0,
                          help="Connection timeout (default: 10s)")
    client_p.add_argument("--no-interactive", dest="interactive",
                          action="store_false", help="Disable interactive mode")
    
    # Server/Peripheral
    server_p = subparsers.add_parser("server", help="Run as SPS peripheral (advertiser)")
    server_p.add_argument("-n", "--name", default="SPS-Peripheral",
                          help="Device name to advertise (default: SPS-Peripheral)")
    server_p.add_argument("--no-interactive", dest="interactive",
                          action="store_false", help="Disable interactive mode")
    
    # Alias for server
    peripheral_p = subparsers.add_parser("peripheral", help="Alias for 'server'")
    peripheral_p.add_argument("-n", "--name", default="SPS-Peripheral",
                              help="Device name to advertise")
    peripheral_p.add_argument("--no-interactive", dest="interactive",
                              action="store_false", help="Disable interactive mode")
    
    args = parser.parse_args()
    
    if not args.command:
        parser.print_help()
        sys.exit(1)
    
    try:
        if args.command == "scan":
            asyncio.run(cmd_scan(args))
        elif args.command == "client":
            asyncio.run(cmd_client(args))
        elif args.command in ("server", "peripheral"):
            asyncio.run(cmd_server(args))
    except KeyboardInterrupt:
        print("\nInterrupted")
    except ImportError as e:
        print(f"Missing dependency: {e}")
        print("Install with: pip install bleak bless")
        sys.exit(1)


if __name__ == "__main__":
    main()
