# elotto_link - wire protocol

`P` discovery · `M<seg>` measure · `K<budget_ms>,<segs>` calibrate · `D` diagnostics · `A` abort ·
`R` reboot. Replies `OK` (to `P`: `OK,cam=IMX219|OV5647`), `Z:<z>[,wsig=][,mau=][,ser=][,byt=][,sl=]`, `D:`, `E:<reason>`, `V:<reason>` (void, not a
fault). Extra fields ignored.
A slave whose `camera_init()` failed at boot (no camera attached) answers `P` with `E:no camera`:
discovery leaves it out of the session — no slot, no drop, no reboot — and the Log names it.
⚠ It still hears the broadcast `M`/`K` and answers them; the master discards replies from
addresses outside the session. Attaching a camera needs a node reboot to take effect.
UDP loss is handled explicitly: every frame carries the sequence it answers, mismatches are dropped
and counted (`net_stale`), a timed-out command is resent under the same sequence so a node replies
from a one-entry cache instead of measuring twice.
⚠ All receive timeouts go through `link_arm_timeout()` — its comment in `nodes.c` says why the
floor is load-bearing.
⚠ A receiver refuses an out-of-range segment count (`V:`, the run is void on that node);
`LINK_MEAS_MS_FOR(nseg)` is 3 × the nominal window (`el_win_ms()`) + 1 s.
⚠ `EL_SEG_MIN`/`EL_SEG_MAX` and the window calibration are compiled into every node: flash all of them together.
