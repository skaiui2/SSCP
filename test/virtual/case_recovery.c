#include "sscp.h"
#include "vnet.h"
#include "../common/transfer.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t core_live;
static void *count_malloc(size_t size)
{
    void *p = malloc(size);
    if (p) ++core_live;
    return p;
}
static void count_free(void *p)
{
    if (p) --core_live;
    free(p);
}
#define malloc count_malloc
#define free count_free
#include "../../sscp.c"
#undef malloc
#undef free

extern uint32_t sscp_now_time(void);

static int g_test_mode;
static struct transfer g_a, g_b;
static void (*g_orig_timeout[2])(struct sscp_stream *);
static uint64_t g_timeouts[2];
static uint64_t g_retrans_packets;
static uint32_t g_highest_seq[2];
static int g_data_seen[2];

/* Test A: 6-packet consecutive burst drop */
#define BURST_DROP_COUNT 6
static uint32_t g_burst_tx_pkts;
static uint32_t g_burst_drop_seqs[BURST_DROP_COUNT];
static int g_burst_dropped;

/* Test B: dropped fast retransmission recovered by RTO */
static uint32_t g_rexdrop_tx_pkts;
static int g_rexdrop_has_target;
static uint32_t g_rexdrop_target_seq;
static int g_rexdrop_drops;
static int g_rexdrop_target_tx;
static int g_rexdrop_pre_rto_rexmits;

/* Test C: unit test packet counter */
static int g_unit_tx_count;

/* Test D: scattered multi-hole SACK across > 128 packets */
#define MULTI_DROP_COUNT 8
static const uint32_t g_multi_drop_targets[MULTI_DROP_COUNT] = {
    40U, 55U, 75U, 100U, 130U, 165U, 200U, 235U
};
static uint32_t g_multi_tx_pkts;
static uint32_t g_multi_drop_seqs[MULTI_DROP_COUNT];
static int g_multi_dropped;
static uint64_t g_data_sack_flag;
static uint64_t g_plain_ack_30b;
static uint64_t g_sack_ack_pkts;

static void count_timeout(struct sscp_stream *ss)
{
    unsigned idx = (ss->src_fd == 1 || ss->src_fd == 3 || ss->src_fd == 7) ? 0U : 1U;
    ++g_timeouts[idx];
    g_orig_timeout[idx](ss);
}

static void observe_recovery(unsigned direction, int event, const void *buf, size_t len, uint32_t now)
{
    const struct sscp_hdr *h = buf;
    uint32_t seq, bytes;
    (void)now;
    if (event != VNET_SEND || len < sizeof(*h))
        return;

    if (h->flags & SSCP_FLAG_DATA) {
        if (h->flags & SSCP_FLAG_SACK)
            ++g_data_sack_flag;
        bytes = ntohs(h->len);
        seq = ntohl(h->seq);
        if (!g_data_seen[direction]) {
            g_highest_seq[direction] = seq + bytes;
            g_data_seen[direction] = 1;
            return;
        }
        if (SEQ_LT(seq, g_highest_seq[direction])) {
            ++g_retrans_packets;
        } else {
            g_highest_seq[direction] = seq + bytes;
        }
        return;
    }

    if ((h->flags & SSCP_FLAG_ACK) && !(h->flags & SSCP_FLAG_FIN)) {
        if (h->flags & SSCP_FLAG_SACK) {
            if (len > sizeof(*h))
                ++g_sack_ack_pkts;
        } else if (len == sizeof(*h)) {
            ++g_plain_ack_30b;
        }
    }
}

