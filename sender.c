/* SENDER — backward sliding-window XOR FEC (rate 1:2, window 3) + ARQ backstop.
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source delivers frame i here at t0 + i*20ms
 *                  (harness format: 4-byte big-endian seq + 160-byte payload)
 *   send 47001  -> relay uplink toward the receiver (OUR wire format, below)
 *   bind 47004  <- NACK feedback from the receiver, via the relay (ARQ)
 *
 * FEC (unchanged from the FEC-only pass): every data frame is forwarded once,
 * plus a backward-looking parity every 2 frames over the last 3 frames
 * (parity_i = frame_i ^ frame_{i-1} ^ frame_{i-2}, emitted at even i).
 *
 * ARQ BACKSTOP (new): the sender keeps every payload it has forwarded (the
 * retransmission cache) and listens for NACKs on 47004. On a NACK for seq it
 * re-sends that frame as a RETX packet. This never changes the FEC data path;
 * it only reacts to NACKs, which the receiver issues solely for the residual
 * FEC cannot repair (double-loss-in-window) and only when a round trip still
 * fits before the deadline.
 *
 * OUR WIRE FORMAT on 47001:
 *   DATA   : [0x00][seq:  u32 BE][payload: 160B]                 = 165 bytes
 *   PARITY : [0x01][base: u32 BE][window: u8][xor_payload: 160B] = 166 bytes
 *   RETX   : [0x03][seq:  u32 BE][payload: 160B]                 = 165 bytes
 * NACK on 47003/47004 (receiver -> sender):
 *   NACK   : [0x02][seq:  u32 BE]                                = 5 bytes
 *
 * INSTRUMENTATION -> sender_stats.log at exit (voluntary exit before the
 * harness SIGKILL, window computed from T0/DURATION_S/DELAY_MS).
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD 160
#define FRAME_MS 20.0
#define FEC_WINDOW 3
#define PARITY_EVERY 2
#define TYPE_DATA 0
#define TYPE_PARITY 1
#define TYPE_NACK 2
#define TYPE_RETX 3
#define DATA_HDR 5
#define PARITY_HDR 6
#define NACK_LEN 5
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
static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ---- run-wide state ---- */
static unsigned char *g_frames = NULL; /* retransmission cache: payload by seq */
static uint8_t *g_have = NULL;          /* seq is present in the cache          */
static int g_n_expected = 0;
static long g_data_sent = 0, g_parity_sent = 0;
static long g_data_bytes = 0, g_parity_bytes = 0;
static long g_nacks_recv = 0, g_retx_sent = 0, g_retx_bytes = 0;

static int g_out_fd;
static struct sockaddr_in g_relay;

static void send_to_relay(const unsigned char *buf, size_t len, int kind) {
    sendto(g_out_fd, buf, len, 0, (struct sockaddr *)&g_relay, sizeof g_relay);
    if (kind == TYPE_PARITY)   { g_parity_sent++; g_parity_bytes += (long)len; }
    else if (kind == TYPE_RETX){ g_retx_sent++;   g_retx_bytes   += (long)len; }
    else                       { g_data_sent++;   g_data_bytes   += (long)len; }
}

/* FEC encode: XOR the last FEC_WINDOW payloads ending at `base`. Unchanged. */
static void emit_parity(uint32_t base) {
    unsigned char pkt[PARITY_HDR + PAYLOAD];
    pkt[0] = TYPE_PARITY;
    uint32_t nb = htonl(base);
    memcpy(pkt + 1, &nb, 4);
    pkt[5] = (unsigned char)FEC_WINDOW;
    unsigned char *xorbuf = pkt + PARITY_HDR;
    memcpy(xorbuf, g_frames + (size_t)base * PAYLOAD, PAYLOAD);
    for (int k = 1; k < FEC_WINDOW; k++) {
        const unsigned char *f = g_frames + (size_t)(base - k) * PAYLOAD;
        for (int b = 0; b < PAYLOAD; b++) xorbuf[b] ^= f[b];
    }
    send_to_relay(pkt, sizeof pkt, TYPE_PARITY);
}

/* ARQ: re-send a cached frame in response to a NACK. */
static void handle_nack(uint32_t seq) {
    g_nacks_recv++;
    if (seq >= (uint32_t)g_n_expected || !g_have[seq]) return;
    unsigned char pkt[DATA_HDR + PAYLOAD];
    pkt[0] = TYPE_RETX;
    uint32_t nb = htonl(seq);
    memcpy(pkt + 1, &nb, 4);
    memcpy(pkt + DATA_HDR, g_frames + (size_t)seq * PAYLOAD, PAYLOAD);
    send_to_relay(pkt, sizeof pkt, TYPE_RETX);
}

