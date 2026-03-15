# iOptron® Mount RS-232 Command Language

**Version:** 3.10  
**Date:** January 4th, 2021  

## Abbreviations

- `YYMMDD` –  
  - `YY`: last two digits of the year (assumed 21st Century)  
  - `MM`: month  
  - `DD`: day of the month  
- `s` – `"+"` or `"–"` sign, cannot be replaced by other characters  
- `MMM` – minutes  
- `TTTTTTTT(T)` – 0.01 arc-seconds  
- `XXXXX(XXXXXXXX)` – milliseconds  
- `n` – digit(s)  

All digits above include leading zeroes to match the format of each command.

## Applicable Products

Currently, this document applies to the following products:

- CEM120 series
- CEM70 series
- GEM45 series with firmware 210101 and later
- CEM40 series with firmware 210101 and later
- GEM28 series
- CEM26 series

Series definitions:

- **CEM120 series**: CEM120-EC2, CEM120-EC, CEM120  
- **CEM70 series**: CEM70G-EC, CEM70-EC, CEM70G, CEM-70  
- **GEM45 series**: GEM45(G)-EC, GEM45(G)  
- **CEM40 series**: CEM40(G)-EC, CEM40(G)  
- **GEM28 series**: GEM28-EC, GEM28  
- **CEM26 series**: CEM26-EC, CEM26  

Unless specified, all commands apply to all products.

---

## Get Information and Settings

### `:GLS#` – Get longitude, latitude, and status

**Command:**  
`:GLS#`  

**Response:**  
`sTTTTTTTTTTTTTTTTnnnnnn#`  

The response includes a sign and 22 digits.

- **Sign + first 8 digits**: current longitude  
  - Range: `[-64,800,000, +64,800,000]`  
  - East is positive  
  - Resolution: 0.01 arc-second  

- **9th–16th digits**: current latitude + 90 degrees  
  - Range: `[0, 64,800,000]`  
  - North is positive  
  - Resolution: 0.01 arc-second  

- **17th digit – GPS status:**
  - `0`: GPS module malfunction or no GPS module  
  - `1`: GPS works but no valid data received  
  - `2`: valid GPS data received  

- **18th digit – System status:**
  - `0`: stopped at non-zero position  
  - `1`: tracking, periodic error correction disabled  
  - `2`: slewing  
  - `3`: auto-guiding  
  - `4`: meridian flipping  
  - `5`: tracking, periodic error correction enabled (non-encoder edition only)  
  - `6`: parked  
  - `7`: stopped at zero (home) position  

- **19th digit – Tracking rate:**
  - `0`: sidereal  
  - `1`: lunar  
  - `2`: solar  
  - `3`: King  
  - `4`: custom  

- **20th digit – Arrow button moving speed:**
  - `1`: 1× sidereal  
  - `2`: 2×  
  - `3`: 8×  
  - `4`: 16×  
  - `5`: 64×  
  - `6`: 128×  
  - `7`: 256×  
  - `8`: 512×  
  - `9`: maximum speed (model dependent)  

- **21st digit – Time source:**
  - `1`: RS-232 or Ethernet  
  - `2`: hand controller  
  - `3`: GPS module  

- **22nd digit – Hemisphere:**
  - `0`: Southern Hemisphere  
  - `1`: Northern Hemisphere  

---

### `:GUT#` – Get time-related information

**Command:**  
`:GUT#`  

**Response:**  
`sMMMnXXXXXXXXXXXXX#`  

Response includes a sign and 17 digits:

- **Sign + first 3 digits**: minute offset from UTC (time zone)  
  - Daylight Saving Time is *not* included in this value  

- **4th digit – Daylight Saving Time status:**
  - `0`: DST not observed  
  - `1`: DST has been observed  

- **5th–17th digits**: current UTC time  
  - `value = (JD(current UTC) – J2000) * 8.64e+7`  
  - Resolution: 1 millisecond  
  - `JD(current UTC)` is the Julian Date of current UTC time  

---

### `:GEP#` – Get RA, Dec, side of pier, and pointing state

**Command:**  
`:GEP#`  

**Response:**  
`sTTTTTTTTTTTTTTTTTnn#`  

Response includes a sign and 19 digits:

- **Sign + first 8 digits**: current declination  
  - Range: `[-32,400,000, +32,400,000]`  
  - Resolution: 0.01 arc-second  

- **9th–17th digits**: current right ascension  
  - Range: `[0, 129,600,000]`  
  - Resolution: 0.01 arc-second  

- **18th digit – Side of pier:**
  - `0`: pier east  
  - `1`: pier west  
  - `2`: pier indeterminate  

