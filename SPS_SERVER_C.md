# u-blox SPS C Server Implementation

This document provides a complete **SPS Server** implementation in C pseudocode that can be adapted to any BLE stack.

**Related Documentation:**
- [SPS Protocol Specification](SPS_PROTOCOL.md)
- [C Client Implementation](SPS_CLIENT_C.md)
- [Python Server Implementation](SPS_SERVER_PYTHON.md)

---

## Overview

An **SPS Server**:
- Registers and hosts the SPS GATT service
- Advertises so clients can find it
- Accepts connections from SPS Clients
- Receives data FROM the client (via FIFO writes)
- Sends data TO the client (via FIFO notifications)
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

Replace the generic `gatt_register_service()`, `gatt_notify()` functions with your stack's equivalents.

---

## Complete Implementation

```c
/*
 * u-blox SPS SERVER Implementation (Pseudocode)
 * =============================================
 * This code runs on YOUR device (embedded MCU, etc.)
 * It acts as the SPS SERVER - clients connect to it.
 *
 * Role: SERVER (hosts GATT service, accepts connections, notifies clients)
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
#include <stdio.h>

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

#define SPS_INITIAL_CREDITS         8     // Credits to grant on connection
#define SPS_CREDIT_GRANT_THRESHOLD  4     // Grant credits after N packets received
#define SPS_TX_BUFFER_SIZE          1024  // Size of transmit buffer
#define SPS_RX_BUFFER_SIZE          512   // Size of receive buffer

/* ============================================================================
 * GATT Property Flags (adapt to your stack)
 * ============================================================================ */

#define GATT_PROP_READ          0x02
#define GATT_PROP_WRITE_NO_RSP  0x04
#define GATT_PROP_WRITE         0x08
#define GATT_PROP_NOTIFY        0x10

/* ============================================================================
 * SPS Server Context Structure
 * ============================================================================ */

typedef struct {
    // Service handle
    uint16_t service_handle;
    
    // GATT handles (assigned when creating service)
    uint16_t fifo_handle;           // Handle for FIFO characteristic
    uint16_t fifo_cccd_handle;      // Handle for FIFO CCCD
    uint16_t credits_handle;        // Handle for Credits characteristic
    uint16_t credits_cccd_handle;   // Handle for Credits CCCD
    
    // Connection state
    uint16_t conn_handle;           // Handle of connected client (0xFFFF if none)
    bool fifo_cccd_enabled;         // Client enabled FIFO notifications
    bool credits_cccd_enabled;      // Client enabled Credits notifications
    
    // Flow control state
    uint8_t tx_credits;             // Credits we have to SEND to client
    uint8_t rx_credits_pending;     // Packets received, need to grant back
    
    // TX buffer for pending data
    uint8_t tx_buffer[SPS_TX_BUFFER_SIZE];
    uint16_t tx_buffer_len;
    
    // RX buffer for received data
    uint8_t rx_buffer[SPS_RX_BUFFER_SIZE];
    uint16_t rx_buffer_len;
    
    // Callback for received data
    void (*on_data_received)(uint8_t *data, uint16_t len);
    
} sps_server_t;

/* ============================================================================
 * Platform-specific stubs - REPLACE THESE with your BLE stack functions
 * ============================================================================ */

// Register a GATT service
extern int gatt_register_service(const uint8_t *uuid, uint16_t *handle);

// Add a characteristic to a service
extern int gatt_add_characteristic(uint16_t svc_handle, const uint8_t *uuid,
                                   uint8_t properties, uint16_t *handle);

// Add a CCCD to a characteristic
extern int gatt_add_cccd(uint16_t char_handle, uint16_t *cccd_handle);

// Send a notification
extern int gatt_notify(uint16_t conn_handle, uint16_t attr_handle,
                       const uint8_t *data, uint16_t len);

// Start advertising
extern int ble_start_advertising(const uint8_t *service_uuid);

// Stop advertising
extern int ble_stop_advertising(void);

// Initialize BLE stack
extern int ble_init(void);

// Delay function
extern void delay_ms(uint32_t ms);

// Your application's data handler
extern void process_rx_data(uint8_t *data, uint16_t len);

/* ============================================================================
 * SPS Server Implementation
 * ============================================================================ */

/**
 * Initialize SPS server context.
 * Call this before using any other SPS functions.
 */
void sps_server_init(sps_server_t *ctx) {
    memset(ctx, 0, sizeof(sps_server_t));
    ctx->conn_handle = 0xFFFF;  // No connection
}

/**
 * Register the SPS GATT service.
 * Call this during BLE stack initialization.
 */
int sps_server_register_service(sps_server_t *ctx) {
    int rc;
    
    // Create SPS Service
    rc = gatt_register_service(SPS_SERVICE_UUID, &ctx->service_handle);
    if (rc != 0) return rc;
    
    // Add FIFO characteristic
    // Properties: Write Without Response (client writes), Notify (server notifies)
    rc = gatt_add_characteristic(
        ctx->service_handle,
        SPS_FIFO_UUID,
        GATT_PROP_WRITE_NO_RSP | GATT_PROP_WRITE | GATT_PROP_NOTIFY,
        &ctx->fifo_handle
    );
    if (rc != 0) return rc;
    
    // Add FIFO CCCD (Client Characteristic Configuration Descriptor)
    rc = gatt_add_cccd(ctx->fifo_handle, &ctx->fifo_cccd_handle);
    if (rc != 0) return rc;
    
    // Add Credits characteristic
    // Properties: Write Without Response (client writes), Notify (server notifies)
    rc = gatt_add_characteristic(
        ctx->service_handle,
        SPS_CREDITS_UUID,
        GATT_PROP_WRITE_NO_RSP | GATT_PROP_WRITE | GATT_PROP_NOTIFY,
        &ctx->credits_handle
    );
    if (rc != 0) return rc;
    
    // Add Credits CCCD
    rc = gatt_add_cccd(ctx->credits_handle, &ctx->credits_cccd_handle);
    if (rc != 0) return rc;
    
    // Initialize state
    ctx->conn_handle = 0xFFFF;  // No connection
    ctx->tx_credits = 0;
    ctx->rx_credits_pending = 0;
    ctx->tx_buffer_len = 0;
    ctx->fifo_cccd_enabled = false;
    ctx->credits_cccd_enabled = false;
    
    return 0;
}

/**
 * Start advertising the SPS service.
 * Call this when ready to accept connections.
 */
int sps_server_start_advertising(sps_server_t *ctx) {
    // Include SPS Service UUID in advertisement data
    return ble_start_advertising(SPS_SERVICE_UUID);
}

/**
 * Stop advertising.
 */
int sps_server_stop_advertising(sps_server_t *ctx) {
    return ble_stop_advertising();
}

/**
 * Called when a client connects.
 * Register this callback with your BLE stack.
 */
void sps_on_connect(sps_server_t *ctx, uint16_t conn_handle) {
    ctx->conn_handle = conn_handle;
    ctx->tx_credits = 0;
    ctx->rx_credits_pending = 0;
    ctx->tx_buffer_len = 0;
    ctx->rx_buffer_len = 0;
    ctx->fifo_cccd_enabled = false;
    ctx->credits_cccd_enabled = false;
    
    printf("SPS: Client connected, handle=%d\n", conn_handle);
}

/**
 * Called when a client disconnects.
 * Register this callback with your BLE stack.
 */
void sps_on_disconnect(sps_server_t *ctx) {
    printf("SPS: Client disconnected\n");
    
    ctx->conn_handle = 0xFFFF;
    ctx->tx_credits = 0;
    ctx->fifo_cccd_enabled = false;
    ctx->credits_cccd_enabled = false;
    
    // Optionally restart advertising to accept new connections
    sps_server_start_advertising(ctx);
}

/**
 * Called when client writes to CCCD (enables/disables notifications).
 * This is triggered by your BLE stack's GATT write handler.
 */
void sps_on_cccd_write(sps_server_t *ctx, uint16_t cccd_handle, uint16_t value) {
    bool enabled = (value == 0x0001);
    
    if (cccd_handle == ctx->fifo_cccd_handle) {
        ctx->fifo_cccd_enabled = enabled;
        printf("SPS: FIFO notifications %s\n", enabled ? "enabled" : "disabled");
    }
    else if (cccd_handle == ctx->credits_cccd_handle) {
        ctx->credits_cccd_enabled = enabled;
        printf("SPS: Credits notifications %s\n", enabled ? "enabled" : "disabled");
        
        // When client enables Credits notifications, grant initial credits
        // This allows the client to start sending data
        if (enabled) {
            sps_grant_credits_to_client(ctx, SPS_INITIAL_CREDITS);
        }
    }
}

/**
 * Called when client writes data TO us (via FIFO characteristic).
 * This is how we RECEIVE data from the client.
 */
void sps_on_fifo_write(sps_server_t *ctx, const uint8_t *data, uint16_t len) {
    // Add to receive buffer
    if (ctx->rx_buffer_len + len <= SPS_RX_BUFFER_SIZE) {
        memcpy(&ctx->rx_buffer[ctx->rx_buffer_len], data, len);
        ctx->rx_buffer_len += len;
    }
    
    // Call user callback if set
    if (ctx->on_data_received) {
        ctx->on_data_received((uint8_t *)data, len);
    }
    
    // Track that we received a packet
    ctx->rx_credits_pending++;
    
    // Grant credits back to client periodically
    if (ctx->rx_credits_pending >= SPS_CREDIT_GRANT_THRESHOLD) {
        sps_grant_credits_to_client(ctx, ctx->rx_credits_pending);
        ctx->rx_credits_pending = 0;
    }
}

/**
 * Called when client writes to Credits characteristic.
 * This is the client granting us credits TO SEND to them.
 */
void sps_on_credits_write(sps_server_t *ctx, uint8_t credits) {
    ctx->tx_credits += credits;
    printf("SPS: Client granted %d credits, total: %d\n", credits, ctx->tx_credits);
    
    // Try to send any buffered data
    sps_flush_tx_buffer(ctx);
}

/**
 * Grant credits TO the client.
 * This tells the client: "I can receive <count> more packets from you"
 * We send this as a NOTIFICATION on the Credits characteristic.
 */
void sps_grant_credits_to_client(sps_server_t *ctx, uint8_t count) {
    if (!ctx->credits_cccd_enabled) {
        return;  // Client hasn't enabled notifications
    }
    
    if (ctx->conn_handle == 0xFFFF) {
        return;  // No connection
    }
    
    uint8_t credits_data[1] = { count };
    gatt_notify(ctx->conn_handle, ctx->credits_handle, credits_data, 1);
    printf("SPS: Granted %d credits to client\n", count);
}

/**
 * Queue data to send TO the client.
 * The data will be sent when we have credits.
 *
 * @param ctx   SPS server context
 * @param data  Data to send
 * @param len   Length of data
 * @return 0 on success, -1 if buffer full
 */
int sps_server_send(sps_server_t *ctx, const uint8_t *data, uint16_t len) {
    // Add to TX buffer
    if (ctx->tx_buffer_len + len > SPS_TX_BUFFER_SIZE) {
        return -1;  // Buffer full
    }
    
    memcpy(&ctx->tx_buffer[ctx->tx_buffer_len], data, len);
    ctx->tx_buffer_len += len;
    
    // Try to send immediately if we have credits
    sps_flush_tx_buffer(ctx);
    return 0;
}

/**
 * Send a string to the client.
 */
int sps_server_send_string(sps_server_t *ctx, const char *str) {
    return sps_server_send(ctx, (const uint8_t *)str, strlen(str));
}

/**
 * Send pending data from TX buffer if we have credits.
 * Data is sent as NOTIFICATIONs on the FIFO characteristic.
 */
void sps_flush_tx_buffer(sps_server_t *ctx) {
    if (!ctx->fifo_cccd_enabled) {
        return;  // Client hasn't enabled notifications
    }
    
    if (ctx->conn_handle == 0xFFFF) {
        return;  // No connection
    }
    
    // Use fixed MTU of 247 (payload = MTU - 3 = 244 bytes)
    uint16_t max_packet = 244;
    
    while (ctx->tx_credits > 0 && ctx->tx_buffer_len > 0) {
        uint16_t chunk_size = (ctx->tx_buffer_len < max_packet) ? 
                              ctx->tx_buffer_len : max_packet;
        
        // Send notification to client
        int rc = gatt_notify(ctx->conn_handle, ctx->fifo_handle, 
                             ctx->tx_buffer, chunk_size);
        if (rc != 0) {
            break;  // Notification failed
        }
        
        ctx->tx_credits--;
        
        // Remove sent data from buffer
        memmove(ctx->tx_buffer, &ctx->tx_buffer[chunk_size], 
                ctx->tx_buffer_len - chunk_size);
        ctx->tx_buffer_len -= chunk_size;
        
        printf("SPS: TX to client: %d bytes, credits remaining: %d\n", 
               chunk_size, ctx->tx_credits);
    }
}

/**
 * Read received data from buffer.
 */
uint16_t sps_server_read(sps_server_t *ctx, uint8_t *buf, uint16_t max_len) {
    uint16_t read_len = (ctx->rx_buffer_len < max_len) ? 
                        ctx->rx_buffer_len : max_len;
    
    if (read_len > 0) {
        memcpy(buf, ctx->rx_buffer, read_len);
        memmove(ctx->rx_buffer, &ctx->rx_buffer[read_len], 
                ctx->rx_buffer_len - read_len);
        ctx->rx_buffer_len -= read_len;
    }
    
    return read_len;
}

/**
 * Check if data is available to read.
 */
uint16_t sps_server_data_available(sps_server_t *ctx) {
    return ctx->rx_buffer_len;
}

/**
 * Check if a client is connected.
 */
bool sps_server_is_connected(sps_server_t *ctx) {
    return ctx->conn_handle != 0xFFFF;
}

/**
 * Check if we can send data (have credits and client connected).
 */
bool sps_server_can_send(sps_server_t *ctx) {
    return ctx->conn_handle != 0xFFFF && 
           ctx->fifo_cccd_enabled && 
           ctx->tx_credits > 0;
}

/* ============================================================================
 * Main GATT Event Handler
 * ============================================================================ */

// Global server instance (or pass context through your BLE stack)
static sps_server_t g_sps_server;

/**
 * Main GATT event handler - dispatch to appropriate SPS handler.
 * Your BLE stack will call this (or similar) for GATT events.
 */
void gatt_event_handler(uint16_t conn_handle, uint16_t attr_handle, 
                        uint8_t *data, uint16_t len) {
    sps_server_t *ctx = &g_sps_server;
    
    if (attr_handle == ctx->fifo_handle) {
        sps_on_fifo_write(ctx, data, len);
    }
    else if (attr_handle == ctx->credits_handle) {
        if (len > 0) {
            sps_on_credits_write(ctx, data[0]);
        }
    }
    else if (attr_handle == ctx->fifo_cccd_handle || 
             attr_handle == ctx->credits_cccd_handle) {
        if (len >= 2) {
            uint16_t cccd_value = data[0] | (data[1] << 8);
            sps_on_cccd_write(ctx, attr_handle, cccd_value);
        }
    }
}

/* ============================================================================
 * Example Usage
 * ============================================================================ */

// Data receive callback
void my_data_handler(uint8_t *data, uint16_t len) {
    printf("Application received %d bytes\n", len);
    
    // Echo back to client
    sps_server_send(&g_sps_server, data, len);
}

/**
 * Example main application
 */
int app_main(void) {
    // Initialize SPS server context
    sps_server_init(&g_sps_server);
    g_sps_server.on_data_received = my_data_handler;
    
    // Initialize BLE stack (platform-specific)
    ble_init();
    
    // Register SPS service
    int rc = sps_server_register_service(&g_sps_server);
    if (rc != 0) {
        printf("Failed to register SPS service: %d\n", rc);
        return -1;
    }
    
    // Start advertising
    sps_server_start_advertising(&g_sps_server);
    
    printf("SPS Server started, waiting for connections...\n");
    
    // Application loop
    int counter = 0;
    while (1) {
        // Example: send periodic data to connected client
        if (sps_server_is_connected(&g_sps_server)) {
            char msg[64];
            int len = snprintf(msg, sizeof(msg), "Server message #%d\n", ++counter);
            sps_server_send(&g_sps_server, (uint8_t *)msg, len);
        }
        
        delay_ms(5000);
    }
    
    return 0;
}
```

