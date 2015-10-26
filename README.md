Here you can find a sources of sdwriter:
a command-line tool for writing images to SD card.

Linux and Mac OS X are supported.
Version for Windows is under development yet.

=== Usage ===

When called without parameters, sdwriter print a usage screen
and a list of available SD cards:

    $ sdwriter
    SD image writer, Version 1.0.0
    Copyright (C) 2015 Serge Vakulenko

    Usage:
           sdwriter [-v] [-d device] sdcard.img

    Args:
           sdcard.img          Binary file with SD card image
           -v                  Verify only
           -d device           Use specified disk device
           -h, --help          Print this help message
           -V, --version       Print version

    Available disk devices:

            /dev/rdisk3 - size 7892 MB, APPLE SD Card Reader
            /dev/rdisk4 - size 2030 MB, Generic- SD/MMC


=== Write image to SD card ===

When invoked with a parameter - name of file containing SD image,
sdwriter prompts to select the SD card device, and then
writes the image to the specified target:

    $ sudo sdwriter sdcard.img
    Password: ******
    SD image writer, Version 1.0.0
    Copyright (C) 2015 Serge Vakulenko

      1. /dev/rdisk3 - size 7892 MB, APPLE SD Card Reader
      2. /dev/rdisk4 - size 2030 MB, Generic- SD/MMC
      q. Cancel

    Select disk device (1-2, q): 2

         Source: sdcard.img
    Destination: /dev/rdisk4
           Size: 104.9 MB
          Write: ################################################## done
          Speed: 6.6 MB/sec


=== Sources ===

Sources are distributed under the terms of GPL.
You can download sources using Git:

    git clone https://github.com/sergev/sdwriter.git

To build it on Ubuntu, an additional package needs
to be installed:

    sudo apt-get install libudev-dev

To build the program on Linux or Mac OS X, run:

    make

___
Regards,
Serge Vakulenko