static void write_stats(double t0, double duration_s, double delay_ms) {
    FILE *f = fopen("sender_stats.log", "w");
    if (!f) { perror("fopen sender_stats.log"); return; }
    long raw = (long)g_n_expected * PAYLOAD;
    long total = g_data_bytes + g_parity_bytes + g_retx_bytes;
    fprintf(f, "# sender_stats.log — sliding XOR FEC (r=1:2, w=3) + ARQ backstop\n");
    fprintf(f, "# T0=%.6f DURATION_S=%.3f DELAY_MS=%.3f generated_at=%.6f\n",
            t0, duration_s, delay_ms, now_epoch());
    fprintf(f, "# ---- SUMMARY ----\n");
    fprintf(f, "frames_expected: %d\n", g_n_expected);
    fprintf(f, "data_packets_sent: %ld (%ld bytes)\n", g_data_sent, g_data_bytes);
    fprintf(f, "parity_packets_sent: %ld (%ld bytes)\n", g_parity_sent, g_parity_bytes);
    fprintf(f, "nacks_received: %ld\n", g_nacks_recv);
    fprintf(f, "retransmissions_sent: %ld (%ld bytes)\n", g_retx_sent, g_retx_bytes);
    fprintf(f, "total_uplink_bytes(sender view): %ld\n", total);
    fprintf(f, "raw_stream_bytes(n*160): %ld\n", raw);
    fprintf(f, "uplink_overhead(sender view, x): %.4f  "
               "[relay_stats.json authoritative; also counts feedback lane]\n",
            raw ? (double)total / raw : 0.0);
    fclose(f);
}

int main(void) {
    double t0 = getenv_double("T0", now_epoch());
    double duration_s = getenv_double("DURATION_S", 30.0);
    double delay_ms = getenv_double("DELAY_MS", 60.0);
    g_n_expected = (int)(duration_s * 1000.0 / FRAME_MS + 1e-6);
    if (g_n_expected < 1) g_n_expected = 1;
    double end_time = t0 + delay_ms / 1000.0
                       + (g_n_expected - 1) * FRAME_MS / 1000.0 + END_MARGIN_S;

    g_frames = calloc((size_t)g_n_expected, PAYLOAD);
    g_have   = calloc((size_t)g_n_expected, 1);
    if (!g_frames || !g_have) { perror("calloc"); return 1; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47010");
        return 1;
    }

    /* NACK feedback socket */
    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in fb_addr = {0};
    fb_addr.sin_family = AF_INET;
    fb_addr.sin_port = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fb_fd, (struct sockaddr *)&fb_addr, sizeof fb_addr) < 0)
        perror("bind 47004"); /* non-fatal: FEC still runs without ARQ */

    set_nonblock(in_fd);
    set_nonblock(fb_fd);

    g_out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    g_relay.sin_family = AF_INET;
    g_relay.sin_port = htons(47001);
    g_relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char buf[2048];
    int maxfd = (in_fd > fb_fd ? in_fd : fb_fd) + 1;
    while (!g_stop && now_epoch() < end_time) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in_fd, &rfds);
        FD_SET(fb_fd, &rfds);
        struct timeval tv = {0, 100000}; /* 100ms wake to re-check end_time */
        int r = select(maxfd, &rfds, NULL, NULL, &tv);
        if (r <= 0) continue;

        /* NACKs first: they are time-critical */
        if (FD_ISSET(fb_fd, &rfds)) {
            ssize_t n;
            while ((n = recvfrom(fb_fd, buf, sizeof buf, 0, NULL, NULL)) > 0) {
                if (n >= NACK_LEN && buf[0] == TYPE_NACK) {
                    uint32_t seq;
                    memcpy(&seq, buf + 1, 4);
                    handle_nack(ntohl(seq));
                }
            }
        }

        if (FD_ISSET(in_fd, &rfds)) {
            ssize_t n;
            while ((n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL)) > 0) {
                if (n < 4) continue;
                uint32_t net_seq;
                memcpy(&net_seq, buf, 4);
                uint32_t seq = ntohl(net_seq);
                if (seq >= (uint32_t)g_n_expected) continue;

                size_t plen = (size_t)n - 4;
                if (plen > PAYLOAD) plen = PAYLOAD;
                memcpy(g_frames + (size_t)seq * PAYLOAD, buf + 4, plen);
                g_have[seq] = 1;

                unsigned char dpkt[DATA_HDR + PAYLOAD];
                dpkt[0] = TYPE_DATA;
                memcpy(dpkt + 1, &net_seq, 4);
                memcpy(dpkt + DATA_HDR, buf + 4, plen);
                send_to_relay(dpkt, DATA_HDR + plen, TYPE_DATA);

                if (seq >= (uint32_t)(FEC_WINDOW - 1) && seq % PARITY_EVERY == 0)
                    emit_parity(seq);
            }
        }
    }
    write_stats(t0, duration_s, delay_ms);
    return 0;
}
