/* SPDX-License-Identifier: Apache-2.0 */
/* u-blox Serial Port Service (SPS) - portable C implementation, see sps.h. */

#include "sps.h"

#include <string.h>

const uint8_t SPS_SERVICE_UUID[16] = {0x01, 0xD7, 0xE9, 0x01, 0x4F, 0xF3, 0x44, 0xE7,
                                      0x83, 0x8F, 0xE2, 0x26, 0xB9, 0xE1, 0x56, 0x24};
const uint8_t SPS_FIFO_UUID[16] = {0x03, 0xD7, 0xE9, 0x01, 0x4F, 0xF3, 0x44, 0xE7,
                                   0x83, 0x8F, 0xE2, 0x26, 0xB9, 0xE1, 0x56, 0x24};
const uint8_t SPS_CREDITS_UUID[16] = {0x04, 0xD7, 0xE9, 0x01, 0x4F, 0xF3, 0x44, 0xE7,
                                      0x83, 0x8F, 0xE2, 0x26, 0xB9, 0xE1, 0x56, 0x24};

static void set_state(sps_t *s, sps_state_t state)
{
    if (s->state == state) {
        return;
    }
    s->state = state;
    if (s->port.on_state != NULL) {
        s->port.on_state(s->port.ctx, state);
    }
}

static void reset_link(sps_t *s)
{
    s->initial_granted = false;
    s->flushing = false;
    s->rx_pending = 0;
    s->tx_credits = 0;
    s->tx_len = 0;
}

static int grant(sps_t *s, uint8_t count)
{
    if (count == 0) {
        return SPS_OK;
    }
    if (s->port.send_credits(s->port.ctx, count) != 0) {
        return SPS_ERR_PORT;
    }
    s->stats.credits_sent += count;
    return SPS_OK;
}

static void grant_initial(sps_t *s)
{
    if (!s->flow_control || s->initial_granted) {
        return;
    }
    if (grant(s, SPS_INITIAL_CREDITS) == SPS_OK) {
        s->initial_granted = true;
    }
}

int sps_init(sps_t *s, sps_role_t role, const sps_port_t *port, bool flow_control)
{
    if (s == NULL || port == NULL || port->send_fifo == NULL) {
        return SPS_ERR_ARG;
    }
    if (role != SPS_ROLE_CLIENT && role != SPS_ROLE_SERVER) {
        return SPS_ERR_ARG;
    }
    if (flow_control && port->send_credits == NULL) {
        return SPS_ERR_ARG;
    }
    if (role == SPS_ROLE_CLIENT && port->subscribe == NULL) {
        return SPS_ERR_ARG;
    }
    memset(s, 0, sizeof(*s));
    s->role = role;
    s->port = *port;
    s->flow_control = flow_control;
    s->packet_size = SPS_DEFAULT_PACKET_SIZE;
    s->state = SPS_STATE_DISCONNECTED;
    return SPS_OK;
}

int sps_connected(sps_t *s)
{
    if (s == NULL || s->state != SPS_STATE_DISCONNECTED) {
        return SPS_ERR_STATE;
    }
    reset_link(s);
    set_state(s, SPS_STATE_CONNECTED);

    if (s->role == SPS_ROLE_SERVER) {
        /* With flow control the initial credits follow the Credits subscription. */
        if (!s->flow_control) {
            set_state(s, SPS_STATE_READY);
        }
        return SPS_OK;
    }

    if (!s->flow_control) {
        /* No Credits characteristic traffic at all, some firmware drops the link otherwise. */
        if (s->port.subscribe(s->port.ctx, SPS_CHAR_FIFO) != 0) {
            sps_disconnected(s);
            return SPS_ERR_PORT;
        }
        set_state(s, SPS_STATE_READY);
        return SPS_OK;
    }

    /* UBX-16011192 figure 5: Credits CCCD first, FIFO CCCD second, then the credits. */
    if (s->port.subscribe(s->port.ctx, SPS_CHAR_CREDITS) != 0 ||
        s->port.subscribe(s->port.ctx, SPS_CHAR_FIFO) != 0 || grant(s, SPS_INITIAL_CREDITS) != SPS_OK) {
        sps_disconnected(s);
        return SPS_ERR_PORT;
    }
    s->initial_granted = true;
    return SPS_OK;
}

void sps_disconnected(sps_t *s)
{
    if (s == NULL) {
        return;
    }
    reset_link(s);
    set_state(s, SPS_STATE_DISCONNECTED);
}