- **19th digit – Pointing state:**
  - `0`: counterweight up  
  - `1`: normal  

> **Note:** Ignore side of pier and pointing state if the mount is not equatorial.

---

### `:GAC#` – Get altitude and azimuth

**Command:**  
`:GAC#`  

**Response:**  
`sTTTTTTTTTTTTTTTTT#`  

Response includes a sign and 17 digits:

- **Sign + first 8 digits**: current altitude  
  - Range: `[-32,400,000, +32,400,000]`  
  - Resolution: 0.01 arc-second  

- **Last 9 digits**: current azimuth  
  - Range: `[0, 129,600,000]`  
  - Resolution: 0.01 arc-second  

---

### `:GTR#` – Get custom tracking rate

**Command:**  
`:GTR#`  

**Response:**  
`nnnnn#`  

Tracking rate is `n.nnnn × sidereal rate`.

- Valid range: `[0.1000, 1.9000] ×` sidereal  
- This rate is only applied when a **Custom Tracking Rate** is selected.

---

### `:GPC#` – Get parking position

**Command:**  
`:GPC#`  

**Response:**  
`TTTTTTTTTTTTTTTTT#`  

- **First 8 digits**: altitude of parking position  
  - Range: `[0, 32,400,000]`  
  - Resolution: 0.01 arc-second  

- **Last 9 digits**: azimuth of parking position  
  - Range: `[0, 129,600,000]`  
  - Resolution: 0.01 arc-second  

---

### `:GSR#` – Get maximum slewing speed

**Command:**  
`:GSR#`  

**Response:**  
`"7#"`, `"8#"`, or `"9#"`  

- `7`: 256× sidereal rate  
- `8`: 512× sidereal rate  
- `9`: maximum speed (model dependent)  

---

### `:GAL#` – Get altitude limit

**Command:**  
`:GAL#`  

**Response:**  
`snn#`  

- Sign + 2 digits: altitude limit  
- Range: `[-89, +89]` degrees  
- Resolution: 1 degree  

Altitude limit applies to **tracking** and **slewing**. Arrow-button movement is not affected. Tracking stops if the mount goes below this altitude.

---

### `:AG#` – Get guiding rates (RA and Dec)

**Command:**  
`:AG#`  

**Response:**  
`nnnn#`  

- First 2 digits: RA guiding rate `0.nn × sidereal`  
- Last 2 digits: Dec guiding rate `0.nn × sidereal`  

Valid ranges:

- RA guiding rate: `[0.01, 0.90] × sidereal`  
- Dec guiding rate: `[0.10, 0.99] × sidereal`  

> **Note:** Only available in equatorial mounts.

---

### `:GMT#` – Get meridian treatment behavior

**Command:**  
`:GMT#`  

**Response:**  
`nnn#`  

- **1st digit – behavior:**
  - `0`: stop at limit  
  - `1`: flip at limit  

- **Last 2 digits**: degrees past meridian limit  

> **Note:** Only available in equatorial mounts.

---

### `:GGF#` – Get RA auto-guiding filter status

**Command:**  
`:GGF#`  

**Response:**  
`"0"` or `"1"`  

- `0`: RA auto-guiding signals get through  
- `1`: RA auto-guiding signals filtered  

When filter is enabled, **all** RA auto-guiding signals (ST-4 and RS-232/Ethernet) are filtered and RA only accepts correction from the built-in encoder. Dec guiding is unaffected.

> **Note:** Only available in equatorial mounts **with encoders**.

---

### `:GPE#` – Get periodic error data integrity

**Command:**  
`:GPE#`  

**Response:**  
`"0"` or `"1"`  

- `1`: periodic error data complete  
- `0`: periodic error data incomplete  

> **Note:** Only available in equatorial mounts **without encoders**.

---

### `:GPR#` – Get periodic error recording status

**Command:**  
`:GPR#`  

**Response:**  
`"0"` or `"1"`  

- `0`: periodic error recording stopped  
- `1`: periodic error is being recorded  

> **Note:** Only available in equatorial mounts **without encoders**.

---

## Change Settings

### Tracking rate selection – `:RTx#`

**Commands:**  

- `:RT0#` – sidereal  
- `:RT1#` – lunar  
- `:RT2#` – solar  
- `:RT3#` – King  
- `:RT4#` – custom  

**Response:**  
`"1"`  

These select the tracking rate. They do **not** affect slewing or arrow-button movement. Sidereal (`:RT0#`) is assumed at next power up.

---

### Arrow-button moving rate – `:SRn#`

**Command:**  
`:SRn#`  

**Response:**  
`"1"`  

Sets the moving rate for N/S/E/W buttons:

