# Configuring SPS on u-blox Modules

This document describes how to configure SPS on u-blox BLE modules using AT commands and the ucxclient API.

**Related Documentation:**
- [SPS Protocol Specification](SPS_PROTOCOL.md)
- [Python Client Implementation](SPS_CLIENT_PYTHON.md)
- [C Client Implementation](SPS_CLIENT_C.md)

---

## Supported Modules

| Module | BLE | Wi-Fi | SPS Server | SPS Client | Notes |
|--------|-----|-------|------------|------------|-------|
| **NORA-W36** | 5.3 | Wi-Fi 4 | ✓ | ✓ | Dual-mode (Wi-Fi + BLE) |
| **NORA-B26** | 6.0 | — | ✓ | ✓ | BLE-only, Central + Peripheral |
| **NORA-B27** | 6.0 | — | ✓ | — | BLE-only, Peripheral only |

> **Note**: Only modules with BLE Central capability can act as SPS Clients.

---

## Configuring as SPS Server

A module configured as **SPS Server** waits for incoming connections from SPS Clients.

### AT Commands

```
# Enable SPS service
AT+USPS=1

# Start legacy advertising (makes device discoverable)
AT+UBTAL

# Check SPS status
AT+USPS?
# Response: +USPS:1

# Optional: Set device name (visible to scanning clients)
AT+UBTLN="My-SPS-Device"

# Optional: Save configuration (persists after reboot)
AT&W
```

### Using ucxclient API (C)

```c
#include "u_cx_sps.h"
#include "u_cx_bluetooth.h"

// Enable SPS service (server mode)
uCxSpsSetServiceEnable(&ucxHandle, U_SPS_SERVICE_OPTION_ENABLE);

// Start advertising so clients can discover this device
uCxBluetoothLegacyAdvertisementStart(&ucxHandle);

// Register callbacks for SPS events
uCxSpsRegisterConnect(&ucxHandle, sps_connect_callback);       // Client connected
uCxSpsRegisterDisconnect(&ucxHandle, sps_disconnect_callback); // Client disconnected
uCxSpsRegisterDataAvailable(&ucxHandle, sps_data_callback);    // Data received from client
```

---

## Configuring as SPS Client

A module configured as **SPS Client** initiates connections to SPS Servers.

> **Note**: Only modules with BLE Central capability support SPS Client mode (NORA-W36, NORA-B26).

### AT Commands

```
# Scan for BLE devices
AT+UBTD
# Response: +UBTD:<address>,<address_type>,<rssi>,"<name>"
# Example: +UBTD:AABBCCDDEEFF,1,-45,"SPS-Server"

# Connect to a discovered server
AT+UBTC=<address>,<address_type>
# Example: AT+UBTC=AABBCCDDEEFF,1
# Response: +UBTC:0  (connection handle = 0)

# Enable SPS on the connection (with flow control)
AT+USPSC=<conn_handle>,<flow_control>
# flow_control: 0=none, 1=credits (recommended)
# Example: AT+USPSC=0,1
# Response: +UESPSC:0  (SPS connected on handle 0)

# Now you can send/receive data (see Data Transfer section below)
```

### Using ucxclient API (C)

```c
#include "u_cx_sps.h"
#include "u_cx_bluetooth.h"

// After discovering a server via scan...

// Connect to the server
int32_t connHandle;
uCxBluetoothConnect(&ucxHandle, serverAddress, U_BD_ADDRESS_TYPE_PUBLIC, &connHandle);

// Enable SPS on the connection with flow control
uCxSpsConnect2(&ucxHandle, connHandle, 1);  // 1 = flow control enabled

// Register for data available events
uCxSpsRegisterDataAvailable(&ucxHandle, sps_data_callback);
```

---

## Data Transfer

### AT Mode (Command-based)

In AT mode, your host MCU sends/receives data using AT commands:

