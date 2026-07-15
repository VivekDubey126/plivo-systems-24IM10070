#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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
  #include <sys/time.h>
#endif

#define MAX_HISTORY 65536
#define PAYLOAD_SIZE 160
#define NACK_INTERVAL_MS 30

int state_received[MAX_HISTORY];
long long state_missing_time[MAX_HISTORY];
int highest_seq = -1;

void setup_network_env() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

long long get_current_time_ms() {
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

int main() {
    setup_network_env();

    // Socket to receive frames from the relay
    int relay_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay_addr;
    memset(&relay_addr, 0, sizeof(relay_addr));
    relay_addr.sin_family = AF_INET;
    relay_addr.sin_port = htons(47002);
    relay_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(relay_fd, (struct sockaddr *)&relay_addr, sizeof(relay_addr));

    // Socket to send valid frames to the player
    int player_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player_addr;
    memset(&player_addr, 0, sizeof(player_addr));
    player_addr.sin_family = AF_INET;
    player_addr.sin_port = htons(47020);
    player_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Socket to send NACKs back to the relay
    int nack_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in nack_addr;
    memset(&nack_addr, 0, sizeof(nack_addr));
    nack_addr.sin_family = AF_INET;
    nack_addr.sin_port = htons(47003);
    nack_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    memset(state_received, 0, sizeof(state_received));
    memset(state_missing_time, 0, sizeof(state_missing_time));

    long long last_nack_check = get_current_time_ms();
    unsigned char buf[2048];
    unsigned char play_buf[2048];

    while (1) {
        long long now = get_current_time_ms();
        int wait_ms = 10 - (int)(now - last_nack_check);
        if (wait_ms < 0) wait_ms = 0;
        if (wait_ms > 10) wait_ms = 10;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(relay_fd, &read_fds);

        struct timeval tv;
        tv.tv_sec = wait_ms / 1000;
        tv.tv_usec = (wait_ms % 1000) * 1000;

        int ready = select(relay_fd + 1, &read_fds, NULL, NULL, &tv);

        // 1. Process incoming packets from the relay
        if (ready > 0 && FD_ISSET(relay_fd, &read_fds)) {
            int n = recvfrom(relay_fd, (char*)buf, sizeof(buf), 0, NULL, NULL);
            
            // Expected sizes:
            // 1 byte seq + 160 bytes payload = 161 (ARQ fallback or every 10th packet)
            // 1 byte seq + 160 bytes payload + 160 bytes prev payload = 321 (FEC normal)
            if (n == 1 + PAYLOAD_SIZE || n == 1 + PAYLOAD_SIZE * 2) {
                unsigned char seq_8 = buf[0];
                int seq = 0;
                
                // Decompress the 1-byte sequence number back to the full 32-bit sequence number
                if (highest_seq == -1) {
                    seq = seq_8;
                } else {
                    int base = highest_seq - (highest_seq % 256);
                    seq = base + seq_8;
                    // Handle wrap-around transitions
                    if (seq > highest_seq + 128) seq -= 256;
                    else if (seq < highest_seq - 128) seq += 256;
                }

                if (seq > highest_seq) {
                    highest_seq = seq;
                }

                // Process the primary packet `seq`
                int idx = seq % MAX_HISTORY;
                if (!state_received[idx]) {
                    state_received[idx] = 1;
                    
                    // The player expects the original 4-byte network-order sequence number attached
                    uint32_t net_seq = htonl(seq);
                    memcpy(play_buf, &net_seq, 4);
                    memcpy(play_buf + 4, buf + 1, PAYLOAD_SIZE);
                    sendto(player_fd, (char*)play_buf, 4 + PAYLOAD_SIZE, 0, (struct sockaddr *)&player_addr, sizeof(player_addr));
                }

                // If FEC is present, instantly recover the previous packet `seq - 1` if it was lost
                if (n == 1 + PAYLOAD_SIZE * 2 && seq > 0) {
                    int prev_seq = seq - 1;
                    int prev_idx = prev_seq % MAX_HISTORY;
                    if (!state_received[prev_idx]) {
                        state_received[prev_idx] = 1;
                        
                        uint32_t net_seq = htonl(prev_seq);
                        memcpy(play_buf, &net_seq, 4);
                        memcpy(play_buf + 4, buf + 1 + PAYLOAD_SIZE, PAYLOAD_SIZE);
                        sendto(player_fd, (char*)play_buf, 4 + PAYLOAD_SIZE, 0, (struct sockaddr *)&player_addr, sizeof(player_addr));
                    }
                }
            }
        }

        // 2. Periodic ARQ Check: Send NACKs for missing packets
        now = get_current_time_ms();
        if (now - last_nack_check >= 10 && highest_seq >= 0) {
            int start = highest_seq - 100; // Check the last 100 sequence numbers
            if (start < 0) start = 0;

            for (int i = start; i < highest_seq; i++) {
                int idx = i % MAX_HISTORY;
                if (!state_received[idx]) {
                    // Instantly NACK missing gaps, or re-NACK if still missing after interval
                    if (state_missing_time[idx] == 0 || now - state_missing_time[idx] >= NACK_INTERVAL_MS) {
                        state_missing_time[idx] = now;
                        uint32_t net_seq = htonl(i);
                        sendto(nack_fd, (char*)&net_seq, 4, 0, (struct sockaddr *)&nack_addr, sizeof(nack_addr));
                    }
                }
            }
            last_nack_check = now;
        }
    }
    return 0;
}
