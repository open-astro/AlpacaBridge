### The PlayerOne filter wheel SDK on Linux

Note: If you have any problems, please contact us: support@player-one-astronomy.com.

#### The libPlayerOnePW.so relies on libudev.so.1

Please make sure the 'libudev-dev' library already exists on your system, or if not, install it using the command:
(root permission may be required)

for example: on Debian/Ubuntu

sudo apt-get install libudev-dev

#### The udev rules

Please put '99-player_one_astronomy.rules' in the'/lib/udev/rules.d/' or '/etc/udev/rules.d/', you can install it using the command:

sudo install 99-player_one_astronomy.rules /lib/udev/rules.d/
