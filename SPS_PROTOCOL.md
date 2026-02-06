# u-blox SPS Protocol Specification

This document describes the core **Serial Port Service (SPS)** protocol for wireless serial communication over Bluetooth Low Energy (BLE).

**Related Documentation:**
- [Python Client Implementation](SPS_CLIENT_PYTHON.md)
- [Python Server Implementation](SPS_SERVER_PYTHON.md)
- [C Client Implementation](SPS_CLIENT_C.md)
- [C Server Implementation](SPS_SERVER_C.md)
- [u-blox Module Configuration](SPS_UBLOX_CONFIG.md)

---

## Table of Contents

1. [Background Concepts](#background-concepts)
2. [What is SPS?](#what-is-sps)
3. [Client vs Server Roles](#client-vs-server-roles)
4. [Service Definition](#service-definition)
5. [Architecture](#architecture)
6. [Flow Control](#flow-control)
7. [Data Transfer](#data-transfer)
8. [Connection Procedure](#connection-procedure)
9. [Best Practices](#best-practices)
10. [Troubleshooting](#troubleshooting)

---

## Background Concepts

If you're new to Bluetooth Low Energy, here's what you need to know:

### What is Bluetooth Low Energy (BLE)?

BLE is a wireless technology designed for low-power, short-range communication. Unlike Classic Bluetooth (used for audio streaming), BLE is optimized for small, periodic data transfers—perfect for sensors, beacons, and serial data links.

### Key BLE Terms

| Term | Explanation |
|------|-------------|
| **Central** | The device that initiates a connection (typically your phone or PC) |
| **Peripheral** | The device being connected to (typically the sensor or module) |
| **GATT** | Generic Attribute Profile—the protocol for exchanging structured data |
| **Service** | A collection of related features (identified by a UUID) |
| **Characteristic** | A data endpoint within a service (like a register you can read/write) |
| **UUID** | Universally Unique Identifier—a 128-bit ID for services and characteristics |
| **CCCD** | Client Characteristic Configuration Descriptor—used to enable notifications |
| **Notification** | A server-initiated message sent without the client asking |
| **MTU** | Maximum Transmission Unit—the largest packet size both devices agree to use |

### How BLE Data Transfer Works

```
┌──────────────────────────────────────────────────────────────────┐
│                      BLE GATT Communication                      │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│   ┌──────────┐                            ┌──────────┐           │
│   │  Central │                            │Peripheral│           │
│   │ (Client) │                            │ (Server) │           │
│   └────┬─────┘                            └────┬─────┘           │
│        │                                       │                 │
│        │──── Write Request ───────────────────►│                 │
│        │        (Client sends data)            │                 │
│        │                                       │                 │
│        │◄─── Notification ─────────────────────│                 │
│        │        (Server sends data)            │                 │
│        │                                       │                 │
│   "Client writes TO the server"                │                 │
│   "Server notifies the client"                 │                 │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

---

## What is SPS?

### The Problem SPS Solves

Many embedded devices communicate via UART (serial port). When you want to replace a wired serial cable with wireless:

```
BEFORE (Wired):
┌──────────┐      UART Cable       ┌──────────┐
│   PC     │◄═════════════════════►│  Device  │
└──────────┘                       └──────────┘

AFTER (Wireless with SPS):
┌──────────┐       BLE (SPS)       ┌──────────┐
│   PC     │◄ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─►│  Device  │
│  (BLE    │                       │ (u-blox  │
│  Adapter)│                       │  module) │
└──────────┘                       └──────────┘
```

### What SPS Provides

**SPS = "Virtual Serial Port over BLE"**

| Feature | Description |
|---------|-------------|
| Bidirectional | Send and receive data like a serial port |
| Flow Control | Built-in credit system prevents data loss |
| Transparent | Data passes through unchanged (no encoding) |
| Standard GATT | Works with any BLE stack |

### Why Not Just Use Standard BLE?

Standard BLE characteristics have limitations:
- No built-in flow control
- Easy to overflow buffers
- No standard "serial port" profile in BLE spec

SPS adds a **credit-based flow control** layer to ensure reliable delivery.

---

## Client vs Server Roles

Understanding roles is **critical** for implementing SPS correctly.

> **Key Point**: Many u-blox modules can operate as **EITHER** SPS Server **OR** SPS Client!
> This enables module-to-module communication.

### Role Definitions

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         SPS ROLE DEFINITIONS                            │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│   ╔═══════════════════════════════════════════════════════════════╗    │
│   ║                      SPS SERVER                                ║    │
│   ╠═══════════════════════════════════════════════════════════════╣    │
│   ║  • Hosts the SPS GATT service                                  ║    │
│   ║  • Advertises its presence ("I have SPS!")                     ║    │
│   ║  • Waits for connections                                       ║    │
│   ║  • Can be: u-blox module or any device with SPS service        ║    │
│   ║  • Receives writes, sends notifications                        ║    │
│   ╚═══════════════════════════════════════════════════════════════╝    │
│                                                                         │
│   ╔═══════════════════════════════════════════════════════════════╗    │
│   ║                      SPS CLIENT                                ║    │
│   ╠═══════════════════════════════════════════════════════════════╣    │
│   ║  • Scans for SPS servers                                       ║    │
│   ║  • Initiates the BLE connection                                ║    │
│   ║  • Discovers the SPS service and characteristics               ║    │
│   ║  • Can be: u-blox module*, mobile app, PC, embedded MCU        ║    │
│   ║  • Writes to characteristics, receives notifications           ║    │
│   ╚═══════════════════════════════════════════════════════════════╝    │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

> *Only modules with BLE Central capability can be SPS Clients.

### Typical Use Cases

**Use Case 1: Mobile App ↔ u-blox Module**

```
┌───────────────────┐                    ┌───────────────────┐
│    YOUR DEVICE    │                    │   u-blox Module   │
│   (SPS CLIENT)    │                    │   (SPS SERVER)    │
├───────────────────┤                    ├───────────────────┤
│                   │                    │                   │
│  Mobile Phone     │     BLE Link       │  u-blox Module    │
│  Laptop           │◄══════════════════►│  with SPS         │
│  Raspberry Pi     │                    │  enabled          │
│  ESP32            │                    │                   │
│                   │                    │                   │
├───────────────────┤                    ├───────────────────┤
│  YOU IMPLEMENT:   │                    │  ALREADY DONE:    │
│  • BLE scanning   │                    │  • SPS service    │
│  • Connection     │                    │  • Advertising    │
│  • GATT discovery │                    │  • Flow control   │
│  • Data handling  │                    │                   │
│  • Flow control   │                    │                   │
└───────────────────┘                    └───────────────────┘
```

**Use Case 2: Module-to-Module (Serial Cable Replacement)**

```
┌───────────────────┐                    ┌───────────────────┐
│  u-blox Module    │                    │  u-blox Module    │
│   (SPS CLIENT)    │                    │   (SPS SERVER)    │
├───────────────────┤                    ├───────────────────┤
│                   │                    │                   │
│  Initiates BLE    │     BLE Link       │  Advertises &     │
│  connection       │◄══════════════════►│  waits for        │
│                   │                    │  connection       │
│  Scans for        │                    │                   │
│  SPS servers      │                    │  Hosts SPS        │
│                   │                    │  service          │
└───────────────────┘                    └───────────────────┘
```

### Data Direction Terminology

| Direction | BLE Operation | Who Initiates | Description |
|-----------|---------------|---------------|-------------|
| **Client → Server** | Write | CLIENT | Client writes data to FIFO characteristic |
| **Server → Client** | Notify | SERVER | Server sends notification to client |
| **Credits to Server** | Write | CLIENT | Client grants credits to server |
| **Credits to Client** | Notify | SERVER | Server grants credits to client |

### Role Summary Table

| Aspect | SPS Server | SPS Client |
|--------|------------|------------|
| **BLE Role** | Peripheral | Central |
| **GATT Role** | GATT Server | GATT Client |
| **Connection** | Waits for connection | Initiates connection |
| **Advertising** | Advertises SPS UUID | Scans for SPS UUID |
| **Sends Data Via** | Notifications | Writes |
| **Receives Data Via** | Writes from client | Notifications from server |

---

## Service Definition

### Service UUID

The SPS service is identified by this proprietary u-blox UUID:

| Name | UUID |
|------|------|
| **SPS Service** | `2456e1b9-26e2-8f83-e744-f34f01e9d701` |

### Characteristics

The service contains two characteristics:

| Characteristic | UUID | Properties | Purpose |
|----------------|------|------------|---------|
| **FIFO** | `2456e1b9-26e2-8f83-e744-f34f01e9d703` | Write, Write Without Response, Notify | Data transfer |
| **Credits** | `2456e1b9-26e2-8f83-e744-f34f01e9d704` | Write, Write Without Response, Notify | Flow control |

### What Each Characteristic Does

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        FIFO CHARACTERISTIC                              │
├─────────────────────────────────────────────────────────────────────────┤
│  Purpose: Bidirectional data transfer (like UART TX/RX combined)        │
│                                                                         │
│  CLIENT writes to FIFO → Data sent to SERVER                            │
│  SERVER notifies FIFO  → Data sent to CLIENT                            │
│                                                                         │
│  Properties:                                                            │
│    • Write: Acknowledged write (reliable, slower)                       │
│    • Write Without Response: Unacknowledged (faster, for bulk data)     │
│    • Notify: Server pushes data to client (must enable CCCD)            │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                       CREDITS CHARACTERISTIC                            │
├─────────────────────────────────────────────────────────────────────────┤
│  Purpose: Flow control to prevent buffer overflow                       │
│                                                                         │
│  CLIENT writes credits → Tells SERVER: "I can receive N more packets"   │
│  SERVER notifies credits → Tells CLIENT: "You can send N more packets"  │
│                                                                         │
│  Format: Single byte (0-255) = number of packets receiver can accept    │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Architecture

### System Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         SPS ARCHITECTURE                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌─────────────────────────┐         ┌─────────────────────────┐       │
│  │      SPS CLIENT         │         │      SPS SERVER         │       │
│  │    (Your Device)        │         │   (u-blox Module)       │       │
│  ├─────────────────────────┤         ├─────────────────────────┤       │
│  │                         │         │                         │       │
│  │  ┌───────────────────┐  │         │  ┌───────────────────┐  │       │
│  │  │  Application      │  │         │  │  Application      │  │       │
│  │  │  (Your Code)      │  │         │  │  (AT Commands/    │  │       │
│  │  └────────┬──────────┘  │         │  │   UART pass-thru) │  │       │
│  │           │             │         │  └────────┬──────────┘  │       │
│  │           ▼             │         │           │             │       │
│  │  ┌───────────────────┐  │         │  ┌────────▼──────────┐  │       │
│  │  │  Credit Tracking  │  │         │  │  Credit Tracking  │  │       │
│  │  │  tx_credits: N    │  │         │  │  tx_credits: M    │  │       │
│  │  └────────┬──────────┘  │         │  └────────┬──────────┘  │       │
│  │           │             │         │           │             │       │
│  │           ▼             │         │           ▼             │       │
│  │  ┌───────────────────┐  │   BLE   │  ┌───────────────────┐  │       │
│  │  │  GATT Client      │◄═╬════════╬═►│  GATT Server      │  │       │
│  │  │  • Write FIFO     │  │         │  │  • FIFO Service   │  │       │
│  │  │  • Write Credits  │  │         │  │  • Credits Service│  │       │
│  │  │  • Handle Notify  │  │         │  │  • Send Notify    │  │       │
│  │  └───────────────────┘  │         │  └───────────────────┘  │       │
│  │                         │         │                         │       │
│  └─────────────────────────┘         └─────────────────────────┘       │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### Data Flow Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    BIDIRECTIONAL DATA FLOW                              │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│    CLIENT                              SERVER                           │
│    ──────                              ──────                           │
│                                                                         │
│    ┌──────────┐                        ┌──────────┐                     │
│    │ TX Data  │───── WRITE FIFO ──────►│ RX Data  │                     │
│    │ Buffer   │                        │ Buffer   │                     │
│    └──────────┘                        └──────────┘                     │
│                                                                         │
│    ┌──────────┐                        ┌──────────┐                     │
│    │ RX Data  │◄──── NOTIFY FIFO ─────│ TX Data  │                     │
│    │ Buffer   │                        │ Buffer   │                     │
│    └──────────┘                        └──────────┘                     │
│                                                                         │
│    ┌──────────┐                        ┌──────────┐                     │
│    │TX Credits│◄── NOTIFY CREDITS ────│Grants    │                     │
│    │ Counter  │   "You can send 10"   │Credits   │                     │
│    └──────────┘                        └──────────┘                     │
│                                                                         │
│    ┌──────────┐                        ┌──────────┐                     │
│    │Grants    │──── WRITE CREDITS ───►│TX Credits│                     │
│    │Credits   │   "I can receive 5"   │ Counter  │                     │
│    └──────────┘                        └──────────┘                     │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Flow Control

Flow control is **essential** to understand. Without it, you will lose data.

### Why Flow Control is Needed

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     THE PROBLEM WITHOUT FLOW CONTROL                    │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│    CLIENT                              SERVER                           │
│    ──────                              ──────                           │
│                                                                         │
│    ┌──────────┐                        ┌──────────┐                     │
│    │ Sends    │                        │ Buffer   │                     │
│    │ 100      │──────────────────────►│ Size: 10 │                     │
│    │ packets  │                        │          │                     │
│    │ fast!    │                        │ ❌ OVERFLOW!                   │
│    └──────────┘                        └──────────┘                     │
│                                                                         │
│    Result: 90 packets LOST                                              │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### How Credits Solve This

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     THE SOLUTION: CREDIT-BASED FLOW CONTROL             │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│    CLIENT                              SERVER                           │
│    ──────                              ──────                           │
│                                                                         │
│    1. Connection established                                            │
│                                                                         │
│    tx_credits: 0                       "I have buffer space"            │
│         │                                        │                      │
│         │◄────── NOTIFY Credits: 10 ────────────│                      │
│         │                                        │                      │
│    tx_credits: 10                                                       │
│                                                                         │
│    2. Client sends data (has 10 credits)                                │
│                                                                         │
│    tx_credits: 10                      buffer[10]                       │
│         │                                        │                      │
│         │──────── WRITE Data ──────────────────►│ buffer[9]            │
│    tx_credits: 9                                 │                      │
│         │──────── WRITE Data ──────────────────►│ buffer[8]            │
│    tx_credits: 8                                 │                      │
│         │          ...continues...               │                      │
│    tx_credits: 0                                 │ buffer[0] ← FULL     │
│         │                                        │                      │
│         │──────── WRITE Data ──────────────────►│ MUST WAIT!           │
│         ✗ CANNOT SEND (no credits)               │                      │
│                                                                         │
│    3. Server processes data, grants more credits                        │
│                                                                         │
│    tx_credits: 0                       "Processed some, have space"     │
│         │                                        │                      │
│         │◄────── NOTIFY Credits: 5 ─────────────│                      │
│         │                                        │                      │
│    tx_credits: 5                                                        │
│         │                                        │                      │
│         │──────── WRITE Data ──────────────────►│ ✓ OK                 │
│    tx_credits: 4                                                        │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### Credit Rules

| Rule | Explanation |
|------|-------------|
| **Each packet costs 1 credit** | When you send data, decrement your credit counter |
| **Zero credits = must wait** | Never send when credits are zero |
| **Credits accumulate** | If you receive 5, then 3, you have 8 |
| **Grant credits when ready** | Tell the other side you can receive more |
| **Initial credits from server** | Server sends first credits after connection |

### Credit Packet Format

Credits are sent as a single byte:

```
┌─────────────────────────────────────────────────────────────────┐
│  CREDIT PACKET (1 byte)                                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   Byte 0: Credit Count (0-255)                                  │
│                                                                 │
│   Example: 0x0A = 10 credits = "You can send 10 more packets"   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### Client Credit Implementation (Pseudocode)

```
Variables:
  tx_credits = 0           // How many packets I can SEND
  rx_credits_pending = 0   // How many packets I've RECEIVED

On RECEIVE credits notification from server:
  tx_credits += notification_value
  // Now I can send more packets!

On SEND data:
  if (tx_credits > 0) {
      write_fifo(data);
      tx_credits--;
  } else {
      wait_for_credits();  // Block or queue
  }

On RECEIVE data:
  process_data(data);
  rx_credits_pending++;
  if (rx_credits_pending >= 4) {  // Grant in batches
      write_credits(rx_credits_pending);
      rx_credits_pending = 0;
  }
```

### Flow Control Modes

| Mode | Description | When to Use |
|------|-------------|-------------|
| **With Flow Control** (Default) | Credit-based, reliable | Most applications—guaranteed delivery |
| **Without Flow Control** | No credits, best effort | Only when occasional data loss is acceptable |

---

## Data Transfer

### FIFO Characteristic

The FIFO characteristic is used for bidirectional data transfer:

- **Client → Server**: Write or Write Without Response
- **Server → Client**: Notifications (must enable CCCD first)

### What is CCCD?

The **Client Characteristic Configuration Descriptor (CCCD)** is a standard BLE mechanism to enable/disable notifications:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    ENABLING NOTIFICATIONS (CCCD)                        │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│   By default, the server does NOT send notifications.                   │
│   The CLIENT must explicitly enable them by writing to the CCCD.        │
│                                                                         │
│   CLIENT                               SERVER                           │
│      │                                    │                             │
│      │   Write 0x0100 to FIFO CCCD        │                             │
│      │──────────────────────────────────►│  "Notifications enabled"    │
│      │                                    │                             │
│      │◄──────────── NOTIFY ──────────────│  Now server can send data   │
│      │                                    │                             │
│                                                                         │
│   CCCD Values:                                                          │
│   • 0x0000 = Notifications OFF (default)                                │
│   • 0x0100 = Notifications ON  (required for SPS!)                      │
│   • 0x0200 = Indications ON    (not used in SPS)                        │
│                                                                         │
│   NOTE: You must enable CCCD on BOTH FIFO and Credits characteristics   │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### Maximum Packet Size

MTU negotiation is not typically supported, so use a fixed MTU of 247:

```
Max Payload = 247 - 3 (ATT header) = 244 bytes
```

| MTU | Max Payload | Notes |
|-----|-------------|-------|
| 247 | 244 bytes | Fixed MTU used in examples |

### Data Packet Format

Data packets contain raw payload bytes with no additional framing:

```
┌─────────────────────────────────────────────────────────┐
│                    Payload Data                         │
│                   (1 to 244 bytes)                      │
├─────────────────────────────────────────────────────────┤
│  • No header or footer—raw bytes                        │
│  • Length determined by BLE layer                       │
│  • Multiple packets may be needed for large data        │
└─────────────────────────────────────────────────────────┘
```

---

## Connection Procedure

This section shows **exactly what the CLIENT needs to do** to establish an SPS session.

### Complete Connection Sequence

```
┌─────────────────────────────────────────────────────────────────────────┐
│              CLIENT CONNECTION PROCEDURE (Step-by-Step)                 │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  STEP 1: SCAN FOR SPS DEVICES                                           │
│  ────────────────────────────────────────────────────────────────────── │
│  Look for devices advertising: 2456e1b9-26e2-8f83-e744-f34f01e9d701     │
│                                                                         │
│  STEP 2: CONNECT                                                        │
│  ────────────────────────────────────────────────────────────────────── │
│  Establish BLE connection to the device                                 │
│                                                                         │
│  STEP 3: DISCOVER SERVICES                                              │
│  ────────────────────────────────────────────────────────────────────── │
│  Find the SPS Service UUID in the service list                          │
│                                                                         │
│  STEP 4: DISCOVER CHARACTERISTICS                                       │
│  ────────────────────────────────────────────────────────────────────── │
│  Find FIFO (d703) and Credits (d704) characteristics                    │
│  Save their handles for later use                                       │
│                                                                         │
│  STEP 5: ENABLE NOTIFICATIONS (CRITICAL!)                               │
│  ────────────────────────────────────────────────────────────────────── │
│  Write 0x0100 to FIFO CCCD                                              │
│  Write 0x0100 to Credits CCCD                                           │
│  Without this, you will NOT receive any data!                           │
│                                                                         │
│  STEP 6: WAIT FOR INITIAL CREDITS                                       │
│  ────────────────────────────────────────────────────────────────────── │
│  Server sends credits notification after detecting CCCD enable          │
│  Store received credits in tx_credits counter                           │
│                                                                         │
│  STEP 7: SPS READY - EXCHANGE DATA!                                     │
│  ────────────────────────────────────────────────────────────────────── │
│  Send: Write to FIFO (costs 1 credit)                                   │
│  Receive: Handle FIFO notifications                                     │
│  Grant credits: Write to Credits after receiving data                   │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### Summary Checklist

| Step | Action | Notes |
|------|--------|-------|
| 1 | Scan for BLE devices | Filter by SPS Service UUID |
| 2 | Connect | Standard BLE connection |
| 3 | Discover services | Find SPS Service |
| 4 | Discover characteristics | Find FIFO and Credits |
| 5 | Enable notifications | Write 0x0100 to both CCCDs |
| 6 | Wait for credits | Server sends initial credits |
| 7 | Exchange data | Use flow control! |

---

## Best Practices

### Performance Optimization

| Optimization | Benefit | How to Implement |
|--------------|---------|------------------|
| **Higher MTU** | More data per packet | Request MTU 247+ during connection |
| **Write Without Response** | Higher throughput | Use for bulk data when app handles reliability |
| **Batch Credit Grants** | Fewer BLE transactions | Grant 4-8 credits at once, not 1 at a time |
| **Shorter Connection Interval** | Lower latency | Request 7.5-15ms connection interval |

### Reliability

1. **Always Use Flow Control**: Prevents data loss from buffer overflows
2. **Handle Disconnections**: Implement reconnection logic
3. **Buffer Management**: Size buffers appropriately for your use case
4. **Credit Tracking**: Never send more packets than available credits

### Common Pitfalls

| Issue | Cause | Solution |
|-------|-------|----------|
| No data received | CCCD not enabled | **CLIENT must** write 0x0100 to FIFO CCCD |
| Data loss | Flow control ignored | Track credits, wait when zero |
| "No credits" error | Sent before server ready | Wait for initial credits notification |
| Slow throughput | Low MTU | Negotiate higher MTU |
| Connection drops | Supervision timeout | Keep connection active, check RSSI |

---

## Troubleshooting

### Debugging Checklist

**Server Side (u-blox module):**
- [ ] Is SPS service enabled? (`AT+USPS?` should return `1`)
- [ ] Is device advertising? (`AT+UBTA?`)
- [ ] What is the Bluetooth address? (`AT+UMLA=1`)

**Client Side (Your Device):**
- [ ] Can you see the device when scanning?
- [ ] Did you enable notifications on FIFO CCCD?
- [ ] Did you enable notifications on Credits CCCD?
- [ ] Are you waiting for initial credits before sending?
- [ ] Are you granting credits back after receiving data?

### Common Problems and Solutions

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    TROUBLESHOOTING GUIDE                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  PROBLEM: "I can connect but never receive any data"                    │
│  ─────────────────────────────────────────────────────────────────────  │
│  CAUSE: You forgot to enable notifications (CCCD)                       │
│  FIX: Write 0x0100 to both FIFO CCCD and Credits CCCD                   │
│       This is step 5 in the connection procedure!                       │
│                                                                         │
│  ─────────────────────────────────────────────────────────────────────  │
│                                                                         │
│  PROBLEM: "I can receive data but cannot send"                          │
│  ─────────────────────────────────────────────────────────────────────  │
│  CAUSE: tx_credits is zero                                              │
│  FIX: Wait for Credits notification from server                         │
│       Check that you enabled Credits CCCD                               │
│                                                                         │
│  ─────────────────────────────────────────────────────────────────────  │
│                                                                         │
│  PROBLEM: "Server stops sending data after a while"                     │
│  ─────────────────────────────────────────────────────────────────────  │
│  CAUSE: Server ran out of credits to send                               │
│  FIX: Grant credits back to server after processing received data       │
│       Don't forget to write to Credits characteristic!                  │
│                                                                         │
│  ─────────────────────────────────────────────────────────────────────  │
│                                                                         │
│  PROBLEM: "Data appears corrupted or out of order"                      │
│  ─────────────────────────────────────────────────────────────────────  │
│  CAUSE: BLE delivers packets in order; check your buffering             │
│  FIX: Ensure your RX buffer handles partial/fragmented data             │
│       Remember: one BLE packet ≠ one application message                │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Reference

### SPS UUIDs (Quick Reference)

```
Service:  2456e1b9-26e2-8f83-e744-f34f01e9d701
FIFO:     2456e1b9-26e2-8f83-e744-f34f01e9d703
Credits:  2456e1b9-26e2-8f83-e744-f34f01e9d704
```

### UUIDs in Little-Endian Byte Order (for C code)

```c
// Service UUID
static const uint8_t SPS_SERVICE_UUID[] = {
    0xB9, 0xE1, 0x56, 0x24, 0xE2, 0x26, 0x83, 0x8F,
    0xE7, 0x44, 0xF3, 0x4F, 0x01, 0xE9, 0xD7, 0x01
};

// FIFO UUID
static const uint8_t SPS_FIFO_UUID[] = {
    0xB9, 0xE1, 0x56, 0x24, 0xE2, 0x26, 0x83, 0x8F,
    0xE7, 0x44, 0xF3, 0x4F, 0x01, 0xE9, 0xD7, 0x03
};

// Credits UUID
static const uint8_t SPS_CREDITS_UUID[] = {
    0xB9, 0xE1, 0x56, 0x24, 0xE2, 0x26, 0x83, 0x8F,
    0xE7, 0x44, 0xF3, 0x4F, 0x01, 0xE9, 0xD7, 0x04
};
```

### Related Documentation

- [Python Client Implementation](SPS_CLIENT_PYTHON.md)
- [Python Server Implementation](SPS_SERVER_PYTHON.md)
- [C Client Implementation](SPS_CLIENT_C.md)
- [C Server Implementation](SPS_SERVER_C.md)
- [u-blox Module Configuration](SPS_UBLOX_CONFIG.md)
