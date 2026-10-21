# CGX USB reversed Dec guide polarity

## Hardware finding

On 2026-10-13, the northern CGX in the lab (motor firmware 7.17.25, model
0x1788 on both axes, direct USB from the RPi4) was homed, then positioned at
Dec +89 degrees on the Normal pointing branch. A two-second Dec baseline showed
zero drift. One `PulseGuide(Direction=0, Duration=1000)` requested North at the
configured half-sidereal guide rate, 7.52055 arcsec/s. Dec instead changed by
**-7.5702667 arcsec**. IsPulseGuiding was true during the pulse and false after it.
Tracking was disabled and FindHome returned the mount to its reference position.

This is a direction failure, not evidence for an empirical rate multiplier. The
rate discrepancy is less than one 24-bit encoder count (~0.07725 arcsec), and a
single pulse cannot separate quantization, timing and mechanical effects.

## Root cause and fix

The driver interpreted native positive Dec velocity as North, copying the HC
direction shorthand. In its northern Normal encoder geometry,
`Dec = 90 - (DecEncoder - HomeEncoder) * 360 / 2^24`. A positive native pulse
increases encoder counts, so it decreases Dec. North therefore needs negative
velocity; South needs positive. The existing hemisphere and BeyondPole reversals
still follow the derivative of that geometry. RA's West-positive/East-negative
convention is separate and stays unchanged.

Sibling set: North and South, both hemisphere signs, both pointing branches,
single and segmented pulses (the signed velocity is calculated once and carried
through every chunk). The common change covers all of them. HC pass-through and
the unfinished WiFi backend are different implementations and are not changed.

## Regression and limits

The old test pinned the same incorrect byte convention as the driver. It is now
corrected and strengthened: for each Dec pulse it applies an explicit signed
encoder step, independently of the requested ASCOM direction, then requires
North to increase sky Dec and South to decrease it in both hemispheres and
pointing branches. The fake itself does not model guide movement or its rate.

Before the fix, `Celestron CGX USB Telescope Driver - pins guide polarity in both hemispheres and pointing branches`
fails 16 assertions (eight packet signs and eight sky-direction checks). Restoring
`direction == 0 || direction == 3` in `pulse_guide` is the falsifying mutation.

At this initial checkpoint, southern and BeyondPole outcomes were geometry/fake
evidence only. The later [larger-movement session](../celestron-cgx-usb-motion.md)
adds northern BeyondPole direction and flip evidence; southern behavior remains
unqualified, and the first-issue ConformU attempt is not a pass.

## Verification after the fix

- Native arm64 focused CGX suite: 57 cases / 693 assertions passed, including
  the corrected byte matrix, eight sky-direction assertions, and modern-profile
  lifecycle/destruction stress. clang-format 18, changelog validation and docs
  drift passed.
- The rebuilt server was installed on the Pi; installed/build SHA-256 matched.
  Restarting required Connect and a fresh FindHome before the repeat.
- The same North test at Dec +89 on the Normal branch measured zero baseline Dec
  drift and **+8.3427429 arcsec**, expected +7.52055. IsPulseGuiding transitioned
  true to false. This proves the **corrected direction**, not rate accuracy: the
  approximately 11% magnitude excess is unresolved and needs a separate measured
  check. No rate multiplier or fabricated coordinate compensation was applied.
- Tracking was disabled, FindHome completed in about eight seconds, and final
  state was AtHome true, Slewing/Tracking/IsPulseGuiding false, Dec +90.
- A requested repeat with the same setup again measured zero baseline drift and
  correct North polarity, but **+8.1882477 arcsec** versus +7.52055 expected
  (about 8.9% high). The pulse completed, tracking was disabled, and FindHome
  again returned the mount home in about eight seconds. The excess is repeatable
  across these two samples; its cause and any correction remain unqualified.
- A single two-second North pulse from the same Dec +89 setup measured zero
  baseline drift and **+16.2992477 arcsec**, expected +15.0411 (about 8.4% high).
  The sign remained correct, guiding finished, and the mount returned home with
  tracking/guiding off. This is close to twice the repeated one-second response,
  but the sampled trace also shows an early movement transient; it does not by
  itself prove a rate-scaling cause. No correction was applied.
- Two identical one-second North pulses without an intervening slew or tracking
  change measured **+9.0379715 arcsec** for the first pulse after the preparatory
  GOTO, then **+7.5702667 arcsec** for the second (expected +7.52055 each). During
  the two-second pause between them, Dec changed by -0.0772476 arcsec, one encoder
  count. The second response differs from expected by less than one count. This
  supports a first-pulse startup/reversal effect rather than a global guide-rate
  scaling error; backlash or another controller transient is not yet established
  as the cause. No rate adjustment or position compensation was applied. Both
  pulses completed, then tracking was disabled and FindHome returned the mount
  home in about eight seconds, with tracking/guiding off.
