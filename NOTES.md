# NOTES

1. The architecture is a **Hybrid FEC + NACK** system implemented in C++ with zero dynamic memory allocation in the critical receive path.
2. Every UDP packet sent by the sender carries both the current frame **i** and the previous frame **i-1** (328 bytes total), consuming the full 2.0× bandwidth budget in exchange for instant single-packet-loss recovery.
3. The receiver uses a **1024-slot pre-allocated circular array** indexed by `seq & 1023`; there are no heap allocations, hash maps, or trees in the hot path.
4. **Single-packet losses** are recovered in zero extra milliseconds — the next arriving packet carries the missing payload as its FEC piggyback.
5. **Burst losses** (two or more consecutive drops, where FEC fails) trigger an immediate 4-byte NACK to the relay feedback port; the sender retransmits from its own 1024-slot ring buffer within microseconds.
6. NACKs are retried every **30 ms** if the missing frame is still absent, ensuring eventual delivery even if the NACK itself is dropped.
7. The **theoretical optimal delay_ms** this design can handle is approximately **40–60 ms** on Profile A (where maximum one-way jitter is ~20 ms), because a single NACK RTT is the worst-case recovery path.
8. On Profile B (higher jitter / drop rate), a practical lower bound is around **70–80 ms** before the burst-loss recovery window begins to be violated.
9. The design will **break** under sustained burst losses longer than ~3 consecutive packets at the maximum jitter spike, because a second NACK cycle would be needed but may exceed the deadline window.
10. It will also fail if the relay's one-way delay exceeds `delay_ms / 2`, as no amount of FEC or NACK can recover a frame that physically arrives after its deadline.
