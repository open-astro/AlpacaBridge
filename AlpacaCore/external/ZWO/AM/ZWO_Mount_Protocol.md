## Page 1

苏州振旺光电有限公司

# 苏州振旺光电
## 赤道仪通信协议
### ZWO Mount Serial Communication Protocol

<table>
  <thead>
    <tr>
      <th>版本</th>
      <th>说明</th>
      <th>日期</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>V1.0</td>
      <td>初版</td>
      <td>2022/1/18</td>
    </tr>
    <tr>
      <td>V1.0</td>
      <td>Original edition</td>
      <td>2022/1/18</td>
    </tr>
    <tr>
      <td>V1.1</td>
      <td>1. 获取设备信息的命令由:GMI#改为:GVP#<br>2. 新增获取和配置夏令时的接口</td>
      <td>2022/2/11</td>
    </tr>
    <tr>
      <td>V1.1</td>
      <td>1. Command of getting the gear information changed from :GMI# to :GVP#.<br>2. Added new commands of getting and configuring summer time.</td>
      <td>2022/2/11</td>
    </tr>
    <tr>
      <td>V1.2</td>
      <td>1. 增加通信方式说明<br>2. 增加赤道仪/经纬仪模式切换说明<br>3. 增加跟踪限制说明<br>4. 修改设置自定义导星速率范围为 0.10-0.90<br>5. 新增获取自定义导星速率接口</td>
      <td>2022/3/2</td>
    </tr>
    <tr>
      <td>V1.2</td>
      <td>1. Added some extra explanations for some commands.<br>2. Added explanation for mode switching between Equatorial Mode and Altazimuth Mode.</td>
      <td>2022/3/2</td>
    </tr>
  </tbody>
</table>

---


## Page 2

苏州振旺光电有限公司

