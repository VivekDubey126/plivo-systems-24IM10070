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
#define closesocket close
#endif

bool played_frames[1000000] = {0};

int32_t unwrap_seq(uint8_t seq7, int32_t expected) {
    int32_t diff = (int32_t)seq7 - (expected & 0x7F);
    if (diff < -64) diff += 128;
    else if (diff > 64) diff -= 128;
    return expected + diff;
}

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr;
    memset(&in_addr, 0, sizeof(in_addr));
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof(in_addr));

    int player_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player;
    memset(&player, 0, sizeof(player));
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in fb_relay;
    memset(&fb_relay, 0, sizeof(fb_relay));
    fb_relay.sin_family = AF_INET;
    fb_relay.sin_port = htons(47003);
    fb_relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    int32_t max_received_seq = 0;
    int pkts_recv = 0;

    unsigned char buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, (char*)buf, sizeof(buf), 0, NULL, NULL);
        if (n <= 0) continue;
        if (n < 161) continue;

        uint8_t seq7 = buf[0] & 0x7F;
        bool has_extra = (buf[0] & 0x80) != 0;

        int32_t primary_seq = unwrap_seq(seq7, max_received_seq);
        if (primary_seq > max_received_seq) {
            max_received_seq = primary_seq;
        }

        if (primary_seq >= 0 && primary_seq < 1000000 && !played_frames[primary_seq]) {
            played_frames[primary_seq] = true;
            uint8_t out_pkt[164];
            uint32_t net_seq = htonl((uint32_t)primary_seq);
            memcpy(out_pkt, &net_seq, 4);
            memcpy(out_pkt + 4, buf + 1, 160);
            sendto(player_fd, (char*)out_pkt, 164, 0, (struct sockaddr *)&player, sizeof(player));
        }

        if (has_extra && n >= 322) {
            uint8_t extra_seq7 = buf[161] & 0x7F;
            int32_t extra_seq = unwrap_seq(extra_seq7, max_received_seq);
            if (extra_seq >= 0 && extra_seq < 1000000 && !played_frames[extra_seq]) {
                played_frames[extra_seq] = true;
                uint8_t out_pkt[164];
                uint32_t net_seq = htonl((uint32_t)extra_seq);
                memcpy(out_pkt, &net_seq, 4);
                memcpy(out_pkt + 4, buf + 162, 160);
                sendto(player_fd, (char*)out_pkt, 164, 0, (struct sockaddr *)&player, sizeof(player));
            }
        }

        pkts_recv++;
        if (pkts_recv % 4 == 0) {
            static int32_t contiguous_ack = 0;
            while (contiguous_ack < 1000000 && played_frames[contiguous_ack]) {
                contiguous_ack++;
            }
            int32_t highest_contiguous = contiguous_ack - 1;

            uint32_t missing_mask = 0;
            for (int i = 0; i < 32; i++) {
                int32_t check_seq = highest_contiguous + 1 + i;
                if (check_seq >= 0 && check_seq < 1000000 && check_seq <= max_received_seq) {
                    if (!played_frames[check_seq]) {
                        missing_mask |= (1 << i);
                    }
                }
            }

            uint32_t fb[2];
            fb[0] = htonl((uint32_t)highest_contiguous);
            fb[1] = htonl(missing_mask);
            sendto(fb_fd, (char*)fb, 8, 0, (struct sockaddr *)&fb_relay, sizeof(fb_relay));
        }
    }
    return 0;
}
