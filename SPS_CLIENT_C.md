# SPS client in C

[sps.h](sps.h) and [sps.c](sps.c) implement the SPS credit-based flow control for the client role (BLE central) in portable C11: no BLE stack, heap, stdio or globals. They are the C counterpart of `SPSClient` in [sps.py](sps.py). The same code also implements the server role, see [SPS_SERVER_C.md](SPS_SERVER_C.md).

| File | Content |
|------|---------|
| [sps.h](sps.h) | API, constants and the 128-bit UUIDs in Bluetooth byte order |
| [sps.c](sps.c) | Implementation of both roles |
| [test_sps.c](test_sps.c) | Host test with a client and a server wired together in memory |

Build and test on a PC:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror sps.c test_sps.c -o test_sps && ./test_sps
```

Related documentation: [SPS_PROTOCOL.md](SPS_PROTOCOL.md), [SPS_UBLOX_CONFIG.md](SPS_UBLOX_CONFIG.md), [Python client](SPS_CLIENT_PYTHON.md).

## Connect it to your BLE stack

Fill `sps_port_t` with callbacks into the stack and call the `sps_*` functions from its events.

| Port callback | Client implementation |
|---------------|-----------------------|
| `send_fifo` | Write the packet to the FIFO characteristic (write without response) |
| `send_credits` | Write the credit count (1 byte) to the Credits characteristic |
| `subscribe` | Write `0x0001` to the CCCD of the FIFO or Credits characteristic |
| `on_data` | Optional. Data received. The packet is credited back after the callback returns |
| `on_state` | Optional. `SPS_STATE_CONNECTED` and `SPS_STATE_READY` |

| BLE event | Call |
|-----------|------|
| Connected and the SPS service and characteristics are discovered | `sps_connected()` |
| FIFO notification | `sps_on_fifo()` |
| Credits notification | `sps_on_credits()` |
| Disconnected | `sps_disconnected()` |
| Still `SPS_STATE_CONNECTED` a few seconds after `sps_connected()` | `sps_retry_handshake()` once |

`sps_connected()` follows UBX-16011192 figure 5: it enables the Credits notifications first and the FIFO notifications second, then grants the initial credits. With flow control disabled it enables only the FIFO notifications. Flow control must be set the same on both sides. It is not negotiated, see [SPS_UBLOX_CONFIG.md](SPS_UBLOX_CONFIG.md).

`sps_send()` queues the data and sends what the credits allow. It returns the number of bytes queued, which is less than requested when the queue (`SPS_TX_BUFFER_SIZE`, default 1024 bytes) is full. Call it again with the rest later, for example from the application loop. Set the payload per packet with `sps_set_packet_size()` to the ATT MTU minus 3 (default 244).

## Example

The port functions are all the glue the client needs. `ble_write()` and `ble_enable_notify()` stand for your stack.

```c
#include <stdint.h>

#include "sps.h"

extern int ble_write(uint16_t conn, uint16_t handle, const uint8_t *data, uint16_t len);
extern int ble_enable_notify(uint16_t conn, uint16_t cccd_handle);

static sps_t g_sps;
static uint16_t g_conn;
static uint16_t g_fifo, g_fifo_cccd, g_credits, g_credits_cccd; /* from service discovery */

static int send_fifo(void *ctx, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    return ble_write(g_conn, g_fifo, data, len);
}

static int send_credits(void *ctx, uint8_t credits)
{
    (void)ctx;
    return ble_write(g_conn, g_credits, &credits, 1);
}

static int subscribe(void *ctx, sps_char_t ch)
{
    (void)ctx;
    return ble_enable_notify(g_conn, ch == SPS_CHAR_CREDITS ? g_credits_cccd : g_fifo_cccd);
}

int app_init(void)
{
    const sps_port_t port = {.send_fifo = send_fifo, .send_credits = send_credits, .subscribe = subscribe};

    return sps_init(&g_sps, SPS_ROLE_CLIENT, &port, true);
}
```

Then call `sps_connected()`, `sps_on_fifo()`, `sps_on_credits()` and `sps_disconnected()` from the BLE events in the table above, and `sps_send()` to transmit.

## Stack functions

| Stack | Write FIFO and Credits | Enable notifications | Receive notifications |
|-------|------------------------|----------------------|-----------------------|
| NimBLE | `ble_gattc_write_no_rsp_flat()` | `ble_gattc_write_flat()` to the CCCD | `BLE_GAP_EVENT_NOTIFY_RX` |
| Zephyr | `bt_gatt_write_without_response()` | `bt_gatt_subscribe()` | notify callback in `struct bt_gatt_subscribe_params` |
| Nordic SoftDevice | `sd_ble_gattc_write()` with `BLE_GATT_OP_WRITE_CMD` | `sd_ble_gattc_write()` with `BLE_GATT_OP_WRITE_REQ` to the CCCD | `BLE_GATTC_EVT_HVX` |
