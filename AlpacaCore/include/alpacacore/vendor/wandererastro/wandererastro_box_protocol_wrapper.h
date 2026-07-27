// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::vendor::wandererastro {

/// Identity token the WandererBox Pro V3 streams (Plus V3 reports
/// "ZXWBPlusV3" and is a different port layout — deliberately rejected).
inline constexpr char kBoxProV3ModelToken[] = "ZXWBProV3";

/// PWM range of the DC5/DC6/DC7 dew heater channels.
inline constexpr int kBoxPwmMax = 255;

/// Adjustable regulated output (DC3-4) voltage range, in volts (INDI
/// reference range; the hardware regulates 0-13.2 V but the vendor UI and
/// INDI both floor the setpoint at 5 V).
inline constexpr double kBoxDc34VoltageMin = 5.0;
inline constexpr double kBoxDc34VoltageMax = 13.2;
inline constexpr double kBoxDc34VoltageStep = 0.1;

/// Minimum firmware (YYYYMMDD) for calibrated current readings. Older
/// firmware still works — power readings are just uncalibrated (warn only).
inline constexpr int kBoxCalibratedPowerMinFirmware = 20240216;

/**
 * @brief Connection configuration for the WandererBox Pro V3.
 *
 * USB serial only (CH340 adapter, fixed 19200 8N1). The controller streams
 * its full status frame continuously, so identification is passive.
 */
struct BoxConnectionConfig {
    std::string serial_port;  // e.g., "/dev/ttyUSB0"; empty + auto_detect_index>=0 => enumerate at connect
    int baud_rate = 19200;    // WandererBox fixed protocol baud
    int serial_timeout_s = 3;
    int auto_detect_index = -1;  // >=0: enumerate ports at connect time and use this 0-based match
};

/**
 * @brief Information about a detected serial port that may host a WandererBox Pro V3.
 */
struct BoxPortInfo {
    std::string port_path;     // e.g., "/dev/ttyUSB0"
    std::string device_id;     // e.g., "usb-1a86_USB_Serial-if00-port0"
    std::string model;         // identity token, e.g. "ZXWBProV3"
    int firmware_version = 0;  // YYYYMMDD, populated after a successful probe
};

/**
 * @brief Latest known WandererBox Pro V3 state, parsed from the status frame.
 *
 * Frame field order (all 'A'-delimited, per the INDI wandererbox_pro_v3
 * reference): model, firmware, probe1 temp, probe2 temp, probe3 temp,
 * DHT22 humidity, DHT22 temp, total current, 19V current, DC3-4 current,
 * input voltage, USB3.1-1, USB3.1-2, USB3.1-3, USB2.0(1-3), USB2.0(4-6),
 * DC3-4, DC5, DC6, DC7, DC8-9, DC10-11, DC3-4 setpoint (volts x 10).
 */
struct BoxState {
    bool valid = false;        // true once a full frame has been parsed
    int firmware_version = 0;  // YYYYMMDD

    // Environment sensors (raw units from the frame).
    std::array<double, 3> probe_temps{};  // DS18B20 probes 1-3, deg C
    double humidity = 0.0;                // DHT22 relative humidity, %
    double ambient_temp = 0.0;            // DHT22 temperature, deg C
    double dew_point = 0.0;               // computed (Magnus formula), deg C

    // Power monitor.
    double total_current = 0.0;  // A
    double v19_current = 0.0;    // A (DC2 19V rail)
    double adj_current = 0.0;    // A (DC3-4 adjustable rail)
    double input_voltage = 0.0;  // V

    // Output states.
    bool usb31_1 = false;
    bool usb31_2 = false;
    bool usb31_3 = false;
    bool usb2_13 = false;  // USB2.0 ports 1-3 (switched together)
    bool usb2_46 = false;  // USB2.0 ports 4-6 (switched together)
    bool dc3_4 = false;
    int dc5_pwm = 0;  // 0-255
    int dc6_pwm = 0;  // 0-255
    int dc7_pwm = 0;  // 0-255
    bool dc8_9 = false;
    bool dc10_11 = false;
    double dc3_4_voltage = 0.0;  // setpoint, volts (frame value / 10)
};

