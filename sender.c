#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_HISTORY 65536
#define PAYLOAD_SIZE 160

struct frame {
    int valid;
    unsigned char data[4 + PAYLOAD_SIZE];
};

struct frame history[MAX_HISTORY];

int main(void) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr;
    memset(&in_addr, 0, sizeof(in_addr));
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr));

    int feedback_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in feedback_addr;
    memset(&feedback_addr, 0, sizeof(feedback_addr));
    feedback_addr.sin_family = AF_INET;
    feedback_addr.sin_port = htons(47004);
    feedback_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(feedback_fd, (struct sockaddr *)&feedback_addr, sizeof(feedback_addr));

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay;
    memset(&relay, 0, sizeof(relay));
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    for (int i = 0; i < MAX_HISTORY; i++) {
        history[i].valid = 0;
    }

    int max_fd = in_fd > feedback_fd ? in_fd : feedback_fd;

    unsigned char buf[2048];
    for (;;) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(in_fd, &read_fds);
        FD_SET(feedback_fd, &read_fds);

        int p = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (p < 0) continue;

        if (FD_ISSET(in_fd, &read_fds)) {
            ssize_t n = recvfrom(in_fd, (char*)buf, sizeof buf, 0, NULL, NULL);
            if (n > 0) {
                if (n >= 4) {
                    uint32_t seq;
                    memcpy(&seq, buf, 4);
                    seq = ntohl(seq);
                    
                    int idx = seq % MAX_HISTORY;
                    history[idx].valid = 1;
                    memcpy(history[idx].data, buf, n);
                }
                sendto(out_fd, (char*)buf, (int)n, 0, (struct sockaddr *)&relay, sizeof(relay));
            }
        }
        if (FD_ISSET(feedback_fd, &read_fds)) {
            ssize_t n = recvfrom(feedback_fd, (char*)buf, sizeof buf, 0, NULL, NULL);
            if (n == 4) {
                uint32_t seq;
                memcpy(&seq, buf, 4);
                seq = ntohl(seq);

                int idx = seq % MAX_HISTORY;
                if (history[idx].valid) {
                    sendto(out_fd, (char*)history[idx].data, 4 + PAYLOAD_SIZE, 0, (struct sockaddr *)&relay, sizeof(relay));
                }
            }
        }
    }
    return 0;
}