- `n = 1–9`
  - `1`: 1× sidereal  
  - `2`: 2×  
  - `3`: 8×  
  - `4`: 16×  
  - `5`: 64×  
  - `6`: 128×  
  - `7`: 256×  
  - `8`: 512×  
  - `9`: maximum speed available  

Default after power up: **64×**.

---

### RA auto-guiding filter – `:SGF0#`, `:SGF1#`

**Commands:**

- `:SGF0#` – RA auto-guiding signals get through  
- `:SGF1#` – RA auto-guiding filter enabled  

**Response:**  
`"1"`  

For details, see `:GGF#`.

> **Note:** Only available in equatorial mounts with encoders. Filter is **disabled** by default on next power up.

---

### Settings saved across power cycles

The following **Change Settings** commands are saved permanently and reapplied across power cycles:

#### Time zone – `:SGsMMM#`

**Command:**  
`:SGsMMM#`  

**Response:**  
`"1"`  

Sets minute offset from UTC (DST not included).  

- Range: `[-720, +780]` minutes  
- Resolution: 1 minute  

---

#### Daylight Saving Time – `:SDS0#`, `:SDS1#`

**Commands:**

- `:SDS1#` – DST has been observed  
- `:SDS0#` – DST has not been observed  

**Response:**  
`"1"`  

---

#### UTC time – `:SUTXXXXXXXXXXXXX#`

**Command:**  
`:SUTXXXXXXXXXXXXX#`  

**Response:**  
`"1"`  

Sets current UTC time:

- `value = (JD(current UTC) – J2000) * 8.64e+7`  
- Resolution: 1 millisecond  

---

#### Longitude – `:SLOsTTTTTTTT#`

**Command:**  
`:SLOsTTTTTTTT#`  

**Response:**  
`"1"`  

Sets current longitude:

- Range: `[-64,800,000, +64,800,000]`  
- East is positive  
- Resolution: 0.01 arc-second  

---

#### Latitude – `:SLAsTTTTTTTT#`

**Command:**  
`:SLAsTTTTTTTT#`  

**Response:**  
`"1"`  

Sets current latitude:

- Range: `[-32,400,000, +32,400,000]`  
- North is positive  
- Resolution: 0.01 arc-second  

---

#### Hemisphere – `:SHE0#`, `:SHE1#`

**Commands:**

- `:SHE0#` – Southern Hemisphere  
- `:SHE1#` – Northern Hemisphere  

**Response:**  
`"1"`  

---

#### Maximum slewing speed – `:MSRn#`

**Command:**  
`:MSRn#`  

**Response:**  
`"1"`  

- `n ∈ {7, 8, 9}`  
  - `7`: 256×  
  - `8`: 512×  
  - `9`: maximum speed  

---

#### Altitude limit – `:SALsnn#`

**Command:**  
`:SALsnn#`  

**Response:**  
`"1"`  

Sets altitude limit (applies to tracking and slewing, not arrow buttons).

- Range: `[-89, +89]` degrees  
- Resolution: 1 degree  

Tracking stops if mount exceeds this limit.

---

#### Guiding rate – `:RGnnnn#`

**Command:**  
`:RGnnnn#`  

**Response:**  
`"1"`  

- First 2 digits: RA guiding rate `0.nn × sidereal`  
- Last 2 digits: Dec guiding rate `0.nn × sidereal`  

Valid ranges:

- RA: `[0.01, 0.90] × sidereal`  
- Dec: `[0.10, 0.99] × sidereal`  

> **Note:** Only available in equatorial mounts.

---

#### Meridian treatment – `:SMTnnn#`

**Command:**  
`:SMTnnn#`  

**Response:**  
`"1"`  

- 1st digit:  
  - `0`: stop at limit  
  - `1`: flip at limit  
- Last 2 digits: degrees past meridian  

> **Note:** Only available in equatorial mounts.

---

### Reset all settings – `:RAS#`

**Command:**  
`:RAS#`  

**Response:**  
`"1"`  

Resets all settings to default, **except**:

- Time zone  
- Daylight-Saving Time observed  
- Date  
- Time  

---

## Mount Motion

### Slew to RA/Dec – `:MS1#`, `:MS2#`

**Commands:**

- `:MS1#` – slew to “normal” position  
- `:MS2#` – slew to “counterweight up” position (equatorial only)  

**Response:**  

- `"1"` – command accepted  
- `"0"` – target below altitude limit or exceeds mechanical limits  

Requires a previously defined RA/Dec pair (`:SRATTTTTTTTT#` + `:SdsTTTTTTTT#`).

