## Page 1

Sky-Watcher® Application Notes

# SynScan™ Serial Communication Protocol

Version 3.3

This document describes the serial commands of Sky-Watcher® SynScan™ hand control. This information applies to the SynScan™ V3, SynScan™ V4 series GOTO mounts.

Communication to the hand control is 9600 bits/sec, no parity and one stop bit via the RS-232 port on the base of the hand control.

**Note:** This communication protocol applies to SynScan hand control with SynScan™ V3 firmware version 3.38 or SynScan™ V4 firmware version 4.38.06 or later.

## Get Position Commands

The following commands retrieve the position of the telescope in either RA/DEC or AZM-ALT coordinates. The position is returned as a hexadecimal value that represents the fraction of a revolution around the axis. Two examples are given below:
*   If the Get RA/DEC command returns 34AB,12CE then the DEC value is 12CE in hexadecimal. As a percentage of a revolution, this is 4814/65536 = 0.07346. To calculate degrees, simply multiply by 360, giving a value of 26.4441 degrees.
*   If the precise GET AZM-ALT command returns 12AB0500,40000500 then the AZM value is 12AB05 in 24-bit hexadecimal. As a percentage of a revolution, this is 1,223,429/16,777,216 *360 = 26.252 degrees. Note: the last two digits (00) of the AZM and ALT values are discarded or ignored.

The standard commands offer a precision of 1/65536 * 360 * 60 * 60 = about 19.8 arcseconds per unit while the precise commands offer a precision of 1/16777216 * 360 * 60 * 60 = about 0.08 arcseconds per unit (only the upper 24 bits are used).

**Note:** if the telescope has not been aligned, the RA/DEC values will not be meaningful and the AZM-ALT values will be relative to where the telescope was powered on. After alignment, RA/DEC values will reflect the actual sky, and the coordinates will be on J.2000 epoch basis.

<table>
<thead>
<tr>
<th>Command Function</th>
<th>PC Command</th>
<th>Hand Control Response</th>
</tr>
</thead>
<tbody>
<tr>
<td>Get RA/DEC</td>
<td>"E"</td>
<td>"34AB, 12CE#"</td>
</tr>
<tr>
<td>Get precise RA/DEC</td>
<td>"e"</td>
<td>"34AB0500, 12CE0500#"</td>
</tr>
<tr>
<td>Get AZM-ALT</td>
<td>"Z"</td>
<td>"12AB, 4 000#"</td>
</tr>
<tr>
<td>Get precise AZM-ALT</td>
<td>"z"</td>
<td>"12AB0500, 40000500#"</td>
</tr>
</tbody>
</table>

## GOTO Commands

The following commands direct the telescope to GOTO a specified RA/DEC or AZM-ALT position. With the Get Position commands, the values are in hexadecimal and represent the fraction of a rotation around the axis.
**Notes:** GOTO RA/DEC commands will not work unless the telescope is aligned. The execution of GOTO RA/DEC commands will enable tracking if it is stopped.

SSAP-F-160414V1-EN
&lt;page_number&gt;Page 1&lt;/page_number&gt;

---


## Page 2

Sky-Watcher® Application Notes

<table>
  <thead>
    <tr>
      <th>Command Function</th>
      <th>PC Command</th>
      <th>Hand Control Response</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>GOTO RA/DEC</td>
      <td>"R34AB, 12CE"</td>
      <td>"#"</td>
    </tr>
    <tr>
      <td>GOTO precise RA/DEC</td>
      <td>"r34AB0500, 12CE0500"</td>
      <td>"#"</td>
    </tr>
    <tr>
      <td>GOTO AZM-ALT</td>
      <td>"B12AB, 4000"</td>
      <td>"#"</td>
    </tr>
    <tr>
      <td>GOTO precise AZM-ALT</td>
      <td>"b12AB0500, 40000500"</td>
      <td>"#"</td>
    </tr>
  </tbody>
</table>

## Sync Commands

To Sync to an object via serial commands, the user should center a known object in an eyepiece. Then the Sync serial command should be sent, using the celestial coordinates (RA and DEC in J.2000 epoch) for that object. This causes future GOTO or Get Position commands to use coordinates relative to the Sync'd position, improving pointing accuracy to nearby objects. The format for the RA/DEC positions in the Sync command is identical to the GOTO RA/Dec command.