/**
 * @brief Enumerate serial ports that could be a WandererBox Pro V3.
 *
 * Scans /dev/serial/by-id/ for CH340/CH341 USB-serial adapters (vendor 1a86)
 * and listens on each for the streamed status frame, accepting ports whose
 * identity token is exactly "ZXWBProV3". Falls back to /dev/ttyUSB0..9 if
 * /dev/serial/by-id is absent. Fully passive — nothing is written to a
 * candidate port.
 *
 * @return Vector of detected WandererBox Pro V3 ports
 */
std::vector<BoxPortInfo> enumerate_wandererbox_ports();

/**
 * @brief Protocol wrapper for the WandererBox Pro V3 power box.
 *
 * Isolates all serial I/O from the Alpaca driver. ASCII protocol at 19200
 * 8N1 (INDI wandererbox_pro_v3 reference):
 *   - the controller streams the 23-field 'A'-delimited status frame
 *     continuously; a background reader thread parses a rolling token stream
 *     that resynchronises on the "ZXWBProV3" identity token, so state reads
 *     never block on serial I/O
 *   - commands are "\n"-terminated fire-and-forget ASCII numbers:
 *     DC3-4 "101"/"100", DC8-9 "201"/"200", DC10-11 "211"/"210",
 *     USB3.1-1/2/3 "11x"/"12x"/"13x", USB2.0(1-3) "14x", USB2.0(4-6) "15x",
 *     PWM "<ch><value %03d>" (ch 5/6/7), DC3-4 voltage "20<volts*10 %03d>".
 *     (The protocol also has a current-calibration command, "66300744" — not
 *     implemented here; calibrate via the vendor's WandererEmpire app.)
 */
class WandererBoxProtocolWrapper {
public:
    WandererBoxProtocolWrapper();
    ~WandererBoxProtocolWrapper();

    WandererBoxProtocolWrapper(const WandererBoxProtocolWrapper&) = delete;
    WandererBoxProtocolWrapper& operator=(const WandererBoxProtocolWrapper&) = delete;

    /**
     * @brief Connect and wait for the first streamed status frame.
     * @param config Connection configuration
     * @return The detected model token ("ZXWBProV3")
     */
    std::string connect(const BoxConnectionConfig& config);

    /**
     * @brief Disconnect: stop the reader thread and close the serial port.
     */
    void disconnect();

    /** @brief Check if connected. */
    bool is_connected() const;

    /** @brief Get the most recent box state (never blocks on serial I/O). */
    BoxState get_state() const;

    /**
     * @brief Get the device firmware date (YYYY-MM-DD), if known.
     *
     * Captured from the status stream and cleared on disconnect.
     */
    std::optional<std::string> get_firmware_date() const;

    /** @brief Switch the DC3-4 adjustable output on or off ("101"/"100"). */
    void set_dc3_4(bool on);

    /**
     * @brief Set the DC3-4 regulated output voltage ("20<volts*10>").
     * @param volts Setpoint in volts [5.0, 13.2]
     */
    void set_dc3_4_voltage(double volts);

    /**
     * @brief Set a dew heater PWM channel ("<ch><value %03d>").
     * @param channel Heater channel: 5, 6 or 7 (DC5/DC6/DC7)
     * @param value PWM duty [0, 255]
     */
    void set_pwm(int channel, int value);

    /** @brief Switch the DC8-9 parallel output pair ("201"/"200"). */
    void set_dc8_9(bool on);

    /** @brief Switch the DC10-11 parallel output pair ("211"/"210"). */
    void set_dc10_11(bool on);

    /**
     * @brief Switch a USB port group.
     * @param group 0-2: USB3.1 port 1/2/3; 3: USB2.0(1-3); 4: USB2.0(4-6)
     * @param on Desired state
     */
    void set_usb(int group, bool on);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::wandererastro
