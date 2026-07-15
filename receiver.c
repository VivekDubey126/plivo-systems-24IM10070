/*
 * receiver.c  –  Elite POSIX C UDP Receiver
 *
 * Architecture
 * ============
 *  • Single-threaded select() event loop with a 5 ms poll timeout for the
 *    NACK feedback loop. Zero mutex, zero context-switch jitter.
 *  • Wire protocol: 322-byte FEC packets or 162-byte retransmit packets,
 *    both using 2-byte big-endian seq.
 *  • Instant dispatch: frame delivered to port 47020 as 164-byte
 *    (4-byte BE seq + 160-byte payload) the moment it is received or
 *    mathematically recovered via XOR FEC.
 *  • Kalman Filter (1D): tracks the true RTT, filtering out jitter spikes.
 *  • Adaptive ARQ Suppression: NACK suppressed if
 *    now + RTT_kalman * 1.05 > deadline, saving bandwidth on dead frames.
 *  • Token Bucket: caps NACK volume so total overhead stays ≤ 2.0×.
 *  • Reads DELAY_MS from the environment for precise deadline calculation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <sys/select.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

#define BUF          2048
#define BUF_MASK     (BUF - 1)
#define NACK_COOL_MS 20          /* per-frame NACK cooldown in ms          */
#define TOKEN_MAX    10.0        /* burst capacity in tokens               */
#define TOKEN_RATE   0.08        /* tokens per ms  (≈ 8 NACKs / 100 ms)   */

/* ── per-frame jitter buffer slot ───────────────────────────────────────── */

struct Slot {
    uint8_t payload[160];
    uint8_t fec    [160];
    int64_t nack_ms;
    bool    has_payload;
    bool    has_fec;
    bool    delivered;
};

static struct Slot g_jb[BUF];

static int32_t g_highest      = -1;
static int32_t g_next_deliver = -1;
static int64_t g_t0_ms        = -1;
static int     g_delay_ms     = 120;  /* read from env; safe default */

static int                g_player_fd;
static int                g_nack_fd;
static struct sockaddr_in g_player_addr;
static struct sockaddr_in g_nack_addr;

/* ── Kalman filter state ─────────────────────────────────────────────────── */

static double g_k_est = 20.0;   /* estimated RTT in ms */
static double g_k_p   = 5.0;    /* error covariance    */

/* ── Token bucket state ──────────────────────────────────────────────────── */

static double  g_tokens       = TOKEN_MAX;
static int64_t g_last_token_t = 0;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static int64_t now_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

static int make_udp(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int sz = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof(sz));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&sz, sizeof(sz));
    return fd;
}

static void bind_on(int fd, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr*)&a, sizeof(a));
}

