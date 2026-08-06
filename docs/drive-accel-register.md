# The AIM drive's ramp register (0x03)

Source: YZ-AIM manual v2.55, `docs/reference/YZ-AIMManual_v2_55.pdf`, p8 and
p11. Quoted rather than paraphrased because the translation is rough and the
regime boundaries are exact.

## What the register actually is

Register 0x03 is `0..60098`, and its unit is **(r/min)/s -- motor RPM per
second**. It is not mm/s^2 and it is not a step rate. p11:

> When the parameter is less than 60000, the acceleration and deceleration
> curve occurs inside the drive. When the parameter is equal to 60000, no
> acceleration process, the deceleration size determines the Dayu 60000 to
> 60098 according to the position KP, individual bits and 100~98 corresponding
> to the position feedforward. 0%~98%. The larger the position feedforward,
> the smaller the follow-pulse lag.

Three regimes:

| Value | Behavior |
|---|---|
| `0..59999` | The drive runs its OWN accel/decel curve at that rate. |
| `60000` | No internal ramp. The drive follows what it is given. |
| `60001..60098` | No internal accel; decel governed by position KP (0x07). The low two digits are position feedforward, 0% to 98%. |

## Why this matters here, in the vendor's own words

p8, on running below 60000:

> Use the internal acceleration curve: ... will produce a lag pulse
> phenomenon, some occasions that do not need to follow in real time, you can
> use the internal acceleration curve.

and on 60000 and above:

> the drive is invalid according to the acceleration and deceleration permit
> of the external pulse. Use occasion: for example, the pulse output by the
> controller is added and reduced, which does not need the acceleration curve
> inside the drive. If used at this time, it will lag behind the actual pulse.

This machine plans every move itself (`.claude/rules/motion-control.md`, one
command one plan). Running 0x03 below 60000 therefore stacks the drive's ramp
on top of the planner's, which is the "lag pulse phenomenon" the manual names.

## What the firmware wrote before the override existed

Two writers, and NEITHER could reach 60000 in ordinary use:

- `_rearmModbus()` and `armMotionControl()` write `AIM_MODBUS_ARM_ACCEL`,
  which is 50000. Internal ramp on.
- `ModbusServoDriver::streamToSteps()` derives the value per move:
  `accel_mm_s2 * 60 * AIM_REDUCTION / AIM_MM_PER_REV`, that is
  `accel_mm_s2 * 1.5279`, clamped to `[1, 60000]`.

Reaching 60000 by the derived path needs **39,270 mm/s^2**.
`NORMAL_MAX_ACCEL_MM_S2` is 20,000, which lands at 30,558, and dev-board issue
`sd-z1c` records the planner failing above roughly 20,000 anyway. So the
drive's internal ramp has been active for every move this machine has ever
made on the Modbus backend.

## The override

`ServoModbus::setAccelRegOverride(v)`, surfaced as SlopSync `drive-tune`
(0x1130) / `drive-set` (0x3130), field `accel_reg`. `0` means auto: the motion
path writes its own derived value as before. Nonzero PINS the register, and
both writers above defer to it, so it is not stomped one move later.

**The apply is ONE FC 0x06 write of 0x03, on either backend, at any machine
state. Never arm the drive to set it.**

The manual says at p16 that "only modbus enables 1 can change other parameters
and the external pulse signal is invalid", which reads as though 0x03 needs
`0x00 = 1` first. MEASURED 2026-08-04 on the live drive: it does not. 0x03
accepted a write to 60000 while `0x00` read 0, the value stuck, and step/dir
kept running throughout. Arming would be actively harmful, because `0x00 = 1`
makes the drive ignore step pulses and CANNOT BE CLEARED over Modbus
(dev-board sd-opb) -- only a drive power cycle clears it. Which other
registers genuinely need arming is untested; do not generalize from 0x03.

The value lives in drive RAM. Nothing here writes 0x14, so a drive power cycle
restores whatever is in its EEPROM (50000 on this machine). The firmware's own
override field is likewise session-only. A slider reading 0 means "firmware is
not overriding", NOT "the drive is at its default" -- read `GET /api/servo`
for the drive's actual register.

## On the step/dir backend, nothing has ever written 0x03

`_rearmModbus()`, `streamToSteps()` and `armMotionControl()` all belong to the
Modbus path. On backend 0 none of them run, so register 0x03 holds whatever is
in the DRIVE's EEPROM: a factory default, or whatever a past bench session
left there. It has never been read as part of the drift hunt. **Read it before
changing it** -- a servo scan (machine-admin op 3) then `GET /api/servo` names
the number, and if it is well under 60000 the drive has been re-ramping every
pulse train this machine has ever sent.

## Why this does NOT contradict sd-mhn

`sd-mhn` exonerated the drive: 400 pulses forward then 400 back at 0.5 to
50 kHz, every pulse width 1 us to 2 ms, plus 200 cycles of +4/-4 steps, all
conserving position to within 13 um. That is a real measurement and it stands.

What it measured is conservation across a SETTLED reversal: the encoder was
read at standstill either side of every leg, so the drive was always given
time to finish its internal ramp and retire its following error. A ramp is a
filter, and a filter that is allowed to settle conserves.

The untested case is the one the machine actually runs: a direction flip while
the drive is still ramping and still carrying banked following error. Whether
that banked error is retired or discarded across the flip is exactly what
nothing here has measured.

That reading also fits the other three results rather than fighting them:

- `sd-sf5` "settled reversals do not drift" -- settled means no banked error
  to lose, which is the same boundary.
- `sd-4qs` sign-flipping, non-monotone drift -- residual following error at
  the moment a cell ends depends on waveform phase and last direction, so its
  sign is not fixed. A deterministic per-reversal leak cannot do that; a
  discarded ramp residual can.
- `sd-6gz` DIR hold time is not the lever -- correct, because under this
  hypothesis the loss is in the drive's internal ramp state, not in the DIR
  edge timing.

Status: HYPOTHESIS. It is consistent with four measurements and proven by
none. The test is one 0x03 write and a re-run of the existing drift ladder.

## MEASURED: the ramp is not the drift

A/B on the live machine, 2026-08-04, `quad_probe` TEST 5, step/dir only. 100
cycles of +-400 steps (7.67 mm) at a measured 18.5 kHz, 200 UNSETTLED
reversals, encoder settled only at the ends:

| 0x03 | net counts | steps | um/reversal | verdict |
|---|---|---|---|---|
| 50000 (internal ramp on) | -7 | -0.4 | -0.042 | conserves |
| 60000 (ramp off) | -6 | -0.4 | -0.036 | conserves |
| 60000 repeat | -4 | -0.2 | -0.024 | conserves |

Indistinguishable, and the repeat puts the noise floor at +-2 counts. The
drive's internal ramp does NOT lose steps, including in the regime `sd-mhn`
could not reach (unsettled reversals at operational rate and amplitude). It
lags and then RETIRES its following error rather than discarding it.

So 0x03 is not the `sd-t1j` drift (+-40 to 90 steps on the FAS path). That
stays upstream in the ESP32 motion engine, exactly where `sd-mhn` put it.

What this does NOT settle: the manual's "lag pulse phenomenon" is about LAG,
not lost position, and a lagging-but-conservative follower can still feel
different. The 60001..60098 feedforward band is untested. Both are feel
questions, which is what the slider exists for.
