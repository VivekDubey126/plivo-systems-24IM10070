#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <sys/time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define MAX_HISTORY 65536
#define PAYLOAD_SIZE 160
#define NACK_TIMEOUT_MS 40

struct frame_state {
    int received;
    long long missing_time;
};

struct frame_state state[MAX_HISTORY];
int highest_seq = -1;

long long current_time_ms() {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long tt = ft.dwHighDateTime;
    tt <<= 32;
    tt |= ft.dwLowDateTime;
    tt /= 10000;
    return tt;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + (long long)tv.tv_usec / 1000;
#endif
}

int main(void) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr;
    memset(&in_addr, 0, sizeof(in_addr));
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr));

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player;
    memset(&player, 0, sizeof(player));
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    int feedback_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay_back;
    memset(&relay_back, 0, sizeof(relay_back));
    relay_back.sin_family = AF_INET;
    relay_back.sin_port = htons(47003);
    relay_back.sin_addr.s_addr = inet_addr("127.0.0.1");

    for (int i = 0; i < MAX_HISTORY; i++) {
        state[i].received = 0;
        state[i].missing_time = 0;
    }

    long long last_nack_check = current_time_ms();

    unsigned char buf[2048];
    for (;;) {
        long long now = current_time_ms();
        int wait_ms = 10 - (int)(now - last_nack_check);
        if (wait_ms < 0) wait_ms = 0;
        if (wait_ms > 10) wait_ms = 10;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(in_fd, &read_fds);

        struct timeval tv;
        tv.tv_sec = wait_ms / 1000;
        tv.tv_usec = (wait_ms % 1000) * 1000;

        int p = select(in_fd + 1, &read_fds, NULL, NULL, &tv);

        if (p > 0 && FD_ISSET(in_fd, &read_fds)) {
            ssize_t n = recvfrom(in_fd, (char*)buf, sizeof buf, 0, NULL, NULL);
            if (n >= 4) {
                uint32_t seq;
                memcpy(&seq, buf, 4);
                seq = ntohl(seq);

                int idx = seq % MAX_HISTORY;
                if (!state[idx].received) {
                    state[idx].received = 1;
                    sendto(out_fd, (char*)buf, (int)n, 0, (struct sockaddr *)&player, sizeof(player));

                    if ((int)seq > highest_seq) {
                        highest_seq = seq;
                    }
                }
            }
        }

        now = current_time_ms();
        if (now - last_nack_check >= 10 && highest_seq >= 0) {
            int start = highest_seq - 150;
            if (start < 0) start = 0;

            for (int i = start; i < highest_seq; i++) {
                int idx = i % MAX_HISTORY;
                if (!state[idx].received) {
                    if (state[idx].missing_time == 0) {
                        state[idx].missing_time = now;
                        uint32_t net_seq = htonl(i);
                        sendto(feedback_fd, (char*)&net_seq, 4, 0, (struct sockaddr *)&relay_back, sizeof(relay_back));
                    } else if (now - state[idx].missing_time >= NACK_TIMEOUT_MS) {
                        state[idx].missing_time = now;
                        uint32_t net_seq = htonl(i);
                        sendto(feedback_fd, (char*)&net_seq, 4, 0, (struct sockaddr *)&relay_back, sizeof(relay_back));
                    }
                }
            }
            last_nack_check = now;
        }
    }
    return 0;
}
