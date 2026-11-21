#include "sscp.h"
#include "vnet.h"
#include "queue.h"
#include "hashmap.h"
#include "../common/transfer.h"
#include <stdlib.h>
#include <string.h>

struct vnet_packet {
    struct list_node wire_node;
    struct list_node buffer_node;
    uint64_t due_time_us;
    size_t length;
    int is_dropped;
    unsigned char data[MTU];
};

struct vnet_channel {
    unsigned direction;
    struct list_node wire_queue;
    struct list_node buffer_queue;
    uint64_t next_tx_free_us;
    uint32_t queued_packets;
    uint64_t last_due_time_us;
    uint32_t *qdepths;
    uint32_t *qdelays_us;
    uint32_t *owds_us;
    size_t stat_count;
    size_t stat_cap;
};

#define VNET_MAX_APPS 32

struct vnet_network {
    struct vnet_channel channels[2];
    uint64_t clock_tick;
    uint32_t delay_ms;
    uint32_t rate_mbps;
    uint32_t loss_ppm;
    uint32_t queue_limit;
    uint32_t jitter_ms;
    int allow_reorder;
    int markov_drift;
    uint32_t markov_state;
    uint32_t prng_state;
    struct transfer *apps[2];
    struct transfer *apps_by_fd[VNET_MAX_APPS];
    vnet_observer observer;
};

static struct vnet_network g_vnet;
static int g_vnet_initialized;

uint64_t vnet_packets;
uint64_t vnet_dropped;
uint64_t vnet_queue_dropped;
uint64_t vnet_pending;

static uint32_t vnet_random_step(struct vnet_network *net)
{
    net->prng_state ^= net->prng_state << 13;
    net->prng_state ^= net->prng_state >> 17;
    net->prng_state ^= net->prng_state << 5;
    return net->prng_state;
}

static void channel_init(struct vnet_channel *ch, unsigned direction)
{
    ch->direction = direction;
    list_node_init(&ch->wire_queue);
    list_node_init(&ch->buffer_queue);
    ch->next_tx_free_us = 0;
    ch->queued_packets = 0;
    ch->last_due_time_us = 0;
    ch->qdepths = NULL;
    ch->qdelays_us = NULL;
    ch->owds_us = NULL;
    ch->stat_count = 0;
    ch->stat_cap = 0;
}

static void channel_record_stat(struct vnet_channel *ch, uint32_t qdepth, uint32_t qdelay_us, uint32_t owd_us)
{
    if (ch->stat_count == ch->stat_cap) {
        size_t new_cap = ch->stat_cap ? ch->stat_cap * 2 : 1024;
        uint32_t *qd = realloc(ch->qdepths, new_cap * sizeof(uint32_t));
        uint32_t *ql = realloc(ch->qdelays_us, new_cap * sizeof(uint32_t));
        uint32_t *ow = realloc(ch->owds_us, new_cap * sizeof(uint32_t));
        if (!qd || !ql || !ow) {
            if (qd) ch->qdepths = qd;
            if (ql) ch->qdelays_us = ql;
            if (ow) ch->owds_us = ow;
            return;
        }
        ch->qdepths = qd;
        ch->qdelays_us = ql;
        ch->owds_us = ow;
        ch->stat_cap = new_cap;
    }
    ch->qdepths[ch->stat_count] = qdepth;
    ch->qdelays_us[ch->stat_count] = qdelay_us;
    ch->owds_us[ch->stat_count] = owd_us;
    ++ch->stat_count;
}

static void channel_flush_buffer(struct vnet_channel *ch, uint64_t now_us, uint64_t delay_us)
{
    while (!list_empty(&ch->buffer_queue)) {
        struct list_node *node = ch->buffer_queue.next;
        struct vnet_packet *pkt = container_of(node, struct vnet_packet, buffer_node);

        if (pkt->due_time_us - delay_us > now_us) {
            break;
        }

        queue_dequeue(&ch->buffer_queue);
        list_node_init(&pkt->buffer_node);
        --ch->queued_packets;
    }
}

