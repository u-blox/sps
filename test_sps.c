/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Host test for sps.c: a client and a server are wired together through an in-memory link,
 * either queued (like a BLE stack that calls back from its own context) or synchronous.
 *
 *   gcc -std=c11 -Wall -Wextra -Wpedantic -Werror sps.c test_sps.c -o test_sps && ./test_sps
 */
#include "sps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                      \
    do {                                                                 \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_failed++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                \
    } while (0)

enum { EV_FIFO, EV_CREDITS, EV_SUBSCRIBE };
enum { CLIENT = 0, SERVER = 1 };

typedef struct {
    int kind;
    int to;
    uint16_t len;
    uint8_t data[256];
} event_t;

typedef struct {
    sps_t sps;
    int id;
    uint8_t rx[8192];
    size_t rx_len;
    int sub_log[4];
    int sub_count;
    int fail_fifo;          /* send_fifo returns an error */
    int drop_credits_out;   /* silently lose this many outgoing credit values */
    unsigned credits_out;   /* send_credits calls */
    unsigned state_changes;
} node_t;

static node_t g_node[2];
static event_t g_queue[1024];
static size_t g_head;
static size_t g_tail;
static int g_sync; /* deliver at once instead of queueing */

static void deliver(const event_t *ev)
{
    sps_t *to = &g_node[ev->to].sps;

    if (ev->kind == EV_FIFO) {
        sps_on_fifo(to, ev->data, ev->len);
    } else if (ev->kind == EV_CREDITS) {
        sps_on_credits(to, ev->data, ev->len);
    } else if (ev->data[0] == SPS_CHAR_CREDITS) {
        sps_on_credits_subscribed(to);
    }
}

static void post(int kind, int to, const uint8_t *data, uint16_t len)
{
    event_t ev;

    ev.kind = kind;
    ev.to = to;
    ev.len = len;
    memcpy(ev.data, data, len);
    if (g_sync) {
        deliver(&ev);
        return;
    }
    if ((g_tail + 1) % 1024 == g_head) {
        printf("event queue overflow\n");
        exit(2);
    }
    g_queue[g_tail] = ev;
    g_tail = (g_tail + 1) % 1024;
}

static void pump(void)
{
    while (g_head != g_tail) {
        event_t ev = g_queue[g_head];

        g_head = (g_head + 1) % 1024;
        deliver(&ev);
    }
}

static int port_send_fifo(void *ctx, const uint8_t *data, uint16_t len)
{
    node_t *n = ctx;

    if (n->fail_fifo) {
        return -1;
    }
    post(EV_FIFO, 1 - n->id, data, len);
    return 0;
}

static int port_send_credits(void *ctx, uint8_t credits)
{
    node_t *n = ctx;

    n->credits_out++;
    if (n->drop_credits_out > 0) {
        n->drop_credits_out--;
        return 0;
    }
    post(EV_CREDITS, 1 - n->id, &credits, 1);
    return 0;
}

static int port_subscribe(void *ctx, sps_char_t ch)
{
    node_t *n = ctx;
    uint8_t c = (uint8_t)ch;

    n->sub_log[n->sub_count++] = (int)ch;
    post(EV_SUBSCRIBE, 1 - n->id, &c, 1);
    return 0;
}

static void port_on_data(void *ctx, const uint8_t *data, uint16_t len)
{
    node_t *n = ctx;

    if (n->rx_len + len > sizeof(n->rx)) {
        g_failed++;
        printf("FAIL receive buffer of the test is too small\n");
        return;
    }
    memcpy(n->rx + n->rx_len, data, len);
    n->rx_len += len;
}

static void port_on_state(void *ctx, sps_state_t state)
{
    node_t *n = ctx;

    (void)state;
    n->state_changes++;
}

static void setup(bool flow_control, int sync)
{
    sps_port_t port;
    int i;

    memset(g_node, 0, sizeof(g_node));
    g_head = 0;
    g_tail = 0;
    g_sync = sync;
    for (i = 0; i < 2; i++) {
        memset(&port, 0, sizeof(port));
        port.send_fifo = port_send_fifo;
        port.send_credits = port_send_credits;
        port.on_data = port_on_data;
        port.on_state = port_on_state;
        port.ctx = &g_node[i];
        if (i == CLIENT) {
            port.subscribe = port_subscribe;
        }
        g_node[i].id = i;
        CHECK(sps_init(&g_node[i].sps, i == CLIENT ? SPS_ROLE_CLIENT : SPS_ROLE_SERVER, &port,
                       flow_control) == SPS_OK);
    }
}

