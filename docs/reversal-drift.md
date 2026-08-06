# Reversal drift -- mechanism, evidence, and what is left

What this file is: the durable analysis of the rail-drift hunt. Status,
versions and open work live on the dev board (C-2); this file holds the
mechanism and the measurements that constrain it.

Every number here was taken on 2026-08-03, fw 2.3.49/2.3.51, config held still
(the harness homes ONCE per run -- homing rewrites the whole config to NVS,
sd-921, and that contaminated every measurement taken before this session).

## 1. The three places a step can exist

A step is a real object in three independent places, and until this session
only two of them were ever compared at once:

| | what it is | how it is read |
|---|---|---|
| **A** | what FAS believes it emitted | `/api/status` `position` x 52.1519 steps/mm |
| **B** | what actually left the pin | MSO5074 hardware edge totalizer on PUL |
| **C** | what the drive received | `/api/servo` `counts` / 16 |

`dev_steady_mm` is A-vs-C. A mismatch there names NO layer: it is equally
consistent with FAS miscounting, the pin dropping edges, and the drive missing
them. **B is the arbiter, and the scope is the only instrument that can supply
it.** `tools/three_way.py` is that measurement.

## 2. What is closed

**The drive conserves pulses** (sd-mhn). Every width 1 us - 2 ms at
16.00-16.05 counts/step; conserves 0.5-50 kHz; 400 tiny reversals at
-0.021 um/rev.

**FAS's counter matches the pin exactly.**

```
12 traversals x 160 mm :  A - B = 0.00 on 12/12   (8344 vs 8344 every row)
60 traversals x  40 mm :  A - B = 0.00 on 60/60   (2086 vs 2086 every row)
```

72/72, both directions, identical integers -- not "within noise". This closes
the MCPWM-PCNT accounting hypothesis for the settled regime.

**Rounding cannot accumulate.** `MotionArbiter.cpp:395` converts an ABSOLUTE
target (`target_steps = -mmToNative(target_mm)`), `AIMServoDriver.cpp:871`
truncates that once, and FAS's own position is integer bookkeeping
(`queue_add_entry.cpp:85`, `queue_end.pos += steps`). There is no mm-domain
accumulator anywhere in the chain. `forceStopAndNewPosition()` is the only
thing that rewrites the FAS counter and all its call sites are homing or the
bench fake-home.

## 3. The regime split -- the central fact

```
60 SETTLED reversals, 2.4 m of travel :  -0.18 steps = -3.5 um, never past +/-0.9
continuous STROKING, per 25-stroke cell:  +/-40 to 90 steps, sign flips freely
```

Same machine, same firmware, same session, same window. This is not a
difference of degree, and it is what any candidate mechanism has to explain.

**Why the two regimes differ in the library** (`queue_add_entry.cpp:60`):

```c
if ((isQueueEmpty() && !isRunning()) && ((dirPin & PIN_EXTERNAL_FLAG) == 0)) {
    SET_DIRECTION_PIN_STATE(this, dir);   // direct GPIO write, NO pause entry
} else {
    toggle_dir = (dir != queue_end.dir);  // a steps=0 pause entry carries it
}
```

Settled reversal -> queue empty -> DIR is a plain GPIO write with no pulses in
flight. At-speed reversal -> queue running -> the toggle rides a pause entry,
in a pulse train that is still going.

## 4. The runt: real, confirmed twice, NOT the position defect

Automated confirmation (`tools/reversal_capture.py`, 39 DIR edges):

```
cap  5   local train width 199.5 us  ->  pulse at dt +60.5 us, width 99.5 us
cap 21   local train width 199.5 us  ->  pulse at dt +42.5 us, width 99.5 us
```

**99.5 us is exactly half of `AIM_DIR_CHANGE_DELAY_US` (200).** Under
COUNT_MODE_UP_DOWN with compare A pinned at 1
(`StepperISR_idf5_esp32_mcpwm_pcnt.cpp:286`) the output is high for half the
loaded period, so the emitter is the dir-change PAUSE entry carrying the dwell
period. The generator action register is shadowed
(`update_gen_action_on_tez/tep`, same file :271-272), so `apply_command`'s
`gen_utea = 1` ("stay low") misses its update point when the ISR lands
mid-cycle and one more pulse goes out. `steps = 0`, so `queue_end.pos` never
counts it.

