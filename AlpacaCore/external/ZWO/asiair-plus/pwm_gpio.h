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
 *   3      15      GPIO0_B7     DC master enable / control signal
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

#define PWM_GPIO_MAGIC  'C'   /* ioctl type/magic byte (0x43) */
#define PWM_GPIO_DEV    "/dev/pwm-gpio-misc"

/* Work modes */
#define PWM_GPIO_MODE_GPIO  1
#define PWM_GPIO_MODE_PWM   2

/*
 * struct gpio_level_s - GPIO level read/write
 * @index: GPIO index (0–11)
 * @level: GPIO value (0 = low, 1 = high)
 */
typedef struct gpio_level_s {
	int index;
	int level;
} gpio_level_t;  /* 8 bytes */

/*
 * struct pwm_parm_s - PWM configuration
 * @index:     GPIO index (0–11)
 * @period_ns: PWM period in nanoseconds
 * @duty_ns:   PWM duty (high time) in nanoseconds
 */
typedef struct pwm_parm_s {
	int index;
	int period_ns;
	int duty_ns;
} pwm_param_t;  /* 12 bytes */

/*
 * struct work_mode_s - GPIO/PWM mode selection
 * @index: GPIO index (0–11)
 * @mode:  1 = GPIO mode, 2 = PWM mode
 */
typedef struct work_mode_s {
	int index;
	int mode;
} work_mode_t;  /* 8 bytes */

/*
 * ioctl commands
 *
 * All commands take the GPIO index (0–11) as the first field.
 * The driver validates index < nr_gpios before dispatching.
 */

/* Read GPIO level: pass index, get back level */
#define PWM_GPIO_GET_LEVEL    _IOR(PWM_GPIO_MAGIC, 1, gpio_level_t)   /* 0x80084301 */

/* Set GPIO level: requires GPIO mode (mode=1), output direction */
#define PWM_GPIO_SET_LEVEL    _IOW(PWM_GPIO_MAGIC, 2, gpio_level_t)   /* 0x40084302 */

/* Read current PWM config: pass index, get back period_ns + duty_ns */
#define PWM_GPIO_GET_CONFIG   _IOR(PWM_GPIO_MAGIC, 3, pwm_param_t)    /* 0x800c4303 */

/* Set PWM config: requires PWM mode (mode=2). Sets period and duty cycle */
#define PWM_GPIO_SET_CONFIG   _IOW(PWM_GPIO_MAGIC, 4, pwm_param_t)    /* 0x400c4304 */

/* Disable GPIO output (set to input / hi-Z). Pass index only */
#define PWM_GPIO_DISABLE      _IOW(PWM_GPIO_MAGIC, 5, int)            /* 0x40044305 */

/* Enable GPIO output. Pass index only */
#define PWM_GPIO_ENABLE       _IOW(PWM_GPIO_MAGIC, 6, int)            /* 0x40044306 */

/* Set work mode (GPIO vs PWM) */
#define PWM_GPIO_SET_MODE     _IOW(PWM_GPIO_MAGIC, 7, work_mode_t)    /* 0x40084307 */

/* Read physical button/key state */
#define PWM_GPIO_GET_KEYS     _IOW(PWM_GPIO_MAGIC, 8, gpio_level_t)   /* 0x40084308 */

/*
 * Usage examples (pseudocode):
 *
 * // Turn on DC port 1 (index 4) at full power:
 * int fd = open("/dev/pwm-gpio-misc", O_RDWR);
 * gpio_level_t gl = { .index = 4, .level = 1 };
 * ioctl(fd, PWM_GPIO_SET_LEVEL, &gl);
 *
 * // Set DC port 1 to 50% PWM for dew heater:
 * work_mode_t wm = { .index = 4, .mode = PWM_GPIO_MODE_PWM };
 * ioctl(fd, PWM_GPIO_SET_MODE, &wm);
 * pwm_param_t pp = { .index = 4, .period_ns = 1000000, .duty_ns = 500000 };
 * ioctl(fd, PWM_GPIO_SET_CONFIG, &pp);
 *
 * // Cut power to USB3 port 1 (index 10):
 * gpio_level_t usb = { .index = 10, .level = 0 };
 * ioctl(fd, PWM_GPIO_SET_LEVEL, &usb);
 *
 * // Read button state (index 2):
 * gpio_level_t key = { .index = 2 };
 * ioctl(fd, PWM_GPIO_GET_KEYS, &key);
 * // key.level now contains button state
 */

#endif /* PWM_GPIO_H */
