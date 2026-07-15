# Experiment Log

| Profile | `delay_ms` | Miss % | Overhead | Changes / Rationale |
|---|---|---|---|---|
| A.json | 200 ms | 0.00% | 1.10x | Baseline ARQ implementation. Very safe but high latency. |
| A.json | 100 ms | 0.00% | 1.98x | Elite POSIX C rewrite (select() loops, Kalman Filter, Token Bucket). |
| B.json | 80 ms  | 12.60% | 2.08x | Tested minimal limit on Gilbert-Elliott burst profile. Fails on heavy bursts. |
| B.json | 105 ms | 0.93%  | 1.96x | Locked in optimal delay. Token Bucket Rate increased to 0.05 to absorb burst recovery overhead. |

**Final Lock-in**: `delay_ms = 105`
