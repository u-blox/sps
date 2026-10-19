# SPS server in C

[sps.h](sps.h) and [sps.c](sps.c) implement the SPS credit-based flow control for the server role (BLE peripheral) in portable C11: no BLE stack, heap, stdio or globals. They are the C counterpart of `SPSPeripheral` in [sps.py](sps.py). The same code also implements the client role, see [SPS_CLIENT_C.md](SPS_CLIENT_C.md) for the files, the build and the test.

Related documentation: [SPS_PROTOCOL.md](SPS_PROTOCOL.md), [SPS_UBLOX_CONFIG.md](SPS_UBLOX_CONFIG.md), [Python server](SPS_SERVER_PYTHON.md).

## GATT service

Create the service and two characteristics with the UUIDs from `sps.h` (Bluetooth byte order, see [SPS_PROTOCOL.md](SPS_PROTOCOL.md) for the strings). Both characteristics need Write, Write Without Response and Notify, and a CCCD.

| Characteristic | UUID | Client writes | Server notifies |
|----------------|------|---------------|-----------------|
| FIFO | `2456e1b9-26e2-8f83-e744-f34f01e9d703` | Data to the server | Data to the client |
| Credits | `2456e1b9-26e2-8f83-e744-f34f01e9d704` | Credits for the server | Credits for the client |

Advertise the service UUID `2456e1b9-26e2-8f83-e744-f34f01e9d701`.

## Connect it to your BLE stack

| Port callback | Server implementation |
|---------------|-----------------------|
| `send_fifo` | Notify the packet on the FIFO characteristic |
| `send_credits` | Notify the credit count (1 byte) on the Credits characteristic |
| `subscribe` | Not used |
| `on_data` | Optional. Data received. The packet is credited back after the callback returns |
| `on_state` | Optional. `SPS_STATE_CONNECTED` and `SPS_STATE_READY` |

| BLE event | Call |
|-----------|------|
| Client connected | `sps_connected()` |
| Client enabled the Credits notifications (CCCD write) | `sps_on_credits_subscribed()` |
| Write to the FIFO characteristic | `sps_on_fifo()` |
| Write to the Credits characteristic | `sps_on_credits()` |
| Disconnected | `sps_disconnected()`, then restart advertising |

The server grants `SPS_INITIAL_CREDITS` when the client enables the Credits notifications. If the stack does not report CCCD writes, the first credits or the first data written by the client trigger the grant, like in `sps.py`. `sps_send()` queues data until the client has granted credits, see [SPS_CLIENT_C.md](SPS_CLIENT_C.md) for its return value. Flow control must be set the same on both sides, see [SPS_UBLOX_CONFIG.md](SPS_UBLOX_CONFIG.md).

## Example

The port functions are all the glue the server needs. `ble_notify()` stands for your stack.

```c
#include <stdint.h>

#include "sps.h"

extern int ble_notify(uint16_t conn, uint16_t attr_handle, const uint8_t *data, uint16_t len);

static sps_t g_sps;
static uint16_t g_conn;
static uint16_t g_fifo, g_credits; /* from the service registration */

static int send_fifo(void *ctx, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    return ble_notify(g_conn, g_fifo, data, len);
}

static int send_credits(void *ctx, uint8_t credits)
{
    (void)ctx;
    return ble_notify(g_conn, g_credits, &credits, 1);
}

int app_init(void)
{
    const sps_port_t port = {.send_fifo = send_fifo, .send_credits = send_credits};

    return sps_init(&g_sps, SPS_ROLE_SERVER, &port, true);
}
```

Then call `sps_connected()`, `sps_on_credits_subscribed()`, `sps_on_fifo()`, `sps_on_credits()` and `sps_disconnected()` from the BLE events in the table above, and `sps_send()` to transmit.

## Stack functions

| Stack | Notify | Receive writes | CCCD changes |
|-------|--------|----------------|--------------|
| NimBLE | `ble_gatts_notify_custom()` | access callback, `BLE_GATT_ACCESS_OP_WRITE_CHR` | `BLE_GAP_EVENT_SUBSCRIBE` |
| Zephyr | `bt_gatt_notify()` | `write` callback of `BT_GATT_CHARACTERISTIC()` | callback of `BT_GATT_CCC()` |
| Nordic SoftDevice | `sd_ble_gatts_hvx()` | `BLE_GATTS_EVT_WRITE` | `BLE_GATTS_EVT_WRITE` on the CCCD handle |