static int send_a_filtered(void *user, const void *buf, size_t len)
{
    const struct sscp_hdr *h = buf;
    if (len >= sizeof(*h) && (h->flags & SSCP_FLAG_DATA)) {
        uint32_t seq = ntohl(h->seq);
        if (g_test_mode == 1) {
            int i;
            ++g_burst_tx_pkts;
            for (i = 0; i < g_burst_dropped; ++i) {
                if (g_burst_drop_seqs[i] == seq)
                    return vnet_send(user, buf, len);
            }
            if (g_burst_tx_pkts >= 40 && g_burst_dropped < BURST_DROP_COUNT) {
                g_burst_drop_seqs[g_burst_dropped++] = seq;
                return (int)len;
            }
        } else if (g_test_mode == 2) {
            ++g_rexdrop_tx_pkts;
            if (!g_rexdrop_has_target && g_rexdrop_tx_pkts == 30) {
                g_rexdrop_has_target = 1;
                g_rexdrop_target_seq = seq;
                g_rexdrop_target_tx = 1;
                ++g_rexdrop_drops;
                return (int)len;
            }
            if (g_rexdrop_has_target && seq == g_rexdrop_target_seq) {
                ++g_rexdrop_target_tx;
                if (g_timeouts[0] == 0)
                    ++g_rexdrop_pre_rto_rexmits;
                if (g_rexdrop_drops < 2) {
                    ++g_rexdrop_drops;
                    return (int)len;
                }
            }
        } else if (g_test_mode == 4) {
            int i;
            for (i = 0; i < g_multi_dropped; ++i) {
                if (g_multi_drop_seqs[i] == seq)
                    return vnet_send(user, buf, len);
            }
            ++g_multi_tx_pkts;
            if (g_multi_dropped < MULTI_DROP_COUNT &&
                g_multi_tx_pkts == g_multi_drop_targets[g_multi_dropped]) {
                g_multi_drop_seqs[g_multi_dropped++] = seq;
                return (int)len;
            }
        }
    }
    return vnet_send(user, buf, len);
}

static int unit_send(void *user, const void *buf, size_t len)
{
    const struct sscp_hdr *h = buf;
    (void)user;
    if (len >= sizeof(*h) && (h->flags & SSCP_FLAG_DATA))
        ++g_unit_tx_count;
    return (int)len;
}

static void reset_counters(int mode)
{
    g_test_mode = mode;
    g_timeouts[0] = g_timeouts[1] = 0;
    g_retrans_packets = 0;
    g_highest_seq[0] = g_highest_seq[1] = 0;
    g_data_seen[0] = g_data_seen[1] = 0;
    g_burst_tx_pkts = 0;
    g_burst_dropped = 0;
    memset(g_burst_drop_seqs, 0, sizeof(g_burst_drop_seqs));
    g_rexdrop_tx_pkts = 0;
    g_rexdrop_has_target = 0;
    g_rexdrop_target_seq = 0;
    g_rexdrop_drops = 0;
    g_rexdrop_target_tx = 0;
    g_rexdrop_pre_rto_rexmits = 0;
    g_unit_tx_count = 0;
    g_multi_tx_pkts = 0;
    g_multi_dropped = 0;
    memset(g_multi_drop_seqs, 0, sizeof(g_multi_drop_seqs));
    g_data_sack_flag = 0;
    g_plain_ack_30b = 0;
    g_sack_ack_pkts = 0;
}

static int check_clean_state(const char *tag)
{
    int clean = !core_live && !sscp_timer_tree.count && list_empty(&sscp_stream_queue);
    if (!clean) {
        fprintf(stderr, "%s: dirty state core_live=%" PRId64 " timers=%u stream_empty=%d\n",
                tag, core_live, (unsigned)sscp_timer_tree.count, list_empty(&sscp_stream_queue));
    }
    return clean;
}

