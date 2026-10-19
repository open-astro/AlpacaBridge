### Changed

- **Bisque pulse guide runs in an operation slot**, Bisque telescope, issue #1336. The guide body that sends DirectGuide to TheSkyX now runs in a `util::AsyncOperation` instead of a hand-rolled thread; disconnect and the destructor cancel and join it, and a failed guide is still reported by `IsPulseGuiding` until the next `PulseGuide` (tests `Bisque PulseGuide - a failed DirectGuide is reported until the next guide` and `Bisque PulseGuide - a failure kept from a guide is dropped by disconnect and reconnect`).
