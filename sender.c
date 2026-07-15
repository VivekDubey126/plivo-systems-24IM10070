/*
 * sender.c  –  Elite POSIX C UDP Sender
 *
 * Architecture
 * ============
 *  • Single-threaded select() event loop: zero mutex, zero context-switch jitter.
 *  • Wire protocol: 2-byte (uint16_t) big-endian seq + 160-byte payload
 *                   + optional 160-byte XOR(i-1 ⊕ i-2) FEC  = 322 bytes.
 *  • FEC is emitted for 4 out of every 5 frames (80 % rate).
 *    Baseline overhead = (322×0.8 + 162×0.2) / 160 = 1.81×, leaving a
 *    comfortable 0.19× budget for NACK retransmissions.
 *  • NACK response: 2-byte NACK received on port 47004 → retransmit the
 *    requested frame as a 162-byte packet instantly.
 *  • Zero heap allocation: all frame storage uses a pre-allocated ring of
 *    1 024 slots indexed by `seq & RING_MASK` (bitwise mask, not modulo).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <winsock2.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <sys/select.h>
#  include <unistd.h>
#endif

#define RING      1024
#define RING_MASK (RING - 1)
#define FEC_SKIP  5          /* one plain frame every 5 → 80 % FEC rate */

static uint8_t g_payload[RING][160];
static bool    g_valid  [RING];

/* ── socket helpers ─────────────────────────────────────────────────────── */

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

/* ── endian helpers ─────────────────────────────────────────────────────── */

static uint32_t r32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4); return ntohl(v);
}
static void w16(uint8_t *p, uint16_t v) {
    v = htons(v); memcpy(p, &v, 2);
}
static uint16_t r16(const uint8_t *p) {
    uint16_t v; memcpy(&v, p, 2); return ntohs(v);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
    memset(g_valid, 0, sizeof(g_valid));

    int src_fd  = make_udp(); bind_on(src_fd,  47010);
    int nack_fd = make_udp(); bind_on(nack_fd, 47004);
    int out_fd  = make_udp();

    struct sockaddr_in relay = addr_for(47001);

    uint8_t ibuf[512];
    uint8_t obuf[322];

    int maxfd = (src_fd > nack_fd) ? src_fd : nack_fd;

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(src_fd,  &fds);
        FD_SET(nack_fd, &fds);

        select(maxfd + 1, &fds, NULL, NULL, NULL);

        /*
         * Priority 1 – drain NACK socket.
         * A NACK means the receiver already knows a packet is missing and is
         * waiting for retransmission. Service it before ingesting new frames.
         */
        if (FD_ISSET(nack_fd, &fds)) {
            for (;;) {
                struct timeval z = {0, 0};
                fd_set tmp; FD_ZERO(&tmp); FD_SET(nack_fd, &tmp);
                if (select(nack_fd + 1, &tmp, NULL, NULL, &z) <= 0) break;

                int n = recvfrom(nack_fd, (char*)ibuf, sizeof(ibuf), 0, NULL, NULL);
                if (n < 2) break;

                uint16_t seq  = r16(ibuf);
                int      slot = seq & RING_MASK;
                if (g_valid[slot]) {
                    /* Retransmit: 2-byte seq + 160-byte payload = 162 bytes */
                    w16(obuf, seq);
                    memcpy(obuf + 2, g_payload[slot], 160);
                    sendto(out_fd, (const char*)obuf, 162, 0,
                           (struct sockaddr*)&relay, sizeof(relay));
                }
            }
        }

        /*
         * Priority 2 – ingest new source frame.
         * Source delivers 164 bytes: 4-byte big-endian seq + 160-byte payload.
         */
        if (FD_ISSET(src_fd, &fds)) {
            int n = recvfrom(src_fd, (char*)ibuf, sizeof(ibuf), 0, NULL, NULL);
            if (n == 164) {
                uint32_t seq32 = r32(ibuf);
                uint16_t seq16 = (uint16_t)seq32;
                uint8_t *pay   = ibuf + 4;
                int      slot  = seq16 & RING_MASK;

                /* Cache payload */
                memcpy(g_payload[slot], pay, 160);
                g_valid[slot] = true;

                /*
                 * Burst-Breaker FEC = XOR(payload[i-1], payload[i-2])
                 * Surviving two consecutive drops with zero ARQ delay.
                 */
                uint8_t xb[160];
                memset(xb, 0, 160);
                if (seq32 >= 1) {
                    int s1 = (seq16 - 1) & RING_MASK;
                    if (g_valid[s1])
                        for (int k = 0; k < 160; k++) xb[k] ^= g_payload[s1][k];
                }
                if (seq32 >= 2) {
                    int s2 = (seq16 - 2) & RING_MASK;
                    if (g_valid[s2])
                        for (int k = 0; k < 160; k++) xb[k] ^= g_payload[s2][k];
                }

                /*
                 * 80 % FEC rate: every 5th frame is sent plain (162 bytes)
                 * to preserve bandwidth budget for retransmissions.
                 */
                bool fec  = (seq32 % FEC_SKIP != 0);
                int  plen;

                w16(obuf, seq16);
                memcpy(obuf + 2, pay, 160);
                if (fec) {
                    memcpy(obuf + 162, xb, 160);
                    plen = 322;
                } else {
                    plen = 162;
                }

                sendto(out_fd, (const char*)obuf, plen, 0,
                       (struct sockaddr*)&relay, sizeof(relay));
            }
        }
    }
    return 0;
}
