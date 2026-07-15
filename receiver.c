/* RECEIVER — backward sliding-window XOR FEC decoder + ARQ backstop.
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from the sender, via the hostile relay (OUR format)
 *   send 47020  -> harness player, fixed format: 4-byte BE seq + 160B payload.
 *   send 47003  -> NACK feedback to the sender, via the relay (ARQ)
 *
 * WIRE FORMAT on 47002 (from sender.c):
 *   DATA   : [0x00][seq  u32 BE][160B]
 *   PARITY : [0x01][base u32 BE][win u8][160B xor]  covers {base..base-win+1}
 *   RETX   : [0x03][seq  u32 BE][160B]   (a retransmitted data frame)
 * NACK on 47003 (to sender): [0x02][seq u32 BE]
 *
 * FEC (unchanged): sequence tracking, duplicate suppression, reorder handling,
 * a jitter buffer that releases each frame the instant a correct copy exists
 * (no artificial delay — DELAY_MS is slack for reconstruction), and a fixpoint
 * XOR solver that repairs any parity equation with one unknown, repeating for
 * chained recovery.
 *
 * ARQ BACKSTOP (new): FEC handles the vast majority of losses with no round
 * trip. For the residual it cannot repair — two losses inside one 3-frame
 * window — the receiver issues a NACK, but ONLY when:
 *   (a) the frame is still missing after FEC could possibly have recovered it
 *       (i.e. after its last covering parity would have arrived), AND
 *   (b) a round trip still fits before the frame's playout deadline
 *       (deadline - now > ~2x the observed one-way delay).
 * NACKs are de-duplicated per seq (with a small bounded retry for a lost
 * NACK/resend), and duplicate retransmissions are suppressed. If neither (a)
 * nor (b) can be met — the usual case at a tight delay — no NACK is sent and
 * the design degrades gracefully to FEC-only.
 *
 * Stats -> receiver_stats.log at exit (voluntary exit before harness SIGKILL).
 */
