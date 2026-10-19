/* SPDX-License-Identifier: Apache-2.0 */
/*
 * u-blox Serial Port Service (SPS) - portable C implementation.
 *
 * The credit-based flow control of SPS (UBX-16011192) for both roles, with no
 * BLE stack, heap, stdio or globals. It is the C counterpart of SPSClient and
 * SPSPeripheral in sps.py. Connect it to your BLE stack with the callbacks in
 * sps_port_t and feed it the GATT events:
 *
 *                  CLIENT (central)               SERVER (peripheral)
 *   send_fifo      write FIFO                     notify FIFO
 *   send_credits   write Credits                  notify Credits
 *   subscribe      write the CCCD of the char     not used
 *   sps_on_fifo    FIFO notification received     FIFO write received
 *   sps_on_credits Credits notification received  Credits write received
 *
 * Call everything from one context (or serialize the calls). The callbacks may
 * run synchronously, for example a loopback test, because sps_flush() is
 * re-entrancy safe.
 */
#ifndef SPS_H
#define SPS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same value as VERSION and sps.py: the u-connectXpress release it is checked against. */
#define SPS_VERSION "3.5.0"

#define SPS_DEFAULT_PACKET_SIZE 244 /* ATT MTU 247 - 3 byte header */
#define SPS_INITIAL_CREDITS 8       /* credits granted when the link is ready */
#define SPS_CREDIT_THRESHOLD 4      /* grant credits after this many packets */

#ifndef SPS_TX_BUFFER_SIZE
#define SPS_TX_BUFFER_SIZE 1024 /* bytes queued by sps_send() while waiting for credits */
#endif

#define SPS_OK 0
#define SPS_ERR_STATE (-1) /* no connection */
#define SPS_ERR_PORT (-2)  /* a port callback failed */
#define SPS_ERR_ARG (-3)   /* invalid argument */

/*
 * 128-bit UUIDs in Bluetooth byte order (little endian: the bytes of the UUID string
 * in reverse), as used by NimBLE, Zephyr, Nordic SoftDevice and the air interface.
 *   service 2456e1b9-26e2-8f83-e744-f34f01e9d701
 *   FIFO    2456e1b9-26e2-8f83-e744-f34f01e9d703
 *   credits 2456e1b9-26e2-8f83-e744-f34f01e9d704
 */
extern const uint8_t SPS_SERVICE_UUID[16];
extern const uint8_t SPS_FIFO_UUID[16];
extern const uint8_t SPS_CREDITS_UUID[16];

typedef enum {
    SPS_ROLE_CLIENT = 0, /* BLE central, GATT client */
    SPS_ROLE_SERVER = 1  /* BLE peripheral, GATT server */
} sps_role_t;

typedef enum {
    SPS_STATE_DISCONNECTED = 0,
    SPS_STATE_CONNECTED = 1, /* link up, waiting for the first credits */
    SPS_STATE_READY = 2      /* data can flow */
} sps_state_t;

typedef enum {
    SPS_CHAR_FIFO = 0,
    SPS_CHAR_CREDITS = 1
} sps_char_t;

/* Return 0 from the callbacks that return int, anything else is an error. */
typedef struct {
    /* One packet of at most the packet size. */
    int (*send_fifo)(void *ctx, const uint8_t *data, uint16_t len);
    /* One credit count (1..255). Required with flow control. */
    int (*send_credits)(void *ctx, uint8_t credits);
    /* Client only: enable notifications of one characteristic (write its CCCD). */
    int (*subscribe)(void *ctx, sps_char_t ch);
    /* Optional: data received. The packet is credited back after the callback returns. */
    void (*on_data)(void *ctx, const uint8_t *data, uint16_t len);
    /* Optional: state change. */
    void (*on_state)(void *ctx, sps_state_t state);
    void *ctx;
} sps_port_t;

typedef struct {
    uint32_t bytes_sent;
    uint32_t bytes_received;
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t credits_sent;
    uint32_t credits_received;
} sps_stats_t;

typedef struct {
    sps_role_t role;
    sps_port_t port;
    sps_state_t state;
    sps_stats_t stats;
    bool flow_control;
    bool initial_granted;
    bool flushing;
    uint8_t rx_pending;   /* packets received but not yet credited back */
    uint16_t packet_size;
    uint16_t tx_credits;  /* packets we may still send */
    uint16_t tx_len;
    uint8_t tx_buf[SPS_TX_BUFFER_SIZE];
} sps_t;

/*
 * Prepare a context. send_fifo is required; send_credits is required with
 * flow_control; subscribe is required for the client. Flow control must be set
 * the same on both sides (on u-blox modules: AT+UDSC parameter 4). It is not negotiated.
 */
int sps_init(sps_t *s, sps_role_t role, const sps_port_t *port, bool flow_control);

/*
 * The BLE link is up (service discovered, for the client). The client enables the
 * Credits notifications first and the FIFO notifications second, then grants
 * SPS_INITIAL_CREDITS, as UBX-16011192 requires. On SPS_ERR_PORT the context is
 * back in DISCONNECTED and the caller must drop the link.
 */
int sps_connected(sps_t *s);

/* The BLE link is gone. Clears credits and queued data. */
void sps_disconnected(sps_t *s);

/*
 * Client: call once if the state is still SPS_STATE_CONNECTED a few seconds after
 * sps_connected(), for example when a first grant was lost while pairing.
 */
int sps_retry_handshake(sps_t *s);

/* Server: the client enabled the Credits notifications (CCCD write). Grants the initial credits. */
void sps_on_credits_subscribed(sps_t *s);

/* GATT events, see the table at the top. */
void sps_on_fifo(sps_t *s, const uint8_t *data, uint16_t len);
void sps_on_credits(sps_t *s, const uint8_t *data, uint16_t len);

/*
 * Queue data and send what the credits allow. Returns the number of bytes queued
 * (less than len when the queue is full, 0 when it is full) or a negative SPS_ERR_*.
 * Send the rest after more credits arrived, see sps_tx_free().
 */
int sps_send(sps_t *s, const uint8_t *data, uint16_t len);

/* Send queued data if the state and credits allow. Called by sps_send() and sps_on_credits(). */
void sps_flush(sps_t *s);

/* Free bytes in the transmit queue. */
uint16_t sps_tx_free(const sps_t *s);

/* Payload per packet, 1..SPS_TX_BUFFER_SIZE. Use ATT MTU - 3. */
int sps_set_packet_size(sps_t *s, uint16_t size);

#ifdef __cplusplus
}
#endif

#endif /* SPS_H */