```
# Write data to SPS (up to 1000 bytes)
AT+USPSWB=<conn_handle>,<length>,"<data>"
# Example: AT+USPSWB=0,5,"Hello"

# Read data from SPS
AT+USPSRB=<conn_handle>,<max_bytes>
# Example: AT+USPSRB=0,100
# Response: +USPSRB:0,5,"World"

# Data available URC (unsolicited response)
# +USPSDA:<conn_handle>,<bytes_available>
```

### Using ucxclient API (C)

```c
// Write data to server/client
uint8_t txData[] = "Hello!";
uCxSpsWrite(&ucxHandle, connHandle, txData, sizeof(txData) - 1);

// Read data (call when +USPSDA URC received)
uint8_t rxBuf[256];
int32_t bytesRead = uCxSpsRead(&ucxHandle, connHandle, 256, rxBuf);
```

### Data Mode Settings

Configure how SPS data is delivered to your host:

```
# Set SPS read mode
AT+USPSRM=<mode>

# Modes:
# 0 = Buffered (default) - Data buffered, URC notifies when available
# 1 = Direct String - Data sent immediately as string
# 2 = Direct Binary - Data sent immediately as binary

# Example: Set to buffered mode
AT+USPSRM=0

# With Buffered mode, you receive:
# +USPSDA:0,15  (connection 0 has 15 bytes available)
# Then read with: AT+USPSRB=0,15
```

---

## Serial Cable Replacement (Transparent Mode)

Use SPS as a **wireless serial cable replacement** between two u-blox modules.

### Configuration Overview

| Side | Role | Configuration |
|------|------|---------------|
| **Side A** | SPS Server | Enable SPS service, start advertising |
| **Side B** | SPS Client | Scan, connect, enable SPS on connection |

### Side A: Configure as SPS Server

```
# 1. Enable SPS service
AT+USPS=1

# 2. Start advertising (makes device discoverable)
AT+UBTAL

# 3. (Optional) Save configuration
AT&W

# Now Side A is waiting for connections...
```

### Side B: Configure as SPS Client

```
# 1. Scan for BLE devices
AT+UBTD
# Response: +UBTD:AABBCCDDEEFF,1,-45,"u-blox-SPS"

# 2. Connect to the server
AT+UBTC=AABBCCDDEEFF,1
# Response: +UBTC:0

# 3. Enable SPS with flow control
AT+USPSC=0,1
# Response: +UESPSC:0

# Now both sides can exchange data over SPS!
```

### Transparent Mode (Serial Pass-Through)

In Transparent Mode, the module acts as a pure serial bridge:
- Any data received on UART TX is sent over SPS
- Any data received from SPS is output on UART RX
- No AT commands during data transfer
- Use escape sequence (+++) to return to AT mode

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         TRANSPARENT MODE                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│   ┌─────────┐    UART    ┌─────────┐  SPS  ┌─────────┐    UART   ┌────────┐
│   │  Host   │◄══════════►│ u-blox  │◄─────►│ u-blox  │◄═════════►│ Host   │
│   │  MCU A  │            │ (Client)│       │ (Server)│           │ MCU B  │
│   └─────────┘            └─────────┘       └─────────┘           └────────┘
│                                                                         │
│   The hosts see only serial data—BLE is invisible!                      │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### Escape Sequence Configuration

```
# Configure escape sequence settings
AT+UTMES=<pre_timeout>,<post_timeout>,<escape_timeout>

# Default values:
# pre_timeout:    1000ms  (silence before escape)
# post_timeout:   1000ms  (silence after escape)
# escape_timeout:  200ms  (max time between '+' characters)

# Example: Read current settings
AT+UTMES?
# Response: +UTMES:1000,1000,200

# Configure escape character (default is '+' = ASCII 43)
ATS2=43

# To exit Transparent Mode:
# 1. Wait 1 second (no data)
# 2. Send "+++"
# 3. Wait 1 second (no data)
# 4. Module returns to AT mode with "OK"
```

---

## Complete Example: Two-Module Serial Bridge

### Module A (Server Side)

```
# Reset to defaults
AT+UFACTORY

# Set device name for easy identification
AT+UBTLN="SerialBridge-Server"

# Enable SPS service
AT+USPS=1

# Save and reboot
AT&W
AT+CPWROFF

# After reboot, start advertising
AT+UBTAL

# Server is now ready and waiting for connections
# When client connects, +UESPSC URC will appear
```

