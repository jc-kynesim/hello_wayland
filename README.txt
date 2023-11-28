hello_wayland
-------------

A simple(*) test program that demonstrates how to play video in drmprime
buffers from ffmpeg to Wayland. It has two modes - it can play to an EGL
surface mapped onto a Wayland buffer or it can map the drmprime buffer
directly to a wayland buffer and play using that (more efficient).

No attempt is made to provide proper pacing or correct scaling. A window
size is picked and the video is scaled to fill that to that at (probably)
the refresh speed of the display.

(*) Nothing in Wayland is truely simple

To build
--------

Get required libs:

sudo apt install libavcodec-dev libavformat-dev libavfilter-dev libdrm-dev\
 libepoxy-dev libwayland-dev libwayland-bin\
 libwayland-client0 libwayland-egl1 wayland-protocols

Build:

meson setup build
cd build
meson compile

To use
------

To play via EGL:
./hello_wayland <stream>

To play directly to Wayland:
./hello_wayland -d <stream>

To get a full list of options:
./hello_wayland -h


