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

/*
 * pwm_gpio.h — Reverse-engineered ioctl interface for ZWO ASIAIR Plus
 *
 * Module: pwm_gpio.ko
 * Author: JerryCui (ZWO)
 * Source: /home/jerry/rk3568/rk356x_linux_210520/pwm_gpio/ko/pwm_gpio.c
 * License: GPL
 *
 * Decoded from DWARF debug info and aarch64 disassembly of pwm_gpio.ko
 * (kernel 4.19.219, built for ASIAIR Plus RK3568)
 *
 * TWO COMMAND NUMBERINGS LIVE IN THIS HEADER (see "ioctl commands" below):
 *   - PWM_GPIO_GET_LEVEL .. PWM_GPIO_GET_KEYS: the LEGACY decode. It is an early
 *     mis-decode of the module's command numbers (mode and level shifted, enable
 *     and disable swapped), kept byte for byte because the stock-firmware path of
 *     AsiairPlusProtocolWrapper is field-validated with exactly these numbers.
 *   - PWM_GPIO_ABI_*: the corrected numbering, the module's real ABI, which the
 *     OpenAstro mainline image's clean-room pwm_gpio_compat module implements.
 *
 * The driver registers /dev/pwm-gpio-misc (misc device, major 10 minor 55)
 * and controls 12 GPIOs defined in the "airplus-gpios" device tree node.
 *
 * Each GPIO can operate in two modes:
 *   - GPIO mode (mode=1): simple high/low output or input
 *   - PWM mode  (mode=2): software PWM via kernel hrtimer
 *
 * GPIO index mapping (from device tree, verified with live GPIO state):
 *
 *   Index  GPIO#   RK3568 Pin   Function
 *   -----  ------  ----------   ----------------------------------
 *   0      29      GPIO0_D5     Network/status LED
 *   1      30      GPIO0_D6     Status LED
 *   2      5       GPIO0_A5     Physical button (input)
 *   3      15      GPIO0_B7     DC master enable / control signal (rk-flashtool's
 *                                 corrected header and its live buzzer test call
 *                                 this the piezo buzzer; unresolved here)
 *   4      150     GPIO4_C6     DC power port 1 (PWM capable)
 *   5      149     GPIO4_C5     DC power port 2 (PWM capable)
 *   6      146     GPIO4_C2     DC power port 3 (PWM capable)
 *   7      147     GPIO4_C3     DC power port 4 (PWM capable)
 *   8      18      GPIO0_C2     USB2 port 1 power enable
 *   9      6       GPIO0_A6     USB2 port 2 power enable
 *   10     8       GPIO0_B0     USB3 port 1 power enable
 *   11     23      GPIO0_C7     USB3 port 2 power enable
 */

#ifndef PWM_GPIO_H
#define PWM_GPIO_H

#include <linux/ioctl.h>
#include <stdint.h>

#define PWM_GPIO_MAGIC 'C' /* ioctl type/magic byte (0x43) */
#define PWM_GPIO_DEV "/dev/pwm-gpio-misc"

/* Work modes */
#define PWM_GPIO_MODE_GPIO 1
#define PWM_GPIO_MODE_PWM 2

/*
 * struct gpio_level_s - GPIO level read/write
 * @index: GPIO index (0–11)
 * @level: GPIO value (0 = low, 1 = high)
 */
typedef struct gpio_level_s {
    int32_t index;
    int32_t level;
} gpio_level_t; /* 8 bytes */
static_assert(sizeof(gpio_level_t) == 8, "kernel ioctl struct size");

/*
 * struct pwm_parm_s - PWM configuration
 * @index:     GPIO index (0–11)
 * @period_ns: PWM period in nanoseconds
 * @duty_ns:   PWM duty (high time) in nanoseconds
 */
typedef struct pwm_parm_s {
    int32_t index;
    int32_t period_ns;
    int32_t duty_ns;
} pwm_param_t; /* 12 bytes */
static_assert(sizeof(pwm_param_t) == 12, "kernel ioctl struct size");

/*
 * struct work_mode_s - GPIO/PWM mode selection
 * @index: GPIO index (0–11)
 * @mode:  1 = GPIO mode, 2 = PWM mode
 */
typedef struct work_mode_s {
    int32_t index;
    int32_t mode;
} work_mode_t; /* 8 bytes */
static_assert(sizeof(work_mode_t) == 8, "kernel ioctl struct size");

