#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
typedef int ssize_t;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#define closesocket close
#endif

uint8_t history[1024][160];

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr;
    memset(&in_addr, 0, sizeof(in_addr));
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr));

    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in fb_addr;
    memset(&fb_addr, 0, sizeof(fb_addr));
    fb_addr.sin_family = AF_INET;
    fb_addr.sin_port = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(fb_fd, (struct sockaddr *)&fb_addr, sizeof(fb_addr));

#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fb_fd, FIONBIO, &mode);
#else
    fcntl(fb_fd, F_SETFL, O_NONBLOCK);
#endif

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay;
    memset(&relay, 0, sizeof(relay));
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    uint32_t current_seq = 0;
    int32_t bytes_sent_budget = 500;
    
    uint32_t feedback_ack_seq = 0;
    uint32_t feedback_missing_mask = 0;

    unsigned char buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, (char*)buf, sizeof(buf), 0, NULL, NULL);
        if (n <= 0) continue;

        if (n >= 164) {
            uint32_t net_seq;
            memcpy(&net_seq, buf, 4);
            current_seq = ntohl(net_seq);
            memcpy(history[current_seq % 1024], buf + 4, 160);
        } else {
            continue;
        }

        bytes_sent_budget += 315;

        unsigned char fb_buf[64];
        for (;;) {
            ssize_t fb_n = recvfrom(fb_fd, (char*)fb_buf, sizeof(fb_buf), 0, NULL, NULL);
            if (fb_n == 8) {
                uint32_t ack_net, mask_net;
                memcpy(&ack_net, fb_buf, 4);
                memcpy(&mask_net, fb_buf + 4, 4);
                feedback_ack_seq = ntohl(ack_net);
                feedback_missing_mask = ntohl(mask_net);
            } else {
                break;
            }
        }

        int32_t target_extra_seq = -1;
        if (feedback_missing_mask != 0) {
            for (int i = 0; i < 32; i++) {
                if (feedback_missing_mask & (1 << i)) {
                    int32_t candidate = feedback_ack_seq + 1 + i;
                    if (candidate >= 0 && (int32_t)current_seq - candidate < 100 && candidate < (int32_t)current_seq) {
                        target_extra_seq = candidate;
                        feedback_missing_mask &= ~(1 << i);
                        break;
                    }
                }
            }
        }

        if (target_extra_seq == -1 && current_seq > 0) {
            target_extra_seq = current_seq - 1;
        }

        uint8_t pkt[400];
        pkt[0] = current_seq & 0x7F;
        memcpy(pkt + 1, history[current_seq % 1024], 160);
        int len = 161;

        if (bytes_sent_budget >= 322 && target_extra_seq >= 0) {
            pkt[0] |= 0x80;
            pkt[161] = target_extra_seq & 0x7F;
            memcpy(pkt + 162, history[target_extra_seq % 1024], 160);
            len = 322;
        }

        sendto(out_fd, (char*)pkt, len, 0, (struct sockaddr *)&relay, sizeof(relay));
        bytes_sent_budget -= len;
    }
    return 0;
}