<table>
  <thead>
    <tr>
      <th>Command Function</th>
      <th>PC Command</th>
      <th>Hand Control Response</th>
      <th>Applies to Handset Versions</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Sync RA/DEC</td>
      <td>"S34AB, 12CE"</td>
      <td>"#"</td>
      <td>3.37 or later<br>4.37.03 or later</td>
    </tr>
    <tr>
      <td>Sync precise RA/DEC</td>
      <td>"s34AB0500, 12CE0500"</td>
      <td>"#"</td>
      <td>3.37 or later<br>4.37.03 or later</td>
    </tr>
  </tbody>
</table>

## Tracking Commands

The following commands retrieve or set the tracking mode.
Depending on the mount type, following tracking modes are available:
0 = Tracking off
1 = Alt/Az tracking
2 = Equatorial tracking
3 = PEC mode (Sidereal + PEC)

<table>
  <thead>
    <tr>
      <th>Command Function</th>
      <th>PC Command</th>
      <th>Hand Control Response</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Get Tracking Mode</td>
      <td>"t"</td>
      <td>chr(mode) & "#"</td>
    </tr>
    <tr>
      <td>Set Tracking Mode</td>
      <td>"T" & chr(mode)</td>
      <td>"#"</td>
    </tr>
  </tbody>
</table>

## Slewling Commands

The following commands allow you to slew (move) the telescope at fixed or variable rates.
For **variable rates**, multiply the desired rate by 4 and then separate it into a high and low byte. For example if the desired tracking rate is 150 arcseconds/second, then:
trackRateHigh = (150 * 4) \ 256 = 2, and
trackRateLow = (150 * 4) mod 256 = 88

For **fixed rates**, simply use a value from 1-9 (or 0 to stop) to mimic the equivalent hand control rates.
**Note:** in most configurations, issuing the slew commands will override (or conflict with) the tracking mode of the mount. Hence it is always best to disable tracking first using the **Tracking Commands**, issue the slew command, and then re-enable tracking. The main exception to this is when tracking equatorially - the **fixed rate** slews at 1 or 2 will not override tracking. This can be useful to simulate auto-guiding.

SSAP-F-160414V1-EN
&lt;page_number&gt;Page 2&lt;/page_number&gt;

---


## Page 3

Sky-Watcher® Application Notes

<table>
  <thead>
    <tr>
      <th>Command Function</th>
      <th>PC Command</th>
      <th>Hand Control Response</th>
      <th>Applies to Handset Versions</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Variable rate Azm (or RA) slew in positive direction</td>
      <td>"P" & chr(3) & chr(16) & chr(6) & chr(trackRateHigh) & chr(trackRateLow) & chr(0) & chr(0)</td>
      <td>"#"</td>
      <td>3.03 or later<br>4.01.01 or later</td>
    </tr>
    <tr>
      <td>Variable rate Azm (or RA) slew in negative direction</td>
      <td>"P" & chr(3) & chr(16) & chr(7) & chr(trackRateHigh) & chr(trackRateLow) & chr(0) & chr(0)</td>
      <td>"#"</td>
      <td>3.03 or later<br>4.01.01 or later</td>
    </tr>
    <tr>
      <td>Variable rate Alt (or Dec) slew in positive direction</td>
      <td>"P" & chr(3) & chr(17) & chr(6) & chr(trackRateHigh) & chr(trackRateLow) & chr(0) & chr(0)</td>
      <td>"#"</td>
      <td>3.03 or later<br>4.01.01 or later</td>
    </tr>
    <tr>
      <td>Variable rate Alt (or Dec) slew in negative direction</td>
      <td>"P" & chr(3) & chr(17) & chr(7) & chr(trackRateHigh) & chr(trackRateLow) & chr(0) & chr(0)</td>
      <td>"#"</td>
      <td>3.03 or later<br>4.01.01 or later</td>
    </tr>
    <tr>
      <td>Fixed rate Azm (or RA) slew in positive direction</td>
      <td>"P" & chr(2) & chr(16) & chr(36) & chr(rate) & chr(0) & chr(0) & chr(0)</td>
      <td>"#"</td>
      <td>3.03 or later<br>4.01.01 or later</td>
    </tr>
    <tr>
      <td>Fixed rate Azm (or RA) slew in negative direction</td>
      <td>"P" & chr(2) & chr(16) & chr(37) &</td>
      <td>"#"</td>
      <td>3.03 or later<br>4.01.01 or later</td>
    </tr>
  </tbody>
</table>

<footer>SSAP-F-160414V1-EN</footer>&lt;page_number&gt;Page 3&lt;/page_number&gt;

---


## Page 4

Sky-Watcher® Application Notes

