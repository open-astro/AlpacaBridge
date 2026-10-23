# ZWO AM5 Undocumented Commands

## Mount serial number

### **:GMA#**
Get the mount serial number. Returns twelve hexadecimal characters followed by `#`.

Evidence: the [official ZWO ASCOM driver](https://www.zwoastro.com/software/ascom-driver/)
package 6.5.37 maps action 31 to `:GMA#` with a hash-terminated response.
`DataProc.GetMountSN` sends that action through serial or TCP;
`TelescopeHardware.IsValidMountSNResponse` requires twelve hexadecimal characters.
The getter runs before time/location synchronization, with no configuration or motion write.
The extracted `ASCOM.ASIMount.Server.exe` SHA-256 is
`e5f4bc178246b3837ef0fbcb979f85403e04f738029d3671f063e0b8b369954a`.
This is static vendor-code evidence, not a published protocol guarantee.

Read-only AM5 checks on 2026-10-23 returned fresh, matching serials over USB and
handset Wi-Fi on firmware 1.8.6 and after this unit's update to 1.8.8. Its serial
was unchanged across that observed update and remains private. Support on other
models/firmware, stability across other firmware changes and factory uniqueness
across two physical mounts were not verified. Do not copy the vendor getter's
cached-value fallback when verifying a new connection.

<!-- TODO: qualify :GMA# on AM3/AM5N/AM7 and establish its minimum supported firmware. -->

## Altitude limits
Limits are preserved after power cycle.

### **:SLLnn#**
Set low altitude limit (0-30&deg;).
Returns "1" on success, "0" on error. Example: :SLL20#

### **:GLL#**
Get low altitude limit (0-30&deg;).
Returns **nn#** in degrees

### **:SLHnn#**
Set hight altitude limit (60-90&deg;).
Returns "1" on success, "0" on error. Example: :SLH80#

### **:GLH#**
Get high altitude limit (60-90&deg;).
Returns **nn#** in degrees

### **:SLE#**
Enable altitude limitis.
Returns "1" on success.

### **:SLD#**
Disable altitude limitis.
Returns "1" on success.

### **:GLC#**
Get altitude limits status:
**0#** - limits disabled, **1#** - limits enabled


## Clear alignment data
Each sync adds alignment point. They can only be removed all at once.

### **:NSC#**
Clear alignment data (remove all alignment points).
Returns "1" on success.


## Slew speed
Slew speed is preserved after power cycle.

### **:SRlnnnn#**
Set slew speed: 720 or 1440. (720 is Low and 1440 is High)
Returns "1" on success, "0" on error.  Example :SRl1440# or :SRl720#

### **:GRl#**
Get slew speed.
Returns **nnnn#** speed: 720 or 1440

## Unknown
### **:GFD1#**
No idea what it does - ASCOM driver issues it with :GR# and :GD# commands every second or so.
Returns **22438#**

### **:GFR1#**
No idea what it does - ASCOM driver issues it with :GR# and :GD# commands every second or so.
Returns **22438#**

Both GFR1 and GFD1 always return the same value **22438#**