int sps_retry_handshake(sps_t *s)
{
    if (s == NULL || s->role != SPS_ROLE_CLIENT || !s->flow_control ||
        s->state != SPS_STATE_CONNECTED) {
        return SPS_ERR_STATE;
    }
    return grant(s, SPS_INITIAL_CREDITS);
}

void sps_on_credits_subscribed(sps_t *s)
{
    if (s != NULL && s->role == SPS_ROLE_SERVER && s->state != SPS_STATE_DISCONNECTED) {
        grant_initial(s);
    }
}

void sps_on_fifo(sps_t *s, const uint8_t *data, uint16_t len)
{
    if (s == NULL || data == NULL || len == 0 || s->state == SPS_STATE_DISCONNECTED) {
        return;
    }
    s->stats.bytes_received += len;
    s->stats.packets_received++;
    if (s->port.on_data != NULL) {
        s->port.on_data(s->port.ctx, data, len);
    }
    if (!s->flow_control) {
        return;
    }
    /* The first packet proves the client is subscribed even if the grant at subscription was lost. */
    if (s->role == SPS_ROLE_SERVER) {
        grant_initial(s);
    }
    if (s->rx_pending < UINT8_MAX) {
        s->rx_pending++;
    }
    if (s->rx_pending >= SPS_CREDIT_THRESHOLD && grant(s, s->rx_pending) == SPS_OK) {
        s->rx_pending = 0;
    }
}

void sps_on_credits(sps_t *s, const uint8_t *data, uint16_t len)
{
    uint16_t credits;

    if (s == NULL || data == NULL || len == 0 || !s->flow_control ||
        s->state == SPS_STATE_DISCONNECTED) {
        return;
    }
    credits = data[0];
    s->stats.credits_received += credits;
    s->tx_credits = (credits > (uint16_t)(UINT16_MAX - s->tx_credits))
                        ? UINT16_MAX
                        : (uint16_t)(s->tx_credits + credits);

    /* Credits written by the client mean it is subscribed and waits for ours. */
    if (s->role == SPS_ROLE_SERVER) {
        grant_initial(s);
    }
    if (s->state == SPS_STATE_CONNECTED) {
        set_state(s, SPS_STATE_READY);
    }
    sps_flush(s);
}

void sps_flush(sps_t *s)
{
    if (s == NULL || s->state != SPS_STATE_READY || s->flushing) {
        return;
    }
    s->flushing = true;
    while (s->tx_len > 0 && (!s->flow_control || s->tx_credits > 0)) {
        uint16_t n = (s->tx_len < s->packet_size) ? s->tx_len : s->packet_size;

        if (s->port.send_fifo(s->port.ctx, s->tx_buf, n) != 0) {
            break;
        }
        /* The callback may have dropped the link. */
        if (s->state != SPS_STATE_READY || s->tx_len < n) {
            break;
        }
        if (s->flow_control) {
            s->tx_credits--;
        }
        s->stats.bytes_sent += n;
        s->stats.packets_sent++;
        /* tx_len may have grown inside the callback; the first n bytes are ours. */
        memmove(s->tx_buf, s->tx_buf + n, (size_t)(s->tx_len - n));
        s->tx_len = (uint16_t)(s->tx_len - n);
    }
    s->flushing = false;
}

int sps_send(sps_t *s, const uint8_t *data, uint16_t len)
{
    uint16_t room;

    if (s == NULL || (data == NULL && len > 0)) {
        return SPS_ERR_ARG;
    }
    if (s->state == SPS_STATE_DISCONNECTED) {
        return SPS_ERR_STATE;
    }
    room = sps_tx_free(s);
    if (len > room) {
        len = room;
    }
    if (len > 0) {
        memcpy(s->tx_buf + s->tx_len, data, len);
        s->tx_len = (uint16_t)(s->tx_len + len);
    }
    sps_flush(s);
    return len;
}

uint16_t sps_tx_free(const sps_t *s)
{
    return (uint16_t)(SPS_TX_BUFFER_SIZE - s->tx_len);
}

int sps_set_packet_size(sps_t *s, uint16_t size)
{
    if (s == NULL || size == 0 || size > SPS_TX_BUFFER_SIZE) {
        return SPS_ERR_ARG;
    }
    s->packet_size = size;
    return SPS_OK;
}
