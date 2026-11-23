### Fixed

- **CGX ambiguous-pulse refusal names its cause**, Celestron CGX driver, issue #2467. A PulseGuide on an axis whose running pulse lost its first acknowledgement is refused with `InvalidOperation` and a text that says so, instead of the generic overlap message. Tests also pin that a kept pulse failure refuses a Dec pulse too with nothing sent, that a FindHome or Park that superseded a slew and then failed its start leaves the slew's Tracking restore unsent, and that a retarget whose GOTO is refused restores Tracking (`test_celestron_cgx_motion_owner.cpp`).
