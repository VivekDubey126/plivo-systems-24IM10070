#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#define BUF      2048
#define BUF_MASK (BUF - 1)
#define NACK_COOLDOWN_MS 20
#define TOKEN_BUCKET_MAX 10.0
#define TOKEN_RATE       0.05  // 5 NACKs per 100ms allowed

struct RxSlot {
    uint8_t payload[160];
    uint8_t fec[160];
    int64_t nack_ms;
    bool    has_payload;
    bool    has_fec;
    bool    delivered;
};

static struct RxSlot g_jb[BUF];

static int32_t g_highest      = -1;
static int32_t g_next_deliver = -1;
static int64_t g_t0_ms        = -1;

static int g_player_fd;
static int g_nack_fd;
static struct sockaddr_in g_player_addr;
static struct sockaddr_in g_nack_addr;

// --- Algorithmic Core State ---
static double g_kalman_est = 15.0; // Initial RTT estimate
static double g_kalman_p   = 5.0;  // Initial covariance
static double g_tokens     = TOKEN_BUCKET_MAX;
static int64_t g_last_token_ms = 0;

static int64_t now_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int make_udp() {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int sz = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
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

static uint32_t read_be32(const uint8_t* p) {
    uint32_t v; memcpy(&v, p, 4); return ntohl(v);
}
static void write_be32(uint8_t* p, uint32_t v) {
    v = htonl(v); memcpy(p, &v, 4);
}

static void deliver(int32_t seq) {
    if (seq < 0) return;
    int s = seq & BUF_MASK;
    if (g_jb[s].delivered || !g_jb[s].has_payload) return;
    g_jb[s].delivered = true;
    uint8_t out[164];
    write_be32(out, (uint32_t)seq);
    memcpy(out + 4, g_jb[s].payload, 160);
    sendto(g_player_fd, out, 164, 0, (struct sockaddr*)&g_player_addr, sizeof(g_player_addr));
}

static void xor_try(int32_t j);

static void store_payload(int32_t seq, const uint8_t* pay) {
    if (seq < 0) return;
    int s = seq & BUF_MASK;
    if (g_jb[s].has_payload) return;
    
    g_jb[s].has_payload = true;
    memcpy(g_jb[s].payload, pay, 160);
    
    if (seq > g_highest)      g_highest = seq;
    if (g_next_deliver < 0)   g_next_deliver = seq;
    
    // DP transition mapping: Dispatch immediately (optimal policy)
    deliver(seq);
    
    xor_try(seq);
    if (seq >= 1) xor_try(seq - 1);
    if (seq >= 2) xor_try(seq - 2);
}

static void xor_try(int32_t j) {
    if (j < 2) return;
    int sj = j & BUF_MASK;
    if (!g_jb[sj].has_fec || !g_jb[sj].has_payload) return;
    
    int32_t a = j - 1, b = j - 2;
    int sa = a & BUF_MASK, sb = b & BUF_MASK;
    bool ha = g_jb[sa].has_payload, hb = g_jb[sb].has_payload;
    
    if (!ha && hb) {
        uint8_t rec[160];
        for (int k = 0; k < 160; k++) rec[k] = g_jb[sj].fec[k] ^ g_jb[sb].payload[k];
        store_payload(a, rec);
    }
    if (!hb && (ha || g_jb[sa].has_payload)) {
        uint8_t rec[160];
        for (int k = 0; k < 160; k++) rec[k] = g_jb[sj].fec[k] ^ g_jb[sa].payload[k];
        store_payload(b, rec);
    }
}

// 1D Kalman Filter Update
static void kalman_update(double measurement) {
    double Q = 0.5; // Process variance
    double R = 4.0; // Measurement variance
    g_kalman_p = g_kalman_p + Q;
    double K = g_kalman_p / (g_kalman_p + R);
    g_kalman_est = g_kalman_est + K * (measurement - g_kalman_est);
    g_kalman_p = (1 - K) * g_kalman_p;
}

int main() {
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
    memset(g_jb, 0, sizeof(g_jb));

    int in_fd     = make_udp(); bind_on(in_fd, 47002);
    g_player_fd   = make_udp();
    g_nack_fd     = make_udp();
    
    g_player_addr = addr_for(47020);
    g_nack_addr   = addr_for(47003);

    uint8_t ibuf[512];
    uint8_t nack_buf[4];
    
    g_last_token_ms = now_ms();

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(in_fd, &fds);
        
        struct timeval tv = {0, 5000}; // 5ms timeout for async poll

        int ready = select(in_fd + 1, &fds, NULL, NULL, &tv);
        int64_t now = now_ms();

        // 1. Process Network Ingress (If any)
        if (ready > 0 && FD_ISSET(in_fd, &fds)) {
            while (true) {
                // Drain socket buffer
                fd_set read_check;
                FD_ZERO(&read_check);
                FD_SET(in_fd, &read_check);
                struct timeval zero_tv = {0, 0};
                if (select(in_fd + 1, &read_check, NULL, NULL, &zero_tv) <= 0) break;

                int n = recvfrom(in_fd, ibuf, sizeof(ibuf), 0, NULL, NULL);
                if (n != 324 && n != 164) break;

                uint32_t seq = read_be32(ibuf);
                
                if (g_t0_ms < 0) g_t0_ms = now - (int64_t)seq * 20;

                // RTT sample heuristic: If this is an expected sequence, we can estimate RTT
                int64_t expected_arrival = g_t0_ms + (int64_t)seq * 20;
                double diff = (double)(now - expected_arrival);
                if (diff > 0 && diff < 100) kalman_update(diff);

                int slot = seq & BUF_MASK;
                if (n == 324 && !g_jb[slot].has_fec) {
                    g_jb[slot].has_fec = true;
                    memcpy(g_jb[slot].fec, ibuf + 164, 160);
                }
                
                store_payload((int32_t)seq, ibuf + 4);
                
                if (n == 324) {
                    xor_try((int32_t)seq);
                    if (seq >= 1) xor_try((int32_t)seq - 1);
                }
            }
        }

        // 2. Token Bucket Regeneration
        double dt = (double)(now - g_last_token_ms);
        g_tokens += dt * TOKEN_RATE;
        if (g_tokens > TOKEN_BUCKET_MAX) g_tokens = TOKEN_BUCKET_MAX;
        g_last_token_ms = now;

        // 3. Adaptive ARQ Feedback Loop
        if (g_highest >= 0 && g_next_deliver >= 0) {
            for (int32_t s = g_next_deliver; s < g_highest; s++) {
                int slot = s & BUF_MASK;
                if (g_jb[slot].has_payload) continue;
                
                // Rate Limiting & Cooldown
                if (g_jb[slot].nack_ms > 0 && now - g_jb[slot].nack_ms < NACK_COOLDOWN_MS) continue;
                if (g_tokens < 1.0) break; // Out of bandwidth budget

                // Adaptive ARQ Suppression based on Kalman RTT
                int64_t deadline = g_t0_ms + (int64_t)s * 20 + 200; // Assuming max 200ms delay for threshold check
                if (g_t0_ms > 0 && (double)now + g_kalman_est * 1.2 > (double)deadline) {
                    // Packet will arrive too late anyway. Suppress NACK.
                    continue; 
                }

                g_jb[slot].nack_ms = now;
                g_tokens -= 1.0; // Consume token
                
                write_be32(nack_buf, (uint32_t)s);
                sendto(g_nack_fd, nack_buf, 4, 0, (struct sockaddr*)&g_nack_addr, sizeof(g_nack_addr));
            }
        }

        // Slide delivery window
        while (g_next_deliver >= 0 && g_next_deliver <= g_highest && g_jb[g_next_deliver & BUF_MASK].delivered) {
            g_next_deliver++;
        }
    }
    return 0;
}