It cannot be a ramp artifact: coming out of a reversal the train must ramp UP
from slow, so the first pulse after should be the WIDEST. A pulse at twice the
frequency of the train it interrupts is backwards -- the carriage cannot double
its speed instantaneously at a turnaround.

**It is a motion-quality defect, not a position one.** PCNT counts the extra
pulse and the next `apply_command` takes its catch-up branch
(`val2 = steps - FAS_CNT_VAL`, then `isr_pcnt_counter_clear`) and emits one
fewer. Cost is timing: one step up to 6 ms early, then dead air. See sd-ccy.

A challenge to that verdict was raised and KILLED, recorded in full at sd-dix:
the catch-up branch only runs when `H_LIM != steps`, and on the
`what_is_next` `!isPrepared` path `prepare_for_next_command` has already set
`H_LIM = steps` and cleared the counter, so the runt looks uncompensated on a
shallow queue. Prediction: deeper queue -> `_nextCommandIsPrepared` true more
often -> less drift. Measured: `plan_ahead` 4 -> 20 roughly DOUBLED the drift
(mean |drift| 41 -> 93 steps). The prediction inverted, so the mechanism is
wrong and sd-ccy stands.

## 4a. It is not the signaling layer -- two measurements say so

**DIR hold time is not the lever (sd-6gz).** `AIM_MCPWM_PCNT_RETIME` selects
which step edge PCNT counts, which sets where the DIR toggle lands: 1 = falling
(+2.5us hold), 0 = rising (DIR flips while the pulse is still HIGH, i.e.
NEGATIVE hold on every reversal).

```
retime=1  mean |drift| 48.75 steps  n=20  median ~51
retime=0  mean |drift| 62.84 steps  n=10  median ~30
difference 14.09, permutation p = 0.5555
```

Flat. The retime=0 mean is inflated by a single 282-step outlier; by median it
is LOWER. Removing the hold margin entirely, on every reversal, produced no
measurable change.

**The digital chain closes end to end DURING stroking.** Rate sweep, identical
path and identical 50 commanded reversals per cell, only the dynamics differ:

```
             peak velocity     turns/cell   mean |drift|      A-B (of 521,500)
rate 0.40    ~475-709 mm/s        0 - 3      27.06 (n=5)      -10 .. -50
rate 0.80    ~777-1107 mm/s       4 - 34      9.79 (n=5)       -7 .. -29
rate 1.59    ~1065 mm/s          60 - 130    48.75 (n=20)     unmeasurable
```

At both low rates `A-B` is 1 part in 10^4 to 10^5. FAS emits exactly what it
counts, the pin carries it, the drive counts it -- **and the rail still
drifts.** This also retroactively confirms the ~750-step A-B "surplus" at 1.59
was pure chording artifact: it vanishes precisely when the hunt does.

**Drift tracks the HUNT, not the speed.** rate 0.80 and rate 1.59 reach
essentially the same peak velocity (1107 vs 1065 mm/s) but differ ~4x in
turns/cell and ~5x in drift, and that difference is significant
(p = 0.0070). Drift is NOT monotonic in speed -- 0.40 is worse than 0.80, and
neither low arm separates from the other (p = 0.11).

## 5. What the elimination forces

Every digital loss path is closed: the drive counts every pulse, FAS counts
every pulse it emits, and the pin carries exactly those pulses at rates where
the measurement is trustworthy. Yet `drift = C - A` is real and reproducible.

**Nothing is lost. The shaft is not where the pulses say.**

The AIM drive is a closed-loop SERVO, not a stepper. sd-mhn exonerated it for
COUNTING -- bit-banged pulses, `quad_probe`, no motion engine, no inertia, no
rope tension. It was never tested for TRACKING under load. Those are different
claims.

At an at-speed reversal the shaft lags the commanded position by the following
error at that velocity. Then the target reverses and LEAVES. The error is never
made up, because there is nothing left to catch up to. The encoder faithfully
reports where the shaft actually is, and `dev = enc - FAS` is exactly that
abandoned error, accumulated over reversals.