static int run_burst_drop_test(void)
{
    const uint64_t total = 1048576;
    struct vnet_endpoint ea = {0}, eb = {1};
    struct sscp_transport_class ta = {.send = send_a_filtered, .user = &ea};
    struct sscp_transport_class tb = {.send = vnet_send, .user = &eb};
    uint64_t steps = 0;
    int ok = 0;

    reset_counters(1);
    vnet_init(25, 100, 0, 1);
    vnet_observe(observe_recovery);
    if (transfer_init(&g_b, &tb, 2, 1, 0, total, SSCP_CC_AIMD) ||
        transfer_init(&g_a, &ta, 1, 2, 1, total, SSCP_CC_AIMD))
        return 0;
    vnet_attach(&g_a, &g_b);

    g_orig_timeout[0] = g_a.stream->cc_on_timeout;
    g_orig_timeout[1] = g_b.stream->cc_on_timeout;
    g_a.stream->cc_on_timeout = count_timeout;
    g_b.stream->cc_on_timeout = count_timeout;

    while (steps < (uint64_t)60000U * SSCP_TICK_SCALE) {
        ++steps;
        if (vnet_step() || transfer_step(&g_a) || transfer_step(&g_b))
            break;
        if (g_a.done && g_b.done) {
            ok = 1;
            break;
        }
    }

    ok = ok && g_a.sent == total && g_b.received == total &&
         g_a.hash == g_b.hash && g_a.peer_hash == g_a.hash &&
         g_burst_dropped == BURST_DROP_COUNT &&
         g_retrans_packets == BURST_DROP_COUNT &&
         g_timeouts[0] == 0 && g_timeouts[1] == 0;

    printf("TestA(burst6): ok=%d dropped=%d retrans=%" PRIu64 " timeouts=%" PRIu64 "/%" PRIu64 "\n",
           ok, g_burst_dropped, g_retrans_packets, g_timeouts[0], g_timeouts[1]);

    transfer_cleanup(&g_a);
    transfer_cleanup(&g_b);
    vnet_free();
    return ok && check_clean_state("TestA");
}

static int run_dropped_rexmit_test(void)
{
    const uint64_t total = 262144;
    struct vnet_endpoint ea = {0}, eb = {1};
    struct sscp_transport_class ta = {.send = send_a_filtered, .user = &ea};
    struct sscp_transport_class tb = {.send = vnet_send, .user = &eb};
    uint64_t steps = 0;
    int ok = 0;

    reset_counters(2);
    vnet_init(10, 100, 0, 2);
    vnet_observe(observe_recovery);
    if (transfer_init(&g_b, &tb, 4, 3, 0, total, SSCP_CC_AIMD) ||
        transfer_init(&g_a, &ta, 3, 4, 1, total, SSCP_CC_AIMD))
        return 0;
    vnet_attach(&g_a, &g_b);

    g_orig_timeout[0] = g_a.stream->cc_on_timeout;
    g_orig_timeout[1] = g_b.stream->cc_on_timeout;
    g_a.stream->cc_on_timeout = count_timeout;
    g_b.stream->cc_on_timeout = count_timeout;

    while (steps < (uint64_t)60000U * SSCP_TICK_SCALE) {
        ++steps;
        if (vnet_step() || transfer_step(&g_a) || transfer_step(&g_b))
            break;
        if (g_a.done && g_b.done) {
            ok = 1;
            break;
        }
    }

    ok = ok && g_a.sent == total && g_b.received == total &&
         g_a.hash == g_b.hash && g_a.peer_hash == g_a.hash &&
         g_rexdrop_drops == 2 &&
         g_rexdrop_pre_rto_rexmits == 1 &&
         g_timeouts[0] >= 1;

    printf("TestB(dropped_rexmit_rto): ok=%d drops=%d pre_rto_rexmit=%d rto=%" PRIu64 " total_retrans=%" PRIu64 "\n",
           ok, g_rexdrop_drops, g_rexdrop_pre_rto_rexmits, g_timeouts[0], g_retrans_packets);

    transfer_cleanup(&g_a);
    transfer_cleanup(&g_b);
    vnet_free();
    return ok && check_clean_state("TestB");
}

static void build_unit_sack_ack(struct sscp_buf *sb,
                                uint8_t *raw,
                                uint32_t base_id,
                                uint32_t map0)
{
    struct sscp_hdr *sh = (struct sscp_hdr *)raw;
    struct sscp_sack_ext ext;
    uint32_t w0 = htonl(map0);
    uint16_t ext_len = (uint16_t)(sizeof(ext) + sizeof(w0));

    memset(sb, 0, sizeof(*sb));
    memset(sh, 0, sizeof(*sh));
    sh->flags = SSCP_FLAG_ACK | SSCP_FLAG_SACK;
    sh->len = htons(ext_len);
    ext.base_id = htonl(base_id);
    ext.map_words = htons(1U);
    memcpy(raw + sizeof(*sh), &ext, sizeof(ext));
    memcpy(raw + sizeof(*sh) + sizeof(ext), &w0, sizeof(w0));
    sb->data = raw;
    sb->len = sizeof(*sh) + ext_len;
}

