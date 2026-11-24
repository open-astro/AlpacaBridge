### Fixed

- **OnStep telescope link loss at the 30 s bound**, OnStep driver, issue #2495. The status poll now runs through `util::PolledState`, so a mount that stops answering is a lost link 30 s (`util::kLinkStalenessBound`) after the last frame it answered, as on the other `PolledState` drivers; before, the bound ran from the fault latching, about three seconds later.

### Changed

- **OnStep telescope fault latch counts only the status poll**, OnStep driver, issue #2495. A failed `:GU#` read by a command, the end-of-slew read, the pulse-end measurement and the connect's first reads no longer count toward the link fault, and a good one no longer clears it: only the 1 s status poll decides. A mount that answers none of the connect's first reads now connects without a fault, and its getters say the state is not available until the poll answers. A write while faulted throws the fault, and the poll finds the 30 s bound within a second. A frame no poll refreshed for 30 s is a lost link too. Decision 0010 rule 7 now allows the pulse-end one-shot measurement slot.
