# RUNLOG

| # | Profile | delay_ms | Miss % | Overhead | Notes / Changes |
|---|---------|----------|--------|----------|-----------------|
| 1 | A.json  | 200      | 0.00%  | 1.92×    | Baseline Hybrid FEC+NACK. Passed comfortably. |
| 2 | A.json  | 75       | 0.53%  | 1.92×    | Shrunk delay aggressively. Still valid (<1%). |
| 3 | A.json  | 60       | 1.07%  | 1.92×    | Too aggressive — just above 1% limit. INVALID. |
| 4 |         |          |        |          |                 |
| 5 |         |          |        |          |                 |
| 6 |         |          |        |          |                 |
| 7 |         |          |        |          |                 |
| 8 |         |          |        |          |                 |

## Grading Target

**Profile**: A.json  
**delay_ms**: 75  
**Miss %**: 0.53% ✅  
**Overhead**: 1.92× ✅
