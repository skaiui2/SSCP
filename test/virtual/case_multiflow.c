#include "sscp.h"
#include "vnet.h"
#include "../common/transfer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

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

static uint64_t retrans_flow[2];
static uint64_t wire_bytes_flow[2];
static uint32_t highest_seq_flow[2];
static int data_seen_flow[2];
static uint64_t timeouts_fd[5];
static void (*orig_timeout_fd[5])(struct sscp_stream *);

static void count_timeout(struct sscp_stream *ss)
{
    if (ss->src_fd >= 1 && ss->src_fd <= 4) {
        ++timeouts_fd[ss->src_fd];
        if (orig_timeout_fd[ss->src_fd])
            orig_timeout_fd[ss->src_fd](ss);
    }
}

static void observe(unsigned direction, int event, const void *buf, size_t len, uint32_t now)
{
    const struct sscp_hdr *h = buf;
    (void)now;
    if (len < sizeof(*h))
        return;
    if (direction == 0 && event == VNET_SEND && (h->flags & SSCP_FLAG_DATA)) {
        int idx = (h->fd == 2) ? 0 : ((h->fd == 4) ? 1 : -1);
        uint32_t bytes = ntohs(h->len);
        uint32_t seq = ntohl(h->seq);
        if (idx < 0 || !bytes)
            return;
        if (!data_seen_flow[idx]) {
            highest_seq_flow[idx] = seq;
            data_seen_flow[idx] = 1;
        }
        if (SEQ_LT(seq, highest_seq_flow[idx])) {
            ++retrans_flow[idx];
        } else {
            highest_seq_flow[idx] = seq + bytes;
            wire_bytes_flow[idx] += bytes;
        }
    }
}

struct flow_cfg {
    uint64_t bytes;
    int cc;
    int budget_pacing;
    uint32_t ss_g_q16;
    uint32_t ca_g_q16;
    int start_ca;
};

static void apply_flow_cfg(struct transfer *a, struct transfer *b, const struct flow_cfg *cfg)
{
    if (cfg->start_ca) {
        a->stream->cwnd = MSS;
        a->stream->ssthresh = MSS;
        b->stream->cwnd = MSS;
        b->stream->ssthresh = MSS;
    }
    if (cfg->budget_pacing >= 0) {
        a->stream->budget_pacing = (uint8_t)cfg->budget_pacing;
        b->stream->budget_pacing = (uint8_t)cfg->budget_pacing;
    }
    if (cfg->ss_g_q16 > 0) {
        a->stream->static_ss_gain_q16 = cfg->ss_g_q16;
        b->stream->static_ss_gain_q16 = cfg->ss_g_q16;
    }
    if (cfg->ca_g_q16 > 0) {
        a->stream->static_ca_gain_q16 = cfg->ca_g_q16;
        b->stream->static_ca_gain_q16 = cfg->ca_g_q16;
    }
    if (cfg->cc == SSCP_CC_PROB) {
        sscp_prob_configure(a->stream, 100, 8000);
        sscp_prob_configure(b->stream, 100, 8000);
    }
    orig_timeout_fd[a->fd] = a->stream->cc_on_timeout;
    orig_timeout_fd[b->fd] = b->stream->cc_on_timeout;
    a->stream->cc_on_timeout = count_timeout;
    b->stream->cc_on_timeout = count_timeout;
}