#include <arpa/inet.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD 160
#define FRAME_MS 20.0
#define TYPE_DATA 0
#define TYPE_PARITY 1
#define TYPE_NACK 2
#define TYPE_RETX 3
#define DATA_HDR 5
#define PARITY_HDR 6
#define NACK_LEN 5
#define MAX_WINDOW 8
#define MAX_NACKS 3          /* bounded retries per seq (lost NACK/resend)   */
#define END_MARGIN_S 0.3

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double now_epoch(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static double getenv_double(const char *name, double def) {
    const char *v = getenv(name);
    return (v && *v) ? atof(v) : def;
}

/* ---- run-wide config ---- */
static double g_t0, g_delay_ms;
static int g_n = 0;

/* ---- per-frame state (indexed by seq) ---- */
static unsigned char *g_frame = NULL;
static uint8_t *g_known = NULL;
static uint8_t *g_data_recv = NULL;
static uint8_t *g_forwarded = NULL;
static uint8_t *g_rebuilt = NULL;      /* delivered via FEC reconstruction   */
static uint8_t *g_via_retx = NULL;     /* delivered via ARQ retransmission   */
static double *g_fwd_ts = NULL;
static uint8_t *g_nack_count = NULL;
static double *g_last_nack_ts = NULL;

/* ---- per-parity state (indexed by base) ---- */
static unsigned char *g_par = NULL;
static uint8_t *g_par_recv = NULL;
static uint8_t *g_par_win = NULL;
static uint8_t *g_par_done = NULL;

/* ---- counters ---- */
static long g_dup_data = 0, g_dup_par = 0;
static long g_recovered = 0, g_reorder_events = 0;
static long g_max_reorder_depth = 0, g_reorder_depth_sum = 0;
static long g_late = 0;
static long g_data_pkts = 0, g_par_pkts = 0, g_rx_bytes = 0;
static long g_nacks_sent = 0, g_retx_recv = 0, g_retx_dup = 0;
static long g_retx_recovery = 0, g_retx_late = 0;
static long g_owd_n = 0;
static double g_owd_min = 1e18, g_owd_max = -1e18, g_owd_sum = 0, g_owd_sumsq = 0;
static double g_owd_max_s = 0.0;   /* running max one-way delay, in seconds   */
static int g_scan_lo = 0;           /* NACK sweep lower cursor                 */

static int g_out_fd, g_fb_fd;
static struct sockaddr_in g_player, g_relay_fb;
static long g_max_seq_seen = -1;

static double deadline_of(uint32_t seq) {
    return g_t0 + g_delay_ms / 1000.0 + (double)seq * FRAME_MS / 1000.0;
}

static void forward_frame(uint32_t seq) {
    if (g_forwarded[seq] || !g_known[seq]) return;
    unsigned char out[4 + PAYLOAD];
    uint32_t nb = htonl(seq);
    memcpy(out, &nb, 4);
    memcpy(out + 4, g_frame + (size_t)seq * PAYLOAD, PAYLOAD);
    sendto(g_out_fd, out, sizeof out, 0, (struct sockaddr *)&g_player,
           sizeof g_player);
    g_forwarded[seq] = 1;
    double now = now_epoch();
    g_fwd_ts[seq] = now;
    if (now > deadline_of(seq)) g_late++;
}

/* FEC fixpoint solver — unchanged from the FEC-only pass. */
static void solve(void) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int base = 0; base < g_n; base++) {
            if (!g_par_recv[base] || g_par_done[base]) continue;
            int w = g_par_win[base];
            int unknown = -1, n_unknown = 0;
            for (int k = 0; k < w; k++) {
                int m = base - k;
                if (m < 0) { n_unknown = 2; break; }
                if (!g_known[m]) { unknown = m; n_unknown++; }
            }
            if (n_unknown == 0) { g_par_done[base] = 1; continue; }
            if (n_unknown != 1) continue;
            unsigned char *dst = g_frame + (size_t)unknown * PAYLOAD;
            memcpy(dst, g_par + (size_t)base * PAYLOAD, PAYLOAD);
            for (int k = 0; k < w; k++) {
                int m = base - k;
                if (m == unknown) continue;
                unsigned char *f = g_frame + (size_t)m * PAYLOAD;
                for (int b = 0; b < PAYLOAD; b++) dst[b] ^= f[b];
            }
            g_known[unknown] = 1;
            g_rebuilt[unknown] = 1;
            g_recovered++;
            g_par_done[base] = 1;
            forward_frame((uint32_t)unknown);
            changed = 1;
        }
    }
}

static void handle_data(uint32_t seq, const unsigned char *payload, size_t plen,
                        double arr, int is_retx) {
    if (is_retx) {
        g_retx_recv++;
        if (g_known[seq]) { g_retx_dup++; return; }   /* FEC/earlier retx won */
        if (plen > PAYLOAD) plen = PAYLOAD;
        memcpy(g_frame + (size_t)seq * PAYLOAD, payload, plen);
        g_known[seq] = 1;
        g_via_retx[seq] = 1;
        if (arr <= deadline_of(seq)) g_retx_recovery++; else g_retx_late++;
        forward_frame(seq);
        solve();
        return;
    }

    if (g_data_recv[seq]) { g_dup_data++; return; }
    g_data_recv[seq] = 1;

    if ((long)seq < g_max_seq_seen) {
        long depth = g_max_seq_seen - (long)seq;
        g_reorder_events++;
        g_reorder_depth_sum += depth;
        if (depth > g_max_reorder_depth) g_max_reorder_depth = depth;
    } else {
        g_max_seq_seen = (long)seq;
    }

    double owd = (arr - (g_t0 + (double)seq * FRAME_MS / 1000.0)) * 1000.0;
    if (owd < g_owd_min) g_owd_min = owd;
    if (owd > g_owd_max) g_owd_max = owd;
    if (owd / 1000.0 > g_owd_max_s) g_owd_max_s = owd / 1000.0;
    g_owd_sum += owd; g_owd_sumsq += owd * owd; g_owd_n++;

    if (!g_known[seq]) {
        if (plen > PAYLOAD) plen = PAYLOAD;
        memcpy(g_frame + (size_t)seq * PAYLOAD, payload, plen);
        g_known[seq] = 1;
    }
    forward_frame(seq);
    solve();
}