static uint64_t channel_calc_tx_finish(struct vnet_channel *ch, uint64_t now_us, size_t len, uint32_t rate_mbps)
{
    uint64_t tx_start_us;
    uint64_t tx_duration_us;

    tx_start_us = now_us;
    if (rate_mbps == 0) {
        return tx_start_us;
    }

    if (tx_start_us < ch->next_tx_free_us) {
        tx_start_us = ch->next_tx_free_us;
    }

    tx_duration_us = (len * 8 + rate_mbps - 1) / rate_mbps;
    ch->next_tx_free_us = tx_start_us + tx_duration_us;

    return ch->next_tx_free_us;
}

static void channel_enqueue(struct vnet_channel *ch, struct vnet_packet *pkt)
{
    if (!g_vnet.allow_reorder) {
        if (pkt->due_time_us < ch->last_due_time_us) {
            pkt->due_time_us = ch->last_due_time_us;
        }
        ch->last_due_time_us = pkt->due_time_us;
        queue_enqueue(&ch->wire_queue, &pkt->wire_node);
    } else {
        struct list_node *cur;
        for (cur = ch->wire_queue.prev; cur != &ch->wire_queue; cur = cur->prev) {
            struct vnet_packet *p = container_of(cur, struct vnet_packet, wire_node);
            if (p->due_time_us <= pkt->due_time_us)
                break;
        }
        list_add_next(cur, &pkt->wire_node);
    }
    queue_enqueue(&ch->buffer_queue, &pkt->buffer_node);
    ++ch->queued_packets;
    ++vnet_pending;
}

static struct vnet_packet *channel_pop_arrived(struct vnet_channel *ch, uint64_t now_us)
{
    struct list_node *node;
    struct vnet_packet *pkt;

    if (list_empty(&ch->wire_queue)) {
        return NULL;
    }

    node = ch->wire_queue.next;
    pkt = container_of(node, struct vnet_packet, wire_node);
    if (pkt->due_time_us > now_us) {
        return NULL;
    }

    queue_dequeue(&ch->wire_queue);

    if (!list_empty(&pkt->buffer_node)) {
        list_remove(&pkt->buffer_node);
        list_node_init(&pkt->buffer_node);
        --ch->queued_packets;
    }
    --vnet_pending;

    return pkt;
}

static void channel_clear(struct vnet_channel *ch)
{
    while (!list_empty(&ch->wire_queue)) {
        struct list_node *node = queue_dequeue(&ch->wire_queue);
        struct vnet_packet *pkt = container_of(node, struct vnet_packet, wire_node);
        free(pkt);
    }

    list_node_init(&ch->wire_queue);
    list_node_init(&ch->buffer_queue);
    ch->next_tx_free_us = 0;
    ch->queued_packets = 0;
    ch->last_due_time_us = 0;
    free(ch->qdepths);
    free(ch->qdelays_us);
    free(ch->owds_us);
    ch->qdepths = NULL;
    ch->qdelays_us = NULL;
    ch->owds_us = NULL;
    ch->stat_count = 0;
    ch->stat_cap = 0;
}

static int cmp_u32_asc(const void *pa, const void *pb)
{
    uint32_t a = *(const uint32_t *)pa;
    uint32_t b = *(const uint32_t *)pb;
    return (a > b) - (a < b);
}

static uint32_t pct_u32_sorted(const uint32_t *arr, size_t n, double p)
{
    size_t idx;
    if (!n) return 0;
    idx = (size_t)(p * (double)(n - 1) + 0.5);
    if (idx >= n) idx = n - 1;
    return arr[idx];
}