<table>
  <tr>
    <td></td>
    <td>chr (rate) &<br>chr (0) &<br>chr (0) &<br>chr (0)</td>
    <td></td>
    <td></td>
  </tr>
  <tr>
    <td>Fixed rate ALT (or DEC)<br>slew in positive direction</td>
    <td>"P" &<br>chr (2) &<br>chr (17) &<br>chr (36) &<br>chr (rate) &<br>chr (0) &<br>chr (0) &<br>chr (0)</td>
    <td>"#"</td>
    <td>3.03 or later<br>4.01.01 or later</td>
  </tr>
  <tr>
    <td>Fixed rate ALT (or DEC)<br>slew in negative direction</td>
    <td>"P" &<br>chr (2) &<br>chr (17) &<br>chr (37) &<br>chr (rate) &<br>chr (0) &<br>chr (0) &<br>chr (0)</td>
    <td>"#"</td>
    <td>3.03 or later<br>4.01.01 or later</td>
  </tr>
</table>

**Time/Location Commands (Hand Control)**

The following commands set the time and location in the hand control.
The format of the location commands is: ABCDEFGH, where:
A is the number of degrees of latitude.
B is the number of minutes of latitude.
C is the number of seconds of latitude.
D is 0 for north and 1 for south.
E is the number of degrees of longitude.
F is the number of minutes of longitude.
G is the number of seconds of longitude.
H is 0 for east and 1 for west.
For example, to set the location to 118°20'17" W, 33°50'41" N, you would send (note that latitude is before longitude):
"W" & chr(33) & chr(50) & chr(41) & chr(0) & chr(118) & chr(20) & chr(17) & chr(1)

The format of the time commands is: QRSTUVWX, where:
Q is the hour (24 hour clock).
R is the minutes.
S is the seconds.
T is the month.
U is the day.
V is the year (century assumed as 20).
W is the offset from GMT for the time zone. **Note:** if zone is negative, use 256-zone.
X is 1 to enable Daylight Savings and 0 for Standard Time.
For example, to set the time to 3:26:00PM on April 6, 2005 in the Eastern time zone (-5 UTC: 256-5 = 251) you would send:
"H" & chr(15) & chr(26) & chr(0) & chr(4) & chr(6) & chr(5) & chr(251) & chr(1)

**Note:** All values are sent in binary format, not ASCII.

SSAP-F-160414V1-EN
&lt;page_number&gt;Page 4&lt;/page_number&gt;

---


## Page 5

Sky-Watcher® Application Notes

<table>
  <thead>
    <tr>
      <th>Command Function</th>
      <th>PC Command</th>
      <th>Hand Control</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Get Location</td>
      <td>"w"</td>
      <td>chr(A) & chr(B) & chr(C) & chr(D) & chr(E) & chr(F) & chr(G) & chr(H) & "#"</td>
    </tr>
    <tr>
      <td>Set Location</td>
      <td>"W" & chr(A) & chr(B) & chr(C) & chr(D) & chr(E) & chr(F) & chr(G) & chr(H)</td>
      <td>"#"</td>
    </tr>
    <tr>
      <td>Get Time</td>
      <td>"h"</td>
      <td>chr(Q) & chr(R) & chr(S) & chr(T) & chr(U) & chr(V) & chr(W) & chr(X) & "#"</td>
    </tr>
    <tr>
      <td>Set Time</td>
      <td>"H" & chr(Q) & chr(R) & chr(S) & chr(T) & chr(U) & chr(V) & chr(W) & chr(X)</td>
      <td>"#"</td>
    </tr>
  </tbody>
</table>

Miscellaneous Commands

<table>
  <thead>
    <tr>
      <th>Command Function</th>
      <th>PC Command</th>
      <th>Hand Control Response</th>
      <th>Notes</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Get Version</td>
      <td>"V"</td>
      <td>Replies 6 hexadecimal digits in ASCII and ends with "#", i.e. if the version is 04.37.07, then the hand control will responses "042507#",</td>
      <td>Hand Control responses its firmware version in 6 hexadecimal digits in ASCII. Each hexadecimal digits will be one of '0'~'9' and 'A'~'F'.</td>
    </tr>
  </tbody>
</table>

<footer>SSAP-F-160414V1-EN</footer>&lt;page_number&gt;Page 5&lt;/page_number&gt;

---


## Page 6

Sky-Watcher® Application Notes