static int run_unit_timer_guard_test(void)
{
    struct sscp_transport_class tc = {.send = unit_send, .user = NULL};
    struct sscp_stream *ss;
    unsigned char payload[4 * MSS];
    uint8_t sack_raw[sizeof(struct sscp_hdr) + sizeof(struct sscp_sack_ext) + 4];
    struct sscp_buf sack_buf;
    struct list_node *n;
    uint32_t expire0;
    uint32_t una;
    int i, ok;

    assert(sizeof(struct sscp_hdr) == 30U);
    reset_counters(3);
    vnet_init(10, 100, 0, 3);
    ss = sscp_stream_alloc(&tc, 5, 6);
    assert(ss != NULL);
    assert(sscp_set_cc(5, SSCP_CC_AIMD) == 0);

    ss->state = SSCP_ESTABLISHED;
    ss->cwnd = 16U * MSS;
    ss->snd_wnd = 65535U;
    ss->srtt = 50U * SSCP_TICK_SCALE;
    ss->rto = 200U * SSCP_TICK_SCALE;

    memset(payload, 0x5a, sizeof(payload));
    assert(sscp_send(5, payload, sizeof(payload)) == 0);

    for (i = 0; i < 30 * (int)SSCP_TICK_SCALE; ++i)
        assert(vnet_step() == 0);

    assert(g_unit_tx_count == 4);
    assert(ss->snd_nxt - ss->snd_una == 4U * MSS);
    assert(ss->t_retrans.active);

    expire0 = ss->t_retrans.expire;
    una = ss->snd_una;
    g_unit_tx_count = 0;

    for (n = ss->snd_q.next; n != &ss->snd_q; n = n->next) {
        struct sscp_buf *sb = container_of(n, struct sscp_buf, list);
        sb->retransmitted = 1;
    }
    ss->fr_active = 0;
    ss->dup_acks = 0;

    /* Packet #3 (idx 2 from base_id=1) is SACKed; packets #1 and #2 are missing. */
    build_unit_sack_ack(&sack_buf, sack_raw, 1U, 1U << 2);

    for (i = 0; i < 20 * (int)SSCP_TICK_SCALE; ++i)
        assert(vnet_step() == 0);

    /* Case 1: SACK over hole where all segments are already retransmitted -> 0 sent, no timer reset. */
    sscp_process_ack(ss, &sack_buf, una, 65535U);
    assert(g_unit_tx_count == 0);
    assert(ss->fr_active == 1);
    assert(ss->sacked_bytes == MSS);
    assert(ss->t_retrans.active);
    assert(ss->t_retrans.expire == expire0);

    /* Case 2: plain dupACK train while fr_active == 1 does not re-enter even with un-retransmitted segments. */
    for (n = ss->snd_q.next; n != &ss->snd_q; n = n->next) {
        struct sscp_buf *sb = container_of(n, struct sscp_buf, list);
        sb->retransmitted = 0;
    }
    for (i = 0; i < 6; ++i)
        sscp_process_ack(ss, NULL, una, 65535U);
    assert(g_unit_tx_count == 0);
    assert(ss->t_retrans.expire == expire0);

    /* Case 3: post-RTO (fr_active == 0) with SACK over all-retransmitted gap + 3 dupACKs -> 0 sent, no timer reset. */
    for (n = ss->snd_q.next; n != &ss->snd_q; n = n->next) {
        struct sscp_buf *sb = container_of(n, struct sscp_buf, list);
        sb->retransmitted = 1;
    }
    ss->fr_active = 0;
    ss->dup_acks = 0;
    for (i = 0; i < 3; ++i)
        sscp_process_ack(ss, &sack_buf, una, 65535U);
    assert(g_unit_tx_count == 0);
    assert(ss->t_retrans.expire == expire0);

    /* Case 4: post-RTO (fr_active == 0) non-SACK 3 dupACKs sends via sscp_retransmit -> timer DOES reset. */
    ss->fr_active = 0;
    ss->dup_acks = 0;
    for (i = 0; i < 3; ++i)
        sscp_process_ack(ss, NULL, una, 65535U);
    assert(g_unit_tx_count == 1);
    assert(ss->t_retrans.active);
    assert(SEQ_GT(ss->t_retrans.expire, expire0));

    sscp_stream_free(ss);
    vnet_free();

    ok = check_clean_state("TestC");
    printf("TestC(unit_timer_guard): ok=%d\n", ok);
    return ok;
}

