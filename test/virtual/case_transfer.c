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

static uint64_t retrans_packets, timeouts[2];
static uint64_t srtt_samples, srtt_sum;
static uint32_t srtt_max, highest_seq[2];
static uint32_t *rtt_arr = NULL;
static size_t rtt_cnt = 0, rtt_cap = 0;
static int cmp_u32_ct(const void *pa, const void *pb) { uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb; return (a > b) - (a < b); }
static uint64_t wire_bytes[2];
static int data_seen[2], sequence_ok = 1;
static void (*timeout_handler[2])(struct sscp_stream *);

static FILE *xpl_fp = NULL;
static FILE *csv_fp = NULL;
static int64_t xpl_first_tick = -1;
static uint32_t xpl_base_seq = 0;
static uint32_t xpl_prev_ack = 0;
static double xpl_prev_t = 0.0;
static int xpl_has_prev_ack = 0;

static void count_timeout(struct sscp_stream *ss)
{
    ++timeouts[ss->src_fd - 1];
    timeout_handler[ss->src_fd - 1](ss);
}

static void observe(unsigned direction, int event, const void *buf, size_t len, uint32_t now)
{
    const struct sscp_hdr *h = buf;
    struct sscp_stream *s;
    uint32_t seq, bytes;
    if (len < sizeof(*h)) { sequence_ok = 0; return; }
    bytes = ntohs(h->len);
    seq = ntohl(h->seq);
    if (event == VNET_SEND && (h->flags & SSCP_FLAG_DATA)) {
        if (!data_seen[direction]) {
            highest_seq[direction] = seq;
            data_seen[direction] = 1;
        }
        if (len != sizeof(*h) + bytes || !bytes) sequence_ok = 0;
        if (SEQ_LT(seq, highest_seq[direction])) ++retrans_packets;
        else {
            if (seq != highest_seq[direction]) sequence_ok = 0;
            highest_seq[direction] = seq + bytes;
            wire_bytes[direction] += bytes;
        }
        if (direction == 0 && bytes > 0) {
            if (xpl_first_tick < 0) {
                xpl_first_tick = (int64_t)now;
                xpl_base_seq = seq;
            }
            if (xpl_fp) {
                double t = (double)(now - (uint32_t)xpl_first_tick) / ((double)SSCP_TICK_SCALE * 1000.0);
                uint32_t start = seq - xpl_base_seq;
                uint32_t end = start + bytes;
                fprintf(xpl_fp, "white\ndarrow %.12f %u\nuarrow %.12f %u\nline %.12f %u %.12f %u\n",
                        t, start, t, end, t, start, t, end);
            }
            if (csv_fp) {
                fprintf(csv_fp, "0,send,%u,%u,%u,0,%" PRIu32 ",%.3f\n",
                        h->flags, bytes, seq, now, (double)now / SSCP_TICK_SCALE);
            }
        }
    }
    if (direction == 1 && event == VNET_INPUT_RETURNED && (h->flags & SSCP_FLAG_ACK)) {
        if (xpl_first_tick >= 0) {
            double t = (double)(now - (uint32_t)xpl_first_tick) / ((double)SSCP_TICK_SCALE * 1000.0);
            uint32_t ack = ntohl(h->ack) - xpl_base_seq;
            if (xpl_fp) {
                fprintf(xpl_fp, "green\n");
                if (!xpl_has_prev_ack) {
                    fprintf(xpl_fp, "dot %.12f %u\n", t, ack);
                } else {
                    fprintf(xpl_fp, "line %.12f %u %.12f %u\n", xpl_prev_t, xpl_prev_ack, t, xpl_prev_ack);
                    if (ack != xpl_prev_ack) {
                        fprintf(xpl_fp, "line %.12f %u %.12f %u\n", t, xpl_prev_ack, t, ack);
                    }
                }
                xpl_prev_ack = ack;
                xpl_prev_t = t;
                xpl_has_prev_ack = 1;
            }
            if (csv_fp) {
                fprintf(csv_fp, "1,arrival,%u,0,0,%u,%" PRIu32 ",%.3f\n",
                        h->flags, ntohl(h->ack), now, (double)now / SSCP_TICK_SCALE);
            }
        }
        s = hashmap_get(&sscp_stream_map, (void *)(uintptr_t)1);
        if (s && s->srtt) {
            srtt_sum += s->srtt;
            ++srtt_samples;
            if (s->srtt > srtt_max) srtt_max = s->srtt;
            if (s->rtt_sample) {
                if (rtt_cnt == rtt_cap) {
                    size_t nc = rtt_cap ? rtt_cap * 2 : 1024;
                    uint32_t *nr = realloc(rtt_arr, nc * sizeof(uint32_t));
                    if (nr) { rtt_arr = nr; rtt_cap = nc; }
                }
                if (rtt_cnt < rtt_cap) rtt_arr[rtt_cnt++] = s->rtt_sample;
            }
        }
    }
}

