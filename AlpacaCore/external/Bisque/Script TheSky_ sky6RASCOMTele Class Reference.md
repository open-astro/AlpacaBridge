## Page 1

3/31/26, 3:37 PM Script TheSky: sky6RASCOMTele Class Reference

&lt;img&gt;SB&lt;/img&gt;

# SOFTWARE BISQUE

# sky6RASCOMTele Class Reference

TheSky6 Classic Objects

---

The sky6RASCOMTele object. More...

Inheritance diagram for sky6RASCOMTele:
<mermaid>
graph TD
    A[QObject] --> B[sky6RASCOMTele]
    C[QScriptable] --> B
    D[QAxBindable] --> B
</mermaid>

## Public Slots

void **Abort ()**
Use this method to abort (stop) any telescope operation currently in progress for example a slew.

void **CommuateMotors ()**
Performs a commutation of the control system's motors (Paramount GT-1100 only). More...

void **Connect ()**
Establishes communication between this telescope object and the TheSky.

void **DoCommand** (int IParam, QString qsParam)
Use this method to send ASCII commands to the telescope, and receive ASCII responses from the telescope. More...

void **Disconnect ()**
Terminates the connection between this telescope object (RASCOMTele) and the TheSky. More...

void **FindHome ()**
Locates the telescope's home position.

void **FocusInFast ()**
Turns the motorized focuser clockwise using the fastest focusing rate.

void **FocusInSlow ()**
Turns the motorized focuser clockwise using the slowest focusing rate.

void **FocusOutFast ()**
Turns the motorized focuser counterclockwise using the highest focusing rate.

void **FocusOutSlow ()**
Turns the motorized focuser counterclockwise using the lowest focusing rate.

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html &lt;page_number&gt;1/14&lt;/page_number&gt;

---


## Page 2

3/31/26, 3:37 PM
Script TheSky: sky6RASCOMTele Class Reference

void **GetAzAlt ()**
Gets the azimuth and altitude coordinates of the telescope. More...

void **GetRaDec ()**
Gets the equatorial (right ascension and declination) coordinates of the telescope. More...

void **Jog** (double dJogAmtInMins, QString lpszDirection)
Slews the telescope a fixed angular distance, or "jogs" the telescope in a specific direction. More...

void **Park ()**
Slews the telescope to its park position, parks the telescope and because of requirements of a third party application established in TheSky, terminates communications between the telescope and TheSky. More...

void **SetParkPosition ()**
Sets the park position for the telescope. More...

void **SetTracking** (int lOn, int lIgnoreRates, double dRaRate, double dDecRate)
Sets the telescope's tracking rate in the right ascension and declination axis for those devices that are capable. More...

void **SlewToAzAlt** (double dAz, double dAlt, QString lpszObjectName)
Slews the telescope to the specified azimuth and altitude coordinates. More...

void **SlewToRaDec** (double dRa, double dDec, QString lpszObjectName)
Slews the telescope to the specified right ascension and declination coordinates. More...

void **Sync** (double dRa, double dDec, QString lpszObjectName)
Synchronizes the telescope's control system at the given right ascension and declination coordinates. More...

void **ParkAndDoNotDisconnect ()**
This method is a competitor to **Park()** and slews the telescope to its park position and parks the telescope but does not terminate connection to the mount as **Park()** does. More...

void **Unpark ()**
Unparks the telescope. More...

bool **IsParked ()**
Returns non zero if the mount is parked or zero if the mount is not parked. More...

void **ConnectAndDoNotUnpark ()**
This method is a competitor to **Connect()**. More...

## Properties

int **Asynchronous**
The property holds if the **sky6RASCOMTele** object is operating synchronously or asynchronously. More...

double **dAlt**
A double value that is the current altitude (in decimal degrees) of the telescope. More...

double **dAz**
A double value that is the current azimuth (in decimal degrees) of the telescope. More...

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html
&lt;page_number&gt;2/14&lt;/page_number&gt;

---


## Page 3