static void handle_parity(uint32_t base, int win, const unsigned char *xorbuf) {
    if (base >= (uint32_t)g_n || win < 1 || win > MAX_WINDOW) return;
    if (g_par_recv[base]) { g_dup_par++; return; }
    g_par_recv[base] = 1;
    g_par_win[base] = (uint8_t)win;
    memcpy(g_par + (size_t)base * PAYLOAD, xorbuf, PAYLOAD);
    solve();
}

static void send_nack(uint32_t seq) {
    unsigned char pkt[NACK_LEN];
    pkt[0] = TYPE_NACK;
    uint32_t nb = htonl(seq);
    memcpy(pkt + 1, &nb, 4);
    sendto(g_fb_fd, pkt, sizeof pkt, 0, (struct sockaddr *)&g_relay_fb,
           sizeof g_relay_fb);
    g_nacks_sent++;
}

/* ARQ trigger: NACK a frame only after FEC could no longer recover it and a
 * round trip still fits before its deadline (see requirement 2). */
static void nack_sweep(double now) {
    if (g_owd_n == 0) return;                 /* no delay estimate yet         */
    double owd = g_owd_max_s;
    double rtt_margin = 2.0 * owd + 0.005;    /* NACK hop + resend hop + slack */
    double parity_grace = owd;                /* let the last covering parity land */

    /* frames whose availability has passed by now */
    int cur = (int)((now - g_t0) / (FRAME_MS / 1000.0)) + 1;
    if (cur > g_n) cur = g_n;

    while (g_scan_lo < g_n &&
           (g_known[g_scan_lo] || now > deadline_of((uint32_t)g_scan_lo) + 0.001))
        g_scan_lo++;

    for (int s = g_scan_lo; s < cur; s++) {
        if (g_known[s]) continue;
        double dl = deadline_of((uint32_t)s);
        if (now > dl) continue;                                 /* hopeless    */
        double fec_giveup = g_t0 + (s + 2) * (FRAME_MS / 1000.0) + parity_grace;
        if (now < fec_giveup) continue;              /* FEC may still recover  */
        if (dl - now < rtt_margin) continue;         /* no time for a round trip */
        if (g_nack_count[s] >= MAX_NACKS) continue;
        if (g_nack_count[s] > 0 && now - g_last_nack_ts[s] < rtt_margin) continue;
        send_nack((uint32_t)s);
        g_nack_count[s]++;
        g_last_nack_ts[s] = now;
    }
}

