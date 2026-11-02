### Breaking changes

- **iOptron `SlewToAltAz` and `SlewToAltAzAsync` throw `InvalidOperation` while `Tracking` is true** (issue #1377, ITelescopeV4): a tracking mount drifts off the requested Alt/Az, so the interface refuses the slew. The driver used to start it. **After upgrading:** a client turns `Tracking` off before an Alt/Az slew. This is the iOptron half of #1377; the OnStep half shipped in #1650. Test: `test_ioptron_telescope.cpp` "iOptron Telescope Driver - an Alt/Az slew is refused while Tracking is true".
