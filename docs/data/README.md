# Session archive

Moved out of CLAUDE.md on 2026-08-27. It is an index of this directory, so it
belongs next to the directory rather than in every session's context.
⚠ Pooling rules for these sessions are in CLAUDE.md and stay there — read them
before combining any two of these.

`docs/data/` holds the post-2026-07-29 sessions.

| directory | what |
|---|---|
| `2026-08-05_6of49_fullpass/` | 5005 items, ~14 h — ran with `cam_cal=0` on all four nodes |
| `2026-08-08_6of49_aborted3404/` | 3404 items + all four ladders |
| `2026-08-08_6of49_pool11/` | 462 items, complete, first pass on the results code |
| `2026-08-13_6of49_fullpass_unattended/` | 5005 items, uncentred, with the README that motivated centring |
| `2026-08-18_6of49_unlim_run1s/` | first unlimited-mode session |
| `2026-08-19_6of49_unlim_overnight/` | 29 blocks; the session that produced `[D11]`, `[D18]`, `[D19]` |
| `2026-08-19_6of49_unlim_full8000/` | 8000 items, 18 rounds, 11,6 h — hit the buffer stop; the replay set for `[D42]` |
| `2026-08-26_aborted7354_rescue/` | 7354 items, 36 rounds, σ 1,004056 — pulled from RAM before the fold-off trial |
| `2026-08-26_foldoff_trial_slave0/` | the D17 re-test: slave0 fold-off against three folded nodes |
| `2026-08-26_specdump/` | the periodograms that refuted the row line and named the drift `[D43]` |
| `_live_*` / `_short_*` | a complete 5005 pass (13,4 h, curl-started, no gates); a 1995/5005 partial |
| `_analyze_*.py` | the operator's own analysis scripts — they skip `#` header lines |

