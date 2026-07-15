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
#include <unistd.h>
#endif

static const int BUF_SIZE  = 1024;
static const int BUF_MASK  = BUF_SIZE - 1;
static const int NACK_WAIT = 30;

struct RxSlot {
    uint8_t  payload[160];
    int64_t  nack_time;
    bool     received;
};

static RxSlot jb[BUF_SIZE];

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

static void net_init() {
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
}

static int make_udp() {
    return (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

static void bind_on(int fd, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr*)&a, sizeof(a));
}

static struct sockaddr_in addr_for(uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

static uint32_t read_be32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return ntohl(v);
}

static void write_be32(uint8_t* p, uint32_t v) {
    v = htonl(v);
    memcpy(p, &v, 4);
}

int main() {
    net_init();
    memset(jb, 0, sizeof(jb));

    int in_fd     = make_udp();  bind_on(in_fd, 47002);
    int player_fd = make_udp();
    int nack_fd   = make_udp();
    struct sockaddr_in player_addr = addr_for(47020);
    struct sockaddr_in nack_addr   = addr_for(47003);

    int32_t next_deliver = -1;
    int32_t highest      = -1;

    uint8_t in_buf[512];
    uint8_t play_buf[164];
    uint8_t nack_buf[4];

    int64_t last_check = now_ms();

    auto store = [&](uint32_t seq, const uint8_t* pay) {
        if (next_deliver == -1) next_deliver = (int32_t)seq;
        if ((int32_t)seq < next_deliver) return;
        int s = (int)(seq & (uint32_t)BUF_MASK);
        if (!jb[s].received) {
            jb[s].received  = true;
            jb[s].nack_time = 0;
            memcpy(jb[s].payload, pay, 160);
        }
        if ((int32_t)seq > highest) highest = (int32_t)seq;
    };

    while (true) {
        int64_t now = now_ms();
        int wait_ms = 10 - (int)(now - last_check);
        if (wait_ms < 0) wait_ms = 0;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in_fd, &rfds);
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = wait_ms * 1000;

        if (select(in_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
            int n = recvfrom(in_fd, (char*)in_buf, sizeof(in_buf), 0, NULL, NULL);

            if (n == 324) {
                uint32_t cs = read_be32(in_buf);
                store(cs, in_buf + 4);
                if (cs > 0) {
                    store(cs - 1, in_buf + 164);
                }
                for (int32_t s = next_deliver; s < (int32_t)cs; s++) {
                    int slot = s & BUF_MASK;
                    if (!jb[slot].received && jb[slot].nack_time == 0) {
                        jb[slot].nack_time = now_ms();
                        write_be32(nack_buf, (uint32_t)s);
                        sendto(nack_fd, (char*)nack_buf, 4, 0,
                               (struct sockaddr*)&nack_addr, sizeof(nack_addr));
                    }
                }
            } else if (n == 164) {
                store(read_be32(in_buf), in_buf + 4);
            }
        }

        while (next_deliver != -1 && jb[next_deliver & BUF_MASK].received) {
            int s = next_deliver & BUF_MASK;
            write_be32(play_buf, (uint32_t)next_deliver);
            memcpy(play_buf + 4, jb[s].payload, 160);
            sendto(player_fd, (char*)play_buf, 164, 0,
                   (struct sockaddr*)&player_addr, sizeof(player_addr));
            jb[s].received  = false;
            jb[s].nack_time = 0;
            next_deliver++;
        }

        int64_t chk = now_ms();
        if (chk - last_check >= 10 && next_deliver != -1 && highest > next_deliver) {
            for (int32_t s = next_deliver; s < highest; s++) {
                int slot = s & BUF_MASK;
                if (!jb[slot].received) {
                    if (jb[slot].nack_time == 0 || chk - jb[slot].nack_time >= NACK_WAIT) {
                        jb[slot].nack_time = chk;
                        write_be32(nack_buf, (uint32_t)s);
                        sendto(nack_fd, (char*)nack_buf, 4, 0,
                               (struct sockaddr*)&nack_addr, sizeof(nack_addr));
                    }
                }
            }
            last_check = chk;
        }
    }
    return 0;
}