/*
 * ioctl commands, LEGACY decode (stock-firmware compatibility)
 *
 * All commands take the GPIO index (0–11) as the first field.
 * The driver validates index < nr_gpios before dispatching.
 *
 * These names and values are an early mis-decode of pwm_gpio.ko and are KEPT
 * UNCHANGED for the stock-firmware path (ZWO pwm_gpio.ko, kernel 4.19.219) of
 * AsiairPlusProtocolWrapper, whose ioctl stream is field-validated with exactly these
 * numbers (ConformU, NINA, a flat panel). Do not renumber them and do not use
 * them for new code. What each value really is, per the corrected numbering
 * below:
 *
 *   legacy name          value        real command
 *   PWM_GPIO_GET_LEVEL   0x80084301   GET_LEVEL (correct)
 *   PWM_GPIO_SET_LEVEL   0x40084302   SET_MODE  (work_mode_t; the "level" is the mode)
 *   PWM_GPIO_GET_CONFIG  0x800c4303   GET_CONFIG (correct)
 *   PWM_GPIO_SET_CONFIG  0x400c4304   SET_CONFIG (correct)
 *   PWM_GPIO_DISABLE     0x40044305   PWM ENABLE (starts the module's soft-PWM)
 *   PWM_GPIO_ENABLE      0x40044306   PWM DISABLE (stops it)
 *   PWM_GPIO_SET_MODE    0x40084307   LEVEL_GET (a level read)
 *   PWM_GPIO_GET_KEYS    0x40084308   SET_LEVEL
 *
 * So the legacy sequence SET_MODE(GPIO) -> ENABLE -> SET_LEVEL(raw) runs, on the
 * stock module, LEVEL_GET -> PWM DISABLE -> SET_MODE(mode = raw): ZWO's driver
 * takes mode 0 as "release the line to input" (the DC gate's pull-up powers the
 * port, ON) and mode 1 as "GPIO output, low" (OFF). That is where the "inverted
 * polarity", "needs ENABLE" and "the module's PWM mode is broken" observations
 * in zwo.instructions.md come from. The stock path stays on this decode until
 * the corrected numbering is re-validated on stock firmware.
 */

/* Read GPIO level: pass index, get back level */
#define PWM_GPIO_GET_LEVEL _IOR(PWM_GPIO_MAGIC, 1, gpio_level_t) /* 0x80084301 */

/* Set GPIO level: requires GPIO mode (mode=1), output direction */
#define PWM_GPIO_SET_LEVEL _IOW(PWM_GPIO_MAGIC, 2, gpio_level_t) /* 0x40084302 */

/* Read current PWM config: pass index, get back period_ns + duty_ns */
#define PWM_GPIO_GET_CONFIG _IOR(PWM_GPIO_MAGIC, 3, pwm_param_t) /* 0x800c4303 */

/* Set PWM config: requires PWM mode (mode=2). Sets period and duty cycle */
#define PWM_GPIO_SET_CONFIG _IOW(PWM_GPIO_MAGIC, 4, pwm_param_t) /* 0x400c4304 */

/* Disable GPIO output (set to input / hi-Z). Pass index only */
#define PWM_GPIO_DISABLE _IOW(PWM_GPIO_MAGIC, 5, int) /* 0x40044305 */

/* Enable GPIO output. Pass index only */
#define PWM_GPIO_ENABLE _IOW(PWM_GPIO_MAGIC, 6, int) /* 0x40044306 */

/* Set work mode (GPIO vs PWM) */
#define PWM_GPIO_SET_MODE _IOW(PWM_GPIO_MAGIC, 7, work_mode_t) /* 0x40084307 */

/* Read physical button/key state */
#define PWM_GPIO_GET_KEYS _IOW(PWM_GPIO_MAGIC, 8, gpio_level_t) /* 0x40084308 */

static_assert(PWM_GPIO_SET_LEVEL == 0x40084302u, "legacy decode value, kept for stock firmware");
static_assert(PWM_GPIO_ENABLE == 0x40044306u, "legacy decode value, kept for stock firmware");
static_assert(PWM_GPIO_SET_MODE == 0x40084307u, "legacy decode value, kept for stock firmware");

