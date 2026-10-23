# T-012 A/B: does the running-indicator poll cost frame time?

Same hover benchmark, same build, only `DOCKFORGE_RUNNING_POLL_MS` differs (300ms =
production, 5000ms = effectively one poll per run). Two runs each, alternating, so a
machine that drifts during the test cannot make one side look better by accident.

| run | poll | p99_mean | p99_max | over_budget_windows | verdict |
|---|---|---|---|---|---|
| poll300 | 300ms | 16.572ms | 16.90ms | 2/15 | OVER_BUDGET |
| poll5000 | 5000ms | 16.555ms | 16.73ms | 1/15 | OVER_BUDGET |
| poll300b | 300ms | 16.554ms | 16.60ms | 0/15 | WITHIN_BUDGET |
| poll5000b | 5000ms | 16.570ms | 16.67ms | 0/15 | WITHIN_BUDGET |

## Conclusion

The tracker is not the cause of the tail latency.

The two configurations differ by 0.017ms in one pair and 0.016ms in the other, and
the sign flips between pairs. Meanwhile the run-to-run spread within one
configuration is 0.018ms (16.572 vs 16.554) - the same magnitude. A 0.5ms
enumeration happening three times per second cannot move a p99 that is already
bounded by the 16ms timer quantum.

The real bound is the frame timer: a 16ms target produces p50 16.01ms, and the p99
of a 62-frame window is the second-worst frame, which lands at 16.6-16.9ms whether
or not any work happens. T-010 measured 16.488ms mean and passed; this run measured
16.57ms and grazed the line. That is the same tail-latency issue already recorded as
T-006a, not a regression from T-012.

Recorded as WATCH rather than PASS, with the A/B above as the reason: a number that
moves by the same amount whether or not the feature runs is not evidence about the
feature.