3/31/26, 3:37 PM
Script TheSky: sky6RASCOMTele Class Reference

double **dDec**
A double value that is the declination of the telescope. More...

double **dDecTrackingRate**
Returns the tracking rate of the telescope for the declination axis. More...

double **dRa**
Returns the right ascension of the telescope. More...

double **dRaTrackingRate**
Returns the tracking rate of the telescope for the right ascension axis. More...

double **LastSlewError**
Returns the error code of the last telescope slew. More...

int **IsConnected**
Returns non-zero if the connection to the telescope exists or zero if there is no connection. More...

int **IsInLimit**
Returns a value that determines if the telescope is within a telescope limit region. More...

int **IsSlewComplete**
Returns a value that determines if the telescope slew is complete (non-zero) or if the telescope is slewing (zero). More...

int **IsTracking**
Returns a value that determines if the telescope tracking is currently on. More...

QString **DoCommandOutput**
This property holds the result of calling the **DoCommand()** method. More...

## Detailed Description

The **sky6RASCOMTele** object.

The **sky6RASCOMTele** Object allows scripted control of most any computerized "go to" mount compatible with TheSky.

## Member Function Documentation

### CommutateMotors

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html
&lt;page_number&gt;3/14&lt;/page_number&gt;

---


## Page 4

3/31/26, 3:37 PM Script TheSky: sky6RASCOMTele Class Reference

```c++
void sky6RASCOMTele::CommuateMotors ( )
```
slot

Performs a commutation of the control system's motors (Paramount GT-1100 only).

This command is specific to the SBIG TCE-1 control system. This control system must go through a one-time 'initialization' called commutation.

---

**ConnectAndDoNotUnpark**

```c++
void sky6RASCOMTele::ConnectAndDoNotUnpark ( )
```
slot

This method is a competitor to Connect().

This method may only be needed if ParkAndDoNotDisconnect() is used instead of Park(). It establishes a connection to the telescope without automatically unparking as Connect() does.
This allows the flexibility of connecting without changing the state of park. Some mounts like the Paramount automatically unpark upon disconnect avoiding park state conflicts with finding home. This method is not part of the TheSky6 standard and was added in TheSky build 9231 (see Application.build).

See also
ParkAndDoNotDisconnect Unpark IsParked

---

**Disconnect**

```c++
void sky6RASCOMTele::Disconnect ( )
```
slot

Terminates the connection between this telescope object (RASCOMTele) and the TheSky.

Note that this command does *not* terminate the telescope link in TheSky so that multiple clients can continue to use the telescope. If you must actually terminate TheSky's link to the telescope, do that manually or by sky6RASCOMTheSky::DisconnectTelescope(), noting the multiple client warning.

---

**DoCommand**

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html &lt;page_number&gt;4/14&lt;/page_number&gt;

---


## Page 5

3/31/26, 3:37 PM Script TheSky: sky6RASCOMTele Class Reference

```c++
void sky6RASCOMTele::DoCommand ( int lParam,
                                 QString qsParam
                                )
```
slot

Use this method to send ASCII commands to the telescope, and receive ASCII responses from the telescope.

This command is useful, for example, to send commands to a serial communication based telescope that are not included in the RASCOMTele object.

**Parameters**

**lParam** This parameter is used to perform different operations, depending upon the value of lParam (see) below.

**qsParam** is the string to be sent

The result of calling this method is stored in the **DoCommandOutput()** property.

**lParam values:**

0 Locks the serial port. This operation is performed first so that other processes cannot access the port.

1 Purges the serial port's transmit buffer. This ensures that only the correct characters are sent to the telescope.

2 Purges the serial port's receive buffer. This ensures that only the correct number of characters (response) is received from the telescope.

3 Sends the ASCII command out the serial port.

4 Get the number of bytes that are available to read from the serial port.

5 Reads a string of ASCII text from the serial port.

6 Unlocks the serial port so that other processes can access it.

