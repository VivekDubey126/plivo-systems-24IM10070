#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#endif

static const int  BUF      = 2048;
static const int  BUF_MASK = BUF - 1;
static const int  RTT_MS   = 15;
static const int  COOLDOWN = 25;

struct RxSlot {
    uint8_t payload[160];
    uint8_t fec[160];
    int64_t nack_ms;
    bool    has_payload;
    bool    has_fec;
    bool    delivered;
};

static RxSlot g_jb[BUF];

#ifdef _WIN32
static CRITICAL_SECTION g_cs;
#define LOCK()   EnterCriticalSection(&g_cs)
#define UNLOCK() LeaveCriticalSection(&g_cs)
#else
static pthread_mutex_t g_cs = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&g_cs)
#define UNLOCK() pthread_mutex_unlock(&g_cs)
#endif

static int32_t g_highest      = -1;
static int32_t g_next_deliver = -1;
static int64_t g_t0_ms        = -1;

static int             g_player_fd;
static int             g_nack_fd;
static struct sockaddr_in g_player_addr;
static struct sockaddr_in g_nack_addr;

static void net_init() {
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    InitializeCriticalSection(&g_cs);
#endif
}

static int64_t now_ms() {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)(t / 10000ULL);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
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

static void deliver_locked(int32_t seq) {
    if (seq < 0) return;
    int s = seq & BUF_MASK;
    if (g_jb[s].delivered || !g_jb[s].has_payload) return;
    g_jb[s].delivered = true;
    uint8_t out[164];
    write_be32(out, (uint32_t)seq);
    memcpy(out + 4, g_jb[s].payload, 160);
    UNLOCK();
    sendto(g_player_fd, (char*)out, 164, 0,
           (struct sockaddr*)&g_player_addr, sizeof(g_player_addr));
    LOCK();
}

static void xor_try(int32_t j);

static void store_locked(int32_t seq, const uint8_t* pay) {
    if (seq < 0) return;
    int s = seq & BUF_MASK;
    if (g_jb[s].has_payload) return;
    g_jb[s].has_payload = true;
    memcpy(g_jb[s].payload, pay, 160);
    if (seq > g_highest)      g_highest = seq;
    if (g_next_deliver < 0)   g_next_deliver = seq;
    deliver_locked(seq);
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
        for (int k = 0; k < 160; k++)
            rec[k] = g_jb[sj].fec[k] ^ g_jb[sb].payload[k];
        store_locked(a, rec);
    }
    if (!hb && (ha || g_jb[sa].has_payload)) {
        uint8_t rec[160];
        for (int k = 0; k < 160; k++)
            rec[k] = g_jb[sj].fec[k] ^ g_jb[sa].payload[k];
        store_locked(b, rec);
    }
}

#ifdef _WIN32
static DWORD WINAPI nack_thread(LPVOID) {
#else
static void* nack_thread(void*) {
#endif
    uint8_t nb[4];
    while (true) {
#ifdef _WIN32
        Sleep(5);
#else
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 5000;
        select(0, NULL, NULL, NULL, &tv);
#endif
        int64_t now = now_ms();
        LOCK();
        if (g_highest < 0 || g_next_deliver < 0) { UNLOCK(); continue; }
        int32_t hi  = g_highest;
        int32_t nd  = g_next_deliver;
        UNLOCK();

        for (int32_t s = nd; s < hi; s++) {
            int slot = s & BUF_MASK;
            LOCK();
            bool hp = g_jb[slot].has_payload;
            int64_t nt = g_jb[slot].nack_ms;
            UNLOCK();
            if (hp) continue;
            if (nt > 0 && now - nt < COOLDOWN) continue;
            int64_t t0 = g_t0_ms;
            if (t0 > 0) {
                int64_t deadline = t0 + (int64_t)s * 20 + 200;
                if (now + RTT_MS / 2 > deadline) continue;
            }
            LOCK();
            g_jb[slot].nack_ms = now;
            UNLOCK();
            write_be32(nb, (uint32_t)s);
            sendto(g_nack_fd, (char*)nb, 4, 0,
                   (struct sockaddr*)&g_nack_addr, sizeof(g_nack_addr));
        }

        LOCK();
        while (g_next_deliver >= 0 && g_next_deliver <= g_highest &&
               g_jb[g_next_deliver & BUF_MASK].delivered)
            g_next_deliver++;
        UNLOCK();
    }
#ifndef _WIN32
    return nullptr;
#else
    return 0;
#endif
}

int main() {
    net_init();
    memset(g_jb, 0, sizeof(g_jb));

    int in_fd     = make_udp(); bind_on(in_fd, 47002);
    g_player_fd   = make_udp();
    g_nack_fd     = make_udp();
    g_player_addr = addr_for(47020);
    g_nack_addr   = addr_for(47003);

#ifdef _WIN32
    CreateThread(NULL, 0, nack_thread, NULL, 0, NULL);
#else
    pthread_t t; pthread_create(&t, NULL, nack_thread, NULL); pthread_detach(t);
#endif

    uint8_t ibuf[512];

    while (true) {
        int n = recvfrom(in_fd, (char*)ibuf, sizeof(ibuf), 0, NULL, NULL);
        if (n != 324 && n != 164) continue;

        uint32_t seq = read_be32(ibuf);
        int64_t  now = now_ms();

        LOCK();
        if (g_t0_ms < 0) g_t0_ms = now - (int64_t)seq * 20;

        int slot = seq & BUF_MASK;
        if (n == 324 && !g_jb[slot].has_fec) {
            g_jb[slot].has_fec = true;
            memcpy(g_jb[slot].fec, ibuf + 164, 160);
        }
        store_locked((int32_t)seq, ibuf + 4);
        if (n == 324) {
            xor_try((int32_t)seq);
            if (seq >= 1) xor_try((int32_t)seq - 1);
        }
        UNLOCK();
    }
    return 0;
}
