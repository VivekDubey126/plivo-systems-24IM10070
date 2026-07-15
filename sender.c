#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <unistd.h>
#endif

#define MAX_HISTORY 65536
#define PAYLOAD_SIZE 160

// The sender keeps a rolling history of sent packets for ARQ retransmissions.
unsigned char history[MAX_HISTORY][PAYLOAD_SIZE];
int history_valid[MAX_HISTORY];

void setup_network_env() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

int main() {
    setup_network_env();

    // Socket to receive frames from the local source
    int src_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(47010);
    src_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(src_fd, (struct sockaddr *)&src_addr, sizeof(src_addr));

    // Socket to receive NACKs from the receiver (via relay)
    int nack_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in nack_addr;
    memset(&nack_addr, 0, sizeof(nack_addr));
    nack_addr.sin_family = AF_INET;
    nack_addr.sin_port = htons(47004);
    nack_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(nack_fd, (struct sockaddr *)&nack_addr, sizeof(nack_addr));

    // Socket to send frames to the relay
    int relay_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay_addr;
    memset(&relay_addr, 0, sizeof(relay_addr));
    relay_addr.sin_family = AF_INET;
    relay_addr.sin_port = htons(47001);
    relay_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    memset(history_valid, 0, sizeof(history_valid));

    int max_fd = (src_fd > nack_fd) ? src_fd : nack_fd;
    unsigned char buf[2048];
    unsigned char out_buf[2048];

    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(src_fd, &read_fds);
        FD_SET(nack_fd, &read_fds);

        int ready = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ready < 0) continue;

        // 1. Handle new frames from the source
        if (FD_ISSET(src_fd, &read_fds)) {
            int n = recvfrom(src_fd, (char*)buf, sizeof(buf), 0, NULL, NULL);
            if (n == 4 + PAYLOAD_SIZE) {
                uint32_t seq;
                memcpy(&seq, buf, 4);
                seq = ntohl(seq);

                // Store in history for potential ARQ fallback
                int idx = seq % MAX_HISTORY;
                history_valid[idx] = 1;
                memcpy(history[idx], buf + 4, PAYLOAD_SIZE);

                // Compress sequence number to 1 byte to save bandwidth
                out_buf[0] = (unsigned char)(seq & 0xFF);
                memcpy(out_buf + 1, buf + 4, PAYLOAD_SIZE);
                int out_len = 1 + PAYLOAD_SIZE;

                // Hybrid FEC logic: Attach the previous payload 90% of the time.
                // This keeps our bandwidth overhead around 1.9x (below the 2.0x limit),
                // but provides instant error correction for the vast majority of drops.
                if (seq > 0 && (seq % 10 != 0)) {
                    int prev_idx = (seq - 1) % MAX_HISTORY;
                    if (history_valid[prev_idx]) {
                        memcpy(out_buf + 1 + PAYLOAD_SIZE, history[prev_idx], PAYLOAD_SIZE);
                        out_len += PAYLOAD_SIZE;
                    }
                }

                sendto(relay_fd, (char*)out_buf, out_len, 0, (struct sockaddr *)&relay_addr, sizeof(relay_addr));
            }
        }

        // 2. Handle ARQ NACKs from the feedback channel
        if (FD_ISSET(nack_fd, &read_fds)) {
            int n = recvfrom(nack_fd, (char*)buf, sizeof(buf), 0, NULL, NULL);
            if (n == 4) {
                uint32_t req_seq;
                memcpy(&req_seq, buf, 4);
                req_seq = ntohl(req_seq);

                int idx = req_seq % MAX_HISTORY;
                if (history_valid[idx]) {
                    // Send the requested packet (without FEC redundancy)
                    out_buf[0] = (unsigned char)(req_seq & 0xFF);
                    memcpy(out_buf + 1, history[idx], PAYLOAD_SIZE);
                    sendto(relay_fd, (char*)out_buf, 1 + PAYLOAD_SIZE, 0, (struct sockaddr *)&relay_addr, sizeof(relay_addr));
                }
            }
        }
    }
    return 0;
}