static void write_stats(double duration_s) {
    FILE *f = fopen("receiver_stats.log", "w");
    if (!f) { perror("fopen receiver_stats.log"); return; }

    long delivered = 0, on_time = 0, direct = 0;
    long fec_true_loss = 0, fec_early = 0, retx_delivered = 0;
    for (int s = 0; s < g_n; s++) {
        if (!g_forwarded[s]) continue;
        delivered++;
        if (g_via_retx[s]) retx_delivered++;
        else if (g_rebuilt[s]) { if (g_data_recv[s]) fec_early++; else fec_true_loss++; }
        else direct++;
        if (g_fwd_ts[s] <= deadline_of((uint32_t)s)) on_time++;
    }
    long unrecoverable = g_n - delivered;

    long stuck = 0;
    for (int base = 0; base < g_n; base++) {
        if (!g_par_recv[base] || g_par_done[base]) continue;
        int w = g_par_win[base], nu = 0;
        for (int k = 0; k < w; k++) { int m = base - k; if (m >= 0 && !g_known[m]) nu++; }
        if (nu > 1) stuck++;
    }

    long bursts = 0, burst_max = 0, burst_min = -1, missing = 0;
    double burst_sum = 0; int run = 0;
    for (int s = 0; s < g_n; s++) {
        if (!g_forwarded[s]) { run++; missing++; }
        else if (run > 0) {
            bursts++; burst_sum += run;
            if (run > burst_max) burst_max = run;
            if (burst_min < 0 || run < burst_min) burst_min = run;
            run = 0;
        }
    }
    if (run > 0) { bursts++; burst_sum += run; if (run > burst_max) burst_max = run;
                   if (burst_min < 0 || run < burst_min) burst_min = run; }

    double owd_mean = g_owd_n ? g_owd_sum / g_owd_n : 0.0;
    double owd_var = g_owd_n ? (g_owd_sumsq / g_owd_n - owd_mean * owd_mean) : 0.0;
    double owd_std = owd_var > 0 ? sqrt(owd_var) : 0.0;
    long raw = (long)g_n * PAYLOAD;

    fprintf(f, "# receiver_stats.log — sliding XOR FEC decoder (r=1:2, w=3) + ARQ backstop\n");
    fprintf(f, "# T0=%.6f DURATION_S=%.3f DELAY_MS=%.3f generated_at=%.6f\n",
            g_t0, duration_s, g_delay_ms, now_epoch());
    fprintf(f, "# ---- SUMMARY ----\n");
    fprintf(f, "frames_expected: %d\n", g_n);
    fprintf(f, "data_packets_received: %ld  duplicate_data_packets: %ld\n",
            g_data_pkts, g_dup_data);
    fprintf(f, "parity_packets_received: %ld  duplicate_parity_packets: %ld\n",
            g_par_pkts, g_dup_par);
    fprintf(f, "frames_delivered: %ld  (direct: %ld  via_FEC: %ld  via_ARQ: %ld)\n",
            delivered, direct, g_recovered, retx_delivered);
    fprintf(f, "fec_reconstructions: %ld  (true_loss: %ld  early_reconstruct: %ld)\n",
            g_recovered, fec_true_loss, fec_early);
    fprintf(f, "# ---- ARQ ----\n");
    fprintf(f, "nacks_sent: %ld\n", g_nacks_sent);
    fprintf(f, "retransmissions_received: %ld  duplicate_retransmissions: %ld\n",
            g_retx_recv, g_retx_dup);
    fprintf(f, "retransmission_recoveries(delivered on time via ARQ): %ld\n",
            g_retx_recovery);
    fprintf(f, "retransmissions_too_late(arrived after deadline): %ld\n", g_retx_late);
    fprintf(f, "# ---- OUTCOME ----\n");
    fprintf(f, "residual_unrecoverable_losses(never delivered): %ld (%.2f%%)\n",
            unrecoverable, 100.0 * unrecoverable / (g_n ? g_n : 1));
    fprintf(f, "unrecoverable_groups(parities stuck >1 unknown): %ld\n", stuck);
    fprintf(f, "late_packets(delivered after deadline): %ld (%.2f%%)\n",
            g_late, 100.0 * g_late / (g_n ? g_n : 1));
    fprintf(f, "on_time_frames: %ld (%.2f%%)\n",
            on_time, 100.0 * on_time / (g_n ? g_n : 1));
    fprintf(f, "effective_miss(not on time or never delivered): %ld (%.2f%%)\n",
            g_n - on_time, 100.0 * (g_n - on_time) / (g_n ? g_n : 1));
    fprintf(f, "reorder_events: %ld  max_depth=%ld  mean_depth=%.3f\n",
            g_reorder_events, g_max_reorder_depth,
            g_reorder_events ? (double)g_reorder_depth_sum / g_reorder_events : 0.0);
    fprintf(f, "one_way_delay_ms(availability->arrival, DATA first-arrivals): "
               "min=%.3f mean=%.3f max=%.3f stddev=%.3f n=%ld\n",
            g_owd_n ? g_owd_min : 0.0, owd_mean, g_owd_n ? g_owd_max : 0.0,
            owd_std, g_owd_n);
    fprintf(f, "burst_loss_segments(undelivered runs): %ld  min_len=%ld  "
               "mean_len=%.3f  max_len=%ld  total_missing=%ld\n",
            bursts, burst_min < 0 ? 0 : burst_min,
            bursts ? burst_sum / bursts : 0.0, burst_max, missing);
    fprintf(f, "downlink_bytes_received(receiver view): %ld  raw(n*160): %ld  "
               "[relay_stats.json authoritative for the overhead cap]\n",
            g_rx_bytes, raw);
    fprintf(f, "# ---- UNDELIVERED FRAMES ----\n");
    for (int s = 0; s < g_n; s++)
        if (!g_forwarded[s]) fprintf(f, "seq=%d UNDELIVERED\n", s);
    fclose(f);
}