- Read-only `MC_GET_POS_BACKLASH` (`0x40`) and `MC_GET_NEG_BACKLASH` (`0x41`)
  queries returned one-byte `00` on both motors in two rounds, with valid reply
  addresses/checksums. The daemon was stopped for exclusive diagnostic access;
  no setting was written. Configured positive/negative backlash compensation
  therefore does not explain these runs.
- A North-then-South pair without an intervening slew or tracking change measured
  **+8.2654953 arcsec** and **-8.6517334 arcsec**, expected +7.52055 and -7.52055.
  Baseline drift was zero; the two-second pause after North settled by one
  encoder count (-0.0772476 arcsec). Both pulses had correct direction and
  completed normally. Unlike the same-direction pair's second pulse, the
  opposite-direction second pulse had excess movement. This strengthens the
  reversal-transient hypothesis, but does not establish the mechanism or actual
  optical sky displacement from motor-encoder readback alone. The mount returned
  home in about eight seconds, tracking/guiding off. No compensation was added.
- A same-direction pair at half the previous Dec guide setting (25% sidereal,
  expected +3.760275 arcsec per one-second North pulse) measured **+4.6348572**
  then **+3.8623810 arcsec**. Baseline drift was zero, and the pause settled by
  one encoder count. The initial response again had substantially more excess
  than the same-direction follow-up. The follow-up is close to expected at this
  encoder resolution, but is not an exact rate-accuracy pass. This supports a
  reversal/startup effect across two guide settings, not a blanket rate
  multiplier. The original 50% guide setting (7.52055 arcsec/s) was restored and
  read back. FindHome completed in about eight seconds, tracking/guiding off.
- One three-second North pulse at restored 50% rate exercised the driver's
  255-centisecond plus 45-centisecond chunk path. Expected +22.56165 arcsec;
  measured **+23.2515335 arcsec** (about 3.1% high). IsPulseGuiding stayed true in
  the samples across the handoff and became false at approximately 3.08 seconds
  after the initiating call. No extra handoff jump was evident at the sampled
  cadence. Baseline drift was zero; the final two-second pause settled by one
  encoder count. The roughly +0.69 arcsec excess remains consistent with the
  previously observed initial transient, not proof of exact guide accuracy.
  Tracking was disabled and FindHome returned the mount home in about eight
  seconds; the original guide rate was verified unchanged.
- Three-second South at 50% rate measured **-22.6335526 arcsec**, expected
  -22.56165; completion error was within one encoder count. Guiding status stayed
  true through the handoff. The subsequent settling read changed by two counts.
  The mount returned home, tracking/guiding off. A three-second East RA test
  subsequently measured -22.7870 arcsec relative to its measured tracking
  baseline, expected -22.56165; guiding status covered the handoff and the mount
  returned home. This is a sampled-baseline comparison, not optical sky accuracy.
- Overlap rejection was verified on hardware: an immediate South pulse during
  a one-second North pulse returned InvalidOperation (1035), with no replacement
  pulse accepted. The original North pulse completed and the mount returned home.
  Its +8.4972382 arcsec first-pulse result retains the known reversal-transient
  limitation.

The full bench-test result must not be summarized as a guide-accuracy pass merely
because the sign is now correct.

The later approved [stage 3 Dec guide sequence](../celestron-cgx-usb-motion.md#stage-3-northern-normal-branch-dec-guide-evidence)
records 50%/100% N1, N1, S1, S1, N5, S5 at Dec +85 on northern Normal, including
immediate and settled errors and raw verification of signed percent bytes,
duration chunks and empty ACKs. Small post-inactive tails do not account for
the larger early/reversal excess. Its mechanism remains unresolved; no
compensation was added, and this remains encoder evidence, not optical accuracy.

The [latest ConformU and post-GOTO evidence](../celestron-cgx-usb-motion.md#latest-approved-session-and-software-review)
records the first extended HA -9 North guide excess and the BeyondPole repeats
after 0/2/10 s waits. One-count-then-flat baselines and persistent settled excess
do not support leftover GOTO drift as the explanation. The mechanism remains
unresolved; no arrival sleep or coordinate compensation was introduced.

## Other measurements from the same session

| Basic lab check | Result |
| --- | --- |
| FindHome | First completion ~16 s; AtHome true, stopped; operator confirmed physical stop |
| 1-degree slew to Dec +89 | ~1.2 s; Dec +89.000051; same pier side; returned home |
| 5-degree slew to Dec +85 | ~2.9 s; Dec +84.999998; same pier side; returned home |
| 1 s West RA pulse | +7.5083 arcsec above measured tracking baseline; expected +7.52055 |
| 1 s East RA pulse | -7.5050 arcsec relative to measured tracking baseline; expected -7.52055 |

The operator confirmed both slew/return sequences. RA values are encoder-derived
axis changes, not sky measurements at the pole. These bounded checks improve the
driver's evidence, but do not qualify general pointing accuracy or every motion
path. Every completed check left the mount home with tracking/guiding off.