void vnet_get_stats(unsigned direction, struct vnet_stats *out)
{
    struct vnet_channel *ch;
    size_t i, n;
    uint64_t qsum = 0, qdsum = 0;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (direction > 1 || !g_vnet_initialized) return;
    ch = &g_vnet.channels[direction];
    n = ch->stat_count;
    if (!n) return;
    qsort(ch->qdepths, n, sizeof(uint32_t), cmp_u32_asc);
    qsort(ch->qdelays_us, n, sizeof(uint32_t), cmp_u32_asc);
    qsort(ch->owds_us, n, sizeof(uint32_t), cmp_u32_asc);
    for (i = 0; i < n; ++i) {
        qsum += ch->qdepths[i];
        qdsum += ch->qdelays_us[i];
    }
    out->samples = (uint64_t)n;
    out->q_max = ch->qdepths[n - 1];
    out->q_avg = (double)qsum / (double)n;
    out->q_p95 = (double)pct_u32_sorted(ch->qdepths, n, 0.95);
    out->q_p99 = (double)pct_u32_sorted(ch->qdepths, n, 0.99);
    out->qdelay_avg_ms = (double)qdsum / (double)n / 1000.0;
    out->qdelay_p50_ms = (double)pct_u32_sorted(ch->qdelays_us, n, 0.50) / 1000.0;
    out->qdelay_p95_ms = (double)pct_u32_sorted(ch->qdelays_us, n, 0.95) / 1000.0;
    out->qdelay_p99_ms = (double)pct_u32_sorted(ch->qdelays_us, n, 0.99) / 1000.0;
    out->qdelay_max_ms = (double)ch->qdelays_us[n - 1] / 1000.0;
    out->owd_p50_ms = (double)pct_u32_sorted(ch->owds_us, n, 0.50) / 1000.0;
    out->owd_p95_ms = (double)pct_u32_sorted(ch->owds_us, n, 0.95) / 1000.0;
    out->owd_p99_ms = (double)pct_u32_sorted(ch->owds_us, n, 0.99) / 1000.0;
    out->owd_max_ms = (double)ch->owds_us[n - 1] / 1000.0;
}

uint32_t sscp_now_time(void)
{
    return (uint32_t)g_vnet.clock_tick;
}

void vnet_observe(vnet_observer callback)
{
    g_vnet.observer = callback;
}

void vnet_attach(struct transfer *a, struct transfer *b)
{
    g_vnet.apps[0] = b;
    g_vnet.apps[1] = a;
}

void vnet_attach_fd(int local_fd, struct transfer *t)
{
    if (local_fd >= 0 && local_fd < VNET_MAX_APPS) {
        g_vnet.apps_by_fd[local_fd] = t;
    }
}

void vnet_set_jitter(uint32_t jitter_ms, int allow_reorder)
{
    g_vnet.jitter_ms = jitter_ms;
    g_vnet.allow_reorder = allow_reorder;
}

void vnet_set_markov_drift(int enable)
{
    g_vnet.markov_drift = enable;
}

void vnet_queue_limit(uint32_t packets)
{
    g_vnet.queue_limit = packets;
}

void vnet_free(void)
{
    if (!g_vnet_initialized) {
        return;
    }
    channel_clear(&g_vnet.channels[0]);
    channel_clear(&g_vnet.channels[1]);
    vnet_pending = 0;
    g_vnet.apps[0] = NULL;
    g_vnet.apps[1] = NULL;
    memset(g_vnet.apps_by_fd, 0, sizeof(g_vnet.apps_by_fd));
    g_vnet.observer = NULL;
}

void vnet_init(uint32_t delay_ms, uint32_t mbps, uint32_t loss_ppm, uint32_t seed)
{
    vnet_free();
    g_vnet_initialized = 1;

    channel_init(&g_vnet.channels[0], 0);
    channel_init(&g_vnet.channels[1], 1);

    memset(g_vnet.apps_by_fd, 0, sizeof(g_vnet.apps_by_fd));
    g_vnet.clock_tick = 0;
    g_vnet.delay_ms = delay_ms;
    g_vnet.rate_mbps = mbps;
    g_vnet.loss_ppm = loss_ppm;
    g_vnet.queue_limit = 0;
    g_vnet.jitter_ms = 0;
    g_vnet.allow_reorder = 0;
    g_vnet.markov_drift = 0;
    g_vnet.markov_state = 0;
    g_vnet.prng_state = seed ? seed : 1;

    vnet_packets = 0;
    vnet_dropped = 0;
    vnet_queue_dropped = 0;
}

