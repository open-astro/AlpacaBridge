## Page 1

# 1. How the motor controller control the motor speed

*   In the motor controller, there is a hardware timer **T1** that is used to generate stepping pulse for stepper motor or reference position for servomotor. The input clock's frequency of the timer, plus the preset value of this timer, determine the slewing speed of the motors.
*   When T1 generates an interrupt, it might
    *   Drive the motor to move 1 step (1 micro-step or 1 encoder tick) for low speed slewing.
    *   Drive the motor to move up to 32 steps for high speed slewing. This method applies to motor controller firmware version 2.xx. For motor controller with firmware 3.xx or above, the motor controller always drive the motor controller 1 steps/interrupt.

# 2. Two motion mode

*   **GOTO mode**: The master device tells the motor controller the desired destination, and then send a "Start" command. The motor controller will control the motor to move to that destination. The master device can check the motor status, real-time position, cancel the slewing during the GOTO.
*   **Speed(Tracking) mode**: The master device calculate a proper preset value for **T1** and send it to the motor controller, and then send a "Start" command. The motor controller will control the motor to slew at the desired speed. The master device can check the motor status, real-time position, cancel the slewing during the GOTO.
*   There is a command which is used to select between the two motion mode for the next "Start" command. Generally, the motor should be at full stop status before setting the motion mode.
*   Generally, the motor controller returns to "Speed Mode" when the motor stops automatically.
*   A typical slewing session include:
    *   Check whether the motor is in full stop status. If not, stop it.
    *   Set the motion mode.
    *   Set the parameters, for example, destination or preset value of T1.
    *   Set the "Start" command.
    *   For a GOTO slewing, check the motor status to confirm that the motor stops (Generally means arriving the destination.). For a Speed mode slewing, send "Stop" command to end the session.

# 3. Calculation on Master Device

A Skywatcher motor controller does not do complex calculation. The master device do it instead.

*   Calculate the angle

A Skywatcher motor controller only counts the step or the ticks of an incremental encoder on the motor shaft. But a master device can inquire the motor controller the resolution of the telescope axis (how many steps the telescope axis have for one revolution). We called it **CPR** (Counts per revolution). With CPR, the master device can convert an angle to steps or vise versa.

---


## Page 2

Please note that **CPR** might be different for the two axes of a mount.

*   Calculate the T1 preset value.
    A Skywatcher MC can report the T1's input clock frequency **TMR_Freq** (Mention at the beginning of this article). A master device can use **TMR_Freq** and **CPR** to calculate the T1 preset value for desired motor speed.

    Speed_CountsPerSec = Speed_DegPerSec * CPR / 360

    T1_Preset = TMR_Freq / Speed_CountsPerSec

    = TMR_Freq * 360 / Speed_DegPerSec / CPR

*   Calculate the T1 preset value for high speed slewing
    T1 preset value can be too small for high speed slewing, if T1's input clock frequency is low. To solve this problem, the motor use a slightly different way to control motor speed when high speed slewing is required (For example, move an axis with higher then 128x sidereal rate). When T1 generates an interrupt, the motor controller moves N micro-steps for a stepper motor, or change the reference position for N steps for a DC servo motor. That means, for the same T1 preset value, the motor will run N times faster than changing only 1 steps for each T1 interrupt event.

    Currently, N is a fixed number, and a master device can inquire the motor controller for it. It might be 16, 32 or 64.

    The formula for calculating T1 preset value for high speed slewing is:

    T1_Preset = N * TMR_Freq * 360 / Speed_DegPerSec / CPR

When a master wants an axis to slew at high speed, it should let the motor controller know when it configures the motor to the **Speed (Tracking) Mode**. For **GOTO mode**, the motor controller will take care of it automatically.

## 4. Command Format:

*   The command always starts with a ":" character and ends with a carriage return character 0x0D.
*   If a second ":" character is received by the motor controller before the carriage return character, then the motor controller will abandon the characters received and starts receiving a new command.
*   Motor controller will process the command and send response after it receives the carriage return character.
*   A response from the motor controller always starts with a "=" character and ends with a carriage return character, if the response is normal.

---


## Page 3