static void connect_both(void)
{
    CHECK(sps_connected(&g_node[SERVER].sps) == SPS_OK);
    CHECK(sps_connected(&g_node[CLIENT].sps) == SPS_OK);
    pump();
}

static uint8_t pat(size_t i, uint8_t seed)
{
    return (uint8_t)(i * 7u + seed);
}

/* Both sides send `total` bytes at the same time. Returns once everything arrived. */
static void transfer_both(size_t total)
{
    static uint8_t src[2][4096];
    size_t off[2] = {0, 0};
    int iter;
    int side;

    for (side = 0; side < 2; side++) {
        size_t i;
        for (i = 0; i < total; i++) {
            src[side][i] = pat(i, (uint8_t)(side + 1));
        }
    }
    for (iter = 0; iter < 1000; iter++) {
        for (side = 0; side < 2; side++) {
            int n;
            while (off[side] < total &&
                   (n = sps_send(&g_node[side].sps, src[side] + off[side],
                                 (uint16_t)(total - off[side]))) > 0) {
                off[side] += (size_t)n;
            }
        }
        pump();
        if (off[0] == total && off[1] == total && g_node[0].sps.tx_len == 0 &&
            g_node[1].sps.tx_len == 0 && g_head == g_tail) {
            break;
        }
    }
    CHECK(off[CLIENT] == total && off[SERVER] == total);
    CHECK(g_node[SERVER].rx_len == total && memcmp(g_node[SERVER].rx, src[CLIENT], total) == 0);
    CHECK(g_node[CLIENT].rx_len == total && memcmp(g_node[CLIENT].rx, src[SERVER], total) == 0);
}

static void test_handshake(void)
{
    node_t *c = &g_node[CLIENT];
    node_t *s = &g_node[SERVER];

    setup(true, 0);
    connect_both();
    CHECK(c->sub_count == 2);
    CHECK(c->sub_log[0] == SPS_CHAR_CREDITS); /* UBX-16011192 figure 5 */
    CHECK(c->sub_log[1] == SPS_CHAR_FIFO);
    CHECK(c->sps.state == SPS_STATE_READY && s->sps.state == SPS_STATE_READY);
    CHECK(c->sps.tx_credits == SPS_INITIAL_CREDITS && s->sps.tx_credits == SPS_INITIAL_CREDITS);
    CHECK(c->sps.stats.credits_sent == SPS_INITIAL_CREDITS);
    CHECK(s->sps.stats.credits_sent == SPS_INITIAL_CREDITS);
    CHECK(sps_connected(&c->sps) == SPS_ERR_STATE); /* already connected */
}

static void test_backpressure_and_integrity(void)
{
    static uint8_t src[4096];
    node_t *c;
    node_t *s;
    size_t off = 0;
    size_t i;
    int n;
    int iter;

    setup(true, 0);
    c = &g_node[CLIENT];
    s = &g_node[SERVER];
    connect_both();
    for (i = 0; i < sizeof(src); i++) {
        src[i] = pat(i, 9);
    }

    /* No pump: the client can only use the initial credits, the rest waits in the queue. */
    while (off < sizeof(src) && (n = sps_send(&c->sps, src + off, (uint16_t)(sizeof(src) - off))) > 0) {
        off += (size_t)n;
    }
    CHECK(c->sps.stats.packets_sent == SPS_INITIAL_CREDITS);
    CHECK(c->sps.tx_credits == 0);
    CHECK(sps_tx_free(&c->sps) == 0);
    CHECK(off == c->sps.stats.bytes_sent + SPS_TX_BUFFER_SIZE); /* sent plus a full queue */

    for (iter = 0; iter < 100 && (off < sizeof(src) || c->sps.tx_len > 0 || g_head != g_tail); iter++) {
        pump();
        while (off < sizeof(src) && (n = sps_send(&c->sps, src + off, (uint16_t)(sizeof(src) - off))) > 0) {
            off += (size_t)n;
        }
    }
    CHECK(off == sizeof(src) && c->sps.tx_len == 0);
    CHECK(s->rx_len == sizeof(src) && memcmp(s->rx, src, sizeof(src)) == 0);
    /* The sender never used a credit it did not get. */
    CHECK(c->sps.stats.packets_sent <= c->sps.stats.credits_received);
    CHECK(c->sps.tx_credits == c->sps.stats.credits_received - c->sps.stats.packets_sent);
}

static void test_bidirectional(void)
{
    setup(true, 0);
    connect_both();
    transfer_both(3000);
}

