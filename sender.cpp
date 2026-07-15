#include <cstdint>
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#endif

static const int RING_SIZE = 1024;
static const int RING_MASK = RING_SIZE - 1;
static const int FEC_SKIP  = 10;

struct RingSlot {
    uint8_t payload[160];
    uint32_t seq;
    bool valid;
};

static RingSlot ring[RING_SIZE];

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
    memset(ring, 0, sizeof(ring));

    int src_fd  = make_udp();  bind_on(src_fd,  47010);
    int nack_fd = make_udp();  bind_on(nack_fd, 47004);
    int out_fd  = make_udp();
    struct sockaddr_in relay = addr_for(47001);

    uint8_t in_buf[512];
    uint8_t out_buf[324];
    int top = src_fd > nack_fd ? src_fd : nack_fd;

    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(src_fd,  &rfds);
        FD_SET(nack_fd, &rfds);
        if (select(top + 1, &rfds, NULL, NULL, NULL) <= 0) continue;

        if (FD_ISSET(src_fd, &rfds)) {
            int n = recvfrom(src_fd, (char*)in_buf, sizeof(in_buf), 0, NULL, NULL);
            if (n != 164) continue;

            uint32_t seq = read_be32(in_buf);
            uint8_t* pay = in_buf + 4;

            int slot = (int)(seq & (uint32_t)RING_MASK);
            ring[slot].seq   = seq;
            ring[slot].valid = true;
            memcpy(ring[slot].payload, pay, 160);

            int len = 0;
            write_be32(out_buf + len, seq);   len += 4;
            memcpy(out_buf + len, pay, 160);  len += 160;

            bool send_fec = (seq > 0) && (seq % FEC_SKIP != 0);
            if (send_fec) {
                int ps = (int)((seq - 1) & (uint32_t)RING_MASK);
                if (ring[ps].valid && ring[ps].seq == seq - 1) {
                    memcpy(out_buf + len, ring[ps].payload, 160);
                    len += 160;
                }
            }

            sendto(out_fd, (char*)out_buf, len, 0,
                   (struct sockaddr*)&relay, sizeof(relay));
        }

        if (FD_ISSET(nack_fd, &rfds)) {
            int n = recvfrom(nack_fd, (char*)in_buf, sizeof(in_buf), 0, NULL, NULL);
            if (n < 4) continue;

            uint32_t want = read_be32(in_buf);
            int slot = (int)(want & (uint32_t)RING_MASK);

            if (ring[slot].valid && ring[slot].seq == want) {
                write_be32(out_buf, want);
                memcpy(out_buf + 4, ring[slot].payload, 160);
                sendto(out_fd, (char*)out_buf, 164, 0,
                       (struct sockaddr*)&relay, sizeof(relay));
            }
        }
    }
    return 0;
}