static void check_sequences(void)
{
    struct list_node *node;
    struct sscp_stream *s;
    uint32_t end;
    for (node = sscp_stream_queue.next; node != &sscp_stream_queue; node = node->next) {
        s = container_of(node, struct sscp_stream, node);
        end = s->snd_seq_q;
        if (s->state == SSCP_FIN_WAIT || s->state == SSCP_LAST_ACK) ++end;
        if (SEQ_GT(s->snd_una, s->snd_nxt) || SEQ_GT(s->snd_nxt, end)) sequence_ok = 0;
    }
}
extern uint32_t sscp_now_time(void);
int main(int argc, char **argv)
{
    uint64_t total = UINT64_C(1073741824), sent = 0, received = 0, steps = 0;
    uint64_t hash_a = UINT64_C(14695981039346656037), hash_b = hash_a;
    uint32_t delay = 10, rate = 100, loss = 0, seed = 1, timeout = 36000000;
    uint32_t backlog = TRANSFER_SEND_BACKLOG_BYTES;
    uint32_t queue_packets = 0;
    uint32_t jitter_ms = 0;
    int allow_reorder = 0;
    int markov_drift = 0;
    double srtt_avg;
    int checksum_ok, cleanup_ok;
    struct vnet_endpoint ep_a = {0}, ep_b = {1};
    struct sscp_transport_class ta = {.send = vnet_send, .user = &ep_a};
    struct sscp_transport_class tb = {.send = vnet_send, .user = &ep_b};
    struct transfer a, b;
    FILE *summary;
    const char *xpl_path = NULL;
    const char *csv_path = NULL;
    int start_ca = 0;
    int budget_pacing = -1;
    uint32_t init_g_q16 = 0;
    uint32_t static_ss_g_q16 = 0;
    uint32_t static_ca_g_q16 = 0;
    int64_t pacing_rttvar_q16 = -1;
    int i, ok = 0, cc = SSCP_CC_AIMD;
    for (i = 1; i < argc; i += 2) {
        uint64_t value;
        char *end;
        if (i + 1 >= argc) return 2;
        if (!strcmp(argv[i], "--xpl")) {
            xpl_path = argv[i + 1];
            continue;
        }
        if (!strcmp(argv[i], "--events-csv")) {
            csv_path = argv[i + 1];
            continue;
        }
        value = strtoull(argv[i + 1], &end, 10);
        if (*end) return 2;
        if (!strcmp(argv[i], "--bytes")) total = value;
        else if (!strcmp(argv[i], "--delay-ms")) delay = (uint32_t)value;
        else if (!strcmp(argv[i], "--mbps")) rate = (uint32_t)value;
        else if (!strcmp(argv[i], "--loss-ppm")) loss = (uint32_t)value;
        else if (!strcmp(argv[i], "--seed")) seed = (uint32_t)value;
        else if (!strcmp(argv[i], "--queue-packets")) {
            if (value > UINT32_MAX) return 2;
            queue_packets = (uint32_t)value;
        }
        else if (!strcmp(argv[i], "--timeout-ms")) timeout = (uint32_t)value;
        else if (!strcmp(argv[i], "--cc")) cc = (int)value;
        else if (!strcmp(argv[i], "--jitter-ms")) jitter_ms = (uint32_t)value;
        else if (!strcmp(argv[i], "--reorder")) allow_reorder = (int)value;
        else if (!strcmp(argv[i], "--markov-drift")) markov_drift = (int)value;
        else if (!strcmp(argv[i], "--send-backlog-bytes")) {
            if (value < MSS || value > UINT32_MAX) return 2;
            backlog = (uint32_t)value;
        }
        else if (!strcmp(argv[i], "--start-ca")) start_ca = (int)value;
        else if (!strcmp(argv[i], "--init-g")) init_g_q16 = (uint32_t)value;
        else if (!strcmp(argv[i], "--budget-pacing")) budget_pacing = (int)(value != 0);
        else if (!strcmp(argv[i], "--ss-g")) static_ss_g_q16 = (uint32_t)value;
        else if (!strcmp(argv[i], "--ca-g")) static_ca_g_q16 = (uint32_t)value;
        else if (!strcmp(argv[i], "--rttvar-k")) pacing_rttvar_q16 = (int64_t)value;
        else return 2;
    }
    if (!total || loss > 1000000 || !timeout ||
        (cc != SSCP_CC_AIMD && cc != SSCP_CC_PROB && cc != SSCP_CC_CUBIC)) return 2;
    summary = fopen("summary.json", "w");
    if (!summary) return 2;
    vnet_init(delay, rate, loss, seed);
    vnet_queue_limit(queue_packets);
    vnet_set_jitter(jitter_ms, allow_reorder);
    vnet_set_markov_drift(markov_drift);
    vnet_observe(observe);
    if (xpl_path) {
        xpl_fp = fopen(xpl_path, "w");
        if (xpl_fp) {
            fprintf(xpl_fp, "double double\ntitle\nSSCP 128M Adaptive Bisection Pacing TSG\nxlabel\nTime (seconds)\nylabel\nSequence offset (bytes)\n");
        }
    }
    if (csv_path) {
        csv_fp = fopen(csv_path, "w");
        if (csv_fp) {
            fprintf(csv_fp, "direction,event,flags,payload_bytes,seq,ack,t_tick,t_ms\n");
        }
    }
    if (sscp_init(16)) return 2;
    if (transfer_init(&b, &tb, 2, 1, 0, total, cc) ||
        transfer_init(&a, &ta, 1, 2, 1, total, cc)) return 2;
    if (start_ca) {
        a.stream->cwnd = MSS;
        a.stream->ssthresh = MSS;
        b.stream->cwnd = MSS;
        b.stream->ssthresh = MSS;
    }
    if (budget_pacing >= 0) {
        a.stream->budget_pacing = (uint8_t)budget_pacing;
        b.stream->budget_pacing = (uint8_t)budget_pacing;
    }
    if (init_g_q16 > 0) {
        a.stream->budget_gain_q16 = init_g_q16;
        b.stream->budget_gain_q16 = init_g_q16;
    }
    if (static_ss_g_q16 > 0) {
        a.stream->static_ss_gain_q16 = static_ss_g_q16;
        b.stream->static_ss_gain_q16 = static_ss_g_q16;
    }
    if (static_ca_g_q16 > 0) {
        a.stream->static_ca_gain_q16 = static_ca_g_q16;
        b.stream->static_ca_gain_q16 = static_ca_g_q16;
    }
    if (pacing_rttvar_q16 >= 0) {
        a.stream->pacing_rttvar_q16 = (uint32_t)pacing_rttvar_q16;
        b.stream->pacing_rttvar_q16 = (uint32_t)pacing_rttvar_q16;
    }
    if (cc == SSCP_CC_PROB &&
        (sscp_prob_configure(a.stream, 100, 8000) || sscp_prob_configure(b.stream, 100, 8000))) return 2;
    vnet_attach(&a, &b);
    timeout_handler[0] = a.stream->cc_on_timeout;
    timeout_handler[1] = b.stream->cc_on_timeout;
    a.stream->cc_on_timeout = count_timeout;
    b.stream->cc_on_timeout = count_timeout;
    a.send_backlog_bytes = backlog;
    printf("Generated transfer: %" PRIu64 " bytes, %s, %u Mbps/direction, %u ms one-way, %u ppm loss, seed %u\n",
        total, rate ? "serialized link" : "fixed delay without serialization", rate, delay, loss, seed);
    while (steps < (uint64_t)timeout * SSCP_TICK_SCALE) {
        ++steps;
        if (vnet_step() || transfer_step(&a) || transfer_step(&b)) break;
        check_sequences();
        if (!sequence_ok) break;
        if (a.done && b.done) { ok = 1; break; }
    }
    sent = a.sent; received = b.received; hash_a = a.hash; hash_b = b.hash;
    checksum_ok = sent == total && received == total && hash_a == hash_b && a.peer_hash == hash_a;
    if (ok && (wire_bytes[0] != total || wire_bytes[1] != sizeof(a.reply))) sequence_ok = 0;
    transfer_cleanup(&a); transfer_cleanup(&b);
    cleanup_ok = !core_live && !sscp_timer_tree.count && list_empty(&sscp_stream_queue);
    ok = ok && checksum_ok && sequence_ok && cleanup_ok;
    srtt_avg = srtt_samples ? (double)srtt_sum / srtt_samples / SSCP_TICK_SCALE : 0;
    struct vnet_stats vstats;
    vnet_get_stats(0, &vstats);
    double rtt_p50 = 0.0, rtt_p95 = 0.0, rtt_p99 = 0.0, rtt_pmax = 0.0;
    if (rtt_cnt > 0) {
        qsort(rtt_arr, rtt_cnt, sizeof(uint32_t), cmp_u32_ct);
        rtt_p50 = (double)rtt_arr[(size_t)(0.50 * (rtt_cnt - 1) + 0.5)] / SSCP_TICK_SCALE;
        rtt_p95 = (double)rtt_arr[(size_t)(0.95 * (rtt_cnt - 1) + 0.5)] / SSCP_TICK_SCALE;
        rtt_p99 = (double)rtt_arr[(size_t)(0.99 * (rtt_cnt - 1) + 0.5)] / SSCP_TICK_SCALE;
        rtt_pmax = (double)rtt_arr[rtt_cnt - 1] / SSCP_TICK_SCALE;
    }
    free(rtt_arr); rtt_arr = NULL; rtt_cnt = rtt_cap = 0;
    fprintf(summary, "{\"success\":%s,\"requested_bytes\":%" PRIu64 ",\"accepted_bytes\":%" PRIu64 ",\"received_bytes\":%" PRIu64 ",\"checksum_algorithm\":\"FNV-1a-64\",\"checksum_a\":\"%016" PRIx64 "\",\"checksum_b\":\"%016" PRIx64 "\",\"simulated_ms\":%.3f,\"simulated_ticks\":%" PRIu64 ",\"tick_scale\":%u,\"cc\":%d,\"mbps\":%u,\"delay_ms\":%u,\"loss_ppm\":%u,\"seed\":%u,\"transport\":\"%s\",\"send_backlog_bytes\":%u,\"packets\":%" PRIu64 ",\"dropped\":%" PRIu64, ok ? "true" : "false", total, sent, received, hash_a, hash_b, (double)steps / SSCP_TICK_SCALE, steps, SSCP_TICK_SCALE, cc, rate, delay, loss, seed, rate ? "serialized_link" : "fixed_delay", backlog, vnet_packets, vnet_dropped);
    fprintf(summary, ",\"queue_packets\":%u,\"queue_dropped\":%" PRIu64
        ",\"checksum_ok\":%s,\"sequence_ok\":%s,\"cleanup_ok\":%s"
        ",\"core_live_allocations\":%" PRId64 ",\"active_timers\":%u"
        ",\"timeouts\":%" PRIu64 ",\"retrans_packets\":%" PRIu64
        ",\"timeouts_a\":%" PRIu64 ",\"timeouts_b\":%" PRIu64
        ",\"srtt_max_ms\":%.6f,\"srtt_avg_ms\":%.6f"
        ",\"q_max\":%u,\"q_avg\":%.3f,\"q_p95\":%.1f,\"q_p99\":%.1f"
        ",\"qdelay_avg_ms\":%.4f,\"qdelay_p50_ms\":%.4f,\"qdelay_p95_ms\":%.4f,\"qdelay_p99_ms\":%.4f,\"qdelay_max_ms\":%.4f"
        ",\"owd_p50_ms\":%.4f,\"owd_p95_ms\":%.4f,\"owd_p99_ms\":%.4f,\"owd_max_ms\":%.4f"
        ",\"rtt_p50_ms\":%.4f,\"rtt_p95_ms\":%.4f,\"rtt_p99_ms\":%.4f,\"rtt_max_ms\":%.4f}\n",
        queue_packets, vnet_queue_dropped, checksum_ok ? "true" : "false",
        sequence_ok ? "true" : "false", cleanup_ok ? "true" : "false",
        core_live, sscp_timer_tree.count, timeouts[0] + timeouts[1], retrans_packets,
        timeouts[0], timeouts[1],
        (double)srtt_max / SSCP_TICK_SCALE, srtt_avg,
        vstats.q_max, vstats.q_avg, vstats.q_p95, vstats.q_p99,
        vstats.qdelay_avg_ms, vstats.qdelay_p50_ms, vstats.qdelay_p95_ms, vstats.qdelay_p99_ms, vstats.qdelay_max_ms,
        vstats.owd_p50_ms, vstats.owd_p95_ms, vstats.owd_p99_ms, vstats.owd_max_ms,
        rtt_p50, rtt_p95, rtt_p99, rtt_pmax);
    printf("%s: received=%" PRIu64 " A=%016" PRIx64 " B=%016" PRIx64 " virtual_ms=%.3f dropped=%" PRIu64 "\n", ok ? "PASS" : "FAIL", received, hash_a, hash_b, (double)steps / SSCP_TICK_SCALE, vnet_dropped);
    fclose(summary);
    if (xpl_fp) {
        fprintf(xpl_fp, "go\n");
        fclose(xpl_fp);
        xpl_fp = NULL;
    }
    if (csv_fp) {
        fclose(csv_fp);
        csv_fp = NULL;
    }
    vnet_free();
    return ok ? 0 : 1;
}
