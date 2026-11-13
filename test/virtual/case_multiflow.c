#include "sscp.h"
#include "vnet.h"
#include "../common/transfer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define MAX_FLOWS 8
#define MAX_FD_SLOTS (2 * MAX_FLOWS + 1)

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

static int g_nflows = 2;
static uint64_t retrans_flow[MAX_FLOWS];
static uint64_t wire_bytes_flow[MAX_FLOWS];
static uint32_t highest_seq_flow[MAX_FLOWS];
static int data_seen_flow[MAX_FLOWS];
static uint64_t timeouts_fd[MAX_FD_SLOTS];
static void (*orig_timeout_fd[MAX_FD_SLOTS])(struct sscp_stream *);

static void count_timeout(struct sscp_stream *ss)
{
    if (ss->src_fd >= 1 && ss->src_fd < MAX_FD_SLOTS) {
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
        int idx;
        uint32_t bytes = ntohs(h->len);
        uint32_t seq = ntohl(h->seq);
        if (h->fd < 2 || (h->fd & 1U))
            return;
        idx = ((int)h->fd - 2) / 2;
        if (idx < 0 || idx >= g_nflows || !bytes)
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
    uint32_t stagger_ms;
    int64_t rttvar_q16;
};

static int parse_flow_spec(const char *spec, struct flow_cfg *fcfg, int *out_idx)
{
    uint64_t vals[9];
    int count = 0;
    const char *p = spec;

    while (*p && count < 9) {
        char *end;
        vals[count++] = strtoull(p, &end, 10);
        if (end == p)
            return -1;
        if (*end == ':') {
            p = end + 1;
        } else if (*end == '\0') {
            p = end;
        } else {
            return -1;
        }
    }
    if (count < 7 || *p != '\0' || vals[0] >= MAX_FLOWS)
        return -1;
    *out_idx = (int)vals[0];
    fcfg[*out_idx].cc = (int)vals[1];
    fcfg[*out_idx].budget_pacing = (int)(vals[2] != 0);
    fcfg[*out_idx].ss_g_q16 = (uint32_t)vals[3];
    fcfg[*out_idx].ca_g_q16 = (uint32_t)vals[4];
    fcfg[*out_idx].start_ca = (int)(vals[5] != 0);
    fcfg[*out_idx].stagger_ms = (uint32_t)vals[6];
    if (count >= 8 && vals[7] > 0)
        fcfg[*out_idx].bytes = vals[7];
    if (count >= 9)
        fcfg[*out_idx].rttvar_q16 = (int64_t)vals[8];
    return 0;
}

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
    if (cfg->rttvar_q16 >= 0) {
        a->stream->pacing_rttvar_q16 = (uint32_t)cfg->rttvar_q16;
        b->stream->pacing_rttvar_q16 = (uint32_t)cfg->rttvar_q16;
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

static int start_flow(int idx,
                      struct transfer *a, struct transfer *b,
                      struct sscp_transport_class *ta, struct sscp_transport_class *tb,
                      const struct flow_cfg *cfg)
{
    int cfd = 2 * idx + 1;
    int sfd = 2 * idx + 2;
    if (transfer_init(&b[idx], tb, sfd, cfd, 0, cfg->bytes, cfg->cc) ||
        transfer_init(&a[idx], ta, cfd, sfd, 1, cfg->bytes, cfg->cc))
        return -1;
    apply_flow_cfg(&a[idx], &b[idx], cfg);
    vnet_attach_fd(cfd, &a[idx]);
    vnet_attach_fd(sfd, &b[idx]);
    return 0;
}

int main(int argc, char **argv)
{
    struct flow_cfg fcfg[MAX_FLOWS];
    uint32_t delay = 25, rate = 100, loss = 0, seed = 1, timeout = 120000;
    uint32_t queue_packets = 50, jitter_ms = 0;
    int allow_reorder = 0, markov_drift = 0;
    int64_t global_rttvar_q16 = -1;
    uint64_t global_bytes = UINT64_C(4194304);
    int i, j, ok = 0;
    uint64_t steps = 0;
    struct vnet_endpoint ep_a = {0}, ep_b = {1};
    struct sscp_transport_class ta = {.send = vnet_send, .user = &ep_a};
    struct sscp_transport_class tb = {.send = vnet_send, .user = &ep_b};
    struct transfer a[MAX_FLOWS], b[MAX_FLOWS];
    int started[MAX_FLOWS] = {0};
    int done[MAX_FLOWS] = {0};
    uint64_t fct_ticks[MAX_FLOWS] = {0};
    int overlap_started = 0, overlap_ended = 0;
    uint64_t overlap_start_ticks = 0, overlap_end_ticks = 0;
    uint64_t rx_at_start[MAX_FLOWS] = {0};
    uint64_t rx_at_end[MAX_FLOWS] = {0};
    FILE *summary;

    for (i = 0; i < MAX_FLOWS; ++i) {
        fcfg[i].bytes = global_bytes;
        fcfg[i].cc = SSCP_CC_AIMD;
        fcfg[i].budget_pacing = 1;
        fcfg[i].ss_g_q16 = 0;
        fcfg[i].ca_g_q16 = 0;
        fcfg[i].start_ca = 0;
        fcfg[i].stagger_ms = 0;
        fcfg[i].rttvar_q16 = -1;
    }

    for (i = 1; i < argc; i += 2) {
        uint64_t v;
        char *end;
        if (i + 1 >= argc) return 2;
        if (!strcmp(argv[i], "--flow")) {
            int fidx = -1;
            if (parse_flow_spec(argv[i + 1], fcfg, &fidx)) return 2;
            continue;
        }
        v = strtoull(argv[i + 1], &end, 10);
        if (*end) return 2;
        if (!strcmp(argv[i], "--nflows")) {
            if (v < 1 || v > MAX_FLOWS) return 2;
            g_nflows = (int)v;
        }
        else if (!strcmp(argv[i], "--bytes")) {
            global_bytes = v;
            for (j = 0; j < MAX_FLOWS; ++j) fcfg[j].bytes = v;
        }
        else if (!strcmp(argv[i], "--bytes0")) fcfg[0].bytes = v;
        else if (!strcmp(argv[i], "--bytes1")) fcfg[1].bytes = v;
        else if (!strcmp(argv[i], "--delay-ms")) delay = (uint32_t)v;
        else if (!strcmp(argv[i], "--mbps")) rate = (uint32_t)v;
        else if (!strcmp(argv[i], "--loss-ppm")) loss = (uint32_t)v;
        else if (!strcmp(argv[i], "--seed")) seed = (uint32_t)v;
        else if (!strcmp(argv[i], "--queue-packets")) queue_packets = (uint32_t)v;
        else if (!strcmp(argv[i], "--jitter-ms")) jitter_ms = (uint32_t)v;
        else if (!strcmp(argv[i], "--reorder")) allow_reorder = (int)(v != 0);
        else if (!strcmp(argv[i], "--markov-drift")) markov_drift = (int)(v != 0);
        else if (!strcmp(argv[i], "--timeout-ms")) timeout = (uint32_t)v;
        else if (!strcmp(argv[i], "--stagger-ms")) fcfg[1].stagger_ms = (uint32_t)v;
        else if (!strcmp(argv[i], "--rttvar-k")) global_rttvar_q16 = (int64_t)v;
        else if (!strcmp(argv[i], "--cc0")) fcfg[0].cc = (int)v;
        else if (!strcmp(argv[i], "--budget0")) fcfg[0].budget_pacing = (int)(v != 0);
        else if (!strcmp(argv[i], "--ss-g0")) fcfg[0].ss_g_q16 = (uint32_t)v;
        else if (!strcmp(argv[i], "--ca-g0")) fcfg[0].ca_g_q16 = (uint32_t)v;
        else if (!strcmp(argv[i], "--start-ca0")) fcfg[0].start_ca = (int)(v != 0);
        else if (!strcmp(argv[i], "--cc1")) fcfg[1].cc = (int)v;
        else if (!strcmp(argv[i], "--budget1")) fcfg[1].budget_pacing = (int)(v != 0);
        else if (!strcmp(argv[i], "--ss-g1")) fcfg[1].ss_g_q16 = (uint32_t)v;
        else if (!strcmp(argv[i], "--ca-g1")) fcfg[1].ca_g_q16 = (uint32_t)v;
        else if (!strcmp(argv[i], "--start-ca1")) fcfg[1].start_ca = (int)(v != 0);
        else return 2;
    }

    if (global_rttvar_q16 >= 0) {
        for (i = 0; i < g_nflows; ++i) {
            if (fcfg[i].rttvar_q16 < 0)
                fcfg[i].rttvar_q16 = global_rttvar_q16;
        }
    }

    summary = fopen("summary_multi.json", "w");
    if (!summary) return 2;

    vnet_init(delay, rate, loss, seed);
    vnet_queue_limit(queue_packets);
    vnet_set_jitter(jitter_ms, allow_reorder);
    vnet_set_markov_drift(markov_drift);
    vnet_observe(observe);

    if (sscp_init(32)) return 2;

    for (i = 0; i < g_nflows; ++i) {
        if (fcfg[i].stagger_ms == 0) {
            if (start_flow(i, a, b, &ta, &tb, &fcfg[i]))
                return 2;
            started[i] = 1;
        }
    }

    while (steps < (uint64_t)timeout * SSCP_TICK_SCALE) {
        int step_err = 0;
        int all_started = 1, all_established = 1, all_done = 1, any_done = 0;

        ++steps;
        for (i = 0; i < g_nflows; ++i) {
            if (!started[i] && steps >= (uint64_t)fcfg[i].stagger_ms * SSCP_TICK_SCALE) {
                if (start_flow(i, a, b, &ta, &tb, &fcfg[i])) {
                    step_err = 1;
                    break;
                }
                started[i] = 1;
            }
        }
        if (step_err || vnet_step())
            break;

        for (i = 0; i < g_nflows; ++i) {
            if (!started[i]) {
                all_started = 0;
                all_established = 0;
                all_done = 0;
                continue;
            }
            if (transfer_step(&a[i]) || transfer_step(&b[i])) {
                step_err = 1;
                break;
            }
            if (!done[i] && a[i].done && b[i].done) {
                done[i] = 1;
                fct_ticks[i] = steps;
            }
            if (!a[i].established)
                all_established = 0;
            if (!done[i])
                all_done = 0;
            else
                any_done = 1;
        }
        if (step_err)
            break;

        if (!overlap_started && all_started && all_established) {
            overlap_started = 1;
            overlap_start_ticks = steps;
            for (i = 0; i < g_nflows; ++i)
                rx_at_start[i] = b[i].received;
        }
        if (overlap_started && !overlap_ended && any_done) {
            overlap_ended = 1;
            overlap_end_ticks = steps;
            for (i = 0; i < g_nflows; ++i)
                rx_at_end[i] = b[i].received;
        }
        if (all_done) {
            ok = 1;
            break;
        }
    }

    double ov_rx[MAX_FLOWS] = {0};
    double share[MAX_FLOWS] = {0};
    double mbps[MAX_FLOWS] = {0};
    double ov_sum = 0.0, ov_sq_sum = 0.0;
    double ov_ms = overlap_ended ? ((double)(overlap_end_ticks - overlap_start_ticks) / SSCP_TICK_SCALE) : 0.0;
    double min_share = 1.0, max_share = 0.0;

    for (i = 0; i < g_nflows; ++i) {
        ov_rx[i] = overlap_ended ? (double)(rx_at_end[i] - rx_at_start[i]) : 0.0;
        ov_sum += ov_rx[i];
        ov_sq_sum += ov_rx[i] * ov_rx[i];
    }
    for (i = 0; i < g_nflows; ++i) {
        share[i] = (ov_sum > 0.0) ? (ov_rx[i] / ov_sum) : 0.0;
        mbps[i] = (ov_ms > 0.0) ? (ov_rx[i] * 8.0 / (ov_ms * 1000.0)) : 0.0;
        if (share[i] < min_share) min_share = share[i];
        if (share[i] > max_share) max_share = share[i];
    }
    if (!overlap_ended || ov_sum <= 0.0) min_share = 0.0;
    double jain = (ov_sum > 0.0 && ov_sq_sum > 0.0) ? ((ov_sum * ov_sum) / ((double)g_nflows * ov_sq_sum)) : 0.0;

    int checksum_ok = ok;
    for (i = 0; i < g_nflows; ++i) {
        if (!started[i] ||
            a[i].sent != fcfg[i].bytes ||
            b[i].received != fcfg[i].bytes ||
            a[i].hash != b[i].hash ||
            a[i].peer_hash != a[i].hash) {
            checksum_ok = 0;
        }
    }

    for (i = 0; i < g_nflows; ++i) {
        if (started[i]) {
            transfer_cleanup(&a[i]);
            transfer_cleanup(&b[i]);
        }
    }
    int cleanup_ok = !core_live && !sscp_timer_tree.count && list_empty(&sscp_stream_queue);
    ok = ok && checksum_ok && cleanup_ok;
    struct vnet_stats vstats;
    vnet_get_stats(0, &vstats);

    fprintf(summary,
        "{\"success\":%s,\"nflows\":%d,\"mbps\":%u,\"delay_ms\":%u,\"queue_packets\":%u,"
        "\"loss_ppm\":%u,\"jitter_ms\":%u,\"overlap_ms\":%.2f,"
        "\"share0\":%.4f,\"share1\":%.4f,\"min_share\":%.4f,\"max_share\":%.4f,\"jain\":%.4f,"
        "\"fct0_ms\":%.2f,\"fct1_ms\":%.2f,"
        "\"retrans0\":%" PRIu64 ",\"retrans1\":%" PRIu64 ","
        "\"timeouts0\":%" PRIu64 ",\"timeouts1\":%" PRIu64 ","
        "\"queue_dropped\":%" PRIu64 ",\"packets\":%" PRIu64 ","
        "\"q_max\":%u,\"q_avg\":%.3f,\"q_p95\":%.1f,\"q_p99\":%.1f,"
        "\"qdelay_avg_ms\":%.4f,\"qdelay_p95_ms\":%.4f,\"qdelay_p99_ms\":%.4f,\"qdelay_max_ms\":%.4f,"
        "\"owd_p99_ms\":%.4f,",
        ok ? "true" : "false", g_nflows, rate, delay, queue_packets, loss, jitter_ms, ov_ms,
        share[0], (g_nflows > 1 ? share[1] : 0.0), min_share, max_share, jain,
        (double)fct_ticks[0] / SSCP_TICK_SCALE,
        (g_nflows > 1 ? (double)fct_ticks[1] / SSCP_TICK_SCALE : 0.0),
        retrans_flow[0], (g_nflows > 1 ? retrans_flow[1] : 0),
        timeouts_fd[1] + timeouts_fd[2], (g_nflows > 1 ? timeouts_fd[3] + timeouts_fd[4] : 0),
        vnet_queue_dropped, vnet_packets,
        vstats.q_max, vstats.q_avg, vstats.q_p95, vstats.q_p99,
        vstats.qdelay_avg_ms, vstats.qdelay_p95_ms, vstats.qdelay_p99_ms, vstats.qdelay_max_ms,
        vstats.owd_p99_ms);

    fprintf(summary, "\"shares\":[");
    for (i = 0; i < g_nflows; ++i) fprintf(summary, "%s%.4f", i ? "," : "", share[i]);
    fprintf(summary, "],\"fcts_ms\":[");
    for (i = 0; i < g_nflows; ++i) fprintf(summary, "%s%.2f", i ? "," : "", (double)fct_ticks[i] / SSCP_TICK_SCALE);
    fprintf(summary, "],\"ov_mbps\":[");
    for (i = 0; i < g_nflows; ++i) fprintf(summary, "%s%.3f", i ? "," : "", mbps[i]);
    fprintf(summary, "],\"retrans\":[");
    for (i = 0; i < g_nflows; ++i) fprintf(summary, "%s%" PRIu64, i ? "," : "", retrans_flow[i]);
    fprintf(summary, "],\"timeouts\":[");
    for (i = 0; i < g_nflows; ++i) fprintf(summary, "%s%" PRIu64, i ? "," : "", timeouts_fd[2 * i + 1] + timeouts_fd[2 * i + 2]);
    fprintf(summary, "]}\n");

    fclose(summary);
    vnet_free();
    printf("MULTIFLOW(n=%d) %s: jain=%.4f min=%.4f max=%.4f fct0=%.1f qdrop=%" PRIu64 "\n",
           g_nflows, ok ? "PASS" : "FAIL", jain, min_share, max_share,
           (double)fct_ticks[0] / SSCP_TICK_SCALE, vnet_queue_dropped);
    return ok ? 0 : 1;
}
