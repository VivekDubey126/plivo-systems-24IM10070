# NOTES

1. The architecture is a **Hybrid XOR FEC + NACK** system implemented in C++17 with platform-native multi-threading (Windows: CreateThread, Linux: pthread).
2. Every UDP packet from the sender carries the current frame **i** plus an XOR parity block `payload[i-1] XOR payload[i-2]` in a 324-byte datagram (96% of frames), saturating the 2.0× bandwidth budget at ~1.99×.
3. The receiver uses a **2048-slot pre-allocated circular array** indexed by `seq & 2047`; there are zero heap allocations in the hot receive path.
4. **Single-packet losses** are recovered in zero added milliseconds — the next arriving FEC packet carries the XOR parity to reconstruct the missing payload instantly.
5. **Burst losses of 2 consecutive packets** are recovered by chaining two XOR equations across packets `j`, `j+1`: recover `j-1` from `fec[j+1] XOR payload[j]`, then recover `j-2` from `fec[j] XOR payload[j-1]`.
6. A dedicated NACK thread polls every 5ms, sending 4-byte retransmit requests to port 47003 with a **25ms per-frame cooldown** to prevent bandwidth blowout.
7. The **NACK give-up threshold** suppresses requests when `now + RTT/2 > estimated_deadline`, saving bandwidth on frames that will arrive too late anyway.
8. The **theoretical optimal delay_ms** is approximately **55ms** on Profile A — limited by the burst-recovery latency of 2 FEC frames × 20ms + relay jitter margin.
9. The design will **break** under burst losses of 3 or more consecutive packets where neither FEC nor a single NACK RTT can recover before the deadline.
10. It will also fail if the relay's one-way delay exceeds ~25ms (half of delay_ms=55ms), as no recovery scheme can overcome physics.
