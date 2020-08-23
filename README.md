#ESP32 LED matrix display driver for micropython
This micropython module is a driver for RGB LED matrix panels. Such panels are available very cheap, but they are intended to be used as part of a larger display where they are driven by an FPGA. Driving such a display from a CPU usually requires a very high CPU load in order to display a flicker-free image. This driver however uses the I2S peripheral of the ESP32 in conjunction with DMA to drive the display efficiently without involving the CPU at all.

##Building micropython with this driver
To start with you need the ESP toolchain and the mircopython source on your computer. Refer to the micropython docs to get started.
To build micropython with this driver included:
```
# setup path and virtualenv

cd <PATH TO MICROPYTHON>/ports/esp32
make USER_C_MODULES=<PATH TO MODULES DIR> CFLAGS_EXTRA=-DMODULE_LEDMATRIX_ENABLED=1

# then flash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash -z 0x1000 build-GENERIC/firmware.bin
```
##Using the driver
Once micropython is built with the module and flashed onto a controller, you can use it to display things on your LED matrix.
###Connecting the matrix to the ESP
I won't give you a step-by-step guide to connect the matrix! There are plenty on the internet already. All input pins of the matrix need to be connected to the ESP. Usually a level shifter is required since the ESP runs on 3.3V and the matrix inputs are 5V IOs. Depending on the power supply and your matrix it works without, but no guarantees! You can use any GPIOs in any order. Just be careful not to block an IO you use otherwise. 
Don't worry too much about the wire layout, but be aware that the frequencies are quite high. So avoid unnecessary long wires. You will also need an external power source for the display if you want to run it at a high brightness since it can draw much more power than a typical USB can supply.

###Initializing the driver
The driver is initialized by the `ledmatrix.init` function. Of course you need to adjust the GPIOs to match your setup.
```
import ledmatrix
ledmatrix.init(io_colors=(2,15,4,16,27,17),io_rows=(5,18,19,21),io_clk=22,io_oe=25,io_lat=26,width=64)
```

The init function takes a bunch of required and optional parameters which are listed below.
```
io_colors
   GPIO lines for the color inputs of the matrix
   The order is R1 G1 B1 [R2 G2 B2]
   The *2 values are only required in the double channel (default) mode.
io_rows
   GPIO lines for the row inputs of the matrix
   The order is from LSB to MSB. On most displays these are named A B C D ...
   The display height is implicitly defined by the number of rows
io_oe
   GPIO line for the BLANK / OE (Output enable) input.
io_lat
   GPIO line for the LAT (latch) input
io_clk
   GPIO line for the CLK (clock) input
width
   Width of the display.
   If multiple segments are chained, the width is just extended as if it would be one longer display.
color_depth, default=4
   Number of bits per color channel.
   A higher color depth requires a higher clock to be flicker-free.
clock_speed_khz, default=2500
   Clock speed of the output in khz. Must be between 313 and 40000.
invert, default=False
    Invert the output signal for use with inverting level shifters.
double_buffer, default=False
    Use double buffering for tearing free updates.
    This doubles the memory requirement.
column_swap, default=True
    Swap the output for every second column, since on many displays these are swapped internally.
single_channel, default=False
    Single channel display with only three color lines.
    Most displays are split vertically into an upper and lower half with two separate sets of color lines.
brightness, default=width-2
    Global brightness control.
    Must be between 0 (off) and width - 2 (max)
```

###Display an image
The driver has its own internal framebuffer. Currently the only a full redraw is supported, so no partial updates. The `show` function is used to copy data from an external buffer into the internal structures.
```
# create the framebuffer object and fill blue
buf = bytearray(64*32*2)
fb = framebuf.FrameBuffer(buf, 64, 32, framebuf.RGB565)
fb.fill(0x001f)

# show on display
ledmatrix.show(buf)
```
The driver also supports monochrome (VLSB) and grayscale (8 bit) images. In this case the color is passed as parameter to the `show` function.

Parameters of the `show` function
```
fb
    Framebuffer data
mode, default=FB_RGB565
   Data format must match the format of the specified framebuffer.
   Possible values are:
        FB_RGB565
        FB_GS8
        FB_MONO_HLSB
mono_color, optional
    Color to use for non-rgb images.
```

###Change the global brightness
The global brightness can be changed independently without redrawing the screen. The specified brightness value must be between 0 (off) and width - 2 (max).
```
ledmatrix.set_brightness(3)
```

###Miscellaneous functions
```
# turn screen off and stop data transfer
ledmatrix.stop()

# turn screen back on
ledmatrix.start()

# deinitialize and free all memory
ledmatrix.deinitialize()
```

## Memory requirements
The driver uses one byte per pixel per bit of color depth for the stream buffer. This doubles for single channel displays. The DMA buffer takes additionally `12 bytes * (2 ^ color_depth - 1)` of memory for every 126 pixels of width.
```
Dual channel mode
stream = width * height * color_depth

Single channel mode
stream = width * height * color_depth * 2

dma = 12 * (2 ^ color_depth - 1) * (1 + width // 126)

total = stream + dma

```

External memory can't be used, since it must be DMA accessible.

Double buffering doubles the required memory.

## Clock frequencies and flickering
The effective frame rate can be calculated by
```
line_freq      = clock / width
sub_image_freq = line_freq / (height / 2)
fps            = sub_image_freq / (2 ^ color_depth - 1)
```
So with a normal 64*32 pixel display and a color depth of 4 bit (default), a clock of 2.5 MHz (default) results in a frame rate of 162fps. The frame rate should be at least 100 fps to be relatively flicker-free. It can be lower at a higher color depth as long as there are no very dark pixels in the image. If a dark image is desired, use the global brightness control instead of a high color depth and dark pixels.

The maximum frequency is limited by the display and the used level shifters. Also the cabling can be a limiting factor. For my test setup the limit is about 16 MHz. Above that the image gets blurry. 

##License
This driver is licensed under the MIT license.
