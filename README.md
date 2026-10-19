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

Note that following older geneation u-blox module also support SPS, but with another AT command set:
- ANNA-B1
- ANNA-B41
- NINA-B1
- NINA-B2
- NINA-B31
- NINA-B41
- NINA-W15
- ODIN-W2

## Documentation Files

| File | Description |
|------|-------------|
| [SPS_PROTOCOL.md](SPS_PROTOCOL.md) | Core protocol specification - service UUIDs, flow control, architecture, connection procedure |
| [SPS_CLIENT_PYTHON.md](SPS_CLIENT_PYTHON.md) | Python SPS Client implementation using Bleak library |
| [SPS_SERVER_PYTHON.md](SPS_SERVER_PYTHON.md) | Python SPS Server implementation using bless library |
| [SPS_CLIENT_C.md](SPS_CLIENT_C.md) | C SPS Client: how to connect [c/sps.c](c/sps.c) to a BLE stack, with an example |
| [SPS_SERVER_C.md](SPS_SERVER_C.md) | C SPS Server: how to connect [c/sps.c](c/sps.c) to a BLE stack, with an example |
| [python/sps.py](python/sps.py) | Python library and command line tool (client and server), run `python python/sps.py --help` |
| [c/sps.h](c/sps.h), [c/sps.c](c/sps.c), [c/test_sps.c](c/test_sps.c) | Portable C implementation of both roles, no BLE stack needed, and its host test |
| [SPS_CSHARP.md](SPS_CSHARP.md) | C# SPS: how to connect [csharp/Sps.cs](csharp/Sps.cs) (both roles, `netstandard2.0`) to a BLE API, with a host test in [csharp/Sps.Tests](csharp/Sps.Tests) |
| [SPS_UBLOX_CONFIG.md](SPS_UBLOX_CONFIG.md) | Configuring u-blox modules - AT commands, ucxclient API, transparent mode |

## Quick Start

### If you're building a client (connecting to a u-blox module):

1. Read [SPS_PROTOCOL.md](SPS_PROTOCOL.md) to understand the protocol
2. Choose your language:
   - Python: [SPS_CLIENT_PYTHON.md](SPS_CLIENT_PYTHON.md)
   - C/Embedded: [SPS_CLIENT_C.md](SPS_CLIENT_C.md)
   - C# / .NET: [SPS_CSHARP.md](SPS_CSHARP.md)

### If you're building a server (hosting SPS service):

1. Read [SPS_PROTOCOL.md](SPS_PROTOCOL.md) to understand the protocol
2. Choose your language:
   - Python: [SPS_SERVER_PYTHON.md](SPS_SERVER_PYTHON.md)
   - C/Embedded: [SPS_SERVER_C.md](SPS_SERVER_C.md)
   - C# / .NET: [SPS_CSHARP.md](SPS_CSHARP.md)

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

The version tracks the latest u-connectXpress release that the documentation and `python/sps.py` have been checked against. It is kept in lock-step in these places, and releases are tagged `X.Y.Z` (no `v` prefix):

| Where | What |
|-------|------|
| [`VERSION`](VERSION) | Source of truth, plain-text version string |
| [`python/sps.py`](python/sps.py) | `__version__ = "X.Y.Z"`, printed by `python python/sps.py --version` |
| [`c/sps.h`](c/sps.h) | `#define SPS_VERSION "X.Y.Z"` |
| [`csharp/Sps.cs`](csharp/Sps.cs) | `public const string Version = "X.Y.Z"` |

| Version | Date | Changes |
|---------|------|---------|
| unreleased | 2026-10-19 | One folder per language: `c/` (`sps.c`, `sps.h`, `test_sps.c`), `python/` (`sps.py`) and `csharp/`. New C# implementation `csharp/Sps.cs` for both roles behind a small transport interface, with a host test. |
| 3.5.0 | 2026-10-19 | Version aligned with u-connectXpress 3.5.0 (`VERSION`, `--version`). New portable C implementation `sps.c`/`sps.h` for both roles with a host test; the C documents describe how to use it instead of embedding a listing. Corrected the UUID byte order for C code. Removed unused import in `sps.py`. |
| 1.3 | 2026-01-22 | Split into multiple files for easier navigation |
| 1.2 | 2026-01-22 | Added NORA-B27 support, generalized module references |
| 1.1 | 2026-01-21 | Added Serial Cable Replacement chapter |
| 1.0 | 2026-01-20 | Initial comprehensive documentation |