### Module B (Client Side)

```
# Reset to defaults
AT+UFACTORY

# Set device name
AT+UBTLN="SerialBridge-Client"

# Scan for servers
AT+UBTD
# Output: +UBTD:AABBCCDDEEFF,1,-42,"SerialBridge-Server"

# Connect to server
AT+UBTC=AABBCCDDEEFF,1
# Output: +UBTC:0

# Enable SPS with flow control
AT+USPSC=0,1
# Output: +UESPSC:0

# The wireless serial link is now active!
```

---

## Useful AT Commands Reference

### SPS Commands

| Command | Description |
|---------|-------------|
| `AT+USPS=<enable>` | Enable/disable SPS service (0=off, 1=on) |
| `AT+USPS?` | Query SPS status |
| `AT+USPSC=<handle>,<fc>` | Connect SPS on existing BLE connection |
| `AT+USPSWB=<h>,<len>,<data>` | Write binary data to SPS |
| `AT+USPSRB=<h>,<len>` | Read binary data from SPS |
| `AT+USPSRM=<mode>` | Set read mode (0=buffered, 1=string, 2=binary) |

### Bluetooth Commands

| Command | Description |
|---------|-------------|
| `AT+UBTAL` | Start legacy advertising |
| `AT+UBTA?` | Query advertising status |
| `AT+UBTD` | Discover (scan for) BLE devices |
| `AT+UBTC=<addr>,<type>` | Connect to BLE device |
| `AT+UBTCL` | List connected devices |
| `AT+UBTDC=<handle>` | Disconnect |

### General Commands

| Command | Description |
|---------|-------------|
| `AT+UBTLN=<name>` | Set local device name |
| `AT+UBTLN?` | Query device name |
| `AT+UMLA=1` | Get Bluetooth address |
| `AT&W` | Save configuration |
| `AT+CPWROFF` | Reboot module |
| `AT+UFACTORY` | Reset to factory defaults |

---

## Debugging

### Checklist

**Server Side:**
- [ ] SPS enabled? (`AT+USPS?` → `+USPS:1`)
- [ ] Advertising? (`AT+UBTA?`)
- [ ] Know the address? (`AT+UMLA=1`)

**Client Side:**
- [ ] Can scan and see server? (`AT+UBTD`)
- [ ] Connected? (`AT+UBTCL`)
- [ ] SPS enabled on connection? (`+UESPSC` URC received)

### Common Issues

| Issue | Cause | Solution |
|-------|-------|----------|
| Device not visible | Not advertising | `AT+UBTAL` |
| Connection fails | Wrong address/type | Verify from scan results |
| No data received | SPS not enabled | `AT+USPSC=<handle>,1` |
| Data loss | No flow control | Use `AT+USPSC=<handle>,1` |

---

## Performance Tips

### Throughput Optimization

| Setting | Default | Optimized | Benefit |
|---------|---------|-----------|---------|
| MTU | 23 | 247+ | More data per packet |
| Connection Interval | 30ms | 7.5ms | Lower latency |
| Flow Control | On | On | Prevents data loss |

### Important Considerations

**Flow Control**: Always use flow control (`AT+USPSC=<handle>,1`) unless you have a specific reason not to.

**Latency**: BLE SPS has inherent latency:
- Minimum: ~7.5ms (one connection interval)
- Typical: 15-50ms depending on settings

**Throughput**:
| Configuration | Typical Throughput |
|---------------|--------------------|
| Default (23 MTU, 30ms CI) | ~20 kbps |
| Optimized (247 MTU, 7.5ms CI) | ~100+ kbps |

---

## Related Documentation

- [SPS Protocol Specification](SPS_PROTOCOL.md)
- [u-connectXpress AT Commands Manual](https://www.u-blox.com/en/docs/UBX-20012413)
- [u-blox Short Range Modules](https://www.u-blox.com/en/short-range-radio-modules)