static int run_multi_hole_sack_test(void)
{
    const uint64_t total = 1048576;
    struct vnet_endpoint ea = {0}, eb = {1};
    struct sscp_transport_class ta = {.send = send_a_filtered, .user = &ea};
    struct sscp_transport_class tb = {.send = vnet_send, .user = &eb};
    uint64_t steps = 0;
    uint32_t max_sacked = 0;
    uint32_t last_sacked = 0;
    int ok = 0;

    reset_counters(4);
    vnet_init(25, 100, 0, 4);
    vnet_observe(observe_recovery);
    if (transfer_init(&g_b, &tb, 8, 7, 0, total, SSCP_CC_AIMD) ||
        transfer_init(&g_a, &ta, 7, 8, 1, total, SSCP_CC_AIMD))
        return 0;
    vnet_attach(&g_a, &g_b);

    g_orig_timeout[0] = g_a.stream->cc_on_timeout;
    g_orig_timeout[1] = g_b.stream->cc_on_timeout;
    g_a.stream->cc_on_timeout = count_timeout;
    g_b.stream->cc_on_timeout = count_timeout;

    while (steps < (uint64_t)60000U * SSCP_TICK_SCALE) {
        struct sscp_stream *live_a;
        ++steps;
        if (vnet_step() || transfer_step(&g_a) || transfer_step(&g_b))
            break;
        live_a = hashmap_get(&sscp_stream_map, (void *)(uintptr_t)7);
        if (live_a) {
            last_sacked = live_a->sacked_bytes;
            if (last_sacked > max_sacked)
                max_sacked = last_sacked;
        }
        if (g_a.done && g_b.done) {
            ok = 1;
            break;
        }
    }

    ok = ok && g_a.sent == total && g_b.received == total &&
         g_a.hash == g_b.hash && g_a.peer_hash == g_a.hash &&
         g_multi_dropped == MULTI_DROP_COUNT &&
         g_retrans_packets == MULTI_DROP_COUNT &&
         g_timeouts[0] == 0 && g_timeouts[1] == 0 &&
         max_sacked > 0 && last_sacked == 0 &&
         g_data_sack_flag == 0 &&
         g_plain_ack_30b > 0 &&
         g_sack_ack_pkts > 0;

    printf("TestD(multi_hole_sack): ok=%d dropped=%d retrans=%" PRIu64 " timeouts=%" PRIu64 "/%" PRIu64
           " max_sacked=%u end_sacked=%u plain_ack=%" PRIu64 " sack_ack=%" PRIu64 "\n",
           ok, g_multi_dropped, g_retrans_packets, g_timeouts[0], g_timeouts[1],
           max_sacked, last_sacked, g_plain_ack_30b, g_sack_ack_pkts);

    transfer_cleanup(&g_a);
    transfer_cleanup(&g_b);
    vnet_free();
    return ok && check_clean_state("TestD");
}

int main(void)
{
    int ok = 1;
    if (sscp_init(16))
        return 2;

    if (!run_burst_drop_test()) ok = 0;
    if (!run_dropped_rexmit_test()) ok = 0;
    if (!run_unit_timer_guard_test()) ok = 0;
    if (!run_multi_hole_sack_test()) ok = 0;

    printf("case_recovery: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