*   If there is something wrong, the motor will response a message starts with a "!" character, followed by error code and a carriage return character.
*   All the character in the command and the response are ASCII characters.
*   A command from the master device has the following parts:
    *   1 byte Leading character: ":"
    *   1 byte command word, check command set table for details
    *   1 byte channel word: "1" for RA/Az axis; "2" for Dec/Alt axis.
    *   1 to 6 bytes of data, depending on command word: character "0" to "9", "A" to "F"
    *   1 byte Ending character: carriage return character.
*   A normal response from the motor controller has the following parts:
    *   1 byte Leading character: "="
    *   1 to 6 bytes of data, depending on which command is processed: "0" to "9", "A" to "F"
    *   1 byte Ending character: carriage return character.
*   An abnormal response from the motor controller has the following parts:
    *   1 byte Leading character: "!"
    *   2 bytes of error code: "0" to "9", "A" to "F"
    *   1 byte Ending character: carriage return character.
*   Data format:
    *   24 bits Data Sample: for HEX number 0x123456, in the data segment of a command or response, it is sent/received in this order: "5" "6" "3" "4" "1" "2".
    *   16 bits Data Sample: For HEX number 0x1234, in the data segment of a command or response, it is sent/received in this order: "3" "4" "1" "2".
    *   8 bits Data Sample: For HEX number 0x12, in the data segment of a command or response, it is sent/received in this order: "1" "2".

---


## Page 4

# 5. Command Set

