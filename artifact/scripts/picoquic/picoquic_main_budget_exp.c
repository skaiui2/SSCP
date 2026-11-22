#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "cc_common.h"
#include "picoquic_newreno.h"
#include "picoquic_cubic.h"
#include "picoquictest_internal.h"
#include "picoquictest_rctl.h"

void picoquic_tls_api_unload(void);

#define MAX_TX_SAMPLES 65536

typedef struct {
    FILE* csv;
    int record_start;
    uint64_t transfer_start_time;
    uint64_t tx_times[MAX_TX_SAMPLES];
    size_t n_tx;
    size_t n_all_tx;
    uint64_t max_cwin;
    uint64_t final_cwin;
    double sum_gain_tail;
    size_t n_gain_tail;
    double min_gain;
    double max_gain;
    int ss_stalled_events;
    uint64_t prev_blocked_ts;
} trace_collector_t;

static void exp_trace_cb(
    void* trace_ctx,
    uint64_t current_time,
    int event_type,
    uint64_t cwin,
    uint64_t bytes_in_transit,
    uint64_t smoothed_rtt,
    uint64_t rtt_variant,
    size_t length,
    double raw_gain,
    double latched_gain,
    uint64_t pacing_rate)
{
    trace_collector_t* tc = (trace_collector_t*)trace_ctx;
    if (!tc->record_start) {
        return;
    }
    if (cwin > tc->max_cwin) tc->max_cwin = cwin;
    tc->final_cwin = cwin;

    if (event_type == picoquic_pacing_trace_packet_sent) {
        tc->n_all_tx++;
        if (length >= 1000 && tc->n_tx < MAX_TX_SAMPLES) {
            tc->tx_times[tc->n_tx++] = current_time;
        }
    }
    if (event_type == picoquic_pacing_trace_window_update) {
        if (latched_gain > 0.0) {
            if (tc->min_gain == 0.0 || latched_gain < tc->min_gain) tc->min_gain = latched_gain;
            if (latched_gain > tc->max_gain) tc->max_gain = latched_gain;
        }
    }
    if (tc->csv != NULL) {
        fprintf(tc->csv, "%" PRIu64 ",%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%zu,%.6f,%.6f,%" PRIu64 "\n",
            current_time - tc->transfer_start_time, event_type, cwin, bytes_in_transit,
            smoothed_rtt, rtt_variant, length, raw_gain, latched_gain, pacing_rate);
    }
}

static double compute_cv_range(const trace_collector_t* tc, size_t start, size_t end)
{
    if (tc->n_tx < 20 || end <= start + 5) return 0.0;
    if (start < 1) start = 1;
    if (end > tc->n_tx) end = tc->n_tx;
    size_t n = end - start;
    double mean = 0.0;
    for (size_t i = start; i < end; i++) {
        mean += (double)(tc->tx_times[i] - tc->tx_times[i - 1]);
    }
    mean /= (double)n;
    if (mean <= 1e-12) return 0.0;
    double var = 0.0;
    for (size_t i = start; i < end; i++) {
        double d = (double)(tc->tx_times[i] - tc->tx_times[i - 1]) - mean;
        var += d * d;
    }
    var /= (double)n;
    return sqrt(var) / mean;
}

static double compute_tail_cv(const trace_collector_t* tc, size_t window_pkts)
{
    size_t start = (tc->n_tx > window_pkts) ? (tc->n_tx - window_pkts) : 1;
    return compute_cv_range(tc, start, tc->n_tx);
}

static double compute_mid_cv(const trace_collector_t* tc)
{
    return compute_cv_range(tc, tc->n_tx / 4, (3 * tc->n_tx) / 4);
}

static const char* mode_name(int mode)
{
    switch (mode) {
    case picoquic_pacing_mode_native: return "Native_Def";
    case picoquic_pacing_mode_native_q2: return "Native_Q2";
    case picoquic_pacing_mode_static_1_0: return "Static_1_0";
    case picoquic_pacing_mode_rfc_2_0: return "RFC_2_0";
    case picoquic_pacing_mode_analytical: return "Analytic_2ln2_1";
    case picoquic_pacing_mode_budget_o1: return "BudgetO1";
    case picoquic_pacing_mode_linux_20_12: return "Linux_20_12";
    default: return "Unknown";
    }
}