static void test_synchronous_link(void)
{
    setup(true, 1);
    connect_both();
    CHECK(g_node[CLIENT].sps.state == SPS_STATE_READY && g_node[SERVER].sps.state == SPS_STATE_READY);
    transfer_both(3000);
}

static void test_no_flow_control(void)
{
    setup(false, 0);
    connect_both();
    CHECK(g_node[CLIENT].sub_count == 1 && g_node[CLIENT].sub_log[0] == SPS_CHAR_FIFO);
    CHECK(g_node[CLIENT].sps.state == SPS_STATE_READY && g_node[SERVER].sps.state == SPS_STATE_READY);
    transfer_both(3000);
    CHECK(g_node[CLIENT].credits_out == 0 && g_node[SERVER].credits_out == 0);
}

static void test_server_fallbacks(void)
{
    node_t *s;
    uint8_t credits = 4;
    uint8_t byte = 0x55;

    /* Credits written before the Credits subscription was seen. */
    setup(true, 0);
    s = &g_node[SERVER];
    CHECK(sps_connected(&s->sps) == SPS_OK);
    CHECK(s->sps.state == SPS_STATE_CONNECTED && s->sps.stats.credits_sent == 0);
    sps_on_credits(&s->sps, &credits, 1);
    CHECK(s->sps.state == SPS_STATE_READY);
    CHECK(s->sps.tx_credits == 4);
    CHECK(s->sps.stats.credits_sent == SPS_INITIAL_CREDITS);

    /* First data packet before any credit exchange. */
    setup(true, 0);
    s = &g_node[SERVER];
    CHECK(sps_connected(&s->sps) == SPS_OK);
    sps_on_fifo(&s->sps, &byte, 1);
    CHECK(s->sps.stats.credits_sent == SPS_INITIAL_CREDITS);
    CHECK(s->sps.rx_pending == 1);
}

static void test_credits_returned_after_threshold(void)
{
    node_t *s;
    uint8_t byte = 1;
    int i;

    setup(true, 0);
    connect_both();
    s = &g_node[SERVER];
    for (i = 0; i < SPS_CREDIT_THRESHOLD - 1; i++) {
        sps_on_fifo(&s->sps, &byte, 1);
    }
    CHECK(s->sps.stats.credits_sent == SPS_INITIAL_CREDITS); /* nothing yet */
    sps_on_fifo(&s->sps, &byte, 1);
    CHECK(s->sps.stats.credits_sent == SPS_INITIAL_CREDITS + SPS_CREDIT_THRESHOLD);
    CHECK(s->sps.rx_pending == 0);
}

static void test_disconnect_resets(void)
{
    node_t *c;
    uint8_t byte = 1;

    setup(true, 0);
    c = &g_node[CLIENT];
    connect_both();
    CHECK(sps_send(&c->sps, &byte, 1) == 1);
    sps_disconnected(&c->sps);
    CHECK(c->sps.state == SPS_STATE_DISCONNECTED);
    CHECK(c->sps.tx_credits == 0 && c->sps.tx_len == 0);
    CHECK(sps_send(&c->sps, &byte, 1) == SPS_ERR_STATE);
    sps_on_fifo(&c->sps, &byte, 1);
    CHECK(c->sps.stats.packets_received == 0);

    pump();
    g_head = g_tail;
    CHECK(sps_connected(&c->sps) == SPS_OK); /* the link can come back */
}

static void test_port_failure(void)
{
    node_t *c;
    uint8_t data[100];

    setup(true, 0);
    c = &g_node[CLIENT];
    connect_both();
    memset(data, 0xA5, sizeof(data));

    c->fail_fifo = 1;
    CHECK(sps_send(&c->sps, data, sizeof(data)) == (int)sizeof(data));
    CHECK(c->sps.stats.packets_sent == 0 && c->sps.tx_len == sizeof(data));
    CHECK(c->sps.tx_credits == SPS_INITIAL_CREDITS); /* no credit spent on a failed write */

    c->fail_fifo = 0;
    sps_flush(&c->sps);
    CHECK(c->sps.stats.packets_sent == 1 && c->sps.tx_len == 0);
    CHECK(c->sps.tx_credits == SPS_INITIAL_CREDITS - 1);
}