<table>
  <thead>
    <tr>
      <th></th>
      <th></th>
      <th></th>
      <th></th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Get Device Version<br>Devices include:<br>16 = AZM/RA Motor<br>17 = ALT/DEC Motor</td>
      <td>"P" &<br>chr(1) &<br>chr(dev) &<br>chr(254) &<br>chr(0) &<br>chr(0) &<br>chr(0) &<br>chr(2)</td>
      <td>where 37 is<br>0x25 in hex.<br>chr(major) &<br>chr(minor) &<br>"#"</td>
      <td>Hand Control responses the motor controller firmware version.</td>
    </tr>
    <tr>
      <td>Get Model<br>0 = EQ6 GOTO Series<br>1 = HEQ5 GOTO Series<br>2 = EQ5 GOTO Series<br>3 = EQ3 GOTO Series<br>4 = EQ8 GOTO Series<br>5 = AZ-EQ6 GOTO Series<br>6 = AZ-EQ5 GOTO Series<br><br>128 ~ 143 = AZ GOTO Series<br>144 ~ 159 = DOB GOTO Series<br>160 = AllView GOTO Series</td>
      <td>"m"</td>
      <td>chr(model) &<br>"#"</td>
      <td>Hand Control responses the model of mount.</td>
    </tr>
    <tr>
      <td>Echo<br>- useful to check communication</td>
      <td>"K" & chr(x)</td>
      <td>chr(x) & "#"</td>
      <td></td>
    </tr>
    <tr>
      <td>Is Alignment Complete?<br>- align=1 if aligned and 0 if not</td>
      <td>"J"</td>
      <td>chr(align) & #</td>
      <td></td>
    </tr>
    <tr>
      <td>Is GOTO in Progress?<br>- Response is ASCII "0"<br>or "1"</td>
      <td>"L"</td>
      <td>prog & "#"</td>
      <td></td>
    </tr>
    <tr>
      <td>Cancel GOTO</td>
      <td>"M"</td>
      <td>"#"</td>
      <td></td>
    </tr>
    <tr>
      <td>Get Mount Pointing State</td>
      <td>"p"</td>
      <td>"E" or "W"<br>& "#"</td>
      <td>Hand Control responses the mount current pointing state.<br>For northern "E" means no flipping (OTA is on the eastern side of meridian), "W" means flipped (OTA is on the western side). For southern hemisphere, "E" means flipped, "W" means not flipped.</td>
    </tr>
  </tbody>
</table>

<footer>SSAP-F-160414V1-EN</footer>
&lt;page_number&gt;Page 6&lt;/page_number&gt;

---


## Page 7

Sky-Watcher® Application Notes

## Additional Commands

### Sending a Slow-Goto command through RS232 to the hand control

1. Convert the angle position to a 24bit number. Example: if the desired position is 220°, then POSITION_24BIT = (220/360) * 2^24 = 10,252,743.
2. Separate POSITION_24BIT into three bytes such that (POSITION_24BIT = PosHighByte * 65536 + PosMedByte * 256 + PosLowByte). Example: PosHightByte = 156, PosMedByte = 113, PosLowByte = 199.
3. Send the following 8 bytes:
    a. Azm Slow Goto: ‘P’, chr( 4), chr(16), chr(23), chr(PosHighByte), chr(PosMedByte), chr(PosLowByte), chr(0).
    b. Alt Slow Goto: ‘P’, chr(4), chr(17), chr(23), chr(PosHighByte), chr(PosMedByte), chr(PosLowByte), chr(0).
4. The character ‘#’, chr(35), is returned from the hand control.

### Resetting the position of Azm/RA or Alt/Dec

1. Convert the angle position to a 24bit number, same as Slow-Goto example.
2. Send the following 8 bytes:
    a. Azm (or RA) Set Position: ‘P’, chr(4), chr(16), chr(4), chr(PosHighByte), chr(PosMedByte), chr(PosLowByte), chr(0).
    b. Alt (or Dec) Set Position: ‘P’, chr(4), chr(17), chr(4), chr(PosHighByte), chr(PosMedByte), chr(PosLowByte), chr(0).
3. The character ‘#’, chr(35), is returned from the hand control.

## Developer Notes

### Timeouts

If the hand control is sent a command that requires it to communicate with another device, then the hand control will make multiple attempts to get the message through in the event of communications problems. Examples include “Get Position” commands, “GOTO in Progress” commands, etc.
Software drivers should be prepared to wait up to 5.0s (worst case scenario) for a hand control response. If serial commands are “blindly” sent without waiting for a response, then some commands may be dropped or the software driver could see responses that are for earlier commands.

<footer>© 2016 Sky-Watcher
www.skywatcher.com
All rights reserved.</footer>
<footer>SSAP-F-160414V1-EN</footer>&lt;page_number&gt;Page 7&lt;/page_number&gt;