---

## API Reference

### Functions

| Function | Description |
|----------|-------------|
| `sps_server_init(ctx)` | Initialize server context |
| `sps_server_register_service(ctx)` | Register SPS GATT service |
| `sps_server_start_advertising(ctx)` | Start BLE advertising |
| `sps_server_stop_advertising(ctx)` | Stop advertising |
| `sps_server_send(ctx, data, len)` | Queue data to send |
| `sps_server_send_string(ctx, str)` | Queue string to send |
| `sps_grant_credits_to_client(ctx, count)` | Grant credits to client |
| `sps_server_read(ctx, buf, len)` | Read received data |
| `sps_server_data_available(ctx)` | Check if data available |
| `sps_server_is_connected(ctx)` | Check if client connected |
| `sps_server_can_send(ctx)` | Check if can send data |

### Callbacks to Implement

| Callback | When to Call |
|----------|--------------|
| `sps_on_connect()` | When client connects |
| `sps_on_disconnect()` | When client disconnects |
| `sps_on_fifo_write()` | When client writes to FIFO |
| `sps_on_credits_write()` | When client writes to Credits |
| `sps_on_cccd_write()` | When client writes to CCCD |

---

## Porting Guide

### NimBLE (ESP32)

```c
// Service registration
ble_gatts_count_cfg(gatt_svr_svcs);
ble_gatts_add_svcs(gatt_svr_svcs);

// Notification
ble_gattc_notify_custom(conn_handle, attr_handle, om);

// Handle GAP events for connect/disconnect
// Handle GATT events for writes
```

### Zephyr BLE

```c
// Service definition
BT_GATT_SERVICE_DEFINE(sps_svc, ...);

// Notification
bt_gatt_notify(conn, attr, data, len);

// Use bt_conn_cb for connect/disconnect
// Use characteristic callbacks for writes
```

### Nordic SDK

```c
// Service registration
sd_ble_gatts_service_add(...);
sd_ble_gatts_characteristic_add(...);

// Notification
sd_ble_gatts_hvx(...);

// Handle BLE_GAP_EVT_CONNECTED/DISCONNECTED
// Handle BLE_GATTS_EVT_WRITE
```

---

## See Also

- [SPS Protocol Specification](SPS_PROTOCOL.md) - Understand the protocol
- [C Client Implementation](SPS_CLIENT_C.md) - Build an SPS client in C
- [u-blox Module Configuration](SPS_UBLOX_CONFIG.md) - Configure u-blox modules