static void test_retry_handshake(void)
{
    node_t *c;

    setup(true, 0);
    c = &g_node[CLIENT];
    g_node[SERVER].drop_credits_out = 1; /* the server's first grant is lost */
    connect_both();
    CHECK(c->sps.state == SPS_STATE_CONNECTED);
    CHECK(sps_retry_handshake(&c->sps) == SPS_OK);
    CHECK(c->credits_out == 2 && c->sps.stats.credits_sent == 2 * SPS_INITIAL_CREDITS);
    pump();

    /* A server has nothing to retry, and a ready client neither. */
    CHECK(sps_retry_handshake(&g_node[SERVER].sps) == SPS_ERR_STATE);
    setup(true, 0);
    connect_both();
    CHECK(sps_retry_handshake(&g_node[CLIENT].sps) == SPS_ERR_STATE);
}

static void test_packet_size(void)
{
    node_t *c;
    uint8_t data[100] = {0};

    setup(true, 0);
    c = &g_node[CLIENT];
    connect_both();
    CHECK(sps_set_packet_size(&c->sps, 0) == SPS_ERR_ARG);
    CHECK(sps_set_packet_size(&c->sps, SPS_TX_BUFFER_SIZE + 1) == SPS_ERR_ARG);
    CHECK(sps_set_packet_size(&c->sps, 20) == SPS_OK);
    CHECK(sps_send(&c->sps, data, sizeof(data)) == (int)sizeof(data));
    CHECK(c->sps.stats.packets_sent == 5 && c->sps.stats.bytes_sent == 100);
}

static void test_credit_saturation(void)
{
    node_t *c;
    uint8_t credits = 255;
    int i;

    setup(true, 0);
    c = &g_node[CLIENT];
    connect_both();
    for (i = 0; i < 400; i++) {
        sps_on_credits(&c->sps, &credits, 1);
    }
    CHECK(c->sps.tx_credits == UINT16_MAX);
}

static void parse_uuid_le(const char *text, uint8_t out[16])
{
    uint8_t be[16];
    int n = 0;
    int i;

    for (; *text != '\0' && n < 32; text++) {
        int v;
        if (*text == '-') {
            continue;
        }
        v = (*text >= 'a') ? (*text - 'a' + 10) : (*text - '0');
        if (n % 2 == 0) {
            be[n / 2] = (uint8_t)(v << 4);
        } else {
            be[n / 2] = (uint8_t)(be[n / 2] | v);
        }
        n++;
    }
    for (i = 0; i < 16; i++) {
        out[i] = be[15 - i];
    }
}

static void test_uuids(void)
{
    uint8_t le[16];

    parse_uuid_le("2456e1b9-26e2-8f83-e744-f34f01e9d701", le);
    CHECK(memcmp(le, SPS_SERVICE_UUID, 16) == 0);
    parse_uuid_le("2456e1b9-26e2-8f83-e744-f34f01e9d703", le);
    CHECK(memcmp(le, SPS_FIFO_UUID, 16) == 0);
    parse_uuid_le("2456e1b9-26e2-8f83-e744-f34f01e9d704", le);
    CHECK(memcmp(le, SPS_CREDITS_UUID, 16) == 0);
}

static void test_init_validation(void)
{
    sps_t s;
    sps_port_t port;

    memset(&port, 0, sizeof(port));
    CHECK(sps_init(NULL, SPS_ROLE_SERVER, &port, false) == SPS_ERR_ARG);
    CHECK(sps_init(&s, SPS_ROLE_SERVER, NULL, false) == SPS_ERR_ARG);
    CHECK(sps_init(&s, SPS_ROLE_SERVER, &port, false) == SPS_ERR_ARG); /* send_fifo missing */
    port.send_fifo = port_send_fifo;
    CHECK(sps_init(&s, SPS_ROLE_SERVER, &port, true) == SPS_ERR_ARG);  /* send_credits missing */
    CHECK(sps_init(&s, SPS_ROLE_SERVER, &port, false) == SPS_OK);
    CHECK(sps_init(&s, SPS_ROLE_CLIENT, &port, false) == SPS_ERR_ARG); /* subscribe missing */
    CHECK(sps_init(&s, (sps_role_t)7, &port, false) == SPS_ERR_ARG);
}

int main(void)
{
    test_uuids();
    test_init_validation();
    test_handshake();
    test_backpressure_and_integrity();
    test_bidirectional();
    test_synchronous_link();
    test_no_flow_control();
    test_server_fallbacks();
    test_credits_returned_after_threshold();
    test_disconnect_resets();
    test_port_failure();
    test_retry_handshake();
    test_packet_size();
    test_credit_saturation();

    printf("sps %s: %d checks, %d failed\n", SPS_VERSION, g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