This is the only mechanism left that explains the founding observation --
**forward and backward pulse counts exactly equal on the scope, and the rail
still drifts** -- without requiring anything to be lost.

**The hunt was proposed as the multiplier and that proposal is WEAK.** Drift
does track reversal count rather than speed across the sweep (same peak
velocity at rate 0.80 and 1.59, ~4x the hunt, ~5x the drift, p = 0.0070), but
the mechanism does not survive sizing. Measured with a deadband ladder over one
capture (`tools/hunt_origin.py`, rate 1.59):

```
deadband              raw / target / position
 2 steps (0.038 mm):   50 /  140  /   98
10 steps (0.192 mm):   50 /   54  /   50
26 steps (0.499 mm):   50 /   50  /   50
```

Every extra reversal is under 0.5 mm and most are under 0.2 mm -- sub-millimeter
wiggles AT the turnaround, where velocity is already near zero. Following error
scales with velocity, so near-zero-velocity micro-reversals abandon almost
nothing. The correlation is real; the causal story connecting it to drift is
not established. Do not cite the hunt as the drift cause without a mechanism.

**The hunt IS localized, and it is ours.** Same capture, three ring stages:

```
commanded 50  |  raw 50  |  target 142  |  position 96
```

`raw_mm` (pre-planner, straight off the wire, SlopSyncHubService.cpp:1653)
equals the commanded count EXACTLY -- the emitter and the wire stream are
clean. `target_mm` (MotionArbiter.cpp:209, fed by the slopmotion engine's
sampled output) is ~2.8x. So the extra reversals are manufactured inside
`lib/slopmotion`, and FAS actually absorbs some of them rather than adding any.
That is a motion-quality defect in our own code -- DIR toggles three times per
stroke end -- whatever its relationship to drift turns out to be.

### What this rules in and out for a re-architecture

- **Quadrature: does NOT fix this.** It is still INCREMENTAL, so abandoned
  following error accumulates identically. Its structural advantage was
  deleting the DIR setup/hold race, and sd-6gz measured that race as costing
  nothing. See sd-6gz before re-opening.
- **Absolute-setpoint Modbus: structurally correct.** Every frame restates
  "be at X", so following error is CORRECTED rather than accumulated. This is
  the only one of the three schemes where the failure mode cannot exist.
  ossm-rs (cloned to `../ossm-rs`, out of this repo) drives this drive that
  way: FC 0x7B, 4-byte BIG-ENDIAN absolute position, with `0x02` speed and
  `0x03` accel set once and the drive running its own profile between frames.
  Their init order matters and is not obvious -- `0x00 ModbusEnable = 1` comes
  AFTER homing, followed by an 800 ms settle, and **enabling Modbus resets
  `0x02` target speed and `0x18` max output to defaults**, so both must be
  re-written afterward. A drive with Modbus enabled and speed not re-set
  ACCEPTS 0x7B frames and does not move -- indistinguishable from "ignores
  0x7B entirely", which is what this repo recorded as bench-verified at
  fw 2.1.21 (`ServoModbus.h`). That prior negative result should be re-tested
  against this sequence before it is trusted.
- **Killing the hunt: cheapest, and helps under any of them.** It is ~4x the
  exposure and it lives in our own trajectory code.

## 6. Not the same bug: sd-z1c

Drift got worse with staleness, which also points at sd-z1c (planner degrading
silently under `infeasible_policy = 'blend'`; span_ratio 0.63 at 50k mm/s^2;
step pin dead ~26 ms with `plan_failed` spiking). Keep them separate: a
short stroke is span loss, and FAS commands it, so A and C both stay put. It
costs travel, not position. It is a plausible CO-factor for why more stalls
means more reversals-under-stress, not a substitute mechanism.

## 7. Instrument honesty

Four tools were built for this and three of them produced confident wrong
answers first. Each now carries a comment about how it lied.

