## Page 1

3/31/26, 3:35 PM Script TheSky: sky6RASCOMTheSky Class Reference

&lt;img&gt;SB&lt;/img&gt;

# SOFTWARE BISQUE

# sky6RASCOMTheSky Class Reference

TheSky6 Classic Objects

---

The sky6RASCOMTheSky object. More...

Inheritance diagram for sky6RASCOMTheSky:
<mermaid>
graph TD
    A[QObject] --> B[QScriptable]
    B --> C[sky6RASCOMTheSky]
    D[QAxBindable] --> C
</mermaid>

## Public Slots

void **Abort ()**

void **AutoMap ()**
Automatically determine the coordinates of the telescope's current position and add this point to a TPoint model. More...

void **Connect ()**
The method creates the RASCOMTheSky object and connects this object to TheSky software.

void **ConnectDome ()**
Establishes communications to the dome. More...

void **CoupleDome ()**
Couples or binds the dome's slit with the telescopes optic. More...

void **Disconnect ()**
Terminates communication between the TheSky object (RASCOMTheSky) and the TheSky6 astronomy software.

void **DisconnectDome ()**
Terminates TheSky's connection/communications to the dome.

void **DisconnectTelescope ()**
Terminates TheSky's connection/communications to the telescope. More...

void **GetObjectAzAlt** (QString lpszObjectName)
Gets the azimuth and altitude of the specified object. More...

void **GetObjectRaDec** (QString lpszObjectName)
Gets the right ascension and declination of the specified object for the current epoch. More...

void **GetObjectRaDec2000** (QString lpszObjectName)

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_the_sky.html &lt;page_number&gt;1/8&lt;/page_number&gt;

---


## Page 2

3/31/26, 3:35 PM
Script TheSky: sky6RASCOMTheSky Class Reference

Gets the right ascension and declination of the specified object in Epoch 2000 coordinates. More...

void **ImageLink** (double dRa2000, double dDec2000, double dImageScale)
Performs an Image Link. More...

void **SetTelescopeParkPosition ()**
Sets the park position for the telescope. More...

void **Quit ()**
Exits TheSky.

void **SetWhenWhere** (double dJulianDay, int lDSTOption, int lUseSystemClock, QString lpszDescription, double dLongitude, double dLatitude, double dTimeZone, double dElevation)
Specifies the location, date and time for TheSky software. More...

## Properties

double **dObjectAlt**
Returns a double value for the altitude of an object. More...

double **dObjectAz**
Returns a double value for the azimuth of an object. More...

double **dObjectDec**
Returns a double value for the declination of an object. More...

double **dObjectRa**
Returns or sets a numeric value for the declination of the center of the TheSky's Virtual Sky. More...

double **dScreenDec**
A double value that is the right ascension of an object. More...

double **dScreenFOV**
Returns or sets a numeric value for the field of view of the TheSky's Virtual Sky. More...

double **dScreenRa**
Returns or sets a numeric value for the right ascension of the center of the TheSky's Virtual Sky. More...

double **dScreenRotation**
Returns or sets a numeric value for the angular rotation (from North) of the Virtual Sky. More...

int **IsConnected**
Returns a value if this object is connected to TheSky. More...

int **IsAsynchronous**
Returns a value if this object is operating asynchronously. More...

## Detailed Description

The sky6RASCOMTheSky object.

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_the_sky.html
&lt;page_number&gt;2/8&lt;/page_number&gt;

---


## Page 3

3/31/26, 3:35 PM
Script TheSky: sky6RASCOMTheSky Class Reference

The **sky6StarChart** object has superceded most of the functionality of this object.
This legacy object is supplied mainly for backward compatibility with older versions of TheSky.
Developers are strongly encouraged to migrate source code to use the StarChart object instead of the RASCOMTheSky object except for the following methods which are not available from any other object:

AutoMap

ConnectDome

CoupleDome

Quit

DisconnectTelescope

DisconnectDome

Member Function Documentation

---

**AutoMap**

void sky6RASCOMTheSky::AutoMap ( )

Automatically determine the coordinates of the telescope's current position and add this point to a TPoint model.

Use this command to automatically map the current telescope position and add this point to an existing TPoint model.

---

**ConnectDome**

void sky6RASCOMTheSky::ConnectDome ( )

Establishes communications to the dome.

Use this command to connect to any Dome TheSky supports. Once communications are established, call the CoupleDome method to keep the dome's slit and the telescope optics automatically aligned.

---

**CoupleDome**

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_the_sky.html
&lt;page_number&gt;3/8&lt;/page_number&gt;

---


## Page 4

3/31/26, 3:35 PM Script TheSky: sky6RASCOMTheSky Class Reference

```c++
void sky6RASCOMTheSky::CoupleDome ( )
```
slot

Couples or binds the dome's slit with the telescopes optic.

Use this command to couple the dome slit to the telescope optic. Once communications are established using the **ConnectDome()** method, enabling this method will keep the dome's slit and the telescope optics aligned.

---

**DisconnectTelescope**

```c++
void sky6RASCOMTheSky::DisconnectTelescope ( )
```
slot

Terminates TheSky's connection/communications to the telescope.

Warning, if multiple cleints are connected to TheSky's telescope, this will "pull the plug" on their connection's to the telescope.

---

**GetObjectAzAlt**

```c++
void sky6RASCOMTheSky::GetObjectAzAlt ( QString lpszObjectName )
```
slot

Gets the azimuth and altitude of the specified object.

Use this command to set the **dObjectAz()** and **dObjectAlt()** properties of the **sky6RASCOMTheSky** object. Any object name is allowed, such as Spica, The Great Nebula in Orion, NGC4565, or SAO2345. See the Find command for details on how to search for objects.

