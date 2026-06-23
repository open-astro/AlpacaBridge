## Page 1

&lt;img&gt;WandererAstro&lt;/img&gt;

# Serial Protocol for WandererCover V4-EC and V4-EC IR
(Firmware version 20250506)

Baud rate: 19200
Data bits: 8
Parity: None
Stop bits: 1

<table>
  <thead>
    <tr>
      <th>Action</th>
      <th>Command</th>
      <th>Example</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Open cover</td>
      <td>1001</td>
      <td></td>
    </tr>
    <tr>
      <td>Close cover</td>
      <td>1000</td>
      <td></td>
    </tr>
    <tr>
      <td>Set flat panel brightness</td>
      <td>1-255</td>
      <td>100 for PWM level of 100</td>
    </tr>
    <tr>
      <td>Turn off the flat light</td>
      <td>9999</td>
      <td></td>
    </tr>
    <tr>
      <td>Set the open position of the cover x</td>
      <td>40000+ x*100</td>
      <td>67000 for open position of 270°</td>
    </tr>
    <tr>
      <td>Set the close position of the cover x</td>
      <td>10000+ x*100</td>
      <td>12055 for close position of 20.55°</td>
    </tr>
    <tr>
      <td>Automatically detect open position</td>
      <td>100001</td>
      <td></td>
    </tr>
    <tr>
      <td>Automatically detect close position</td>
      <td>100000</td>
      <td></td>
    </tr>
    <tr>
      <td>Dew heater OFF</td>
      <td>2000</td>
      <td></td>
    </tr>
    <tr>
      <td>Dew heater LOW</td>
      <td>2050</td>
      <td></td>
    </tr>
    <tr>
      <td>Dew heater HIGH</td>
      <td>2100</td>
      <td></td>
    </tr>
    <tr>
      <td>Dew heater MAX</td>
      <td>2150</td>
      <td></td>
    </tr>
    <tr>
      <td>Enable ASIAIR control</td>
      <td>1500003</td>
      <td></td>
    </tr>
    <tr>
      <td>Disable ASIAIR control</td>
      <td>1500004</td>
      <td></td>
    </tr>
    <tr>
      <td>Set custom brightness 1 for ASIAIR control</td>
      <td>1000000+Brightness</td>
      <td></td>
    </tr>
    <tr>
      <td>Set custom brightness 2 for ASIAIR control</td>
      <td>2000000+Brightness</td>
      <td></td>
    </tr>
    <tr>
      <td>Set custom brightness 3 for ASIAIR control</td>
      <td>3000000+Brightness</td>
      <td></td>
    </tr>
  </tbody>
</table>

WandererCover V4-EC continues transmitting status info to the COM port, this also helps users to identify whether the COM port belongs to the WandererCover V4-EC. The status info has the following format:

WandererCoverV4A***A***A***A***A***A***A***A
| | | | | | | |
1 2 3 4 5 6 7 8

The definition are as follows:

<table>
  <thead>
    <tr>
      <th></th>
      <th>Definition</th>
      <th>Note</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>1</td>
      <td>Firmware version</td>
      <td>YYYYMMDD</td>
    </tr>
    <tr>
      <td>2</td>
      <td>Close position set (°)</td>
      <td>XX.XX</td>
    </tr>
    <tr>
      <td>3</td>
      <td>Open position set (°)</td>
      <td>XX.XX</td>
    </tr>
  </tbody>
</table>

For technical support, please contact: skywatcherwsl2000@gmail.com

---


## Page 2

&lt;img&gt;WandererAstro&lt;/img&gt;

<table>
  <tr>
    <td>4</td>
    <td>Current position (°)</td>
    <td>xx.xx</td>
  </tr>
  <tr>
    <td>5</td>
    <td>Input voltage (V)</td>
    <td>xx.xx</td>
  </tr>
  <tr>
    <td>6</td>
    <td>Flat panel brightness</td>
    <td>0-255</td>
  </tr>
  <tr>
    <td>7</td>
    <td>Dew heater power</td>
    <td>0(OFF) 50(Low) 100(Mid) 150(High)</td>
  </tr>
  <tr>
    <td>8</td>
    <td>If ASIAIR control is enabled</td>
    <td>0(Disabled) 1(Enabled)</td>
  </tr>
</table>

For technical support, please contact: skywatcherws|2000@gmail.com