- If target below altitude limit, no slewing occurs  
- After slewing, tracking is automatically enabled regardless of previous tracking status  

> **Note:** `:MS2#` only available in equatorial mounts.

---

### Slew to Alt/Az – `:MSS#`

**Command:**  
`:MSS#`  

**Response:**  

- `"1"` – command accepted  
- `"0"` – target below altitude limit or exceeds mechanical limits  

Requires previously defined Alt/Az pair (`:SasTTTTTTTT#` + `:SzTTTTTTTTT#`).

- If target below altitude limit, no slewing  
- After slewing, the mount is **stopped** regardless of tracking status  

---

### Stop all slewing – `:Q#`

**Command:**  
`:Q#`  

**Response:**  
`"1"`  

Stops **all** slewing operations, regardless of source. Tracking status is not affected.

---

### Tracking on/off – `:ST0#`, `:ST1#`

**Commands:**

- `:ST0#` – stop tracking  
- `:ST1#` – start tracking  

**Response:**  
`"1"`  

---

### Pulse guiding – `:ZSXXXXX#`, `:ZQXXXXX#`, `:ZEXXXXX#`, `:ZCXXXXX#`

**Commands:**

- `:ZSXXXXX#` – RA+ direction  
- `:ZQXXXXX#` – RA− direction  
- `:ZEXXXXX#` – Dec+ direction  
- `:ZCXXXXX#` – Dec− direction  

**Response:**  
None  

- Moves axis for `XXXXX` milliseconds at current guiding rate  
- `XXXXX` range: `[0, 99999]` ms  

> **Note:**  
> - Only available in equatorial mounts  
> - Supersede `:MnXXXXX#`, `:MeXXXXX#`, `:MsXXXXX#`, `:MwXXXXX#` (deprecated)

---

### Deprecated pulse commands – `:MnXXXXX#`, `:MeXXXXX#`, `:MsXXXXX#`, `:MwXXXXX#`

**Commands:**  
`:MnXXXXX#`, `:MeXXXXX#`, `:MsXXXXX#`, `:MwXXXXX#`  

**Response:**  
None  

- Move for `XXXXX` ms at current guiding rate  
- Range of `XXXXX`: `[0, 99999]` ms  

> **Note:**  
> - Deprecated, will be removed in a future version  
> - Only available in equatorial mounts  

---

### Park / Unpark – `:MP1#`, `:MP0#`

**Park to defined position – `:MP1#`**

- **Response:**  
  - `"1"` – park accepted  
  - `"0"` – park failed  

Parks to most recently defined parking position.  
In parked mode, the mount cannot move until unparked. If powered off while parked, mount is automatically unparked on next power up.

**Unpark – `:MP0#`**

- **Response:**  
  - `"1"`  

Unparks the mount. Has no effect if already unparked.

---

### Go to zero position – `:MH#`

**Command:**  
`:MH#`  

**Response:**  
`"1"`  

Slews to the zero position immediately.

---

### Auto-search zero (home) – `:MSH#`

**Command:**  
`:MSH#`  

**Response:**  
`"1"`  

Automatically searches mechanical zero/home position using homing sensors. Current zero/home position is overwritten if successful.

Designed to be safe in all cases.

> **Available only on:**
> - CEM120 series  
> - CEM70 series  
> - GEM45 series  
> - CEM40 series  

---

### Periodic error recording – `:SPR0#`, `:SPR1#`

**Commands:**

- `:SPR0#` – stop periodic error recording  
- `:SPR1#` – start periodic error recording  

**Response:**  
`"1"`  

> **Note:** Only available in equatorial mounts without encoders.

---

### Periodic error correction playback – `:SPP0#`, `:SPP1#`

**Commands:**

- `:SPP0#` – disable PEC playback  
- `:SPP1#` – enable PEC playback  

**Response:**  
`"1"`  

> **Note:** Only available in equatorial mounts without encoders.

---

### Custom RA tracking rate – `:RRnnnnn#`

**Command:**  
`:RRnnnnn#`  

**Response:**  
`"1"`  

Sets RA tracking rate:

- Rate = `n.nnnn ×` sidereal  
- Valid range: `[0.1000, 1.9000] ×` sidereal  
- Requires `:RT4#` (Custom Tracking Rate) to be selected to take effect  
- Value is remembered across power cycles  

---

### Continuous motion – `:mn#`, `:me#`, `:ms#`, `:mw#`

**Commands:**

- `:mn#` – Dec−  
- `:me#` – RA−  
- `:ms#` – Dec+  
- `:mw#` – RA+  

**Response:**  
None  

Mount moves continuously at selected speed until stopped by:

