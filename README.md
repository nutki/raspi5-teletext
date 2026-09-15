Teletext for Raspberry Pi5
-------------------------

This software generates a teletext signal in software. No hardware
mods are needed. Ported from https://github.com/ali1234/raspi-teletext
Instead of using direct register writes to VideoCore (with `tvctl`),
it relies on a modified `drm-rp1-vec` driver.
Because Pi5's video encoder is missing a composer, the downside compared
to original is that it is not possible to share composite output with
other software (though it is possible to integrate teletext display with
custom programs directly accessing drm API). But on the upside since the
driver modification expands the display into tetelext range instead
of shifting upwards like `tvctl` does, the whole screen is still availiable
for display. Also Pi5 allows to use HDMI and composite output at the same
time, so it is possible to run X on HDMI and teletext.

Usage:

First the kernel patch `drm-rp1-vec.patch` needs to be applied. No full
compilation instructions available now, but the script I use to make and
install after initial setup is:

    make ARCH=arm64 M=drivers/gpu/drm/rp1/rp1-vec CONFIG_DRM_RP1_VEC=m modules && \
    sudo install -D -m 644   drivers/gpu/drm/rp1/rp1-vec/drm-rp1-vec.ko   /lib/modules/$(uname -r)/updates/drm-rp1-vec.ko && \
    sudo depmod -a && \
    sudo update-initramfs -u -k "6.18.39+rpt-rpi-2712"

Where `6.18.39+rpt-rpi-2712` is `uname -r`.

To get composite output out of Raspberry Pi 5, first you need to solder
a cable or header to J7 pads (next to one of HDMI ports, marked "VID").
The round pad (closer to the board's edge) is GND, square pad is signal.
To enable output on composite add

    video=Composite-1:720x576i,tv_mode=PAL

to `/boot/firmware/cmdline.txt`.

Build the programs:

    make

If you're connected via PAL, run the demo:

    ./teletext

and press the text button on your TV remote.

Detailed Usage
--------------

    teletext [-m even field line mask] [-o odd field line mask] \
             [-l white level] [-f] [-]

Optional line mask arguments are a 16 bit mask to create quiet lines
in vbi output, first line is LSB, last is MSB. For example running
"teletext -m 0xFFF0 -o 0x0FFF" will output teletext packets on the
first four lines of even fields and last four lines of odd fields.
If only one mask is provided the same value will be used for both
fields.

The white level is specified as a number between 0 and 100. This
is the "brightness" of the high bits of the teletext signal.
Default is 100.

The '-f' argument activates full-field mode, where every available
ine on the display is used for teletext, allowing much greater
bandwidth. Most decoders do not support this mode.

Running with no arguments will show a demo. Running "teletext -"
will read packets from stdin and display them. You can therefore
pipe packets from another tool, possibly over the network with ssh
or netcat.

The packet format is 42 byte raw binary packets, without the clock
run-in. Each packet received will be transmitted once, so you must
send packets endlessly. See

http://www.etsi.org/deliver/etsi_i_ets/300700_300799/300706/01_60/ets_300706e01p.pdf

for details of the teletext protocol.