int vnet_send(void *user, const void *buf, size_t len)
{
    struct vnet_endpoint *endpoint;
    struct vnet_channel *channel;
    struct vnet_packet *pkt;
    uint64_t now_us;
    uint64_t delay_us;
    uint64_t tx_finish_us;
    int is_random_drop;

    if (len > MTU) {
        return -1;
    }

    endpoint = (struct vnet_endpoint *)user;
    channel = &g_vnet.channels[endpoint->direction];
    now_us = g_vnet.clock_tick * 1000 / SSCP_TICK_SCALE;
    uint64_t cur_delay_ms = g_vnet.delay_ms;
    if (g_vnet.markov_drift) {
        if ((vnet_random_step(&g_vnet) % 400) == 0) {
            g_vnet.markov_state = (g_vnet.markov_state + 1 + (vnet_random_step(&g_vnet) % 2)) % 3;
        }
        if (g_vnet.markov_state == 1) {
            cur_delay_ms = (g_vnet.delay_ms * 3) / 2;
        } else if (g_vnet.markov_state == 2) {
            cur_delay_ms = (g_vnet.delay_ms * 5) / 2;
        }
    }
    uint64_t jitter_us = 0;
    if (g_vnet.jitter_ms > 0) {
        jitter_us = (uint64_t)(vnet_random_step(&g_vnet) % (g_vnet.jitter_ms * 1000 + 1));
    }
    delay_us = cur_delay_ms * 1000 + jitter_us;

    ++vnet_packets;
    if (g_vnet.observer) {
        g_vnet.observer(channel->direction, VNET_SEND, buf, len, (uint32_t)g_vnet.clock_tick);
    }

    channel_flush_buffer(channel, now_us, delay_us);

    if (g_vnet.queue_limit && channel->queued_packets >= g_vnet.queue_limit) {
        ++vnet_dropped;
        ++vnet_queue_dropped;
        return (int)len;
    }

    uint64_t prev_tx_free = channel->next_tx_free_us;
    uint32_t qwait_us = (g_vnet.rate_mbps && prev_tx_free > now_us) ? (uint32_t)(prev_tx_free - now_us) : 0;
    tx_finish_us = channel_calc_tx_finish(channel, now_us, len, g_vnet.rate_mbps);

    is_random_drop = (vnet_random_step(&g_vnet) % 1000000) < g_vnet.loss_ppm;
    if (is_random_drop) {
        ++vnet_dropped;
        if (!g_vnet.queue_limit) {
            return (int)len;
        }
    }

    pkt = malloc(sizeof(*pkt));
    if (!pkt) {
        abort();
    }

    list_node_init(&pkt->wire_node);
    list_node_init(&pkt->buffer_node);
    pkt->length = len;
    pkt->due_time_us = tx_finish_us + delay_us;
    pkt->is_dropped = is_random_drop;
    memcpy(pkt->data, buf, len);

    channel_enqueue(channel, pkt);
    if (!is_random_drop) {
        uint32_t owd_us = (pkt->due_time_us > now_us) ? (uint32_t)(pkt->due_time_us - now_us) : 0;
        channel_record_stat(channel, channel->queued_packets, qwait_us, owd_us);
    }
    return (int)len;
}

static int dispatch_packet(unsigned direction, struct vnet_packet *pkt)
{
    const struct sscp_hdr *hdr;
    int fd;
    int result;

    if (g_vnet.observer) {
        g_vnet.observer(direction, VNET_ARRIVAL, pkt->data, pkt->length, (uint32_t)g_vnet.clock_tick);
    }

    hdr = (const struct sscp_hdr *)pkt->data;
    fd = (int)hdr->fd;

    struct transfer *app =
        (fd >= 0 && fd < VNET_MAX_APPS && g_vnet.apps_by_fd[fd])
            ? g_vnet.apps_by_fd[fd]
            : g_vnet.apps[direction];

    if (app) {
        result = transfer_input(app, pkt->data, pkt->length);
    } else {
        result = sscp_input(NULL, pkt->data, pkt->length);
    }

    if (result < 0 && sscp_is_closed(fd)) {
        result = 0;
    }

    if (result >= 0 && g_vnet.observer) {
        g_vnet.observer(direction, VNET_INPUT_RETURNED, pkt->data, pkt->length, (uint32_t)g_vnet.clock_tick);
    }

    return result;
}

int vnet_step(void)
{
    uint64_t now_us;
    unsigned d;

    ++g_vnet.clock_tick;
    now_us = g_vnet.clock_tick * 1000 / SSCP_TICK_SCALE;

    for (d = 0; d < 2; ++d) {
        struct vnet_channel *channel = &g_vnet.channels[d];
        struct vnet_packet *pkt;

        while ((pkt = channel_pop_arrived(channel, now_us)) != NULL) {
            int result = 0;

            if (!pkt->is_dropped) {
                result = dispatch_packet(d, pkt);
            }

            free(pkt);
            if (result < 0) {
                return -1;
            }
        }
    }

    sscp_timer_process();
    return 0;
}
