# u-blox SPS C Client Implementation

This document provides a complete **SPS Client** implementation in C pseudocode that can be adapted to any BLE stack.

**Related Documentation:**
- [SPS Protocol Specification](SPS_PROTOCOL.md)
- [C Server Implementation](SPS_SERVER_C.md)
- [Python Client Implementation](SPS_CLIENT_PYTHON.md)

---

## Overview

An **SPS Client**:
- Scans for and connects to an SPS Server
- Writes data TO the server (via FIFO characteristic)
- Receives data FROM the server (via FIFO notifications)
- Manages flow control credits in both directions

## Adapting to Your BLE Stack

This is **pseudocode** - you'll need to adapt it to your specific BLE stack:

| BLE Stack | Platform |
|-----------|----------|
| NimBLE | ESP32, generic |
| Zephyr BLE | Zephyr RTOS |
| Nordic SDK | nRF52/53 |
| STM32 BLE | STM32WB |
| BlueZ | Linux |
| Windows BLE | Windows 10/11 |

Replace the generic `gatt_write()`, `gatt_notify()` functions with your stack's equivalents.

---

## Complete Implementation

```c
/*
 * u-blox SPS CLIENT Implementation (Pseudocode)
 * =============================================
 * This code runs on YOUR device (embedded MCU, PC, etc.)
 * It connects to a u-blox module (or any device) running as SPS SERVER.
 *
 * Role: CLIENT (initiates connection, writes to server, receives notifications)
 *
 * NOTE: This is pseudocode - adapt to your BLE stack:
 *       - NimBLE (ESP32)
 *       - Zephyr BLE
 *       - Nordic SDK (nRF52/53)
 *       - STM32 BLE Stack
 *       - etc.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ============================================================================
 * SPS UUIDs (128-bit, Bluetooth byte order - Little Endian)
 * These are the same on all u-blox devices.
 * ============================================================================ */

// UUID: 2456e1b9-26e2-8f83-e744-f34f01e9d701 (Service)
static const uint8_t SPS_SERVICE_UUID[] = {
    0xB9, 0xE1, 0x56, 0x24, 0xE2, 0x26, 0x83, 0x8F,
    0xE7, 0x44, 0xF3, 0x4F, 0x01, 0xE9, 0xD7, 0x01
};

// UUID: 2456e1b9-26e2-8f83-e744-f34f01e9d703 (FIFO - Data)
static const uint8_t SPS_FIFO_UUID[] = {
    0xB9, 0xE1, 0x56, 0x24, 0xE2, 0x26, 0x83, 0x8F,
    0xE7, 0x44, 0xF3, 0x4F, 0x01, 0xE9, 0xD7, 0x03
};

// UUID: 2456e1b9-26e2-8f83-e744-f34f01e9d704 (Credits - Flow Control)
static const uint8_t SPS_CREDITS_UUID[] = {
    0xB9, 0xE1, 0x56, 0x24, 0xE2, 0x26, 0x83, 0x8F,
    0xE7, 0x44, 0xF3, 0x4F, 0x01, 0xE9, 0xD7, 0x04
};

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define SPS_CREDIT_GRANT_THRESHOLD  4    // Grant credits after N packets received
#define SPS_RX_BUFFER_SIZE          512  // Size of receive buffer
#define SPS_TX_BUFFER_SIZE          512  // Size of transmit buffer

/* ============================================================================
 * SPS Client Context Structure
 * ============================================================================ */

typedef struct {
    // Connection state
    uint16_t conn_handle;           // BLE connection handle
    bool is_connected;              // Connection active
    bool notifications_enabled;     // CCCDs have been written
    
    // GATT handles (obtained during service discovery)
    uint16_t fifo_handle;           // Handle for FIFO characteristic
    uint16_t fifo_cccd_handle;      // Handle for FIFO CCCD (to enable notifications)
    uint16_t credits_handle;        // Handle for Credits characteristic
    uint16_t credits_cccd_handle;   // Handle for Credits CCCD
    
    // Flow control state
    uint8_t tx_credits;             // Credits we have to SEND to server
    uint8_t rx_credits_pending;     // Packets received, need to grant back
    
    // Receive buffer
    uint8_t rx_buffer[SPS_RX_BUFFER_SIZE];
    uint16_t rx_buffer_len;
    
    // Transmit buffer (for when we have no credits)
    uint8_t tx_buffer[SPS_TX_BUFFER_SIZE];
    uint16_t tx_buffer_len;
    
    // Callback for received data
    void (*on_data_received)(uint8_t *data, uint16_t len);
    
} sps_client_t;

/* ============================================================================
 * Platform-specific stubs - REPLACE THESE with your BLE stack functions
 * ============================================================================ */

// Write to a GATT characteristic (no response)
extern int gatt_write(uint16_t handle, const uint8_t *data, uint16_t len);

// Write to a GATT characteristic (with response)
extern int gatt_write_with_response(uint16_t handle, const uint8_t *data, uint16_t len);

// Start BLE scan
extern int ble_start_scan(void);

// Connect to a device
extern int ble_connect(const uint8_t *address, uint8_t address_type);

// Discover services
extern int ble_discover_services(uint16_t conn_handle);

/* ============================================================================
 * SPS Client Implementation
 * ============================================================================ */

/**
 * Initialize SPS client context.
 * Call this before using any other SPS functions.
 */
void sps_client_init(sps_client_t *ctx) {
    memset(ctx, 0, sizeof(sps_client_t));
    ctx->conn_handle = 0xFFFF;  // Invalid handle
}

/**
 * Called when we receive a NOTIFICATION from the server's FIFO characteristic.
 * This means the SERVER is sending data TO us.
 *
 * Register this as your BLE stack's notification callback for the FIFO characteristic.
 */
void sps_on_fifo_notification(sps_client_t *ctx, const uint8_t *data, uint16_t len) {
    // Add to receive buffer
    if (ctx->rx_buffer_len + len <= SPS_RX_BUFFER_SIZE) {
        memcpy(&ctx->rx_buffer[ctx->rx_buffer_len], data, len);
        ctx->rx_buffer_len += len;
    }
    
    // Call user callback if set
    if (ctx->on_data_received) {
        ctx->on_data_received((uint8_t *)data, len);
    }
    
    // Track that we received a packet for flow control
    ctx->rx_credits_pending++;
    
    // Grant credits back to server periodically
    // This tells the server: "I processed these, you can send more"
    if (ctx->rx_credits_pending >= SPS_CREDIT_GRANT_THRESHOLD) {
        sps_grant_credits(ctx, ctx->rx_credits_pending);
        ctx->rx_credits_pending = 0;
    }
}

/**
 * Called when we receive a NOTIFICATION from the server's Credits characteristic.
 * This means the SERVER is granting us permission to send more data.
 *
 * Register this as your BLE stack's notification callback for the Credits characteristic.
 */
void sps_on_credits_notification(sps_client_t *ctx, uint8_t credits) {
    ctx->tx_credits += credits;
    
    // Try to send any buffered data
    sps_flush_tx_buffer(ctx);
}

/**
 * Send data TO the SPS server.
 * This WRITES to the server's FIFO characteristic.
 * Each write costs one credit.
 *
 * @param ctx   SPS client context
 * @param data  Data to send
 * @param len   Length of data (max 244 bytes)
 * @return 0 on success, -1 if no credits (data buffered), -2 on error
 */
int sps_write(sps_client_t *ctx, const uint8_t *data, uint16_t len) {
    if (!ctx->is_connected || !ctx->notifications_enabled) {
        return -2;  // Not ready
    }
    
    if (ctx->tx_credits == 0) {
        // No credits - buffer the data for later
        if (ctx->tx_buffer_len + len <= SPS_TX_BUFFER_SIZE) {
            memcpy(&ctx->tx_buffer[ctx->tx_buffer_len], data, len);
            ctx->tx_buffer_len += len;
            return -1;  // Buffered, will send when credits arrive
        }
        return -2;  // Buffer full
    }
    
    // Write to the FIFO characteristic (data goes to server)
    int rc = gatt_write(ctx->fifo_handle, data, len);
    if (rc == 0) {
        ctx->tx_credits--;
    }
    return rc;
}

/**
 * Send buffered data when we have credits.
 * Called automatically when credits arrive.
 */
void sps_flush_tx_buffer(sps_client_t *ctx) {
    // Use fixed MTU of 247 (payload = MTU - 3 = 244 bytes)
    uint16_t max_packet = 244;
    
    while (ctx->tx_credits > 0 && ctx->tx_buffer_len > 0) {
        uint16_t chunk_size = (ctx->tx_buffer_len < max_packet) ? 
                              ctx->tx_buffer_len : max_packet;
        
        int rc = gatt_write(ctx->fifo_handle, ctx->tx_buffer, chunk_size);
        if (rc != 0) {
            break;  // Write failed
        }
        
        ctx->tx_credits--;
        
        // Remove sent data from buffer
        memmove(ctx->tx_buffer, &ctx->tx_buffer[chunk_size], 
                ctx->tx_buffer_len - chunk_size);
        ctx->tx_buffer_len -= chunk_size;
    }
}

/**
 * Grant credits TO the server.
 * This tells the server: "I can receive <count> more packets from you"
 *
 * @param ctx   SPS client context
 * @param count Number of credits to grant (1-255)
 */
void sps_grant_credits(sps_client_t *ctx, uint8_t count) {
    if (ctx->is_connected && ctx->notifications_enabled) {
        gatt_write(ctx->credits_handle, &count, 1);
    }
}

/**
 * Enable notifications (write to CCCDs).
 * Call this after service discovery is complete.
 *
 * This is CRITICAL - without this, you won't receive any data from the server!
 */
int sps_enable_notifications(sps_client_t *ctx) {
    // CCCD value to enable notifications (Little-endian 0x0001)
    uint16_t cccd_enable = 0x0001;
    int rc;
    
    // Enable notifications on FIFO - CRITICAL!
    // Without this, we won't receive data from server
    rc = gatt_write_with_response(ctx->fifo_cccd_handle, 
                                   (uint8_t *)&cccd_enable, 2);
    if (rc != 0) {
        return rc;
    }
    
    // Enable notifications on Credits
    // Without this, we won't know when we can send
    rc = gatt_write_with_response(ctx->credits_cccd_handle, 
                                   (uint8_t *)&cccd_enable, 2);
    if (rc != 0) {
        return rc;
    }
    
    ctx->notifications_enabled = true;
    
    // Initialize flow control state
    ctx->tx_credits = 0;          // Wait for server to grant initial credits
    ctx->rx_credits_pending = 0;
    
    return 0;
}

/**
 * Read received data from buffer.
 *
 * @param ctx      SPS client context
 * @param buf      Buffer to copy data into
 * @param max_len  Maximum bytes to read
 * @return Number of bytes read
 */
uint16_t sps_read(sps_client_t *ctx, uint8_t *buf, uint16_t max_len) {
    uint16_t read_len = (ctx->rx_buffer_len < max_len) ? 
                        ctx->rx_buffer_len : max_len;
    
    if (read_len > 0) {
        memcpy(buf, ctx->rx_buffer, read_len);
        
        // Remove read data from buffer
        memmove(ctx->rx_buffer, &ctx->rx_buffer[read_len], 
                ctx->rx_buffer_len - read_len);
        ctx->rx_buffer_len -= read_len;
    }
    
    return read_len;
}

/**
 * Check if data is available to read.
 */
uint16_t sps_data_available(sps_client_t *ctx) {
    return ctx->rx_buffer_len;
}

/**
 * Check if we can send data (have credits).
 */
bool sps_can_send(sps_client_t *ctx) {
    return ctx->tx_credits > 0;
}

/* ============================================================================
 * Example Usage
 * ============================================================================ */

// Global SPS client instance
static sps_client_t g_sps_client;

// Callback for received data
void my_data_handler(uint8_t *data, uint16_t len) {
    // Process received data here
    printf("Received %d bytes\n", len);
}

// Called when BLE connection is established
void on_connect(uint16_t conn_handle) {
    g_sps_client.conn_handle = conn_handle;
    g_sps_client.is_connected = true;
    
    // Start service discovery
    ble_discover_services(conn_handle);
}

// Called when service discovery finds the SPS service
void on_sps_service_found(uint16_t fifo_handle, uint16_t fifo_cccd,
                          uint16_t credits_handle, uint16_t credits_cccd) {
    g_sps_client.fifo_handle = fifo_handle;
    g_sps_client.fifo_cccd_handle = fifo_cccd;
    g_sps_client.credits_handle = credits_handle;
    g_sps_client.credits_cccd_handle = credits_cccd;
    
    // Enable notifications
    sps_enable_notifications(&g_sps_client);
}

// Called when FIFO notification received (from BLE stack)
void on_fifo_notify(uint8_t *data, uint16_t len) {
    sps_on_fifo_notification(&g_sps_client, data, len);
}

// Called when Credits notification received (from BLE stack)
void on_credits_notify(uint8_t *data, uint16_t len) {
    if (len > 0) {
        sps_on_credits_notification(&g_sps_client, data[0]);
    }
}

// Main application
int main(void) {
    // Initialize
    sps_client_init(&g_sps_client);
    g_sps_client.on_data_received = my_data_handler;
    
    // Initialize BLE stack (platform-specific)
    ble_init();
    
    // Start scanning for SPS servers
    ble_start_scan();
    
    // Main loop
    while (1) {
        // Check if we have data to read
        if (sps_data_available(&g_sps_client) > 0) {
            uint8_t buf[256];
            uint16_t len = sps_read(&g_sps_client, buf, sizeof(buf));
            // Process data...
        }
        
        // Send data if connected
        if (g_sps_client.is_connected && sps_can_send(&g_sps_client)) {
            sps_write(&g_sps_client, (uint8_t *)"Hello", 5);
        }
        
        delay_ms(100);
    }
    
    return 0;
}
```