static struct sockaddr_in addr_for(uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

static void w32(uint8_t *p, uint32_t v) { v = htonl(v); memcpy(p, &v, 4); }
static void w16(uint8_t *p, uint16_t v) { v = htons(v); memcpy(p, &v, 2); }
static uint16_t r16(const uint8_t *p) {
    uint16_t v; memcpy(&v, p, 2); return ntohs(v);
}

/* 1D Kalman filter update on a single RTT measurement */
static void kalman_update(double meas) {
    g_k_p   += 0.5;
    double K = g_k_p / (g_k_p + 4.0);
    g_k_est  = g_k_est + K * (meas - g_k_est);
    g_k_p   *= (1.0 - K);
}

/* ── instant frame dispatch ──────────────────────────────────────────────── */

static void deliver(int32_t seq) {
    if (seq < 0) return;
    int s = seq & BUF_MASK;
    if (g_jb[s].delivered || !g_jb[s].has_payload) return;
    g_jb[s].delivered = true;

    /* Harness requires: 4-byte big-endian seq + 160-byte payload */
    uint8_t out[164];
    w32(out, (uint32_t)seq);
    memcpy(out + 4, g_jb[s].payload, 160);
    sendto(g_player_fd, (const char*)out, 164, 0,
           (struct sockaddr*)&g_player_addr, sizeof(g_player_addr));
}

/* forward declaration */
static void xor_try(int32_t j);

/* Store a recovered or fresh payload, dispatch immediately */
static void store_payload(int32_t seq, const uint8_t *pay) {
    if (seq < 0) return;
    int s = seq & BUF_MASK;
    if (g_jb[s].has_payload) return;   /* duplicate guard */

    g_jb[s].has_payload = true;
    memcpy(g_jb[s].payload, pay, 160);

    if (seq > g_highest)    g_highest = seq;
    if (g_next_deliver < 0) g_next_deliver = seq;

    /* DP optimal policy: dispatch the moment a frame is valid */
    deliver(seq);

    /* Attempt XOR recovery of adjacent frames */
    xor_try(seq);
    if (seq >= 1) xor_try(seq - 1);
    if (seq >= 2) xor_try(seq - 2);
}

/*
 * xor_try(j):
 *   If we have FEC[j] = payload[j-1] XOR payload[j-2], and we have
 *   exactly one of {payload[j-1], payload[j-2]}, recover the other instantly.
 */
static void xor_try(int32_t j) {
    if (j < 2) return;
    int sj = j & BUF_MASK;
    if (!g_jb[sj].has_fec || !g_jb[sj].has_payload) return;

    int32_t a = j - 1, b = j - 2;
    int     sa = a & BUF_MASK, sb = b & BUF_MASK;
    bool    ha = g_jb[sa].has_payload;
    bool    hb = g_jb[sb].has_payload;

    if (!ha && hb) {
        /* Recover payload[j-1] = FEC[j] XOR payload[j-2] */
        uint8_t rec[160];
        for (int k = 0; k < 160; k++)
            rec[k] = g_jb[sj].fec[k] ^ g_jb[sb].payload[k];
        store_payload(a, rec);
    } else if (!hb && ha) {
        /* Recover payload[j-2] = FEC[j] XOR payload[j-1] */
        uint8_t rec[160];
        for (int k = 0; k < 160; k++)
            rec[k] = g_jb[sj].fec[k] ^ g_jb[sa].payload[k];
        store_payload(b, rec);
    }
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
    memset(g_jb, 0, sizeof(g_jb));

    /* Read playout delay from environment (set by the harness) */
    const char *env = getenv("DELAY_MS");
    if (env) g_delay_ms = atoi(env);

    int in_fd   = make_udp(); bind_on(in_fd, 47002);
    g_player_fd = make_udp();
    g_nack_fd   = make_udp();

    g_player_addr = addr_for(47020);
    g_nack_addr   = addr_for(47003);

    uint8_t ibuf[512];
    uint8_t nack_buf[2];

    g_last_token_t = now_ms();

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(in_fd, &fds);

        /* 5 ms timeout drives the NACK feedback loop */
        struct timeval tv = {0, 5000};
        select(in_fd + 1, &fds, NULL, NULL, &tv);

        int64_t now = now_ms();

        /* ── 1. Drain the network socket ─────────────────────────────── */
        if (FD_ISSET(in_fd, &fds)) {
            for (;;) {
                /* Non-blocking drain: keep reading until nothing left */
                struct timeval z = {0, 0};
                fd_set tmp; FD_ZERO(&tmp); FD_SET(in_fd, &tmp);
                if (select(in_fd + 1, &tmp, NULL, NULL, &z) <= 0) break;

                int n = recvfrom(in_fd, (char*)ibuf, sizeof(ibuf), 0, NULL, NULL);
                if (n != 322 && n != 162) break;

                uint16_t seq16 = r16(ibuf);
                int32_t  seq   = (int32_t)seq16;

                /* Initialise T0 on first packet */
                if (g_t0_ms < 0) g_t0_ms = now - (int64_t)seq * 20;

                /* Heuristic RTT sample from packet timing */
                int64_t expected = g_t0_ms + (int64_t)seq * 20;
                double  diff     = (double)(now - expected);
                if (diff > 0.0 && diff < 80.0) kalman_update(diff);

                int slot = seq & BUF_MASK;

                if (n == 322) {
                    /* FEC packet: 2-byte seq + 160-byte payload + 160-byte XOR */
                    if (!g_jb[slot].has_fec) {
                        g_jb[slot].has_fec = true;
                        memcpy(g_jb[slot].fec, ibuf + 162, 160);
                    }
                    store_payload(seq, ibuf + 2);
                    /* Try to recover neighbours using newly cached FEC */
                    xor_try(seq);
                    if (seq >= 1) xor_try(seq - 1);
                } else {
                    /* 162-byte retransmit or plain data packet */
                    store_payload(seq, ibuf + 2);
                }
            }
        }

        /* ── 2. Token bucket regeneration ───────────────────────────── */
        double dt = (double)(now - g_last_token_t);
        g_tokens += dt * TOKEN_RATE;
        if (g_tokens > TOKEN_MAX) g_tokens = TOKEN_MAX;
        g_last_token_t = now;

        /* ── 3. Preemptive NACK feedback loop ───────────────────────── */
        if (g_highest >= 0 && g_next_deliver >= 0 && g_t0_ms > 0) {
            for (int32_t s = g_next_deliver; s < g_highest; s++) {
                int slot = s & BUF_MASK;
                if (g_jb[slot].has_payload) continue;

                /* NACK cooldown: avoid re-requesting too soon */
                if (g_jb[slot].nack_ms > 0 &&
                    now - g_jb[slot].nack_ms < NACK_COOL_MS) continue;

                /* Token bucket: strictly cap total NACK volume */
                if (g_tokens < 1.0) break;

                /*
                 * Adaptive ARQ Suppression (Kalman-based):
                 * If the packet cannot physically arrive before its deadline
                 * even with an instant retransmit, do not waste bandwidth.
                 * deadline = T0 + delay_ms + seq × 20 ms
                 */
                int64_t deadline = g_t0_ms + (int64_t)g_delay_ms
                                   + (int64_t)s * 20;
                if ((double)now + g_k_est * 1.05 > (double)deadline) continue;

                /* Fire NACK: 2-byte big-endian seq */
                g_jb[slot].nack_ms = now;
                g_tokens -= 1.0;
                w16(nack_buf, (uint16_t)s);
                sendto(g_nack_fd, (const char*)nack_buf, 2, 0,
                       (struct sockaddr*)&g_nack_addr, sizeof(g_nack_addr));
            }
        }

        /* ── 4. Advance the delivery window ─────────────────────────── */
        while (g_next_deliver >= 0 &&
               g_next_deliver <= g_highest &&
               g_jb[g_next_deliver & BUF_MASK].delivered) {
            g_next_deliver++;
        }
    }
    return 0;
}
