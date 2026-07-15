#include <cstdint>
#include <cstring>
#include <atomic>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>
#include <unistd.h>
#endif

static const int RING      = 1024;
static const int RING_MASK = RING - 1;
static const int FEC_SKIP  = 25;

static uint8_t g_payload[RING][160];
static bool    g_valid[RING];

#ifdef _WIN32
static CRITICAL_SECTION g_cs;
#define LOCK()   EnterCriticalSection(&g_cs)
#define UNLOCK() LeaveCriticalSection(&g_cs)
#else
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&g_mtx)
#define UNLOCK() pthread_mutex_unlock(&g_mtx)
#endif

static int             g_out_fd;
static struct sockaddr_in g_relay;

static void net_init() {
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    InitializeCriticalSection(&g_cs);
#endif
}

static int make_udp() {
    int fd = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
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

static uint32_t read_be32(const uint8_t* p) {
    uint32_t v; memcpy(&v, p, 4); return ntohl(v);
}
static void write_be32(uint8_t* p, uint32_t v) {
    v = htonl(v); memcpy(p, &v, 4);
}

#ifdef _WIN32
static DWORD WINAPI nack_thread(LPVOID) {
#else
static void* nack_thread(void*) {
#endif
    int fd = make_udp(); bind_on(fd, 47004);
    uint8_t ibuf[16], obuf[164];
    while (true) {
        int n = recvfrom(fd, (char*)ibuf, sizeof(ibuf), 0, NULL, NULL);
        if (n < 4) continue;
        uint32_t seq  = read_be32(ibuf);
        int      slot = seq & RING_MASK;
        LOCK();
        bool valid = g_valid[slot];
        if (valid) {
            write_be32(obuf, seq);
            memcpy(obuf + 4, g_payload[slot], 160);
        }
        UNLOCK();
        if (valid)
            sendto(g_out_fd, (char*)obuf, 164, 0,
                   (struct sockaddr*)&g_relay, sizeof(g_relay));
    }
#ifndef _WIN32
    return nullptr;
#else
    return 0;
#endif
}

int main() {
    net_init();
    memset(g_valid, 0, sizeof(g_valid));

    int src_fd = make_udp(); bind_on(src_fd, 47010);
    g_out_fd   = make_udp();
    g_relay    = addr_for(47001);

#ifdef _WIN32
    CreateThread(NULL, 0, nack_thread, NULL, 0, NULL);
#else
    pthread_t t; pthread_create(&t, NULL, nack_thread, NULL); pthread_detach(t);
#endif

    uint8_t ibuf[512];
    uint8_t obuf[324];

    while (true) {
        int n = recvfrom(src_fd, (char*)ibuf, sizeof(ibuf), 0, NULL, NULL);
        if (n != 164) continue;

        uint32_t seq  = read_be32(ibuf);
        uint8_t* pay  = ibuf + 4;
        int      slot = seq & RING_MASK;

        uint8_t xb[160];
        LOCK();
        memcpy(g_payload[slot], pay, 160);
        g_valid[slot] = true;
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
        UNLOCK();

        bool send_fec = (seq % FEC_SKIP != 0);
        int  plen     = 164;
        write_be32(obuf, seq);
        memcpy(obuf + 4, pay, 160);
        if (send_fec) {
            memcpy(obuf + 164, xb, 160);
            plen = 324;
        }
        sendto(g_out_fd, (char*)obuf, plen, 0,
               (struct sockaddr*)&g_relay, sizeof(g_relay));
    }
    return 0;
}
