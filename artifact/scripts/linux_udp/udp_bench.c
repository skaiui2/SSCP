#define _GNU_SOURCE
#include "sscp.h"
#include "transfer.h"
#include "../../../sscp.c"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static struct timespec g_t0;
static int g_t0_init = 0;

uint32_t sscp_now_time(void)
{
    struct timespec ts;
    int64_t diff_us;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (!g_t0_init) {
        g_t0 = ts;
        g_t0_init = 1;
        return 0;
    }
    diff_us = (int64_t)(ts.tv_sec - g_t0.tv_sec) * 1000000LL +
              ((int64_t)ts.tv_nsec - (int64_t)g_t0.tv_nsec) / 1000LL;
    if (diff_us < 0) diff_us = 0;
#if SSCP_TICK_SCALE == 1000U
    return (uint32_t)diff_us;
#else
    return (uint32_t)(diff_us / 1000ULL);
#endif
}

struct bench_link {
    int sock;
    struct sockaddr_in peer;
    int peer_ready;
    uint64_t tx_data_pkts;
    uint64_t tx_data_bytes;
    uint64_t tx_retrans;
    uint32_t high_seq;
    int high_seen;
    uint32_t *tx_ts;
    size_t tx_cnt, tx_cap;
    uint32_t *rtt_arr;
    size_t rtt_cnt, rtt_cap;
    uint64_t timeouts;
    void (*orig_timeout)(struct sscp_stream *);
};

static struct bench_link *g_link_ptr = NULL;

static void count_timeout_cb(struct sscp_stream *ss)
{
    if (g_link_ptr) {
        ++g_link_ptr->timeouts;
        if (g_link_ptr->orig_timeout)
            g_link_ptr->orig_timeout(ss);
    }
}

static void rec_u32(uint32_t **arr, size_t *cnt, size_t *cap, uint32_t v)
{
    if (*cnt == *cap) {
        size_t nc = *cap ? (*cap * 2) : 4096;
        uint32_t *p = realloc(*arr, nc * sizeof(uint32_t));
        if (!p) return;
        *arr = p;
        *cap = nc;
    }
    (*arr)[(*cnt)++] = v;
}

static int bench_send(void *user, const void *buf, size_t len)
{
    struct bench_link *lk = user;
    const struct sscp_hdr *h = buf;
    ssize_t n;
    if (!lk->peer_ready) return -1;
    n = sendto(lk->sock, buf, len, 0, (struct sockaddr *)&lk->peer, sizeof(lk->peer));
    if (n == (ssize_t)len && len >= sizeof(*h) && (h->flags & SSCP_FLAG_DATA)) {
        uint32_t seq = ntohl(h->seq);
        uint32_t now = sscp_now_time();
        ++lk->tx_data_pkts;
        lk->tx_data_bytes += ntohs(h->len);
        if (!lk->high_seen) {
            lk->high_seq = seq + ntohs(h->len);
            lk->high_seen = 1;
        } else if (SEQ_LT(seq, lk->high_seq)) {
            ++lk->tx_retrans;
        } else {
            lk->high_seq = seq + ntohs(h->len);
        }
        rec_u32(&lk->tx_ts, &lk->tx_cnt, &lk->tx_cap, now);
    }
    return (int)n;
}