/*
 * ioctl commands, CORRECTED numbering (the module's real ABI)
 *
 * Source: rk-flashtool, hardware/asiair-plus-rk3568-256g/pwm_gpio.h, corrected
 * 2026-08-18 against the stock zwoair_imager wrappers (gpio_work_mode_set,
 * pwm_param_set, pwm_enable, pwm_disable, gpio_level_get, gpio_level_set) and
 * verified live on stock firmware with the buzzer. The OpenAstro mainline image
 * (Armbian 6.18) ships pwm_gpio_compat, a clean-room module with this ABI:
 *
 *   SET_MODE  {index, mode}: index 0..11 with a line, mode 1 or 2, else EINVAL.
 *             Records the mode; never touches the pad (GPIO after PWM stops the
 *             PWM first).
 *   SET_LEVEL {index, level}: drives the pad with gpiod_set_value, level
 *             1 = high = DC port ON, 0 = OFF. Needs no PWM_ENABLE: probe already
 *             made each non-input line an output at its boot level.
 *   GET_LEVEL / LEVEL_GET: read the real pad (gpiod_get_value), unlike stock,
 *             whose GET_LEVEL answers 0 at boot (zwo.instructions.md).
 *   PWM_ENABLE / PWM_DISABLE: start / stop the module's own soft-PWM. Never
 *             sent on the GPIO path.
 */

/* Read GPIO level: pass index, get back level */
#define PWM_GPIO_ABI_GET_LEVEL _IOR(PWM_GPIO_MAGIC, 1, gpio_level_t) /* 0x80084301 */

/* Set work mode: PWM_GPIO_MODE_GPIO or PWM_GPIO_MODE_PWM */
#define PWM_GPIO_ABI_SET_MODE _IOW(PWM_GPIO_MAGIC, 2, work_mode_t) /* 0x40084302 */

/* Read current PWM config: pass index, get back period_ns + duty_ns */
#define PWM_GPIO_ABI_GET_CONFIG _IOR(PWM_GPIO_MAGIC, 3, pwm_param_t) /* 0x800c4303 */

/* Set PWM config (PWM mode); takes effect at PWM_GPIO_ABI_PWM_ENABLE */
#define PWM_GPIO_ABI_SET_CONFIG _IOW(PWM_GPIO_MAGIC, 4, pwm_param_t) /* 0x400c4304 */

/* Start the module's soft-PWM on the index (pass a pointer to the index) */
#define PWM_GPIO_ABI_PWM_ENABLE _IOW(PWM_GPIO_MAGIC, 5, int) /* 0x40044305 */

/* Stop the module's soft-PWM on the index (pass a pointer to the index) */
#define PWM_GPIO_ABI_PWM_DISABLE _IOW(PWM_GPIO_MAGIC, 6, int) /* 0x40044306 */

/* Read GPIO level (the alternate query the stock app uses) */
#define PWM_GPIO_ABI_LEVEL_GET _IOW(PWM_GPIO_MAGIC, 7, gpio_level_t) /* 0x40084307 */

/* Set GPIO level: 1 = high, 0 = low */
#define PWM_GPIO_ABI_SET_LEVEL _IOW(PWM_GPIO_MAGIC, 8, gpio_level_t) /* 0x40084308 */

static_assert(PWM_GPIO_ABI_SET_MODE == 0x40084302u, "corrected numbering, rk-flashtool pwm_gpio.h");
static_assert(PWM_GPIO_ABI_PWM_ENABLE == 0x40044305u, "corrected numbering, rk-flashtool pwm_gpio.h");
static_assert(PWM_GPIO_ABI_SET_LEVEL == 0x40084308u, "corrected numbering, rk-flashtool pwm_gpio.h");

/*
 * Usage examples (pseudocode, corrected numbering):
 *
 * // Turn on DC port 1 (index 4):
 * int fd = open("/dev/pwm-gpio-misc", O_RDWR);
 * work_mode_t wm = { .index = 4, .mode = PWM_GPIO_MODE_GPIO };
 * ioctl(fd, PWM_GPIO_ABI_SET_MODE, &wm);
 * gpio_level_t gl = { .index = 4, .level = 1 };
 * ioctl(fd, PWM_GPIO_ABI_SET_LEVEL, &gl);
 *
 * // Beep the buzzer (index 3) at 1 kHz (rk-flashtool's verified example):
 * work_mode_t bm = { .index = 3, .mode = PWM_GPIO_MODE_PWM };
 * ioctl(fd, PWM_GPIO_ABI_SET_MODE, &bm);
 * pwm_param_t bp = { .index = 3, .period_ns = 1000000, .duty_ns = 40000 };
 * ioctl(fd, PWM_GPIO_ABI_SET_CONFIG, &bp);   // config first...
 * int idx = 3;
 * ioctl(fd, PWM_GPIO_ABI_PWM_ENABLE, &idx);  // ...then enable starts it
 */

#endif /* PWM_GPIO_H */
