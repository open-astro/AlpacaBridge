### Fixed
- **ZWO ASIAIR Plus (RK3568) switch on the OpenAstro mainline image** (AlpacaCore, `PwmGpioMiscBackend`): the image's clean-room `pwm_gpio_compat` module implements the real ioctl numbering of ZWO's `pwm_gpio.ko`, which the driver's original decode had shifted, so turning a DC port on always failed ("PWM_GPIO_SET_LEVEL failed on kernel index 4: Invalid argument") and off changed nothing. The driver now reads the device's kernel driver from sysfs when it opens `/dev/pwm-gpio-misc` and, for `pwm_gpio_compat` only, sends SET_MODE(GPIO) once per port and then SET_LEVEL (1 = on, 0 = off); it logs which numbering it chose. Stock firmware (ZWO `pwm_gpio.ko`, kernel 4.19.219), and any device it cannot identify, keeps the field-validated ioctl sequence unchanged.

### Added (tests)
- **ASIAIR Plus pwm_gpio_compat cases**: the ioctl stub plays `pwm_gpio_compat` behind a character device with a fake sysfs tree (on, off, once-per-line SET_MODE, no PWM enable, a connect that writes nothing, soft-PWM edges, off at server start) and checks that a plain file, ZWO's `pwm_gpio`, a missing driver link and a near-miss driver name all keep the legacy numbering.

### Changed
- **ASIAIR Plus RK3568 docs** (`zwo.instructions.md`, `PowerPorts.md`, `SUPPORTED-DRIVERS.md`, `pwm_gpio.h`): document the legacy ioctl decode as a mis-decode kept for stock firmware, explain the "inverted polarity", "needs ENABLE" and "PWM mode unreachable" notes as artifacts of it, and list the OpenAstro mainline image as a supported OS.
