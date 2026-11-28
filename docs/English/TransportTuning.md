# SSCP Transport Tuning & Mechanisms

[参数调优与传输机制说明 (中文)](../中文/参数调优与传输机制.md) · [User Guide](UserGuide.md) · [README](../../README.md)

This document summarizes SSCP's compile-time tuning parameters, recommended profiles for different network environments, and the design of optional dynamic packet-bitmap SACK, delayed ACK, and congestion control.

---

## 1. Compile-Time Tuning Macros (`sscp.h`)

All macros below are guarded by `#ifndef` and can be overridden via `-D<NAME>=<VALUE>` for specific environments or test matrices without editing source code:

| Macro | Default | Purpose |
|---|---|---|
| `SSCP_ACK_BATCH_MAX` | `2U` | Delayed/batched ACK threshold (RFC 5681: ACK at least every 2 full-sized segments). |
| `SSCP_ACK_DELAY_MS` | `4U * SSCP_TICK_SCALE` | Upper bound for delayed ACK timer (`4 ms`); effective delay is `min(srtt >> 2, SSCP_ACK_DELAY_MS)` with a `1 ms` floor. |
| `MTU` | `1460` | Max SSCP packet size on wire; `MSS = MTU - sizeof(struct sscp_hdr)` (`30 B` header, default `MSS = 1430`). |
| `SSCP_IW_SEGS` | `10U` | Initial congestion window in segments (RFC 6928 IW10, initial `cwnd = 10 * MSS`). |
| `SSCP_RTO_INIT` | `500U * SSCP_TICK_SCALE` | Initial RTO (`500 ms`); dynamically clamped to `[SSCP_RTO_MIN, SSCP_RTO_MAX]` (`200 ms .. 2000 ms`) once RTT samples arrive. |
| `CWND_WIN_MAX` | `1 << 20` (1 MiB) | Maximum congestion window. Raise for high-BDP long fat pipes. |
| `SEND_WIN_INIT` / `RECV_WIN_INIT` / `SSCP_RECV_LIMIT` | `1 << 24` (16 MiB) | Initial flow-control windows and receive buffer ceiling. |
| `SSCP_TICK_SCALE` | `1U` | Ticks per millisecond (`1U` = 1 ms for real UDP; `1000U` = 1 μs for virtual simulation). |

---

## 2. Recommended Profiles by Environment

> **Guideline**: The defaults on `main` target general-purpose links (short-RTT WiFi, finite-queue WAN, and intercontinental paths), preserving clean ACK clocking, HyStart delay sensitivity, and unbiased SRTT estimation. Override macros at compile time when tuning for specialized links.

### 2.1 General / LAN / WiFi (5–50 ms RTT)

Use the built-in defaults:

```sh
cmake -S test -B build -DCMAKE_BUILD_TYPE=Release
```

- `SSCP_ACK_BATCH_MAX=2U` and `<= 4 ms` dynamic delay (`min(srtt >> 2, 4 ms)`) keep short-RTT ACK clocking responsive and prevent HyStart slow-start overshoot.

### 2.2 Intercontinental / Encrypted Tunnel / Asymmetric Uplink (150–250 ms RTT)

When running over outer encapsulation (UDP tunnel, AEAD/DTLS obfuscation, PPPoE/IPv6) or high-BDP intercontinental links:

```sh
cmake -S test -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-DMTU=1200 -DSSCP_ACK_BATCH_MAX=4U -DSSCP_ACK_DELAY_MS='(8U*(SSCP_TICK_SCALE))' -DCWND_WIN_MAX='(4U<<20)'"
```

- **Lower `MTU`** (`1200`–`1360`): leaves headroom for outer framing/crypto padding and avoids path fragmentation.
- **Moderate `SSCP_ACK_BATCH_MAX`** (`4U`): reduces reverse-path ACK packet rate on long RTT paths without causing the severe short-RTT / small-cwnd regressions observed with overly aggressive batching (e.g., `8:1 / 15 ms`).
- **Larger `CWND_WIN_MAX`**: a `100 Mbps × 200 ms` path has a `2.5 MB` BDP; raising `CWND_WIN_MAX` from `1 MiB` to `4 MiB` removes the ~`40 Mbps` single-flow cap.

### 2.3 Immediate 1:1 ACK Baseline

Useful for A/B regression benchmarks or ultra-low-latency local links:

```sh
cmake -S test -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-DSSCP_ACK_BATCH_MAX=1U"
```

### 2.4 Embedded / Constrained SRAM

Shrink windows and MTU to fit available RAM (keep `MTU >= 320` so the maximum optional SACK bitmap fits):

```sh
-DMTU=576 -DCWND_WIN_MAX=32768 -DSSCP_RECV_LIMIT=32768 -DSEND_WIN_INIT=32768 -DRECV_WIN_INIT=32768
```

