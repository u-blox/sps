# u-blox Serial Port Service (SPS) Documentation

This folder contains comprehensive documentation for implementing SPS (Serial Port Service) communication with u-blox BLE modules.

## What is SPS?

SPS is a proprietary u-blox protocol for wireless serial communication over Bluetooth Low Energy (BLE). It provides a "virtual serial port" with built-in credit-based flow control.

## Supported Modules

| Module | BLE | Wi-Fi | SPS Server | SPS Client | Notes |
|--------|-----|-------|------------|------------|-------|
| **NORA-W36** | 5.3 | Wi-Fi 4 | ✓ | ✓ | Dual-mode (Wi-Fi + BLE) |
| **NORA-B26** | 6.0 | — | ✓ | ✓ | BLE-only, Central + Peripheral |
| **NORA-B27** | 6.0 | — | ✓ | — | BLE-only, Peripheral only |

## Documentation Files

| File | Description |
|------|-------------|
| [SPS_PROTOCOL.md](SPS_PROTOCOL.md) | Core protocol specification - service UUIDs, flow control, architecture, connection procedure |
| [SPS_CLIENT_PYTHON.md](SPS_CLIENT_PYTHON.md) | Python SPS Client implementation using Bleak library |
| [SPS_SERVER_PYTHON.md](SPS_SERVER_PYTHON.md) | Python SPS Server implementation using bless library |
| [SPS_CLIENT_C.md](SPS_CLIENT_C.md) | C SPS Client implementation (pseudocode for any BLE stack) |
| [SPS_SERVER_C.md](SPS_SERVER_C.md) | C SPS Server implementation (pseudocode for any BLE stack) |
| [SPS_UBLOX_CONFIG.md](SPS_UBLOX_CONFIG.md) | Configuring u-blox modules - AT commands, ucxclient API, transparent mode |

## Quick Start

### If you're building a client (connecting to a u-blox module):

1. Read [SPS_PROTOCOL.md](SPS_PROTOCOL.md) to understand the protocol
2. Choose your language:
   - Python: [SPS_CLIENT_PYTHON.md](SPS_CLIENT_PYTHON.md)
   - C/Embedded: [SPS_CLIENT_C.md](SPS_CLIENT_C.md)

### If you're building a server (hosting SPS service):

1. Read [SPS_PROTOCOL.md](SPS_PROTOCOL.md) to understand the protocol
2. Choose your language:
   - Python: [SPS_SERVER_PYTHON.md](SPS_SERVER_PYTHON.md)
   - C/Embedded: [SPS_SERVER_C.md](SPS_SERVER_C.md)

### If you're configuring u-blox modules:

1. Read [SPS_UBLOX_CONFIG.md](SPS_UBLOX_CONFIG.md) for AT commands and API usage

## SPS UUIDs (Quick Reference)

```
Service:  2456e1b9-26e2-8f83-e744-f34f01e9d701
FIFO:     2456e1b9-26e2-8f83-e744-f34f01e9d703  (data transfer)
Credits:  2456e1b9-26e2-8f83-e744-f34f01e9d704  (flow control)
```

## Key Concepts

- **SPS Server**: Hosts the SPS GATT service, advertises, waits for connections
- **SPS Client**: Scans, connects, discovers service, exchanges data
- **Flow Control**: Credit-based system prevents buffer overflow
- **CCCD**: Must enable notifications on both FIFO and Credits characteristics

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.3 | 2026-01-22 | Split into multiple files for easier navigation |
| 1.2 | 2026-01-22 | Added NORA-B27 support, generalized module references |
| 1.1 | 2026-01-21 | Added Serial Cable Replacement chapter |
| 1.0 | 2026-01-20 | Initial comprehensive documentation |
