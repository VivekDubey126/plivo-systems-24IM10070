# Experiment Log

| Profile | `delay_ms` | Miss % | Overhead | Changes / Rationale |
|---|---|---|---|---|
| A.json | 200 ms | 0.00% | 1.10x | Baseline ARQ implementation. Very safe but high latency. |
| A.json | 75 ms | 0.53% | 1.92x | Simple FEC (payload[i-1]). Failed on burst drops in Profile B. |
| A.json | 75 ms | 0.33% | 1.99x | Upgraded to XOR(payload[i-1] ⊕ payload[i-2]) FEC. Multi-threaded. |
| A.json | 60 ms | 0.87% | 1.99x | Pushing delay lower. Valid. |
| A.json | 55 ms | 0.87% | 1.99x | Found absolute minimum for A.json. Valid. |
| A.json | 54 ms | 1.33% | 1.99x | Invalid. Misses exceeded 1.0% due to NACK recovery time. |

**Final Lock-in**: `delay_ms = 55`