<table>
  <tr>
    <td></td>
    <td></td>
    <td></td>
  </tr>
  <tr>
    <td>V1.3</td>
    <td>1. `度限制 (赤道仪&经纬仪) 命令<br>2. 新增获取 GOTO 最低高度限制命令</td>
    <td>2022/3/7</td>
  </tr>
  <tr>
    <td>V1.3</td>
    <td>1. Added the command of minimum height limitation when GOTO a target (Equatorial mount & Altazimuth mount)<br>2. Added the command of getting the minimum height limitation when GOTO a target.</td>
    <td>2022/3/7</td>
  </tr>
  <tr>
    <td>V1.4</td>
    <td>1. 新增:Rvnnnn.nn#变速接口<br>2. 增加错误码说明</td>
    <td>2022/4/15</td>
  </tr>
  <tr>
    <td>V1.4</td>
    <td>1. Added variable speed command :Rvnnnn.nn#.<br>2. Added explanation for error codes.</td>
    <td>2022/4/15</td>
  </tr>
  <tr>
    <td>V1.5</td>
    <td>1. 对协议格式进行详细说明<br>2. 增加和删减一些接口</td>
    <td>2022/6/9</td>
  </tr>
  <tr>
    <td>V1.5</td>
    <td>1. Added explanation for protocol format.<br>2. Added and deleted some commands.</td>
    <td>2022/6/9</td>
  </tr>
  <tr>
    <td>V1.6</td>
    <td>1. 增加获取状态命令 :GU#</td>
    <td>2022/12/12</td>
  </tr>
  <tr>
    <td>V1.6</td>
    <td>1. Added get statue command :GU#</td>
    <td>2022/12/12</td>
  </tr>
  <tr>
    <td>V1.7</td>
    <td>1. 增加过中天行为命令<br>2. 增加 park 位命令</td>
    <td>2023/2/25</td>
  </tr>
  <tr>
    <td>V1.7</td>
    <td>1. Added the cross Meridian behavior command<br>2. Added park command</td>
    <td>2023/2/25</td>
  </tr>
  <tr>
    <td>V1.8</td>
    <td>1. 移除:GU#中返回值中 PARK 标示的显示<br>2. 修改:hP#返回值说明, 命令:hP#无任何返回值</td>
    <td>2023/04/20</td>
  </tr>
  <tr>
    <td>V1.8</td>
    <td>1. Remove the PARK mark in the return value of GU#<br>2. Modify: hP# return value description, command: hP# no return value</td>
    <td>2023/04/20</td>
  </tr>
  <tr>
    <td>V1.9</td>
    <td>1. 增加清除多星校准数据命令<br>2. 增加高度限位命令</td>
    <td>2023/07/22</td>
  </tr>
  <tr>
    <td>V1.9</td>
    <td>1. Add the command to clear multi-star calibration data<br>2. Add height limit command</td>
    <td>2023/07/22</td>
  </tr>
  <tr>
    <td>V2.0</td>
    <td>1. 新增蓝牙通讯指令<br>2. 新增错误码</td>
    <td>2024/06/03</td>
  </tr>
  <tr>
    <td>V2.0</td>
    <td>1. Add ble command<br>2. Add new errno number</td>
    <td>2024/06/03</td>
  </tr>
</table>

---


## Page 3

苏州振旺光电有限公司

<table>
  <tr>
    <td>V2.1</td>
    <td>1. 新增设置自定义 Park 位命令<br>2. 新增 Park 错误码<br>3. 新增是否归零成功过命令<br>4. 新增解 Park 命令<br>5. 新增获取 Park 状态命令</td>
    <td>2026/02/06</td>
  </tr>
  <tr>
    <td>V2.1</td>
    <td>1. Added the option to set custom Park position command<br>2. Added Park error code<br>3. Added Command to Check if Homing Has Been Successful<br>4. Added Unpark Command<br>5. Added Get Park Status Command</td>
    <td>2026/02/06</td>
  </tr>
</table>

---


## Page 4

苏州振旺光电有限公司

目录

苏州振旺光电 ... 1
赤道仪通信协议 ... 1
*   通信连接说明 ... 5
*   通信协议说明 ... 5
*   赤道仪/经纬仪切换命令 ... 8
*   日期和时间命令 ... 8
*   位置坐标命令 ... 10
*   移动命令 ... 11
*   跟踪和导星命令 ... 15
*   同步命令 ... 19
*   归零命令 ... 19
*   获取状态命令 ... 19
*   Get status ... 20
*   Park 错误码 ... 20
*   Park 命令 ... 21
*   设置自定义 Park 位 ... 22
*   解 Park 命令 ... 22
*   获取 Park 状态 ... 22
*   限位命令 ... 22
*   复合命令 ... 24
*   获取是否成功归零过命令 ... 25
*   蓝牙通讯命令 ... 26
*   使用流程举例 ... 26

---


## Page 5

苏州振旺光电有限公司

*   通信连接说明
客户端使用串口连接赤道仪时，通信波特率为 9600
客户端通过手柄的 WIFI AP 网络连接赤道仪时，连接 IP: 192. 168. 4. 1, PORT: 4030

**About the connection**

Communication to the mount via serial baud rate is 9600.
Wireless communication to the mount via the WIFI of the hand controller: IP 192.168.4.1, PORT 4030.

*   通信协议说明
协议基于 LX200 并且兼容 LX200

**About the Protocol**

The Protocol is based on LX200 also compatible with LX200.

协议规则如下：
命令格式以":"开头，中间是命令码，以"#"结尾，如":AA#".
Commands usually start with ":" and end with "#", such as ":AA#". Fields in the middle represent the command itself.

<table>
<thead>
<tr>
<th colspan="4">命令码格式 Command format</th>
</tr>
</thead>
<tbody>
<tr>
<td>起始字符<br>Starting character</td>
<td>命令码<br>Command Code</td>
<td>结束字符<br>Ending character</td>
<td></td>
</tr>
<tr>
<td>:</td>
<td>nnn</td>
<td>#</td>
<td>n 表示一个字符,命令码不超过 62 字节<br>N represents one character.<br>The whole length of a command code does not exceed 62 characters.</td>
</tr>
<tr>
<td>:</td>
<td>nnn</td>
<td>#</td>
<td></td>
</tr>
</tbody>
</table>

<table>
<thead>
<tr>
<th colspan="6">命令返回格式 Command response format</th>
</tr>
</thead>
<tbody>
<tr>
<td>移动命令或者非法命令<br>Moving Command</td>
<td>设置参数命令<br>Configuring Settings Command</td>
<td>GOTO 命令<br>GOTO Command</td>
<td>获取跟踪状态命令<br>Getting Tracking Status command</td>
<td>获取参数命令<br>Getting Current Settings command</td>
<td>同步命令<br>Sync Command</td>
</tr>
</tbody>
</table>

---


## Page 6

苏州振旺光电有限公司

<table>
  <tr>
    <td>or Illegal Command</td>
    <td></td>
    <td></td>
    <td></td>
    <td></td>
    <td></td>
  </tr>
  <tr>
    <td>无返回</td>
    <td>1: 正确<br>0: 错误</td>
    <td>0 成功<br>e+错误码#</td>
    <td>0# 跟踪未开启<br>1# 跟踪进行中<br>e+错误码#</td>
    <td>以#结尾的字符串<br>例如 : e+错误码#<br>sDD*MM:SS#</td>
    <td>N/A#<br>e+错误码#</td>
  </tr>
  <tr>
    <td>No response</td>
    <td>1: Success<br>0: False</td>
    <td>0 Success<br>e+ error code</td>
    <td>0# Tracking off<br>1# Tracking on<br>e+ error code</td>
    <td>Strings ended with #,<br>such as : sDD*MM:SS#</td>
    <td>N/A#<br>e+ error code</td>
  </tr>
</table>

下文中出现下面的字符为通配符
Characters below are wildcards.

<table>
  <tr>
    <td>通配符 Wildcard</td>
    <td>说明 Statement</td>
    <td>举例 Example</td>
  </tr>
  <tr>
    <td>s</td>
    <td>+或者-</td>
    <td>:SGsHH# (:SG-08#)</td>
  </tr>
  <tr>
    <td></td>
    <td>+ or -</td>
    <td></td>
  </tr>
  <tr>
    <td>HH:MM:SS</td>
    <td>小时:分钟:秒<br>Hour:Minute:Second</td>
    <td>:SLHH:MM:SS# (:SL19:44:32#)</td>
  </tr>
  <tr>
    <td>MM/DD/YY</td>
    <td>月份/日/年<br>Month/Date/Year</td>
    <td>:SCMM/DD/YY# (:SC06/08/22#)</td>
  </tr>
  <tr>
    <td>n</td>
    <td>数字<br>Number</td>
    <td>:Rn# (:R9#)</td>
  </tr>
  <tr>
    <td>x</td>
    <td>字母或者数字<br>Letter or number</td>
    <td>xxx# (78e36d441524#)</td>
  </tr>
  <tr>
    <td>&</td>
    <td>分割多个参数<br>Separete multiple parameters</td>
    <td>MM/DD/YY&HH:MM:SS&sHH:MM#</td>
  </tr>
</table>

命令返回值分 4 种情况
There are 4 kinds of command response values.
1) 无返回。某些命令不需要返回值，例如移动指令。
2) 返回 0, 或 1。该返回用来表示命令指向成功或失败，GOTO 命令使用 0 表示成功，其他命令使用 1 表示成功。

---


## Page 7

苏州振旺光电有限公司

3) 返回 0, 或 1, e+错误码+#, 如 e1#, 数字 1 代码错误码, 可以在下面错误码说明查看具体原因。
4) 返回字符串+#结尾。如 HH:MM:SS#, 该返回用来返回数据。
5) 返回 N/A#或者 e+错误码# (同步命令)
注意：以 G 开头的获取命令，返回值都会带有#

1) No response. Some commands do not require response values, such as moving commands.
2) Response 0 or 1. It stands for success or failure of the command. Success for GOTO commands is 0, for other commands is 1.
3) Response 0 or 1 or e+ error code+#. For example, e1#, the number 1 stands for error code. You can check the explanation below to see the reason.
4) Response character string ends with +#. For example, HH:MM:SS#, it was used to response data.
5) Response N/A# or e+ error code# (Sync commands).
Note: The response values of Get Commands starting with G all comes with #.

错误码说明:
Error code statement:

<table>
<thead>
<tr>
<th>错误码</th>
<th>说明</th>
<th>Statement</th>
</tr>
</thead>
<tbody>
<tr>
<td>1</td>
<td>参数超出范围</td>
<td>Parameter beyond the range</td>
</tr>
<tr>
<td>2</td>
<td>参数格式错误</td>
<td>Parameter format error</td>
</tr>
<tr>
<td>3</td>
<td>在归零, slew, goto 的状态时, 执行 GOTO 会返回该错误码</td>
<td>When the mount is going to the zero position, or slewing or doing GOTO, carrying out GOTO will response this error code.</td>
</tr>
<tr>
<td>4</td>
<td>设备正在移动中</td>
<td>Equipment moving</td>
</tr>
<tr>
<td>5</td>
<td>目标低于地平线</td>
<td>Target under horizon line</td>
</tr>
<tr>
<td>6</td>
<td>目标低于高度限制</td>
<td>Target under height limitation</td>
</tr>
<tr>
<td>7</td>
<td>未同步时间地点</td>
<td>Time and position not synchronized</td>
</tr>
<tr>
<td>8</td>
<td>跟踪过了中天</td>
<td>Meridian has been passed over in tracking</td>
</tr>
<tr>
<td>9</td>
<td>同步坐标与设备指向位于子午线的两侧</td>
<td></td>
</tr>
<tr>
<td>10</td>
<td>经纬仪高度角反转</td>
<td></td>
</tr>
<tr>
<td>11</td>
<td>极圈不允许同步</td>
<td></td>
</tr>
</tbody>
</table>

---


## Page 8

苏州振旺光电有限公司

<table>
  <tr>
    <td>12</td>
    <td>同步坐标点与设备本身坐标偏差过大</td>
  </tr>
</table>

*   赤道仪/经纬仪切换命令

命令: “:AP#”
返回: None
说明: 切换到赤道仪模式

命令: “:AA#”
返回: None
说明: 切换到经纬仪模式
注意: 切换模式后，需要重启赤道仪

**Equatorial/Altazimuth mode switching command**

Command: “:AP#”
Response: None
Statement: Switch to Equatorial mode

Command: “:AA#”
Response: None
Statement: Switch to Altazimuth mode
Note: You need to reboot the mount after mode switched.

*   日期和时间命令

注意: 连接上赤道仪后，需要先设置经纬度，时区，本地时间后，赤道仪才可以正常工作。

**Date/time command**

Note: You need to set the longitude and latitude, time zone and local time after the mount is turned on to ensure it can work properly.

命令: “:SCMM/DD/YY#”
返回: “1: 正确, 0: 错误”
说明: 设置日期
Command: “:SCMM/DD/YY#”
Response: “1: Success, 0: False”
Statement: Set date.

---


## Page 9

苏州振旺光电有限公司

命令: “:GC#”
返回: “MM/DD/YY#”
说明: 获取日期
Command: “:GC#”
Response: “MM/DD/YY#”
Statement: Get date.

命令: “:SLHH:MM:SS#”
返回: “1: 正确, 0: 错误”
说明: 设置时间
Command: “:SLHH:MM:SS#”
Response: “1: Success, 0: False”
Statement: Set time.

命令: “:GL#”
返回: “HH:MM:SS#”
说明: 获取时间
Command: “:GL#”
Response: “HH:MM:SS#”
Statement: Get time.

命令: “:GS#”
返回: “HH:MM:SS#”
说明: 获取恒星时
Command: “:GS#”
Response: “HH:MM:SS#”
Statement: Get sidereal time.

命令: “:GH#”
返回: “1: 夏令时开启, 0: 夏令时未开启”
说明: 获取夏令时配置信息
Command: “:GH#”
Response: “1: daylight saving on, 0: daylight saving off”
Statement: Get the daylight saving setting.

命令: “:SHn#”
返回: “1”
说明: 配置夏令时, n 取值范围为 0-1; 0 代表关闭夏令时, 1 代表打开夏令时
Command: “:SHn#”
Response: “1”
Statement: Set daylight saving. n’s value: 0~1; 0 stands for turning off daylight saving.
1 stands for turning on daylight saving.

---


## Page 10

苏州振旺光电有限公司

*   位置坐标命令
    命令: “:SGsHH#”
    返回: “1: 正确, 0: 错误”
    说明: 设置时区 (小时), 小写字母 s 代表符号 (-/+), 东八区设置为 SG-08#, 西区反而是正的

**Position/coordinates commands:**

Command: ":SGsHH#"
Response: "1: Success, 0: False"
Statement: Set time zone (hour). The lowercase s indicates symbol (-/+). UTC +8 is set to SG-08#. The western time zones use negative offset.

命令: “:SGsHH:MM#”
返回: “1: 正确, 0: 错误”
说明: 设置时区 (小时: 分钟), 同上一个命令基本一样, 增加了分钟设置, 目的是支持一些非整数时区的国家, 比如印度 UTC+5:30

Command: ":SGsHH:MM#"
Response: "1: Success, 0: False"
Statement: Set time zone (hour: minute). This command is basically the same as last command, but with an extra minute setting for countries like India whose time zone is UTC +5:30.

命令: “:GG#”
返回: “sHH:MM#”
说明: 获取时区

Command: ":GG#"
Response: "sHH:MM"
Statement: Get time zone.

命令: “:StsDD*MM:SS#”
返回: “1: 正确, 0: 错误”
说明: 设置纬度, 小写字母 s 代表符号 (-/+), 例如: 设置北纬 31° 15' 24'' (:St+31*15:24#)

Command: ":StsDD*MM:SS#"
Response: "1: Success, 0: False"
Statement: Set latitude. The lowercase s indicates symbol (-/+), for example: N 31°15'24"(:St+31*15:24#).

命令: “:Gt#”
返回: “sDD*MM:SS#”
说明: 获取纬度

---


## Page 11

苏州振旺光电有限公司

Command: ":Gt#"
Response: "sDD*MM#"
Statement: Get latitude.

命令: ":SgsDDD*MM:SS#"
返回: "1: 正确, 0: 错误"
说明: 设置经度, 小写字母 s 代表符号 (-/+), DDD (000° -180° ), MM (00' -59'), SS (秒值 : 0-59)
Command: ":SgsDDD*MM:SS#"
Response: "1: Success, 0: False"
Statement: Set longitude. The lowercase s indicates symbol (-/+). DDD (000°-180°), MM (00'-59'), SS (Second: 0-59).

命令: ":Gg#"
返回: "sDDD*MM:SS#"
说明: 获取经度
Command: ":Gg#"
Response: "sDDD*MM#"
Statement: Get longitude.

命令: ":Gm#"
返回: "E(东), W(西), N(无朝向, 即零位)"
说明: 获取赤道仪朝向
Command: ":Gm#"
Response: "E(East), W(West), N(No direction. Home/zero position)"
Statement: Get the current cardinal direction of the mount.

* 移动命令

命令: ":SrHH:MM:SS#"
返回: "1: 正确, 0: 错误"
说明: 设置目标 RA
Moving commands:
Command: ":SrHH:MM:SS#"
Response: "1: Success, 0: False"
Statement: Set target RA

命令: ":Gr#"
返回: "HH:MM:SS#"
说明: 获取目标 RA
Command: ":Gr#"
Response: "HH:MM:SS#"
Statement: Get target RA

---


## Page 12

苏州振旺光电有限公司

命令: “:SdsDD:MM:SS#”
返回: “1: 正确, 0: 错误”
说明: 设置目标 DEC, 小写字母 s 代表符号 (-/+), DD (代表多少度), MM (代表角分, 1 度 = 60 角分), SS (代表角秒, 1 角分 = 60 角秒)
Command: “:SdsDD:MM:SS#”
Response: “1: Success, e2#: False”
Statement: Set target DEC. The lowercase s indicates symbol (-/+). DD indicates degree. MM indicates arcminute (1° = 60 arcminutes). SS indicates arcsecond (1 arcminute = 60 arcseconds).

命令: “:Gd#”
返回: “sDD*MM:SS#”
说明: 获取目标 DEC
Command: “:Gd#”
Response: “sDD*MM:SS#”
Statement: Get target DEC

命令: “:GR#”
返回: “HH:MM:SS#”
说明: 获取当前 RA
Command: “:GR#”
Response: “HH:MM:SS#”
Statement: Get current RA of mount.

命令: “:GD#”
返回: “sDD*MM:SS#”
说明: 获取当前 DEC
Command: “:GD#”
Response: “sDD*MM:SS#”
Statement: Get current DEC of mount.

命令: “:GZ#”
返回: “DDD*MM:SS#”
说明: 获取方位角
Command: “:GZ#”
Response: “DDD*MM:SS#”
Statement: Get Azimuth

命令: “:GA#”
返回: “sDD*MM:SS#”
说明: 获取高度角
Command: “:GA#”
Response: “sDD*MM:SS#”

---


## Page 13

苏州振旺光电有限公司

Statement: Get altitude

命令: “:MS#”
返回: “0: 正确, e+错误码+#”
说明: GOTO
注意: 必须同步过时间地点后才可以进行 GOTO, 在归零, slew, goto 的状态时发送 GOTO 指令会返回 e3#。
Command: “:MS#”
Response: “1: Success, e2#: False”
Statement: GOTO
Note: The mount will not be able to GOTO before time and position get synchronized. When it is going to zero position, or slewing, or doing GOTO, sending the GOTO command will response e3#.

命令: “:Q#”
返回: None
说明: 停止运动
Command: “:Q#”
Response: None
Statement: Stop moving.

命令: “:Rn#”
返回: None
说明: 设置移动速度, n 代表速度等级, 从 0 到 9, 代表 0.25, 0.5, 1, 2, 4, 8, 20, 60, 720, 1440 倍恒星速
Command: “:Rn#”
Response: None
Statement: Set moving speed. n indicates speed level; The range 0 – 9 stands for 0.25x, 0.5x, 1x, 2x, 4x, 8x, 20x, 60x, 720x and 1440x sidereal tracking rate.

命令: “:RG#”
返回: None
说明: 设置移动速度为 0.5 倍恒星速 (对应 R1)
Command: “:RG#”
Response: None
Statement: Set the moving speed to 0.5x sidereal tracking rate (R1).

命令: “:RC#”
返回: None
说明: 设置移动速度为 1 倍恒星速 (对应 R2)
Command: “:RC#”
Response: None
Statement: Set the moving speed to 1x sidereal tracking rate (R2).

---


## Page 14

苏州振旺光电有限公司

命令: “:RM#”
返回: None
说明: 设置移动速度为 720 倍恒星速 (对应 R8)
Command: “:RM#”
Response: None
Statement: Set the moving speed to 720x sidereal tracking rate (R8).

命令: “:RS#”
返回: None
说明: 设置移动速度为 1440 倍恒星速 (对应 R9)
Command: “:RS#”
Response: None
Statement: Set the moving speed to 1440x sidereal tracking rate (R9).

命令: “:Rvnnnn.nn#”
返回: None
说明: 设置移动速度, nnnn.nn 代表 0.00-1440.00 之间的恒星速, 可以取 2 位小数
Command: “:Rvnnnn.nn#”
Response: None
Statement: Set the moving speed. nnnn.nn stands for sidereal tracking rate between 0.00 ~ 1440.00. The figure is accurate to two decimal places.

命令: “:Me#”
返回: None
说明: 向东移动 (移动前必须设置速度)
Command: “:Me#”
Response: None
Statement: Move towards east (You should set moving speed before it actually moves).

命令: “:Qe#”
返回: None
说明: 停止向东移动
Command: “:Qe#”
Response: None
Statement: Stop moving towards east.

命令: “:Mw#”
返回: None
说明: 向西移动 (移动前必须设置速度)
Command: “:Mw#”
Response: None
Statement: Move towards west (You should set moving speed before it actually moves).

---


## Page 15

苏州振旺光电有限公司

命令: “:Qw#”
返回: None
说明: 停止向西移动
Command: “:Qw#”
Response: None
Statement: Stop moving towards west.

命令: “:Mn#”
返回: None
说明: 向北移动 (移动前必须设置速度)
Command: “:Mn#”
Response: None
Statement: Move towards north (You should set moving speed before it actually moves).

命令: “:Qn#”
返回: None
说明: 停止向北移动
Command: “:Qn#”
Response: None
Statement: Stop moving towards north.

命令: “:Ms#”
返回: None
说明: 向南移动 (移动前必须设置速度)
Command: “:Ms#”
Response: None
Statement: Move towards south (You should set moving speed before it actually moves).

命令: “:Qs#”
返回: None
说明: 停止向南移动
Command: “:Qs#”
Response: None
Statement: Stop moving towards south.

*   跟踪和导星命令

命令: “:TQ#”
返回: None
说明: 使用恒星速跟踪

---


## Page 16

苏州振旺光电有限公司

Tracking/guiding command
Command: ":TQ#"
Response: None
Statement: Use sidereal tracking rate.

命令: “:TS#”
返回: None
说明: 使用太阳速跟踪
Command: ":TS#"
Response: None
Statement: Use solar tracking rate.

命令: “:TL#”
返回: None
说明: 使用月亮速跟踪
Command: ":TL#"
Response: None
Statement: Use lunar tracking rate.

命令: “:GT#”
返回: “0#, 1#, 2#”
说明: 获取跟踪速度 (0: 恒星速, 1: 月亮速, 2: 太阳速)
Command: ":GT#"
Response: “1#, 2#, 3#”
Statement: Get tracking rate (0: Sidereal rate, 1: Solar rate, 2: Lunar rate).

命令: “:Te#”
返回: “1: 正确, 0: 错误”
说明: 开始跟踪
注意: 返回失败可以通过:GAT#命令来获取跟踪失败原因
Command: ":Te#"
Response: “1: Success, 0: False”
Statement: Start tracking.
Note: If failed in starting tracking, you can get the failure reason through command :GAT#.

命令: “:Td#”
返回: “1: 正确, 0: 错误”
说明: 停止跟踪
注意: 在归零, slew, goto 的状态下会返回失败
Command: ":Te#"
Response: “1: Success, 0: False”
Statement: Stop tracking.

---


## Page 17

苏州振旺光电有限公司

Note: It will fail stop tracking when the mount is going to zero position, slewing and doing GOTO.

命令: “:GAT#”
返回: “0# 跟踪未开启, 1# 跟踪进行中, e+错误码+#”
说明: 获取跟踪状态
注意:
跟踪打开情形: 打开跟踪, GOTO 到位
跟踪正常关闭情形: 关闭跟踪, 归零
跟踪异常关闭情形: 跟踪低于地平线, 跟踪过中天 (赤道仪), 没有同步时间地点 (经纬仪)
Command: “:GAT#”
Response: “0# tracking off, 1# tracking on, e+ error code+#”
Statement: Get tracking status.
Note: Conditions of tracking on – It has already been turned on manually; Successfully GOTO target.
Normal conditions of tracking off – It has already been turned off manually; Mount in zero position.
Abnormal conditions of tracking off – Target below the horizon line; Meridian has been passed over in tracking; Time and position are not synchronized (Altazimuth mount).

命令: “:Mgdnnnn#”
返回: None
说明: 导星, d (e\w\s\n) 代表方向东、西、南、北 nnnn (0000-3000) 代表导星时间, 单位 ms
Command: “:Mgdnnnn#”
Response: None
Statement: Guiding. d (e\w\s\n) indicates direction east, west, south, north. nnnn (0000-3000) indicates guiding rate (unit: ms).

命令: “:Rg0.nn#”
返回: None
说明: 设置导星速度, 数值范围为 0.10-0.90
Command: “:Rg0.nn#”
Response: None
Statement: Set guiding rate from 0.10 to 0.90.

命令: “:Ggr#”
返回: “0.nn#”
说明: 获取导星速度
Command: “:Ggr#”
Response: 0.nn#

---


## Page 18

苏州振旺光电有限公司

Statement: Get guiding rate.

命令: “:STannsnn#”
返回: “1: 正确, 0: 错误”
说明: 过中天行为设置
注意:
1) nnsnn 一共 5 位, 第 1 位代表是否进行中天翻转, 1: 翻转, 0: 不翻转, 翻转后继续跟踪, 直到低于地平线。(暂时不支持)
2) 第 2 位代表过中天是否继续跟踪, 1: 跟踪, 0: 停止, 如果选择跟踪, 则按照限位进行停止。如果选择停止, 停止角度是过中天后 1 度。
3) 第 3-5 位代表过中天后限位的角度 (赤道仪过中天后 RA 轴继续跟踪所旋转的角度, s 表示符号, nn 的范围是 0° -15° ) (注: 角度设置为 0 时不可再配置过中天后继续跟踪)
4) 以上任何情况低于地平线默认停止跟踪。
5) 仅赤道仪模式下支持此功能
6) 过中天后限位的角度设置为负数时表示在中天前 n° 时会停止
7) 翻中天仅支持过中天后翻转(在中天前停止则不会翻转)
8) 设置在中天后停止且使能翻转后, 会在跟踪到达限位停止后开始翻转

Command: “:STannsnn#”
Response: “1:Success, 0: False”
Statement: Set the act of crossing the meridian
Note:
1) nnsnn has a total of 5 digits, the first digit represents whether to perform meridian flip, 1: flip, 0: no flip, continue to track after flip, until it is below the horizon. (temporarily not supported)
2) The second digit represents whether to continue to track after meridian, 1: track, 0: stop, if you choose to track, stop according to the limit. If you choose to stop, the stop angle is 1 degree after meridian.
3) The 3rd to 5th digits represent the limit angle after meridian (the RA axis of the equatorial mount continues to track the angle of rotation, s represents the sign, and the range of nn is 0°-15°) (Note: the angle is set to At 0 o'clock, it can no longer be configured to continue tracking after meridian)
4) Any of the above conditions below the horizon will stop tracking by default.
5) This function is only supported in equatorial mode
6) When the limit angle is set to a negative number after meridian, it means that it will stop at n° before meridian
7) Flip meridian only supports flipping after passing meridian (it will not flip if it stops before meridian)
8) After it is set to stop after meridian and the flip is enabled, it will start to flip after the tracking reaches the limit stop

---


## Page 19

苏州振旺光电有限公司

命令: “:GTa#”
返回: “nnsnn#”
说明: 获取过中天行为
Command: “:GTa#”
Response: “nnsnn#”
Statement: Get the act of crossing the meridian

*   **同步命令**
    命令: “:CM#”
    返回: “N/A#: 成功, e+错误码#”
    说明: 同步位置 (需要先设置目标 RA 和 DEC)
    Sync commands:
    Command: “:CM#”
    Response: “N/A#: Success, e2#: Error”
    Statement: Synchronization. It requires you to set target RA and DEC first.

    命令: “:NSC#”
    返回: 1
    说明: 清除设备中的多星校准点信息
    Command: “:NSC#”
    Response: 1
    Statement: Clear the multi-star calibration point information in the device.

*   **归零命令**
    命令: “:hC#”
    返回: None
    说明: RA 和 DEC 归零位 (机械归零)
    Zero/home position command:
    Command: “:hC#”
    Response: None
    Statement: Stop at zero position (Both RA and DEC).

*   **获取状态命令**
    命令: “:GU#”
    返回:
    n: 当前不在跟踪 (或者无此状态)
    N: 静止或者跟踪 (其他状态不显示)

---


## Page 20

苏州振旺光电有限公司

L: 低电压 (或者无此状态)
H: 设备在零位 (或者无此状态)
G / Z: 赤道仪/经纬仪模式
S: RA 轴 stall (或者无此状态)
s: DEC 轴 stall (或者无此状态)
T: RA ST4 导星中 (或者无此状态)
t: DEC ST4 导星中 (或者无此状态)
nn: RA 轴一些标志位
nn: DEC 轴一些标志位
n: RA 轴速度
n: DEC 轴速度
n: 状态
说明: 举例: nNG001000060#

*   Get status

Command: ":GU#"
Response:
n: no tracking (or not show)
N: stop or tracking (or not show)
L: Low power (or not show)
H: at home position (or not show)
G / Z: GEM/AZ Mode
S: RA stall (or not show)
s: DEC stall (or not show)
T: RA ST4 guiding (or not show)
t: DEC ST4 guiding (or not show)
nn: some flags of RA axis
nn: some flags of DEC axis
n: RA rate
n: DEC rate
n: state

*   Park 错误码

<table>
<thead>
<tr>
<th>错误码</th>
<th>数值</th>
<th>说明</th>
<th>Statement</th>
</tr>
</thead>
<tbody>
<tr>
<td>PARK_NONE</td>
<td>0</td>
<td>无意义</td>
<td>No meaning / Placeholder</td>
</tr>
</tbody>
</table>

---


## Page 21

苏州振旺光电有限公司

<table>
  <tr>
    <td>PARK_SUCCEED</td>
    <td>1</td>
    <td>PARK 位设置成功或 PARK 命令接收成功</td>
    <td>Park position set successfully or Park command received successfully</td>
  </tr>
  <tr>
    <td>PARK_REINVENT</td>
    <td>2</td>
    <td>重复设置 PARK 位置</td>
    <td>Duplicate Park position setting</td>
  </tr>
  <tr>
    <td>PARK_EXCEED_MAX_NUM</td>
    <td>3</td>
    <td>超过设置的最大数量</td>
    <td>Exceeds the maximum allowed number of Park positions</td>
  </tr>
  <tr>
    <td>PARK_NOT_GEM_MODE</td>
    <td>4</td>
    <td>不是赤道仪模式</td>
    <td>Not in equatorial (GEM) mode</td>
  </tr>
  <tr>
    <td>PARK_NOT_GO_HOME</td>
    <td>5</td>
    <td>未进行过归零</td>
    <td>Has not performed homing</td>
  </tr>
  <tr>
    <td>PARK_INVALID_NAME</td>
    <td>6</td>
    <td>无效的名称</td>
    <td>Invalid name</td>
  </tr>
  <tr>
    <td>PARK_INVALID_ID</td>
    <td>7</td>
    <td>无效的 id</td>
    <td>Invalid ID</td>
  </tr>
  <tr>
    <td>PARK_INVALID_PARAM</td>
    <td>8</td>
    <td>无效的参数</td>
    <td>Invalid parameter</td>
  </tr>
  <tr>
    <td>PARK_MOVING</td>
    <td>9</td>
    <td>移动中，禁止设置自定义 Park 位</td>
    <td>Moving, setting custom Park position is forbidden</td>
  </tr>
</table>

*   **Park 命令**
命令: “:hP#”
返回: None
说明: 设置 Park
注意:
1) GOTO 到默认的 PARK 位
2) 只在赤道仪模式生效
Command: “:hP#”
Response: None
Statement: Goto Park position
Note:
1) goto default park position
2) Only support in equatorial mode

---


## Page 22

苏州振旺光电有限公司

*   **设置自定义 Park 位**
    命令: “:Sp01#”
    返回: 1, 2, 3, 4, 5, 9, 错误码类型详见 Park 错误码
    说明: 设置自定义 Park 位
    注意:
        1) 必须归零成功过
        2) 只在赤道仪模式生效
    Command: “:Sp01#”
    Response: 1, 2, 3, 4, 5, 9, e3#. For error code types, please refer to the Park Error Codes.
    Description: Set a custom Park position.
    Notes:
        1. Homing must have been successfully completed.
        2. Only effective in equatorial (GEM) mode.

*   **解 Park 命令**
    命令: “:Spu#”
    返回: 0: 失败；1: 成功
    说明: 解除 Park 状态
    注意: 无
    Command: “:Spu#”
    Return: 0 – Failed; 1 – Succeeded
    Description: Unpark
    Note: None

*   **获取 Park 状态**
    命令: “:Gps#”
    返回: 0: 当前不在 Park, 1:Park 移动中, 2:Park 移动结束, 3:Park 移动错误
    说明: 获取 Park 状态
    注意: 要在 Park 命令下发后使用
    Command: ":Gps#"
    Return: 0 – Not in Park; 1 – Park in progress; 2 – Park completed; 3 – Park error
    Description: Get Park Status
    Note: To be used after a Park command is issued.

*   **限位命令**
    命令: “:SLE#”
    返回: 1
    说明: 使能高度限位
    Command: ":SLE#"

---


## Page 23

苏州振旺光电有限公司

Response: 1
Statement: Enable height limit.

命令: “:SLD#”
返回: 1
说明: 去使能高度限位
Command: “:SLD#”
Response: 1
Statement: Disable height limit.

命令: “:GLC#”
返回: “1: 使能, 0: 未使能”
说明: 获取高度限制使能与否
Command: “:GLC#”
Response: 1
Statement: Get height limit enabled or not.

命令: “:SLHnn#”
返回: “1: 正确, 0: 错误”
说明: 设置设备高度上限, nn: 高度上限, 当前范围为 60-90 (unit: deg)
Command: “:SLHnn#”
Response: “1:Success, 0: False”
Statement: Set the upper limit of the device height, nn: the upper limit of the height, the current range is 60-90 (unit:deg)

命令: “:SLLnn#”
返回: “1: 正确, 0: 错误”
说明: 设置设备高度下限, nn: 高度下限, 当前范围为 0-30 (unit: deg)
Command: “:SLLnn#”
Response: “1:Success, 0: False”
Statement: Set the lower limit of the device height, nn: the lower limit of the height, the current range is 0-30(unit:deg)

命令: “:GLH#”
返回: “nn#”
说明: 获取高度上限值
Command: “:GLD#”
Response: “nnH”
Statement: Get the height limit value.

命令: “:GLL#”

---


## Page 24

苏州振旺光电有限公司

返回: “nn#”
说明: 获取高度下限值
Command: “:GLL#”
Response: “nnH”
Statement: Get the lower limit value.

*   **复合命令**

命令: “:SMGEsDD*MM:SS&sDDD*MM:SS#”
返回: “1: 正确, 0: 错误”
说明: 设置纬度, 经度

**Compound commands:**

Command: “:SMTIMM/DD/YY&HH:MM:SS&sHH:MM#”
Response: “1: Success, 0: False”
Statement: Set latitude and longitude.

命令: “:GMGE#”
返回: “sDD*MM:SS&sDDD*MM:SS#”
说明: 获取纬度, 经度
Command: “:GMGE#”
Response: “sDD*MM&sDDD*MM#”
Statement: Get longitude and latitude.

命令: “:SMTIMM/DD/YY&HH:MM:SS&sHH:MM#”
返回: “1: 正确, 0: 错误”
说明: 设置日期, 时间, 时区
Command: “:SMTIMM/DD/YY&HH:MM:SS&sHH:MM#”
Response: “1: Success, 0: False”
Statement: Set date, time and time zone.

命令: “:GMTI#”
返回: “MM/DD/YY&HH:MM:SS&sHH:MM#”
说明: 获取日期, 时间, 时区
Command: “:GMTI#”
Response: “MM/DD/YY&HH:MM:SS&sHH:MM#”
Statement: Get date, time and time zone.

命令: “:GMeq#”
返回: “HH:MM:SS&sDD*MM:SS#”
说明: 获取目标 RA, 目标 DEC
Command: “:GMeq#”
Response: “HH:MM:SS&sDD*MM:SS#”

---


## Page 25

苏州振旺光电有限公司

Statement: Get target RA and DEC.

命令: “:GMEQ#”
返回: “HH:MM:SS&sDD*MM:SS#”
说明: 获取 RA, DEC
Command: ":GMEQ#"
Response: "HH:MM:SS&sDD*MM:SS#"
Statement: Get the current RA/DEC coordinates.

命令: “:GMZA#”
返回: “DDD*MM:SS&sDD*MM:SS#”
说明: 获取方位角, 高度角
Command: ":GMZA#"
Response: "DDD*MM:SS&sDD*MM:SS#"
Statement: Get Azimuth/altitude.

命令: “:SMeqHH:MM:SS&sDD*MM:SS#”
返回: “0: 正确, e+错误码+#”
说明: 设置目标 RA, 目标 DEC, 并进行 GOTO
注意: 必须同步过时间地点后才可以进行 GOTO, 在归零, slew, goto 的状态时发送 GOTO 指令会返回 e3#。
Command: ":SMeqHH:MM:SS&sDD*MM:SS#"
Response: "0: Success, e+ error code+#"
Statement: Set target RA and DEC and GOTO.
Note: The mount will not be able to GOTO before time and position get synchronized. When it is going to zero position, or slewing, or doing GOTO, sending the GOTO command will response e3#.

命令: “:SMMCHH:MM:SS&sDD*MM:SS#”
返回: “N/A#: 成功, e+错误码#”
说明: 设置目标 RA 和 DEC, 并且同步位置
Command: ":SMMCHH:MM:SS&sDD*MM:SS#"
Response: "N/A#: Success, e+ error code+#"
Statement: Set target RA and DEC and sync position.

*   获取是否成功归零过命令

命令: “:Gh#”
返回: 0: 没有成功归零过; 1: 成功归零过
说明:
注意: 无
Command: ":Gh#"

---


## Page 26

苏州振旺光电有限公司

Response: 0 - Homing has never been successful; 1 - Homing has been successful.
Description: Query whether successful homing has been performed.
Notes: None

*   蓝牙通讯命令

命令: “:BSnnnnnn#”
说明: 配置当前设备连接的标识 (以 sn 代称)
返回:
    OK# 连接成功
    NO# 已有蓝牙设备连接过，需要按一下设备上的蓝牙按钮，按完按钮后接收到 OK#即为连接成功 (注意: sn 长度不对也会返回 NO#)

命令: “:SBlnnnnnn#”
说明: 修改蓝牙名称 (该指令执行后需要重启设备才生效)，仅可修改蓝牙名称的后缀，如 AM5_123456 修改为 AM5_134525，前面的“AM5_”不可修改。需要注意的是，仅支持修改数字和字母。
返回:
    0 修改失败
    1 修改成功

命令: “:GB1#”
说明: 获取蓝牙名称
返回: AMx_nnnnnn# (x 代表 3 或者 5)

*   使用流程举例

---


## Page 27

苏州振旺光电有限公司

<mermaid>
graph TD
    subgraph Initialization
        A[GV#]
        B[SG-8#]
        C[St+31°32′56″#]
        D[Sg-125°06′46″#]
        E[SC06/08/22#]
        F[SL19:44:32#]
        G[hC#]
    end

    subgraph GOTO
        H[Sr13:14:38#]
        I[Sd+56°45′05″#]
        J[MS#]
    end

    subgraph Move
        K[R9#]
        L[Me#]
        M[Q#]
    end

    A --> B
    B --> C
    C --> D
    D --> E
    E --> F
    F --> G

    H --> I
    I --> J

    K --> L
    L --> M
</mermaid>

**Procedure example:**

Initialization
Set time zone
Set latitude
Set longitude
Set date
Set time
Go to zero position

GOTO
Set target RA
Set target DEC
GOTO
Move
Set speed
Start moving
Stop moving