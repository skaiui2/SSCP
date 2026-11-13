#ifndef VNET_H
#define VNET_H
#include <stdint.h>
#include <stddef.h>
struct transfer;
typedef void (*vnet_observer)(unsigned direction, int event, const void *buf, size_t len, uint32_t now);
enum { VNET_SEND, VNET_ARRIVAL, VNET_INPUT_RETURNED };
void vnet_observe(vnet_observer callback);
void vnet_attach(struct transfer *a, struct transfer *b);
void vnet_attach_fd(int local_fd, struct transfer *t);
struct vnet_endpoint { unsigned direction; };
void vnet_init(uint32_t delay_ms, uint32_t mbps, uint32_t loss_ppm, uint32_t seed);
void vnet_queue_limit(uint32_t packets);
void vnet_set_jitter(uint32_t jitter_ms, int allow_reorder);
void vnet_set_markov_drift(int enable);
int vnet_send(void *user, const void *buf, size_t len);
int vnet_step(void);
void vnet_free(void);
extern uint64_t vnet_packets, vnet_dropped;
extern uint64_t vnet_queue_dropped, vnet_pending;
struct vnet_stats {
    uint64_t samples;
    uint32_t q_max;
    double q_avg;
    double q_p95;
    double q_p99;
    double qdelay_avg_ms;
    double qdelay_p50_ms;
    double qdelay_p95_ms;
    double qdelay_p99_ms;
    double qdelay_max_ms;
    double owd_p50_ms;
    double owd_p95_ms;
    double owd_p99_ms;
    double owd_max_ms;
};
void vnet_get_stats(unsigned direction, struct vnet_stats *out);
#endif