- `tools/three_way.py` -- **load-bearing.** Standstill reads plus a hardware
  totalizer, one direction per cell. Its `pin_minus_enc` column once used
  `abs()`, which sums a symmetric +/-0.5-step park overshoot as a one-way leak
  and reported -32 steps where the signed answer was -0.18. Signed now.
- `tools/reversal_capture.py` -- **load-bearing.** Reads the pin. Free-runs a
  long record rather than triggering: a level computed from `:CHAN:SCAL/:OFFS`
  came out at 8.9 V on a 3.3 V signal and never fired. `:WAV:MODE RAW` only
  reads while STOPPED.
- `tools/stroke_closure.py` -- **drift column only.** Its `A-B` is derived from
  the 4 ms telemetry ring and is NOT trustworthy: the ring chords reversals,
  and separately the counter window once stayed open past the ring's. Three
  different failures, each of which looked like "the pin emits ~900 extra
  edges".
- `tools/dev_trace.py` -- **drift column only.** It regressed velocity against
  `dev_mm` sampled in a DIFFERENT HTTP request; its jump counts and offset
  spans are meaningless.

Standing rule from this: a path measurement derived from a sampled position
ring cannot resolve single steps. Count edges in hardware or do not claim.

## 8. Incidental finding

DIR toggles ~12 times per second against a commanded 3.2/s (3 transitions per
250 ms record, two of them 8.9 ms apart). The motion dithers roughly 15x more
than it strokes, so there are 15x more DIR-timing events than reversals. This
multiplies whatever the per-reversal cost turns out to be.

## 9. The fix, and the numbers behind every Modbus tunable

Absolute setpoints, streamed to the drive over Modbus FC 0x7B. An absolute
command restates where the shaft SHOULD BE every frame, so an unfinished move
is corrected by the next frame instead of banked forever. Step/dir and
quadrature are both incremental and cannot do this at all.

Measured on the live drive at 115200, 25-stroke cells, same shape as the FAS
drift cells in section 3:

| path | drift over 25 strokes |
|---|---|
| FAS step/dir, 20 cells | +/-40 to 90 steps |
| Modbus absolute, 50 Hz to 333 Hz | 0.12 to 0.62 steps |

Two orders of magnitude, and structural rather than tuned.

### Why the constants in `config_api.h` are what they are

**`AIM_SP_PERIOD_MS = 5` (200 Hz).** Tracking lag is FLAT at ~1.5 mm / ~12 ms
from 43 Hz to 320 Hz, so a faster stream buys nothing. 1 ms frames BREAK it:
the achieved rate collapsed to 228 Hz and the return error blew out to -224
steps against under one step everywhere else. 5 ms sits far below that knee
and leaves bus time for the telemetry and encoder polls sharing the wire.

**Drive limits stay wide open** (`AIM_MODBUS_ARM_*`). Lowering the drive's own
acceleration does smooth the motion, and it is barred anyway: funscripts vary
in speed and acceleration, and clamping the drive flattens the script instead
of smoothing it. Smoothness comes from the setpoint stream being jerk-limited
upstream. Operator ruling, recorded on dev-board sd-s37.

**Fire-and-forget setpoints.** The drive does not echo 0x7B. Every measurement
above was taken with the echo wait disabled; waiting would spend a 15 ms
timeout per frame and stall the motion path outright.

**`AIM_ENC_STALE_*` instead of an echo-failure streak.** With no echo, a fresh
encoder sample is the only honest proof the link is alive. It is also better
evidence than an echo ever was: an echo proves a frame was parsed, an encoder
sample proves the shaft is still being reported.

**`AIM_MODBUS_HOME_STALL_MM = 4`.** Modbus-mode homing detects a hard stop as
following error, not motor current. Steady-state lag is ~1.5 mm at FULL speed
and homing sweeps run far slower, so 4 mm cannot fire on ordinary lag. Needs
no current baseline, no warm-up, and no INA228.

### What this does not fix

Following error itself. The ~12 ms of lag is the servo's own response and
exists identically on FAS -- absolute commands stop it ACCUMULATING, they do
not remove it. The hunt (section 5) is also untouched: it is manufactured
upstream in `lib/slopmotion` and is tracked separately as sd-c7s.
