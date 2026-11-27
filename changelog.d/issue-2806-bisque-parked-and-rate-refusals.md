### Breaking changes
- **Bisque refuses AbortSlew on a parked mount** (Bisque driver, issue #2806): AbortSlew while AtPark is true used to return and send TheSkyX an Abort(); it now raises `InvalidWhileParked` (0x408), as ITelescopeV4 requires. A client that sends AbortSlew to a parked Paramount, for example at the end of a session, now gets an error. **After upgrading:** check AtPark before AbortSlew, or treat `InvalidWhileParked` from it as "nothing to stop".

### Fixed
- **Bisque AbortSlew while parked raises InvalidWhileParked** (Bisque driver, issue #2806): ConformU's `Parked:AbortSlew` check failed. The refusal sends TheSkyX nothing. A Park still in flight is not AtPark, so AbortSlew then still sends Abort() and cancels the Park. For the effect on clients, see Breaking changes. Tests: two `[bisque]` AbortSlew cases in `test_bisque_telescope.cpp`.