- `:qR#`, `:qD#`, or `:Q#`, or  
- Corresponding arrow key on the hand controller  

---

### Stop RA motion – `:qR#`

**Command:**  
`:qR#`  

**Response:**  
`"1"`  

Stops motion from left/right arrows or `:me#` / `:mw#`.  
Other slewing and tracking are not affected.

---

### Stop Dec motion – `:qD#`

**Command:**  
`:qD#`  

**Response:**  
`"1"`  

Stops motion from up/down arrows or `:mn#` / `:ms#`.  
Other slewing and tracking are not affected.

---

## Position

### Synchronize (calibrate) – `:CM#`

**Command:**  
`:CM#`  

**Response:**  
`"1"`  

In **equatorial** mounts:

- Most recently defined RA and Dec become commanded RA/Dec.

In **Alt-Az** mounts:

- Most recently defined Alt/Az become commanded Alt/Az.

Ignored if slewing is in progress. Intended for initial calibration only (not after tracking across the meridian unless pier side is known).

---

### Query available positions – `:QAP#`

**Command:**  
`:QAP#`  

**Response:**  
`"0#"` / `"1#"` / `"2#"`  

Returns number of valid positions for the most recently defined RA/Dec that do not violate mechanical, altitude, or meridian flip limits (normal + counterweight-up).

> **Note:** Equatorial mounts only.

---

### Set target RA – `:SRATTTTTTTTT#`

**Command:**  
`:SRATTTTTTTTT#`  

**Response:**  
`"1"`  

Defines commanded right ascension (RA). Slew and calibrate commands use this RA.

---

### Set target Dec – `:SdsTTTTTTTT#`

**Command:**  
`:SdsTTTTTTTT#`  

**Response:**  
`"1"`  

Defines commanded declination (Dec). Slew and calibrate commands use this Dec.

---

### Set target altitude – `:SasTTTTTTTT#`

**Command:**  
`:SasTTTTTTTT#`  

**Response:**  
`"1"`  

Defines commanded altitude. Move or calibrate commands use this altitude.

- Works with all mounts after slewing commands  
- After synchronization, only works with Alt-Az mounts  

---

### Set target azimuth – `:SzTTTTTTTTT#`

**Command:**  
`:SzTTTTTTTTT#`  

**Response:**  
`"1"`  

Defines commanded azimuth. Move or calibrate commands use this azimuth.

- Works with all mounts after slewing commands  
- After synchronization, only works with Alt-Az mounts  

---

### Set zero position – `:SZP#`

**Command:**  
`:SZP#`  

**Response:**  
`"1"`  

Sets current position as zero position.

---

### Set parking azimuth – `:SPATTTTTTTTT#`

**Command:**  
`:SPATTTTTTTTT#`  

**Response:**  
`"1"`  

Sets azimuth of parking position.

---

### Set parking altitude – `:SPHTTTTTTTT#`

**Command:**  
`:SPHTTTTTTTT#`  

**Response:**  
`"1"`  

Sets altitude of parking position.

---

## Miscellaneous

### Firmware dates – `:FW1#`, `:FW2#`

#### `:FW1#` – Mainboard & hand controller

**Response:**  
`YYMMDDYYMMDD#`  

- First `YYMMDD`: mainboard firmware date  
- Second `YYMMDD`: hand controller firmware date  

#### `:FW2#` – RA & Dec motor boards

**Response:**  
`YYMMDDYYMMDD#`  

- First `YYMMDD`: RA motor board firmware date  
- Second `YYMMDD`: Dec motor board firmware date  

---

### Mount model – `:MountInfo#`

**Command:**  
`:MountInfo#`  

**Response (examples):**

- `"0026"` – CEM26  
- `"0027"` – CEM26-EC  
- `"0028"` – GEM28  
- `"0029"` – GEM28-EC  
- `"0040"` – CEM40(G)  
- `"0041"` – CEM40(G)-EC  
- `"0043"` – GEM45(G)  
- `"0044"` – GEM45(G)-EC  
- `"0070"` – CEM70(G)  
- `"0071"` – CEM70(G)-EC  
- `"0120"` – CEM120  
- `"0121"` – CEM120-EC  
- `"0122"` – CEM120-EC2  

---

## Additional Information

- Command set is ASCII text using only letters, digits, `:`, and `#`.  
- Commands are **case sensitive**.  

Maximum speeds:

- **CEM120 series**: 960× sidereal  
- **CEM70 series**: 900× sidereal  
- **CEM40 & GEM45 series**: 1066× sidereal  
- **CEM26 & GEM28 series**: 1440× sidereal  

---

## Initialization Sequence

When establishing a link, initialize the mount with:

```text
:MountInfo#