static int run_sim_scenario(
    const char* exp_name,
    const char* csv_path,
    FILE* multi_csv,
    int seed_id,
    picoquic_congestion_algorithm_t const* cc_algo,
    int pacing_mode,
    uint64_t picosec_per_byte,
    uint64_t one_way_latency_us,
    uint64_t jitter_us,
    uint64_t queue_delay_max_us,
    double rctl_bytes_per_us,
    uint64_t rctl_bucket_max,
    size_t transfer_bytes,
    int inject_single_loss_at_cwin_pkts,
    int quiet)
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    picoquic_connection_id_t initial_cid = { {0xbd, 0x9e, 0x01, (uint8_t)pacing_mode, cc_algo->congestion_algorithm_number, (uint8_t)(seed_id & 0xff), 1, 2}, 8 };

    picoquic_tp_t client_tp, server_tp;
    picoquic_init_transport_parameters(&client_tp);
    client_tp.initial_max_data = 32ull * 1024 * 1024;
    client_tp.initial_max_stream_data_bidi_local = 32ull * 1024 * 1024;
    client_tp.initial_max_stream_data_bidi_remote = 32ull * 1024 * 1024;
    client_tp.initial_max_stream_data_uni = 32ull * 1024 * 1024;
    server_tp = client_tp;

    int ret = tls_api_one_scenario_init_ex(&test_ctx, &simulated_time,
        PICOQUIC_INTERNAL_TEST_VERSION_1, &client_tp, &server_tp, &initial_cid);
    if (ret != 0) {
        fprintf(stderr, "init failed %d\n", ret);
        return ret;
    }

    trace_collector_t tc;
    memset(&tc, 0, sizeof(tc));
    if (csv_path != NULL) {
        tc.csv = fopen(csv_path, "w");
        if (tc.csv != NULL) {
            fprintf(tc.csv, "time_us,event,cwin,in_transit,srtt_us,rttvar_us,length,raw_gain,latched_gain,pacing_rate\n");
        }
    }

    test_ctx->c_to_s_link->microsec_latency = one_way_latency_us;
    test_ctx->c_to_s_link->picosec_per_byte = picosec_per_byte;
    test_ctx->c_to_s_link->queue_delay_max = queue_delay_max_us;
    test_ctx->s_to_c_link->microsec_latency = one_way_latency_us;
    test_ctx->s_to_c_link->picosec_per_byte = picosec_per_byte;
    test_ctx->s_to_c_link->queue_delay_max = queue_delay_max_us;
    if (jitter_us > 0) {
        test_ctx->c_to_s_link->jitter = jitter_us;
        test_ctx->c_to_s_link->jitter_mode = jitter_gauss;
        test_ctx->c_to_s_link->jitter_seed = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)(seed_id + 1) * 0xBF58476D1CE4E5B9ULL);
        test_ctx->s_to_c_link->jitter = jitter_us;
        test_ctx->s_to_c_link->jitter_mode = jitter_gauss;
        test_ctx->s_to_c_link->jitter_seed = 0x517CC1B727220A95ULL ^ ((uint64_t)(seed_id + 1) * 0x94D049BB133111EBULL);
    }

    if (rctl_bytes_per_us > 0.0) {
        ret = rctl_configure(test_ctx->c_to_s_link, rctl_bytes_per_us, rctl_bucket_max, simulated_time);
        if (ret == 0) {
            ret = rctl_configure(test_ctx->s_to_c_link, rctl_bytes_per_us, rctl_bucket_max, simulated_time);
        }
    }

    picoquic_set_default_congestion_algorithm(test_ctx->qserver, cc_algo);
    picoquic_set_pacing_mode(test_ctx->qserver, pacing_mode);
    picoquic_set_pacing_trace(test_ctx->qserver, exp_trace_cb, &tc);

    if (ret == 0) {
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
    }
    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, queue_delay_max_us, &simulated_time);
    }

    test_api_stream_desc_t scenario[1] = { { 4, 0, 257, transfer_bytes } };
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, scenario, sizeof(scenario));
    }

    uint64_t t_start = simulated_time;
    tc.transfer_start_time = t_start;
    tc.record_start = 1;

    uint64_t blocked_ticks = 0;
    uint64_t unblocked_ss_acks = 0;
    int injected_loss = 0;
    uint64_t last_ack_seen = 0;
    double late_gains_sum = 0.0;
    size_t late_gains_cnt = 0;

    uint64_t ack_loss_mask = 0;

    if (ret == 0) {
        test_ctx->c_to_s_link->loss_mask = &ack_loss_mask;
        test_ctx->s_to_c_link->loss_mask = &loss_mask;
        int nb_trials = 0;
        int nb_inactive = 0;
        while (ret == 0 && nb_trials < 4000000 && nb_inactive < 512 && TEST_CLIENT_READY && TEST_SERVER_READY) {
            int was_active = 0;
            nb_trials++;
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
            if (ret < 0) break;

            if (test_ctx->cnx_server != NULL && test_ctx->cnx_server->nb_paths > 0) {
                picoquic_path_t* sp = test_ctx->cnx_server->path[0];
                if (sp->last_time_acked_data_frame_sent != last_ack_seen) {
                    last_ack_seen = sp->last_time_acked_data_frame_sent;
                    if (sp->last_cwin_blocked_time < sp->last_time_acked_data_frame_sent) {
                        unblocked_ss_acks++;
                    }
                    else {
                        blocked_ticks++;
                    }
                }
                if (inject_single_loss_at_cwin_pkts > 0 && !injected_loss &&
                    sp->send_mtu > 0 &&
                    sp->cwin >= (uint64_t)inject_single_loss_at_cwin_pkts * sp->send_mtu) {
                    loss_mask = 0x3FFull;
                    injected_loss = 1;
                }
                if (injected_loss && test_ctx->s_to_c_link->packets_dropped >= 8) {
                    loss_mask = 0ull;
                }
                /* Collect trailing gain over last portion */
                if (sp->budget.gain_latched > 0.0 &&
                    tc.n_all_tx > 200) {
                    late_gains_sum += sp->budget.gain_latched;
                    late_gains_cnt++;
                }
            }

            if (was_active) nb_inactive = 0;
            else nb_inactive++;

            if (test_ctx->test_finished) {
                if (test_ctx->immediate_exit ||
                    (picoquic_is_cnx_backlog_empty(test_ctx->cnx_client) &&
                     picoquic_is_cnx_backlog_empty(test_ctx->cnx_server))) {
                    break;
                }
            }
        }
    }

    uint64_t t_end = simulated_time;
    uint64_t fct_us = t_end - t_start;
    int verify_rc = (ret == 0) ? tls_api_one_scenario_verify(test_ctx) : ret;

    uint64_t retx = (test_ctx->cnx_server != NULL) ? test_ctx->cnx_server->nb_retransmission_total : UINT64_MAX;
    uint64_t drops = (test_ctx->s_to_c_link != NULL) ? test_ctx->s_to_c_link->packets_dropped : UINT64_MAX;
    double last_gain = (test_ctx->cnx_server != NULL && test_ctx->cnx_server->nb_paths > 0) ? test_ctx->cnx_server->path[0]->budget.gain_latched : 0.0;
    double cv500 = compute_tail_cv(&tc, 500);
    double cvmid = compute_mid_cv(&tc);
    double mbps = (fct_us > 0) ? ((double)transfer_bytes * 8.0 / (double)fct_us) : 0.0;

    if (!quiet) {
        printf("%-22s | %-12s | %-6s | ok=%d | FCT=%7.2f ms (%6.2f Mbps) | pkts=%5zu | maxW=%7" PRIu64 " | finW=%7" PRIu64 " | drop=%4" PRIu64 " | retx=%4" PRIu64 " | g=[%.3f..%.3f] last=%.3f | CV500=%.4f | CVmid=%.4f | ss_ok=%" PRIu64 "/%" PRIu64 "\n",
            exp_name, mode_name(pacing_mode), cc_algo->congestion_algorithm_id,
            verify_rc, (double)fct_us / 1000.0, mbps, tc.n_tx, tc.max_cwin, tc.final_cwin,
            drops, retx, tc.min_gain, tc.max_gain, last_gain, cv500, cvmid,
            blocked_ticks, blocked_ticks + unblocked_ss_acks);
    }
    if (multi_csv != NULL) {
        fprintf(multi_csv, "%s,%d,%s,%s,%d,%.3f,%.3f,%.4f,%.4f,%" PRIu64 ",%" PRIu64 "\n",
            exp_name, seed_id, mode_name(pacing_mode), cc_algo->congestion_algorithm_id, verify_rc,
            (double)fct_us / 1000.0, mbps, cv500, cvmid, drops, retx);
    }

    if (tc.csv != NULL) {
        fclose(tc.csv);
    }
    tls_api_delete_ctx(test_ctx);
    return verify_rc;
}

