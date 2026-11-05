# Phase 5: snapshot and re-simulation. Done, budget met

`scripts/proof_phase5.sh`: 39 checks, 0 fails. Every sim byte compared after re-simulating, all 9 stages, 4 and 8 fighters, items on. Negative control fails as it must. Sounds started equal a run with no rollbacks (781 and 784).

| Measure | Budget | 8 fighters | 8 fighters at 4x slowdown |
|---|---|---|---|
| Snapshot | 4 MB or less | 2.91 MB | 2.91 MB |
| Save plus restore | 1 ms or less | 0.10 ms | 0.50 ms |
| One re-simulated frame | 0.8 ms or less | about 0.02 ms | about 0.13 ms |
| 12-frame rollback | about 10 ms | 0.21 ms | 1.61 ms |

Window W stays at 18, the plan's target.