int main(int argc, char **argv)
{
    struct flow_cfg f0 = {UINT64_C(4194304), SSCP_CC_AIMD, 1, 0, 0, 0};
    struct flow_cfg f1 = {UINT64_C(4194304), SSCP_CC_AIMD, 1, 0, 0, 0};
    uint32_t delay = 25, rate = 100, loss = 0, seed = 1, timeout = 120000;
    uint32_t queue_packets = 50, jitter_ms = 0, stagger_ms = 0;
    int allow_reorder = 0;
    int i, ok = 0;
    uint64_t steps = 0;
    struct vnet_endpoint ep_a = {0}, ep_b = {1};
    struct sscp_transport_class ta = {.send = vnet_send, .user = &ep_a};
    struct sscp_transport_class tb = {.send = vnet_send, .user = &ep_b};
    struct transfer a0, b0, a1, b1;
    int f1_started = 0, f0_done = 0, f1_done = 0;
    uint64_t fct0_ticks = 0, fct1_ticks = 0;
    int overlap_started = 0, overlap_ended = 0;
    uint64_t overlap_start_ticks = 0, overlap_end_ticks = 0;
    uint64_t rx0_at_start = 0, rx1_at_start = 0;
    uint64_t rx0_at_end = 0, rx1_at_end = 0;
    FILE *summary;

    for (i = 1; i < argc; i += 2) {
        uint64_t v;
        char *end;
        if (i + 1 >= argc) return 2;
        v = strtoull(argv[i + 1], &end, 10);
        if (*end) return 2;
        if (!strcmp(argv[i], "--bytes")) { f0.bytes = v; f1.bytes = v; }
        else if (!strcmp(argv[i], "--bytes0")) f0.bytes = v;
        else if (!strcmp(argv[i], "--bytes1")) f1.bytes = v;
        else if (!strcmp(argv[i], "--delay-ms")) delay = (uint32_t)v;
        else if (!strcmp(argv[i], "--mbps")) rate = (uint32_t)v;
        else if (!strcmp(argv[i], "--loss-ppm")) loss = (uint32_t)v;
        else if (!strcmp(argv[i], "--seed")) seed = (uint32_t)v;
        else if (!strcmp(argv[i], "--queue-packets")) queue_packets = (uint32_t)v;
        else if (!strcmp(argv[i], "--jitter-ms")) jitter_ms = (uint32_t)v;
        else if (!strcmp(argv[i], "--reorder")) allow_reorder = (int)(v != 0);
        else if (!strcmp(argv[i], "--timeout-ms")) timeout = (uint32_t)v;
        else if (!strcmp(argv[i], "--stagger-ms")) stagger_ms = (uint32_t)v;
        else if (!strcmp(argv[i], "--cc0")) f0.cc = (int)v;
        else if (!strcmp(argv[i], "--budget0")) f0.budget_pacing = (int)(v != 0);
        else if (!strcmp(argv[i], "--ss-g0")) f0.ss_g_q16 = (uint32_t)v;
        else if (!strcmp(argv[i], "--ca-g0")) f0.ca_g_q16 = (uint32_t)v;
        else if (!strcmp(argv[i], "--start-ca0")) f0.start_ca = (int)(v != 0);
        else if (!strcmp(argv[i], "--cc1")) f1.cc = (int)v;
        else if (!strcmp(argv[i], "--budget1")) f1.budget_pacing = (int)(v != 0);
        else if (!strcmp(argv[i], "--ss-g1")) f1.ss_g_q16 = (uint32_t)v;
        else if (!strcmp(argv[i], "--ca-g1")) f1.ca_g_q16 = (uint32_t)v;
        else if (!strcmp(argv[i], "--start-ca1")) f1.start_ca = (int)(v != 0);
        else return 2;
    }

    summary = fopen("summary_multi.json", "w");
    if (!summary) return 2;

    vnet_init(delay, rate, loss, seed);
    vnet_queue_limit(queue_packets);
    vnet_set_jitter(jitter_ms, allow_reorder);
    vnet_observe(observe);

    if (sscp_init(16)) return 2;

    if (transfer_init(&b0, &tb, 2, 1, 0, f0.bytes, f0.cc) ||
        transfer_init(&a0, &ta, 1, 2, 1, f0.bytes, f0.cc))
        return 2;
    apply_flow_cfg(&a0, &b0, &f0);
    vnet_attach_fd(1, &a0);
    vnet_attach_fd(2, &b0);

    if (stagger_ms == 0) {
        if (transfer_init(&b1, &tb, 4, 3, 0, f1.bytes, f1.cc) ||
            transfer_init(&a1, &ta, 3, 4, 1, f1.bytes, f1.cc))
            return 2;
        apply_flow_cfg(&a1, &b1, &f1);
        vnet_attach_fd(3, &a1);
        vnet_attach_fd(4, &b1);
        f1_started = 1;
    }

    while (steps < (uint64_t)timeout * SSCP_TICK_SCALE) {
        ++steps;
        if (!f1_started && steps >= (uint64_t)stagger_ms * SSCP_TICK_SCALE) {
            if (transfer_init(&b1, &tb, 4, 3, 0, f1.bytes, f1.cc) ||
                transfer_init(&a1, &ta, 3, 4, 1, f1.bytes, f1.cc))
                break;
            apply_flow_cfg(&a1, &b1, &f1);
            vnet_attach_fd(3, &a1);
            vnet_attach_fd(4, &b1);
            f1_started = 1;
        }
        if (vnet_step() || transfer_step(&a0) || transfer_step(&b0))
            break;
        if (f1_started && (transfer_step(&a1) || transfer_step(&b1)))
            break;

        if (!f0_done && a0.done && b0.done) {
            f0_done = 1;
            fct0_ticks = steps;
        }
        if (f1_started && !f1_done && a1.done && b1.done) {
            f1_done = 1;
            fct1_ticks = steps;
        }
        if (!overlap_started && f1_started && a0.established && a1.established) {
            overlap_started = 1;
            overlap_start_ticks = steps;
            rx0_at_start = b0.received;
            rx1_at_start = b1.received;
        }
        if (overlap_started && !overlap_ended && (f0_done || f1_done)) {
            overlap_ended = 1;
            overlap_end_ticks = steps;
            rx0_at_end = b0.received;
            rx1_at_end = b1.received;
        }
        if (f0_done && f1_done) {
            ok = 1;
            break;
        }
    }

    double ov_rx0 = overlap_ended ? (double)(rx0_at_end - rx0_at_start) : 0.0;
    double ov_rx1 = overlap_ended ? (double)(rx1_at_end - rx1_at_start) : 0.0;
    double ov_sum = ov_rx0 + ov_rx1;
    double share0 = (ov_sum > 0.0) ? (ov_rx0 / ov_sum) : 0.0;
    double share1 = (ov_sum > 0.0) ? (ov_rx1 / ov_sum) : 0.0;
    double jain = (ov_sum > 0.0) ? ((ov_sum * ov_sum) / (2.0 * (ov_rx0 * ov_rx0 + ov_rx1 * ov_rx1))) : 0.0;
    double ov_ms = overlap_ended ? ((double)(overlap_end_ticks - overlap_start_ticks) / SSCP_TICK_SCALE) : 0.0;
    double mbps0 = (ov_ms > 0.0) ? (ov_rx0 * 8.0 / (ov_ms * 1000.0)) : 0.0;
    double mbps1 = (ov_ms > 0.0) ? (ov_rx1 * 8.0 / (ov_ms * 1000.0)) : 0.0;

    int checksum_ok = ok &&
        a0.sent == f0.bytes && b0.received == f0.bytes && a0.hash == b0.hash && a0.peer_hash == a0.hash &&
        a1.sent == f1.bytes && b1.received == f1.bytes && a1.hash == b1.hash && a1.peer_hash == a1.hash;

    transfer_cleanup(&a0); transfer_cleanup(&b0);
    if (f1_started) {
        transfer_cleanup(&a1); transfer_cleanup(&b1);
    }
    int cleanup_ok = !core_live && !sscp_timer_tree.count && list_empty(&sscp_stream_queue);
    ok = ok && checksum_ok && cleanup_ok;

    fprintf(summary,
        "{\"success\":%s,\"mbps\":%u,\"delay_ms\":%u,\"queue_packets\":%u,"
        "\"loss_ppm\":%u,\"jitter_ms\":%u,\"stagger_ms\":%u,"
        "\"fct0_ms\":%.2f,\"fct1_ms\":%.2f,\"overlap_ms\":%.2f,"
        "\"overlap_bytes0\":%.0f,\"overlap_bytes1\":%.0f,"
        "\"overlap_mbps0\":%.3f,\"overlap_mbps1\":%.3f,"
        "\"share0\":%.4f,\"share1\":%.4f,\"jain\":%.4f,"
        "\"retrans0\":%" PRIu64 ",\"retrans1\":%" PRIu64 ","
        "\"timeouts0\":%" PRIu64 ",\"timeouts1\":%" PRIu64 ","
        "\"queue_dropped\":%" PRIu64 ",\"packets\":%" PRIu64 "}\n",
        ok ? "true" : "false", rate, delay, queue_packets, loss, jitter_ms, stagger_ms,
        (double)fct0_ticks / SSCP_TICK_SCALE,
        (double)fct1_ticks / SSCP_TICK_SCALE,
        ov_ms, ov_rx0, ov_rx1, mbps0, mbps1, share0, share1, jain,
        retrans_flow[0], retrans_flow[1],
        timeouts_fd[1] + timeouts_fd[2], timeouts_fd[3] + timeouts_fd[4],
        vnet_queue_dropped, vnet_packets);
    fclose(summary);
    vnet_free();
    printf("MULTIFLOW %s: share0=%.4f share1=%.4f jain=%.4f fct0=%.1f fct1=%.1f qdrop=%" PRIu64 "\n",
           ok ? "PASS" : "FAIL", share0, share1, jain,
           (double)fct0_ticks / SSCP_TICK_SCALE,
           (double)fct1_ticks / SSCP_TICK_SCALE,
           vnet_queue_dropped);
    return ok ? 0 : 1;
}