int main(void) {
    g_t0 = getenv_double("T0", now_epoch());
    double duration_s = getenv_double("DURATION_S", 30.0);
    g_delay_ms = getenv_double("DELAY_MS", 60.0);
    g_n = (int)(duration_s * 1000.0 / FRAME_MS + 1e-6);
    if (g_n < 1) g_n = 1;
    double end_time = g_t0 + g_delay_ms / 1000.0
                       + (g_n - 1) * FRAME_MS / 1000.0 + END_MARGIN_S;

    g_frame       = calloc((size_t)g_n, PAYLOAD);
    g_par         = calloc((size_t)g_n, PAYLOAD);
    g_known       = calloc((size_t)g_n, 1);
    g_data_recv   = calloc((size_t)g_n, 1);
    g_forwarded   = calloc((size_t)g_n, 1);
    g_rebuilt     = calloc((size_t)g_n, 1);
    g_via_retx    = calloc((size_t)g_n, 1);
    g_par_recv    = calloc((size_t)g_n, 1);
    g_par_win     = calloc((size_t)g_n, 1);
    g_par_done    = calloc((size_t)g_n, 1);
    g_fwd_ts      = calloc((size_t)g_n, sizeof(double));
    g_nack_count  = calloc((size_t)g_n, 1);
    g_last_nack_ts= calloc((size_t)g_n, sizeof(double));
    if (!g_frame || !g_par || !g_known || !g_data_recv || !g_forwarded ||
        !g_rebuilt || !g_via_retx || !g_par_recv || !g_par_win || !g_par_done ||
        !g_fwd_ts || !g_nack_count || !g_last_nack_ts) { perror("calloc"); return 1; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47002");
        return 1;
    }
    struct timeval tv = {0, 5000}; /* 5ms poll: responsive NACK/deadline checks */
    setsockopt(in_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    g_out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    g_player.sin_family = AF_INET;
    g_player.sin_port = htons(47020);
    g_player.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* NACK feedback: send to the relay's downlink ingress (47003) */
    g_fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    g_relay_fb.sin_family = AF_INET;
    g_relay_fb.sin_port = htons(47003);
    g_relay_fb.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char buf[2048];
    while (!g_stop && now_epoch() < end_time) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
        double now = now_epoch();
        if (n > 0) {
            g_rx_bytes += n;
            if (buf[0] == TYPE_DATA && n >= DATA_HDR) {
                uint32_t seq;
                memcpy(&seq, buf + 1, 4);
                seq = ntohl(seq);
                if (seq < (uint32_t)g_n) {
                    g_data_pkts++;
                    handle_data(seq, buf + DATA_HDR, (size_t)n - DATA_HDR, now, 0);
                }
            } else if (buf[0] == TYPE_RETX && n >= DATA_HDR) {
                uint32_t seq;
                memcpy(&seq, buf + 1, 4);
                seq = ntohl(seq);
                if (seq < (uint32_t)g_n)
                    handle_data(seq, buf + DATA_HDR, (size_t)n - DATA_HDR, now, 1);
            } else if (buf[0] == TYPE_PARITY && n >= PARITY_HDR) {
                uint32_t base;
                memcpy(&base, buf + 1, 4);
                base = ntohl(base);
                int win = buf[5];
                g_par_pkts++;
                handle_parity(base, win, buf + PARITY_HDR);
            }
        }
        nack_sweep(now); /* every wakeup: issue NACKs for the FEC residual */
    }
    write_stats(duration_s);
    return 0;
}