---

## 3. Delayed ACK Rules

To reduce ACK volume without hurting loss recovery, `sscp_schedule_ack()` transmits an **immediate ACK** whenever any of the following holds:

1. An out-of-order or duplicate DATA packet arrives;
2. An in-order packet fills an OOO hole (`filled_hole`), or `rcv_buf_q` still holds remaining OOO segments;
3. A short segment arrives (`payload_len < MSS`);
4. The stream is in fast recovery (`fr_active`);
5. Pending unacknowledged in-order packets reach `SSCP_ACK_BATCH_MAX`.

Otherwise, `t_ack` is armed with `clamp(srtt >> 2, 1 ms, SSCP_ACK_DELAY_MS)` (falling back to `SSCP_ACK_DELAY_MS` when `srtt == 0`).

---

## 4. Optional Dynamic Packet-Bitmap SACK

SSCP sends discrete packets enqueued in `sscp_send()` without ever re-segmenting or coalescing them, while maintaining a byte-stream sequence space. Each DATA buffer is assigned a monotonic 32-bit `pkt_id` preserved across retransmissions.

### 4.1 Wire Format (Zero Overhead in Loss-Free Operation)

- **Fixed `struct sscp_hdr` remains exactly `30 B`** (`seq`, `ack`, `pkt_id`, `fd`, `wnd`, `len`, `cksum`, `flags`, `time_val`).
- **DATA packets and loss-free ACKs**: `SSCP_FLAG_SACK` is clear; **0 B SACK overhead**.
- **OOO ACKs (`SSCP_FLAG_SACK = 0x100`)**: only when `rcv_buf_q` is non-empty, pure ACKs append a dynamically trimmed bitmap extension and set `sh->len` to the extension length (validated and checksummed automatically by `sscp_input()`):

```text
+--------------------+---------------+-----------------+------------------------+
| sscp_hdr (30 B)    | base_id (4 B) | map_words (2 B) | map[0..map_words-1]    |
| flags |= FLAG_SACK | = rcv_pkt_nxt |   (1 .. 64)     | (4 B .. 256 B, big-end)|
+--------------------+---------------+-----------------+------------------------+
```

- Bit `i` represents packet `base_id + i` (bit `0` is the first missing packet, always `0`; received OOO packets are `1`).
- Trimmed to the highest non-zero 32-bit word: only **`10 B`** for 1–32 OOO packets, scaling up to `64` words (`2048` packets ≈ `2.8 MiB` span).

### 4.2 Sender Recovery & Pipe Accounting

1. **Single-pass `snd_q` scan** (`sscp_sack_mark_and_retransmit`):
   - Bit `1`: marks `sb->sacked = 1` and adds payload length to `ss->sacked_bytes`.
   - Bit `0` (not `sacked`, not `retransmitted`): retransmits the head hole (`pkt_id == base_id`) immediately and non-head holes once `highest_id >= pkt_id + 3` (RFC 6675 `dupthresh = 3`), capped at `RETRANS_GAP_MAX` (`4`) segments per ACK.
2. **Single fast-retransmit & RTO interplay**:
   - Each lost packet is fast-retransmitted at most once (`sb->retransmitted`); dropped retransmissions are recovered by RTO without spurious timer resets.
   - First RTO (`timeout_count == 0`) keeps `sb->sacked` and clears `retransmitted` on un-SACKed segments; a second consecutive RTO clears all `sacked` state and `sacked_bytes` (RFC 2018 reneging safety).
3. **Window gating (`sscp_output`)**:
   - Flow control (`snd_wnd`) bounds total unacked flight `raw_flight = schedule_next->seq - snd_una`.
   - Congestion control (`cwnd`) bounds network pipe `pipe = raw_flight - sacked_bytes`.
4. **RTT echo & strict-aliasing-safe checksum**:
   - Pure ACKs with `SSCP_FLAG_SACK` continue to echo DATA timestamps for RTT/RTO estimation.
   - `sscp_checksum()` uses `memcpy`-based 16-bit accumulation so `-O3` strict aliasing cannot reorder packed header stores past checksum computation.

---

## 5. Congestion Control Summary

| Controller | Best Suited For | Key Behavior |
|---|---|---|
| `SSCP_CC_PROB` (default) | Public WAN / intercontinental links with mixed loss & queueing | Maps `srtt - rtt_base` and `p_ema` into a continuous congestion probability `q`; probability-weighted exponential slow start, continuous adaptation during recovery, configurable via `sscp_prob_configure(ss, rtt_span_ms, loss_q16)`. |
| `SSCP_CC_AIMD` | Conservative baseline / Reno-style fairness | Classic additive increase, multiplicative decrease. |
| `SSCP_CC_CUBIC` | High-BDP / high-bandwidth links | RFC 9438 cubic growth with HyStart delay-based slow-start exit. |
