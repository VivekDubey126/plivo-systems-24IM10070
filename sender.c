#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#define RING 1024
#define RING_MASK (RING - 1)

static uint8_t g_payload[RING][160];
static bool    g_valid[RING];

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

int main() {
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
    memset(g_valid, 0, sizeof(g_valid));

    int src_fd  = make_udp(); bind_on(src_fd, 47010);
    int nack_fd = make_udp(); bind_on(nack_fd, 47004);
    int out_fd  = make_udp();
    
    struct sockaddr_in relay = addr_for(47001);

    uint8_t ibuf[512];
    uint8_t obuf[324];

    int max_fd = (src_fd > nack_fd) ? src_fd : nack_fd;

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(src_fd, &fds);
        FD_SET(nack_fd, &fds);

        int ready = select(max_fd + 1, &fds, NULL, NULL, NULL);
        if (ready < 0) continue;

        // Process incoming NACKs first (highest priority)
        if (FD_ISSET(nack_fd, &fds)) {
            while (true) {
                // Non-blocking drain loop
                struct timeval tv = {0, 0};
                fd_set read_check;
                FD_ZERO(&read_check);
                FD_SET(nack_fd, &read_check);
                if (select(nack_fd + 1, &read_check, NULL, NULL, &tv) <= 0) break;

                int n = recvfrom(nack_fd, ibuf, sizeof(ibuf), 0, NULL, NULL);
                if (n < 4) break;
                
                uint32_t seq = read_be32(ibuf);
                int slot = seq & RING_MASK;
                
                if (g_valid[slot]) {
                    uint8_t raw[164];
                    write_be32(raw, seq);
                    memcpy(raw + 4, g_payload[slot], 160);
                    sendto(out_fd, raw, 164, 0, (struct sockaddr*)&relay, sizeof(relay));
                }
            }
        }

        // Process incoming fresh frames
        if (FD_ISSET(src_fd, &fds)) {
            int n = recvfrom(src_fd, ibuf, sizeof(ibuf), 0, NULL, NULL);
            if (n == 164) {
                uint32_t seq  = read_be32(ibuf);
                uint8_t* pay  = ibuf + 4;
                int      slot = seq & RING_MASK;

                // Cache frame
                memcpy(g_payload[slot], pay, 160);
                g_valid[slot] = true;

                // Build sliding XOR matrix for burst protection
                uint8_t xb[160];
                memset(xb, 0, 160);
                if (seq >= 1) {
                    int s1 = (seq - 1) & RING_MASK;
                    if (g_valid[s1])
                        for (int k = 0; k < 160; k++) xb[k] ^= g_payload[s1][k];
                }
                if (seq >= 2) {
                    int s2 = (seq - 2) & RING_MASK;
                    if (g_valid[s2])
                        for (int k = 0; k < 160; k++) xb[k] ^= g_payload[s2][k];
                }

                // 95% FEC rate (gives ~1.97x overhead)
                bool send_fec = (seq % 20 != 0); 
                int  plen     = 164;
                
                write_be32(obuf, seq);
                memcpy(obuf + 4, pay, 160);
                
                if (send_fec) {
                    memcpy(obuf + 164, xb, 160);
                    plen = 324;
                }
                
                sendto(out_fd, obuf, plen, 0, (struct sockaddr*)&relay, sizeof(relay));
            }
        }
    }
    return 0;
}