---

**GetObjectRaDec**

```c++
void sky6RASCOMTheSky::GetObjectRaDec ( QString lpszObjectName )
```
slot

Gets the right ascension and declination of the specified object for the current epoch.

Any object name is allowed, such as Spica, The Great Nebula in Orion, NGC4565, or SAO2345. See the Find command for details on how to search for objects.

---

**GetObjectRaDec2000**

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_the_sky.html &lt;page_number&gt;4/8&lt;/page_number&gt;

---


## Page 5

3/31/26, 3:35 PM
Script TheSky: sky6RASCOMTheSky Class Reference

```cpp
void sky6RASCOMTheSky::GetObjectRaDec2000 ( QString lpszObjectName )
```
slot

Gets the right ascension and declination of the specified object in Epoch 2000 coordinates.

Any object name is allowed, such as Spica, The Great Nebula in Orion, NGC4565, or SAO2345. See the Find command for details on how to search for objects.

---

## ImageLink

```cpp
void sky6RASCOMTheSky::ImageLink ( double dRa2000,
                                    double dDec2000,
                                    double dImageScale
                                  )
```
slot

Performs an Image Link.

This method has been deprecated, use the native ImageLink of TheSky instead.

---

## SetTelescopeParkPosition

```cpp
void sky6RASCOMTheSky::SetTelescopeParkPosition ( )
```
slot

Sets the park position for the telescope.

Use this command to define a new position at which to park the telescope. This method uses the current coordinates of the telescope for the new position.

---

## SetWhenWhere

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_the_sky.html
&lt;page_number&gt;5/8&lt;/page_number&gt;

---


## Page 6

3/31/26, 3:35 PM Script TheSky: sky6RASCOMTheSky Class Reference

```cpp
void sky6RASCOMTheSky::SetWhenWhere ( double dJulianDay,
                                      int   lDSTOption,
                                      int   lUseSystemClock,
                                      QString lpszDescription,
                                      double dLongitude,
                                      double dLatitude,
                                      double dTimeZone,
                                      double dElevation
                                    )
```
slot

Specifies the location, date and time for TheSky software.

**Parameters**

**dJulianDay** A double value that specifies the Julian Day at which to view the sky.
**lDSTOption** A long value that specifies the daylight saving time option to use.
**lUseSystemClock** A long value that informs TheSky to use the computer's internal clock for TheSky's time.
**szDescription** A string value that specifies a description for the location, such as Golden, Colorado, USA.
**dLongitude** A double value that specifies TheSky's longitude setting.
**dLatitude** A double value that specifies TheSky's latitude setting.
**dTimeZone** A double value that specifies TheSky's time zone.
**dElevation** A double value that specifies the elevation used by TheSky.

---

## Property Documentation

*   **dObjectAlt**
    sky6RASCOMTheSky::dObjectAlt
    read write

    Returns a double value for the altitude of an object.

    Before using this property, the method **GetObjectAzAlt()** must first be called.

*   **dObjectAz**

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_the_sky.html &lt;page_number&gt;6/8&lt;/page_number&gt;

---


## Page 7

3/31/26, 3:35 PM
Script TheSky: sky6RASCOMTheSky Class Reference

sky6RASCOMTheSky::dObjectAz
[read] [write]

Returns a double value for the azimuth of an object.

Before using this property, the method **GetObjectAzAlt()** must first be called.

---

**dObjectDec**
sky6RASCOMTheSky::dObjectDec
[read] [write]

Returns a double value for the declination of an object.

Before using this property, one of the methods **GetObjectRaDec(), GetObjectRaDec2000()** must first be called.

---

**dObjectRa**
sky6RASCOMTheSky::dObjectRa
[read] [write]

Returns or sets a numeric value for the declination of the center of the TheSky's Virtual Sky.

Before using this property, one of the methods **GetObjectRaDec(), GetObjectRaDec2000()** must first be called.

---

**dScreenDec**
sky6RASCOMTheSky::dScreenDec
[read] [write]

A double value that is the right ascension of an object.

Use this property to either set the current declination of the center of the Virtual Sky, or get its current value.

---

**dScreenFOV**
sky6RASCOMTheSky::dScreenFOV
[read] [write]

Returns or sets a numeric value for the field of view of the TheSky's Virtual Sky.

Use this property to either set the field of view of the Virtual Sky, or get its current value.

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_the_sky.html
&lt;page_number&gt;7/8&lt;/page_number&gt;

---


## Page 8

3/31/26, 3:35 PM Script TheSky: sky6RASCOMTheSky Class Reference

# dScreenRa

sky6RASCOMTheSky::dScreenRa

Returns or sets a numeric value for the right ascension of the center of the TheSky's Virtual Sky.

Use this property to either set the current right ascension of the center of the Virtual Sky, or get its current value.

# dScreenRotation

sky6RASCOMTheSky::dScreenRotation

Returns or sets a numeric value for the angular rotation (from North) of the Virtual Sky.

Use this property to either set the angular rotation (from North) of the Virtual Sky, or get its current value.

# IsAsynchronous

sky6RASCOMTheSky::IsAsynchronous

Returns a value if this object is operating asynchronously.

Always returns false.

# IsConnected

sky6RASCOMTheSky::IsConnected

Returns a value if this object is connected to TheSky.

Always returns true.

ScriptTheSkyX Examples
(C) Software Bisque, Inc. All rights reserved.

https://www.bisque.com/wp-content/scriptthesky/classsky6_r_a_s_c_o_m_the_sky.html &lt;page_number&gt;8/8&lt;/page_number&gt;