---

## API Reference

### Functions

| Function | Description |
|----------|-------------|
| `sps_client_init(ctx)` | Initialize client context |
| `sps_enable_notifications(ctx)` | Enable FIFO and Credits notifications |
| `sps_write(ctx, data, len)` | Send data to server |
| `sps_grant_credits(ctx, count)` | Grant credits to server |
| `sps_read(ctx, buf, max_len)` | Read received data |
| `sps_data_available(ctx)` | Check if data available |
| `sps_can_send(ctx)` | Check if credits available |

### Callbacks to Register

| Callback | When to Call |
|----------|--------------|
| `sps_on_fifo_notification()` | When FIFO notification received |
| `sps_on_credits_notification()` | When Credits notification received |

---

## Porting Guide

### NimBLE (ESP32)

```c
// Replace gatt_write with:
ble_gattc_write_no_rsp_flat(conn_handle, attr_handle, data, len);

// Replace notification callback registration with:
ble_gattc_notify_custom(conn_handle, attr_handle, notify_callback);
```

### Zephyr BLE

```c
// Replace gatt_write with:
bt_gatt_write_without_response(conn, attr_handle, data, len, false);

// Subscribe to notifications:
bt_gatt_subscribe(conn, &subscribe_params);
```

### Nordic SDK

```c
// Replace gatt_write with:
sd_ble_gattc_write(conn_handle, &write_params);

// Handle BLE_GATTC_EVT_HVX for notifications
```

---

## See Also

- [SPS Protocol Specification](SPS_PROTOCOL.md) - Understand the protocol
- [C Server Implementation](SPS_SERVER_C.md) - Build an SPS server in C
- [u-blox Module Configuration](SPS_UBLOX_CONFIG.md) - Configure u-blox modules