int main(int argc, char** argv)
{
    const char* which = (argc > 1) ? argv[1] : "all";
    const char* sol_dir = getenv("PICOQUIC_DIR");
    picoquic_set_solution_dir(sol_dir ? sol_dir : "./picoquic");
    debug_printf_suspend();

    int ret = 0;

    if (strcmp(which, "smoke") == 0) {
        ret = run_sim_scenario("Smoke", "smoke.csv", NULL, 0, picoquic_newreno_algorithm,
            picoquic_pacing_mode_budget_o1, 20971, 25000, 0, 0, 0.0, 0, 524288, 0, 0);
        picoquic_tls_api_unload();
        return ret;
    }

    int modes_all[] = {
        picoquic_pacing_mode_native,
        picoquic_pacing_mode_native_q2,
        picoquic_pacing_mode_static_1_0,
        picoquic_pacing_mode_budget_o1,
        picoquic_pacing_mode_analytical,
        picoquic_pacing_mode_rfc_2_0,
        picoquic_pacing_mode_linux_20_12
    };
    size_t n_modes = sizeof(modes_all) / sizeof(modes_all[0]);

    if (strcmp(which, "all") == 0 || strcmp(which, "exp1") == 0) {
        printf("\n=== EXP 1A: Pure Uncongested Lossless Slow Start (400 Mbps, 50 ms RTT, q=inf, 3 MB, NewReno) ===\n");
        for (size_t i = 0; i < n_modes; i++) {
            char csv[256];
            snprintf(csv, sizeof(csv), "./exp1a_pure_ss_%s.csv", mode_name(modes_all[i]));
            ret |= run_sim_scenario("Exp1A_400M_PureSS", csv, NULL, 0, picoquic_newreno_algorithm, modes_all[i],
                20971, 25000, 0, 0, 0.0, 0, 3000000, 0, 0);
        }
        printf("\n=== EXP 1B: Lossless Slow Start -> HyStart Exit (100 Mbps, 50 ms RTT, q=inf, 4 MB, NewReno & CUBIC) ===\n");
        for (size_t i = 0; i < n_modes; i++) {
            char csv[256];
            snprintf(csv, sizeof(csv), "./exp1_reno_%s.csv", mode_name(modes_all[i]));
            ret |= run_sim_scenario("Exp1B_SS_100M_Reno", csv, NULL, 0, picoquic_newreno_algorithm, modes_all[i],
                83886, 25000, 0, 0, 0.0, 0, 4000000, 0, 0);
        }
        printf("--- Same link with CUBIC ---\n");
        for (size_t i = 0; i < n_modes; i++) {
            char csv[256];
            snprintf(csv, sizeof(csv), "./exp1_cubic_%s.csv", mode_name(modes_all[i]));
            ret |= run_sim_scenario("Exp1B_SS_100M_CUBIC", csv, NULL, 0, picoquic_cubic_algorithm, modes_all[i],
                83886, 25000, 0, 0, 0.0, 0, 4000000, 0, 0);
        }
    }

    if (strcmp(which, "all") == 0 || strcmp(which, "exp2") == 0) {
        printf("\n=== EXP 2: NewReno SS -> Drop-Tail Loss Recovery -> CA (20 Mbps, 50 ms RTT, q=20ms / ~0.4 BDP, 4 MB) ===\n");
        for (size_t i = 0; i < n_modes; i++) {
            char csv[256];
            snprintf(csv, sizeof(csv), "./exp2_reno_q20_%s.csv", mode_name(modes_all[i]));
            ret |= run_sim_scenario("Exp2_Reno_20M_q20ms", csv, NULL, 0, picoquic_newreno_algorithm, modes_all[i],
                419430, 25000, 0, 20000, 0.0, 0, 4000000, 0, 0);
        }
        printf("--- Tighter queue q=10ms (~0.2 BDP) ---\n");
        for (size_t i = 0; i < n_modes; i++) {
            ret |= run_sim_scenario("Exp2_Reno_20M_q10ms", NULL, NULL, 0, picoquic_newreno_algorithm, modes_all[i],
                419430, 25000, 0, 10000, 0.0, 0, 4000000, 0, 0);
        }
    }

    if (strcmp(which, "all") == 0 || strcmp(which, "exp3") == 0) {
        printf("\n=== EXP 3A: Upstream Huitema pacing_cc_algotest (10 Mbps rctl, 15 ms RTT, 1 MB) ===\n");
        for (size_t i = 0; i < n_modes; i++) {
            ret |= run_sim_scenario("Exp3A_Rctl_CUBIC", NULL, NULL, 0, picoquic_cubic_algorithm, modes_all[i],
                83886, 7500, 0, 0, 1.25, 16 * PICOQUIC_MAX_PACKET_SIZE, 1000000, 0, 0);
        }
        printf("--- Upstream rctl with NewReno ---\n");
        for (size_t i = 0; i < n_modes; i++) {
            ret |= run_sim_scenario("Exp3A_Rctl_Reno", NULL, NULL, 0, picoquic_newreno_algorithm, modes_all[i],
                83886, 7500, 0, 0, 1.25, 16 * PICOQUIC_MAX_PACKET_SIZE, 1000000, 0, 0);
        }

        printf("\n=== EXP 3B: CUBIC Shallow Drop-Tail Queue (20 Mbps, 50 ms RTT, q=12ms / ~0.24 BDP, 6 MB) ===\n");
        for (size_t i = 0; i < n_modes; i++) {
            char csv[256];
            snprintf(csv, sizeof(csv), "./exp3b_cubic_q12_%s.csv", mode_name(modes_all[i]));
            ret |= run_sim_scenario("Exp3B_CUBIC_20M_q12ms", csv, NULL, 0, picoquic_cubic_algorithm, modes_all[i],
                419430, 25000, 0, 12000, 0.0, 0, 6000000, 0, 0);
        }

        printf("\n=== EXP 3C: Clean CUBIC Concave -> Wmax Saddle -> Convex Probe (100 Mbps, 150 ms RTT, burst loss at 40 pkts, 6 MB) ===\n");
        for (size_t i = 0; i < n_modes; i++) {
            char csv[256];
            snprintf(csv, sizeof(csv), "./exp3c_cubic_convex_%s.csv", mode_name(modes_all[i]));
            ret |= run_sim_scenario("Exp3C_CUBIC_Convex", csv, NULL, 0, picoquic_cubic_algorithm, modes_all[i],
                83886, 75000, 0, 0, 0.0, 0, 6000000, 40, 0);
        }
    }

    if (strcmp(which, "all") == 0 || strcmp(which, "multi") == 0) {
        FILE* mcsv = fopen("./picoquic_multiseed_runs.csv", "w");
        if (mcsv != NULL) {
            fprintf(mcsv, "scenario,seed,mode,cc,ok,fct_ms,mbps,cv500,cvmid,drop,retx\n");
        }
        printf("\n=== EXP 4: Multi-Seed Stochastic Jitter Replication (n=20, 1.5 ms Gaussian jitter/dir, 50 ms mean RTT) ===\n");
        for (int seed = 1; seed <= 20; seed++) {
            for (size_t i = 0; i < n_modes; i++) {
                char trace[256];
                snprintf(trace, sizeof(trace), "multi_reno_s%02d_%s.csv", seed, mode_name(modes_all[i]));
                ret |= run_sim_scenario("Multi_Reno_q10ms_jit", trace, mcsv, seed, picoquic_newreno_algorithm, modes_all[i],
                    419430, 23500, 1500, 10000, 0.0, 0, 4000000, 0, 1);
                snprintf(trace, sizeof(trace), "multi_cubic_s%02d_%s.csv", seed, mode_name(modes_all[i]));
                ret |= run_sim_scenario("Multi_CUBIC_q12ms_jit", trace, mcsv, seed, picoquic_cubic_algorithm, modes_all[i],
                    419430, 23500, 1500, 12000, 0.0, 0, 6000000, 0, 1);
            }
        }
        if (mcsv != NULL) {
            fclose(mcsv);
            printf("Wrote picoquic_multiseed_runs.csv (20 seeds x 2 scenarios x %zu modes = %zu runs)\n",
                n_modes, 20 * 2 * n_modes);
        }
    }

    picoquic_tls_api_unload();
    return ret;
}