9 Start an open loop move (see http://www.bisque.com/x2standard/class_open_loop_move_interface.html) where the qsParam is pipe separated string indicating a direction number and device dependent, zero based rate index number. For direction, North = 0, South = 1, East = 2, West = 3.
Here are two examples, to move North at the 2nd open loop move rate, set qsParam="0|1", to move West at the first open loop move rate set qsParam="3|0".

10 Ends the open loop move.

11 Returns 1 if a mount is beyond the pole otherwise 0 (see http://www.bisque.com/x2standard/class_asymmetrical_equatorial_interface.html).

12 Get/set the All Sky option "Use AllSky Image Link for scripted Image Link"

13 Get/set the All Sky option "Use AllSky Image Link for automated pointing runs"

14 Do not use

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html &lt;page_number&gt;5/14&lt;/page_number&gt;

---


## Page 6

3/31/26, 3:37 PM
Script TheSky: sky6RASCOMTele Class Reference

15 Returns the architecture of the CPU that TheSky is currently running on, in text format. The return value depends on what the OS will report and may not detect the actual CPU architecture if the OS hides that information.

Typical returned values (not exhaustive)
- "arm"
- "arm64"
- "i386"
- "ia64"
- "mips"
- "mips64"
- "power"
- "power64"
- "sparc"
- "sparcv9"
- "x86_64"

16 Get or set the current atmospheric pressure setting. To get the current value, set "=pressure" as the string argument. To set the current value, set "pressure=1000" say, as the string argument. Units are mB.

◆ **GetAzAlt**
void sky6RASCOMTele::GetAzAlt ( )
Gets the azimuth and altitude coordinates of the telescope.
The results of calling this method may be found in the dAz(), dAIt() properties.
slot

◆ **GetRaDec**
void sky6RASCOMTele::GetRaDec ( )
Gets the equatorial (right ascension and declination) coordinates of the telescope.
The results of calling this method may be found in the dRa(), dDec() properties.
slot

◆ **IsParked**

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html
&lt;page_number&gt;6/14&lt;/page_number&gt;

---


## Page 7

3/31/26, 3:37 PM Script TheSky: sky6RASCOMTele Class Reference

bool sky6RASCOMTele::IsParked ( )
slot

Returns non zero if the mount is parked or zero if the mount is not parked.

This method may only be needed if **ParkAndDoNotDisconnect()** is used instead of Park(). This method is not part of the TheSky6 standard and was added in TheSky build 9231 (see Application.build).

See also
ParkAndDoNotDisconnect Unpark ConnectAndDoNotUnpark

---

◆ Jog

void sky6RASCOMTele::Jog ( double dJogAmtInMins,
QString lpszDirection
)
slot

Slews the telescope a fixed angular distance, or "jogs" the telescope in a specific direction.

Parameters
dJogAmtInMins A double number that specifies the amount of the jog, in arcminutes.
lpszDirection A string that specifies the direction of the jog.

The following lpszDirection are valid:

"N" Moves the telescope North in declination.
"S" Moves the telescope South in declination.
"E" Moves the telescope East in right ascension.
"W" Moves the telescope West in right ascension.
"L" Moves the telescope Left in azimuth.
"R" Moves the telescope Right in azimuth.
"U" Moves the telescope Up in altitude.
"D" Moves the telescope in down in altitude.

---

◆ Park

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html &lt;page_number&gt;7/14&lt;/page_number&gt;

---


## Page 8

3/31/26, 3:37 PM Script TheSky: sky6RASCOMTele Class Reference

void sky6RASCOMTele::Park ( )
slot

Slews the telescope to its park position, parks the telescope and because of requirements of a third party application established in TheSky, terminates communications between the telescope and TheSky.

For asynchronous operation this method can be monitored with IsSlewComplete() followed by IsConnected(). If the park fails for any reason an exception is thrown and communication to the mount is *not* terminated. It follows that if IsConnected() returns zero after Park, the Park completed successfully and conversely if IsConnected() returns non zero after Park, the park failed and an exception will have been thrown. Note, unparking is automatically handled in Connect().

See also
ParkAndDoNotDisconnect

---

*   **ParkAndDoNotDisconnect**

void sky6RASCOMTele::ParkAndDoNotDisconnect ( )
slot

This method is a competitor to Park() and slews the telescope to its park position and parks the telescope but does not terminate connection to the mount as Park() does.

When the telescope is parked, it cannot be slewed. To unpark the telescope call Unpark() (note Unpark() happens automatically upon Connect()). This method is not part of the TheSky6 standard and was added in TheSky build 9231 (see Application.build).

See also
Park Unpark IsParked ConnectAndDoNotUnpark

---

*   **SetParkPosition**

void sky6RASCOMTele::SetParkPosition ( )
slot

Sets the park position for the telescope.

See also
Park

---

*   **SetTracking**

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html &lt;page_number&gt;8/14&lt;/page_number&gt;

---


## Page 9

3/31/26, 3:37 PM Script TheSky: sky6RASCOMTele Class Reference

```c++
void sky6RASCOMTele::SetTracking ( int IOn,
                                   int lIgnoreRates,
                                   double dRaRate,
                                   double dDecRate
                                  )
```
slot

Sets the telescope's tracking rate in the right ascension and declination axis for those devices that are capable.

**Parameters**

**IOn** A number to turn tracking on or off - Valid values, 1 turns tracking on, 0 turns tracking off.

**lIgnoreRates** A number that specifies whether or not to use the telescope's current tracking rate, or use the sidereal tracking rate.

Valid values:
1 = Ignores the values for dRaRate and dDecRate.
0 = Use the values for dRaRate and dDecRate.

**dRaRate** A double that specifies the new right ascension tracking rate in arcseconds/second - make sure to set lIgnoreRates parameter is set to zero.

**dDecRate** A double that specifies the new declination tracking rate in arcseconds/second - make sure to set lIgnoreRates parameter is set to zero.

---

*   **SlewToAzAlt**

```c++
void sky6RASCOMTele::SlewToAzAlt ( double dAz,
                                    double dAlt,
                                    QString lpszObjectName
                                   )
```
slot

Slews the telescope to the specified azimuth and altitude coordinates.

**Parameters**

**dAz** A double that specifies the azimuth to slew the telescope.

**dAlt** A double number that specifies the altitude to slew the telescope.

**lpszObjectName** A string that specifies the object's name.

---

*   **SlewToRaDec**

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html &lt;page_number&gt;9/14&lt;/page_number&gt;

---


## Page 10

3/31/26, 3:37 PM
Script TheSky: sky6RASCOMTele Class Reference

```c++
void sky6RASCOMTele::SlewToRaDec ( double dRa,
                                    double dDec,
                                    QString lpszObjectName
                                )
```
slot

Slews the telescope to the specified right ascension and declination coordinates.

Note - Coordinates used to slew the telescope are normally for the current epoch.

**Parameters**

**dRa** A double that specifies the right ascension to slew the telescope.
**dDec** A double that specifies the declination to slew the telescope.
**lpszObjectName** A string that specifies the object's name.

---

**Sync**

```c++
void sky6RASCOMTele::Sync ( double dRa,
                            double dDec,
                            QString lpszObjectName
                        )
```
slot

Synchronizes the telescope's control system at the given right ascension and declination coordinates.

**Parameters**

**dRa** A double that specifies the right ascension to synchronize the telescope.
**dDec** A double that specifies the declination to synchronize the telescope.
**lpszObjectName** A string that specifies the object's name.

The telescope must support the "Sync" command for this method to function normally.

---

**Unpark**

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html
&lt;page_number&gt;10/14&lt;/page_number&gt;

---


## Page 11

3/31/26, 3:37 PM Script TheSky: sky6RASCOMTele Class Reference

```c++
void sky6RASCOMTele::Unpark()
```
slot

Unparks the telescope.

This method may only be needed if **ParkAndDoNotDisconnect()** is used instead of Park(). This method is not part of the TheSky6 standard and was added in TheSky build 9231 (see Application.build).

**See also**
*   ParkAndDoNotDisconnect
*   IsParked
*   ConnectAndDoNotUnpark

---

# Property Documentation

---

## Asynchronous

sky6RASCOMTele::Asynchronous
(read) (write)

The property holds if the **sky6RASCOMTele** object is operating synchronously or asynchronously.

See the **Synchronous vs. Asynchronous Execution** topic for more information about the differences between these methods of execution.

---

## dAlt

sky6RASCOMTele::dAlt
(read)

A double value that is the current altitude (in decimal degrees) of the telescope.

Before using this property, the **GetAzAlt()** method must be called first.

---

## dAz

sky6RASCOMTele::dAz
(read)

A double value that is the current azimuth (in decimal degrees) of the telescope.

Before using this property, the **GetAzAlt()** method must be called first.

---

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html &lt;page_number&gt;11/14&lt;/page_number&gt;

---


## Page 12

3/31/26, 3:37 PM Script TheSky: sky6RASCOMTele Class Reference

*   **dDec**
    sky6RASCOMTele::dDec
    A double value that is the declination of the telescope.
    Before using this property, the GetRaDec() method must be called.

*   **dDecTrackingRate**
    sky6RASCOMTele::dDecTrackingRate
    Returns the tracking rate of the telescope for the declination axis.
    This property is only available with certain telescope hardware.

*   **DoCommandOutput**
    sky6RASCOMTele::DoCommandOutput
    This property holds the result of calling the DoCommand() method.
    See also
        DoCommand()

*   **dRa**
    sky6RASCOMTele::dRa
    Returns the right ascension of the telescope.
    Before using this property, the GetRaDec() method must be called.

*   **dRaTrackingRate**

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html &lt;page_number&gt;12/14&lt;/page_number&gt;

---


## Page 13

3/31/26, 3:37 PM Script TheSky: sky6RASCOMTele Class Reference

sky6RASCOMTele::dRaTrackingRate read

Returns the tracking rate of the telescope for the right ascension axis.

This property is only available with certain telescope hardware.

*   **IsConnected**

sky6RASCOMTele::IsConnected read

Returns non-zero if the connection to the telescope exists or zero if there is no connection.

Use this command to determine if TheSky is successfully linked (communicating with) the telescope.

*   **IsInLimit**

sky6RASCOMTele::IsInLimit read

Returns a value that determines if the telescope is within a telescope limit region.

Note: This property is currently not supported.

*   **IsSlewComplete**

sky6RASCOMTele::IsSlewComplete read

Returns a value that determines if the telescope slew is complete (non-zero) or if the telescope is slewing (zero).

Use this command to determine if the telescope is slewing. Note: The telescope must be using asynchronous slewing for this command to work.

See the Synchronous vs. Asynchronous Execution topic for more information about the differences between these methods of execution.

*   **IsTracking**

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html &lt;page_number&gt;13/14&lt;/page_number&gt;

---


## Page 14

3/31/26, 3:37 PM Script TheSky: sky6RASCOMTele Class Reference

sky6RASCOMTele::IsTracking [read]

Returns a value that determines if the telescope tracking is currently on.

The "tracking rate" commonly refers to equatorial telescope's constant motion in the right ascension axis to follow stars. Use this command to determine if the telescope is currently tracking. For equatorial telescopes, the tracking rate is usually set to a sidereal rate in the right ascension axis. For altitude/azimuth mounts, the tracking rate varies for both telescope axes.

---

LastSlewError

sky6RASCOMTele::LastSlewError [read]

Returns the error code of the last telescope slew.

When an error occurs resulting from a telescope slew (any command that causes the telescope to move), call this function to retrieve the specific error code. Software Bisque recommends that this property be monitored after every method that moves the telescope, for example, Park(), FindHome(), Jog(), SlewToRaDec(), SlewToAzAlt().

---

ScriptTheSkyX Examples
(C) Software Bisque, Inc. All rights reserved.

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_tele.html &lt;page_number&gt;14/14&lt;/page_number&gt;