<table>
<tr>
<th>Command</th>
<th>Start</th>
<th>Header</th>
<th>Channel</th>
<th>DB1</th>
<th>DB2</th>
<th>DB3</th>
<th>DB4</th>
<th>DB5</th>
<th>DB6</th>
<th>End</th>
<th>Response</th>
<th>Note</th>
</tr>
<tr>
<td>Set Position</td>
<td>: E</td>
<td></td>
<td>*1</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0x0D</td>
<td>A,X</td>
<td>Motor must be full stopped</td>
</tr>
<tr>
<td>Initialization Done</td>
<td>: F</td>
<td></td>
<td>*1(3')</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Set Motion Mode *4</td>
<td>: G</td>
<td></td>
<td>*1</td>
<td>B0: 0=Tracking, 1=Track long<br/>B1: 0=Slow, 1=Fast<br/>B2: 0=Fast, 1=Slow (G)<br/>B3: 1x Slow Goto<br/>B4: 0=Normal Goto<br/>1=Coarse Goto</td>
<td>0'-F'</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>A,X</td>
<td>Motor must be full stopped</td>
</tr>
<tr>
<td>Inquire Target Increment</td>
<td>: I</td>
<td></td>
<td>*1</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Set Brake Point Increment</td>
<td>: J</td>
<td></td>
<td>*1</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Set Goto Target</td>
<td>: S</td>
<td></td>
<td>*1</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0x0D</td>
<td>A,X</td>
<td>Motor must be full stopped</td>
</tr>
<tr>
<td>Set Step Period<br/>(T1 preset value)</td>
<td>: I</td>
<td></td>
<td>*1</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0x0D</td>
<td>A,X</td>
<td>Do not support changing Step Period (T1 preset vaile) when motor is slewing in high speed mode.</td>
</tr>
<tr>
<td>Set Long Goto Step Period</td>
<td>: I</td>
<td></td>
<td>*1</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Set Brake Steps</td>
<td>: U</td>
<td></td>
<td>*1</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Start Motion</td>
<td>: J</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Stop Motion *4</td>
<td>: K</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Instant Stop *4</td>
<td>: L</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Set Sleep</td>
<td>: D</td>
<td></td>
<td>*1</td>
<td>0'- WakeUp<br/>1'=Sleep</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Set Aux Switch On/Off</td>
<td>: O</td>
<td></td>
<td>*1</td>
<td>0'- Off<br/>1'= On</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Set AutoGuide Speed</td>
<td>: P</td>
<td></td>
<td>*1</td>
<td>0'-1x<br/>1'=0.75x<br/>2'=0.5x<br/>3'=0.25x<br/>4'=0.125x</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Run Bootloader Mode</td>
<td>: Q</td>
<td></td>
<td>*1</td>
<td>5'<br>5' '5</td>
<td></td>
<td></td>
<td>5'<br>5' 'A'</td>
<td></td>
<td></td>
<td>0x0D</td>
<td>No response</td>
<td></td>
</tr>
<tr>
<td>Set Polar Scope LED brightness</td>
<td>: V</td>
<td></td>
<td>*1</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Inquire Counts Per Revolution</td>
<td>: a</td>
<td></td>
<td>*2</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>B,X</td>
<td></td>
</tr>
<tr>
<td>Inquire Timer Interrupt Freq</td>
<td>: b</td>
<td></td>
<td>*1'</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>B,X</td>
<td></td>
</tr>
<tr>
<td>Inquire Brake Status</td>
<td>: c</td>
<td></td>
<td>*2</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>B,X</td>
<td></td>
</tr>
<tr>
<td>Inquire Goto Target Position</td>
<td>: h</td>
<td></td>
<td>*2</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>B,X</td>
<td></td>
</tr>
<tr>
<td>Inquire Step Period</td>
<td>: i</td>
<td></td>
<td>*2</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>B,X</td>
<td></td>
</tr>
<tr>
<td>Inquire Position</td>
<td>: j</td>
<td></td>
<td>*2</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>B,X</td>
<td></td>
</tr>
<tr>
<td>Inquire Break Point</td>
<td>: n</td>
<td></td>
<td>*2</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Inquire Status</td>
<td>: f</td>
<td></td>
<td>*2</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>A,X</td>
<td></td>
</tr>
<tr>
<td>Inquire High Speed Ratio</td>
<td>: g</td>
<td></td>
<td>*2</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>D, X</td>
<td></td>
</tr>
<tr>
<td>Inquire 1X Tracking Period</td>
<td>: D</td>
<td></td>
<td>*1'</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>B,X</td>
<td></td>
</tr>
<tr>
<td>Inquire Tele. Axis Position</td>
<td>: d</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>B,X</td>
<td></td>
</tr>
<tr>
<td>Inquire Motor Board Version</td>
<td>: e</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>B,X *</td>
<td></td>
</tr>
<tr>
<td>Inquire PEC Version</td>
<td>: B</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td>B,X</td>
<td></td>
</tr>
<tr>
<td>Inquire Indexer Position</td>
<td>: f</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Extended Setting</td>
<td>: W</td>
<td></td>
<td>*1</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0x0D</td>
<td>X</td>
<td></td>
</tr>
<tr>
<td>Extended Inquire</td>
<td>: q</td>
<td></td>
<td>*1</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0'-F'</td>
<td>0x0D</td>
<td>X</td>
<td></td>
</tr>
<tr>
<td>Inquire EEPROM Address</td>
<td>: X</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire EEPROM Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Address</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*1</td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td></td>
<td>0x0D</td>
<td></td>
<td></td>
</tr>
<tr>
<td>Inquire Register Value</td>
<td>: Y</td>
<td></td>
<td>*
</table>
---


## Page 5

6. Hardware

*   UART: 9600bps, 1 start bit, 1 stop bit, no parity check.
*   Signal level: 5V or 3.3V.
*   On most of the EQ mount, the TX and RX lines are separated. The motor controller will send its response immediately after it received and process the command.
*   On most the Alt/Az mount, TX and RX lines are connected together, and there is another line(Drop) to indicate that the TX/RX bus is busy. The Drop line is controlled by the master only, which means the master device should pull the Drop line to low level when it starts to send a command and keep pulling it low until it receives the full response from the motor controller, or, a time-out occurs. The motor controller will send its response immediately after it received and process the command, thus the master device should release the TX/RX bus as soon as possible after the last bit of the command is shift out of the hardware register.
*   The motor controller pull its TX line to high level with a 5.1K to 10K resistor, other than that, it does not strongly pull the TX line to high level and other devices can pull the TX line to low level without problem.

6. Wi-Fi Connection

The same protocol runs on the SynScan Wi-Fi dongle or mount with built-in Wi-Fi module.

*   The Wi-Fi dongle/module runs a UDP server and listen to UDP port 11880 to accept commands from host.
*   The command must be sent in a single UDP package; the response is also included in a single package.
*   When the Wi-Fi dongle/module works in access point mount, its IP address is 192.168.4.1. If it runs in station mode, the router that it links to allocates its IP address.

6. Useful Resources

*   Sample Code: https://code.google.com/archive/p/skywatcher/
*   Documents: http://www.skywatcher.com/download/manual/application-development/