static int bench_poll_recv(struct bench_link *lk, struct transfer *app)
{
    for (;;) {
        unsigned char pkt[2048];
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(lk->sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&src, &slen);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (!lk->peer_ready) {
            lk->peer = src;
            lk->peer_ready = 1;
        }
        if (src.sin_addr.s_addr != lk->peer.sin_addr.s_addr || src.sin_port != lk->peer.sin_port)
            continue;
        if (transfer_input(app, pkt, (size_t)n) < 0)
            return -1;
        if (app->sender && app->stream && app->stream->rtt_sample) {
            const struct sscp_hdr *h = (const struct sscp_hdr *)pkt;
            if ((size_t)n >= sizeof(*h) && (h->flags & SSCP_FLAG_ACK))
                rec_u32(&lk->rtt_arr, &lk->rtt_cnt, &lk->rtt_cap, app->stream->rtt_sample);
        }
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void compute_metrics(struct bench_link *lk,
                            double *rtt_p50, double *rtt_p95, double *rtt_p99, double *rtt_max,
                            double *burst50, double *cv500)
{
    *rtt_p50 = *rtt_p95 = *rtt_p99 = *rtt_max = 0.0;
    if (lk->rtt_cnt > 0) {
        qsort(lk->rtt_arr, lk->rtt_cnt, sizeof(uint32_t), cmp_u32);
        *rtt_p50 = (double)lk->rtt_arr[(size_t)(0.50 * (lk->rtt_cnt - 1) + 0.5)] / SSCP_TICK_SCALE;
        *rtt_p95 = (double)lk->rtt_arr[(size_t)(0.95 * (lk->rtt_cnt - 1) + 0.5)] / SSCP_TICK_SCALE;
        *rtt_p99 = (double)lk->rtt_arr[(size_t)(0.99 * (lk->rtt_cnt - 1) + 0.5)] / SSCP_TICK_SCALE;
        *rtt_max = (double)lk->rtt_arr[lk->rtt_cnt - 1] / SSCP_TICK_SCALE;
    }
    *burst50 = 0.0;
    *cv500 = 0.0;
    if (lk->tx_cnt > 10) {
        size_t n = lk->tx_cnt - 1, b50 = 0, start500 = n > 500 ? n - 500 : 0;
        double sum = 0.0, sq = 0.0;
        for (size_t k = 0; k < n; ++k) {
            double g = (double)(lk->tx_ts[k + 1] - lk->tx_ts[k]) / SSCP_TICK_SCALE;
            if (g < 0.050) ++b50;
            if (k >= start500) { sum += g; sq += g * g; }
        }
        *burst50 = 100.0 * (double)b50 / (double)n;
        double m = sum / (double)(n - start500);
        double var = sq / (double)(n - start500) - m * m;
        *cv500 = m > 0 ? sqrt(var > 0 ? var : 0) / m : 0.0;
    }
}

static uint32_t next_wait_us(uint32_t now)
{
    struct rb_node *first = rb_first(&sscp_timer_tree);
    if (!first) return 5000;
    struct sscp_timer *t = container_of(first, struct sscp_timer, node);
    if (!SEQ_LT(now, t->expire)) return 0;
    uint32_t diff = t->expire - now;
#if SSCP_TICK_SCALE == 1000U
    return diff > 5000 ? 5000 : diff;
#else
    return diff * 1000U > 5000 ? 5000 : diff * 1000U;
#endif
}

int main(int argc, char **argv)
{
    int sender = 1;
    const char *peer_ip = "127.0.0.1";
    uint16_t port = 6000;
    uint64_t bytes = UINT64_C(8388608);
    uint32_t seconds = 0, warmup_s = 0, spin_us = 0;
    int cc = SSCP_CC_AIMD, budget = 1, start_ca = 0;
    uint32_t ss_g = 0, ca_g = 0;
    int64_t rttvar_k = -1;
    int trace = 0;
    int i;

    for (i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) return 2;
        if (!strcmp(argv[i], "--role")) sender = !strcmp(argv[i + 1], "A");
        else if (!strcmp(argv[i], "--peer")) peer_ip = argv[i + 1];
        else if (!strcmp(argv[i], "--port")) port = (uint16_t)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--bytes")) bytes = strtoull(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--seconds")) seconds = (uint32_t)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--warmup")) warmup_s = (uint32_t)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--cc")) cc = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--budget")) budget = atoi(argv[i + 1]) != 0;
        else if (!strcmp(argv[i], "--ss-g")) ss_g = (uint32_t)strtoul(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--ca-g")) ca_g = (uint32_t)strtoul(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--rttvar-k")) rttvar_k = strtoll(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--spin-us")) spin_us = (uint32_t)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--start-ca")) start_ca = atoi(argv[i + 1]) != 0;
        else if (!strcmp(argv[i], "--trace")) trace = atoi(argv[i + 1]);
        else return 2;
    }

    if (spin_us > 0) prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
    setvbuf(stdout, NULL, _IONBF, 0); sscp_now_time();
    if (sscp_init(16)) return 1;

    struct bench_link lk = {0};
    lk.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (lk.sock < 0) { perror("socket"); return 1; }
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(lk.sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(lk.sock, SOL_SOCKET, SO_SNDBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in loc = {0};
    loc.sin_family = AF_INET;
    loc.sin_addr.s_addr = htonl(INADDR_ANY);
    loc.sin_port = htons(sender ? 0 : port);
    if (bind(lk.sock, (struct sockaddr *)&loc, sizeof(loc)) < 0) { perror("bind"); return 1; }
    int fl = fcntl(lk.sock, F_GETFL, 0);
    fcntl(lk.sock, F_SETFL, fl | O_NONBLOCK);

    if (sender) {
        lk.peer.sin_family = AF_INET;
        lk.peer.sin_port = htons(port);
        if (inet_pton(AF_INET, peer_ip, &lk.peer.sin_addr) != 1) { fprintf(stderr, "bad peer\n"); return 2; }
        lk.peer_ready = 1;
    }

    struct sscp_transport_class tr = { .send = bench_send, .user = &lk };
    struct transfer app = {0};
    uint64_t target_bytes = seconds ? UINT64_C(1099511627776) : bytes;
    if (transfer_init(&app, &tr, sender ? 1 : 2, sender ? 2 : 1, sender, target_bytes, cc)) return 1;
    app.send_backlog_bytes = (2U << 20);

    app.stream->budget_pacing = (uint8_t)budget;
    if (start_ca) { app.stream->cwnd = MSS; app.stream->ssthresh = MSS; }
    if (ss_g) app.stream->static_ss_gain_q16 = ss_g;
    if (ca_g) app.stream->static_ca_gain_q16 = ca_g;
    if (rttvar_k >= 0) app.stream->pacing_rttvar_q16 = (uint32_t)rttvar_k;
    g_link_ptr = &lk;
    lk.orig_timeout = app.stream->cc_on_timeout;
    app.stream->cc_on_timeout = count_timeout_cb;

    uint32_t est_tick = 0;
    int est_seen = 0, warmup_done = 0;
    uint64_t warm_pkts = 0, warm_retr = 0, warm_to = 0, win_acked = 0;
    uint32_t warm_tick = 0, prev_una = app.stream->snd_una, next_trace = 0;
    uint64_t total_limit_ticks = (seconds ? (uint64_t)(seconds + 25) : 180U) * 1000ULL * SSCP_TICK_SCALE;

    while (1) {
        uint32_t now = sscp_now_time();
        if (now >= total_limit_ticks) break;

        sscp_timer_process();
        int prc = bench_poll_recv(&lk, &app);
        if (prc < 0) { fprintf(stderr, "poll_recv=%d errno=%d\n", prc, errno); break; }
        if (transfer_step(&app) < 0) { fprintf(stderr, "tstep fail\n"); break; }

        now = sscp_now_time();
        if (!est_seen && app.stream->state == SSCP_ESTABLISHED) {
            est_seen = 1;
            est_tick = now;
            prev_una = app.stream->snd_una;
            next_trace = now;
            if (!warmup_s) {
                warmup_done = 1;
                warm_tick = now;
            }
        }
        if (trace && sender && est_seen && !SEQ_LT(now, next_trace)) {
            struct sscp_stream *s = app.stream;
            fprintf(stderr, "T=%u cwnd=%u ss=%u srtt=%u var=%u fl=%u gain=%.3f sh=%u fr=%u dup=%u retr=%llu\n",
                    (unsigned)((now - est_tick) / SSCP_TICK_SCALE), s->cwnd, s->ssthresh,
                    s->srtt, s->rttvar, s->snd_nxt - s->snd_una,
                    (double)s->pacing_gain_q16 / 65536.0, s->budget_pacing, s->fr_active, s->dup_acks,
                    (unsigned long long)lk.tx_retrans);
            next_trace = now + 100U * SSCP_TICK_SCALE;
        }
        if (sender && est_seen) {
            uint32_t cur_una = app.stream->snd_una;
            uint32_t d_una = cur_una - prev_una;
            prev_una = cur_una;
            if (warmup_done) win_acked += d_una;
        }

        if (seconds) {
            if (est_seen && !warmup_done &&
                (uint64_t)(now - est_tick) >= (uint64_t)warmup_s * 1000ULL * SSCP_TICK_SCALE) {
                warmup_done = 1;
                warm_tick = now;
                win_acked = 0;
                warm_pkts = lk.tx_data_pkts;
                warm_retr = lk.tx_retrans;
                warm_to = lk.timeouts;
                lk.rtt_cnt = 0;
                lk.tx_cnt = 0;
            }
            uint64_t target_dur = (uint64_t)(sender ? seconds : (seconds + 2)) * 1000ULL * SSCP_TICK_SCALE;
            if (est_seen && (uint64_t)(now - est_tick) >= target_dur) {
                double dur_ms = (double)(now - warm_tick) / SSCP_TICK_SCALE;
                double mbps = dur_ms > 0 ? ((double)win_acked * 8.0 / (dur_ms * 1000.0)) : 0.0;
                double rtt_p50, rtt_p95, rtt_p99, rtt_max, burst50, cv500;
                compute_metrics(&lk, &rtt_p50, &rtt_p95, &rtt_p99, &rtt_max, &burst50, &cv500);
                if (sender) {
                    printf("{\"success\":true,\"mode\":\"seconds\",\"cc\":%d,\"budget\":%d,\"duration_ms\":%.3f,"
                           "\"acked_bytes\":%" PRIu64 ",\"goodput_mbps\":%.4f,"
                           "\"data_pkts\":%" PRIu64 ",\"retrans\":%" PRIu64 ",\"timeouts\":%" PRIu64 ","
                           "\"burst50_pct\":%.3f,\"cv500\":%.4f,"
                           "\"rtt_p50_ms\":%.3f,\"rtt_p95_ms\":%.3f,\"rtt_p99_ms\":%.3f,\"rtt_max_ms\":%.3f}\n",
                           cc, budget, dur_ms, win_acked, mbps,
                           lk.tx_data_pkts - warm_pkts, lk.tx_retrans - warm_retr, lk.timeouts - warm_to,
                           burst50, cv500, rtt_p50, rtt_p95, rtt_p99, rtt_max);
                } else {
                    printf("{\"success\":true,\"role\":\"B\",\"received\":%" PRIu64 "}\n", app.received);
                }
                goto ok_exit;
            }
        } else if (app.done) {
            double fct_ms = (double)(now - est_tick) / SSCP_TICK_SCALE;
            double total_ms = (double)now / SSCP_TICK_SCALE;
            double rtt_p50, rtt_p95, rtt_p99, rtt_max, burst50, cv500;
            compute_metrics(&lk, &rtt_p50, &rtt_p95, &rtt_p99, &rtt_max, &burst50, &cv500);
            if (sender) {
                double mbps = fct_ms > 0 ? ((double)bytes * 8.0 / (fct_ms * 1000.0)) : 0.0;
                printf("{\"success\":true,\"mode\":\"bytes\",\"cc\":%d,\"budget\":%d,\"bytes\":%" PRIu64 ","
                       "\"fct_ms\":%.3f,\"total_ms\":%.3f,\"goodput_mbps\":%.4f,"
                       "\"data_pkts\":%" PRIu64 ",\"retrans\":%" PRIu64 ",\"timeouts\":%" PRIu64 ","
                       "\"burst50_pct\":%.3f,\"cv500\":%.4f,"
                       "\"rtt_p50_ms\":%.3f,\"rtt_p95_ms\":%.3f,\"rtt_p99_ms\":%.3f,\"rtt_max_ms\":%.3f}\n",
                       cc, budget, bytes, fct_ms, total_ms, mbps,
                       lk.tx_data_pkts, lk.tx_retrans, lk.timeouts,
                       burst50, cv500, rtt_p50, rtt_p95, rtt_p99, rtt_max);
            } else {
                printf("{\"success\":true,\"role\":\"B\",\"received\":%" PRIu64 ",\"hash\":\"%016" PRIx64 "\"}\n",
                       app.received, app.hash);
            }
            goto ok_exit;
        }

        uint32_t wus = next_wait_us(sscp_now_time());
        if (wus > spin_us + 15U) {
            uint32_t sleep_us = wus - spin_us;
            struct pollfd pfd = { .fd = lk.sock, .events = POLLIN };
            struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)sleep_us * 1000L };
            ppoll(&pfd, 1, &ts, NULL);
        }
    }
    fprintf(stderr, "FAILED\n");
    transfer_cleanup(&app);
    close(lk.sock);
    free(lk.tx_ts);
    free(lk.rtt_arr);
    return 1;
ok_exit:
    transfer_cleanup(&app);
    close(lk.sock);
    free(lk.tx_ts);
    free(lk.rtt_arr);
    return 0;
}