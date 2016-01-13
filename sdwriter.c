/*
 * sdwriter - a utility to write disk images to SD card.
 *
 * Copyright (C) 2015 Serge Vakulenko
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <getopt.h>

#ifdef __linux__
#   include <libudev.h>
#endif

#ifdef __APPLE__
#   include <CoreFoundation/CoreFoundation.h>
#   include <IOKit/IOBSD.h>
#   include <IOKit/storage/IOMedia.h>
#   include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#   include <IOKit/usb/IOUSBLib.h>
#endif

#ifdef MINGW32
#   include <windows.h>
#   include <winioctl.h>
#   define fsync(fd)    FlushFileBuffers((HANDLE) _get_osfhandle(fd))
#endif

#ifdef GITCOUNT
#   define VERSION      "1.0."GITCOUNT
#else
#   define VERSION      "1.0.0"
#endif

#ifndef O_BINARY
#   define O_BINARY     0
#endif

const char *device_name;        /* Optional name of target device */
int verify_only;                /* Verify-only option */
int debug_level;
const char *progname;
unsigned progress_count;
const char copyright[] = "Copyright (C) 2015 Serge Vakulenko";

/*
 * Terminate the program with a proper status.
 */
void quit(int ok)
{
    exit(ok ? 0 : -1);
}

/*
 * Signal handler.
 */
void interrupted(int signum)
{
    fprintf(stderr, "\nInterrupted.\n");
    quit(0);
}

/*
 * Compute a time interval elapsed since t0, in microseconds.
 */
unsigned mseconds_elapsed(struct timeval *t0)
{
    struct timeval t1;
    unsigned mseconds;

    gettimeofday(&t1, 0);
    mseconds = (t1.tv_sec - t0->tv_sec) * 1000 +
        (t1.tv_usec - t0->tv_usec) / 1000;
    if (mseconds < 1)
        mseconds = 1;
    return mseconds;
}

/*
 * Get a list of SD card devices.
 */
void get_devices(char *devtab[], int maxdev)
{
    int ndev = 0;

#if defined(__linux__)
    /*
     * Create the udev object.
     */
    struct udev *udev = udev_new();
    if (! udev) {
        printf("Can't create udev\n");
        quit(0);
    }

    /*
     * Create a list of the devices in the 'block' subsystem.
     */
    struct udev_enumerate *enumerate = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(enumerate, "block");
    udev_enumerate_scan_devices(enumerate);

    struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);

    /*
     * For each item enumerated, print out its information.
     */
    struct udev_list_entry *dev_list_entry;
    udev_list_entry_foreach(dev_list_entry, devices) {
        if (ndev >= maxdev)
            break;

        /*
         * Get the filename of the /sys entry for the device
         * and create a udev_device object (dev) representing it.
         */
        const char *path = udev_list_entry_get_name(dev_list_entry);
        struct udev_device *dev = udev_device_new_from_syspath(udev, path);

        /*
         * Get the parent device with the subsystem/devtype pair
         * of "usb"/"usb_device".
         */
        struct udev_device *usb = udev_device_get_parent_with_subsystem_devtype(dev,
               "usb", "usb_device");
        if (! usb) {
            //printf("Unable to find parent usb device.\n");
            continue;
        }

        /* Get the 'removable' attribute. */
        const char *removable = udev_device_get_sysattr_value(dev, "removable");
        if (! removable || *removable != '1') {
            continue;
        }

        /* Get the disk size in 512-byte blocks. */
        unsigned size = strtoul(udev_device_get_sysattr_value(dev, "size"), 0, 0);
        if (size == 0) {
            /* SD reader without SD card inserted. */
            continue;
        }

        /* Get the path to the device node in /dev. */
        const char *devpath = udev_device_get_devnode(dev);

        /* From here, we can call get_sysattr_value() for each file
         * in the device's /sys entry. The strings passed into these
         * functions (idProduct, idVendor, serial, etc.) correspond
         * directly to the files in the directory which represents
         * the USB device. Note that USB strings are Unicode, UCS2
         * encoded, but the strings returned from
         * udev_device_get_sysattr_value() are UTF-8 encoded.
         */
        const char *vendor  = udev_device_get_sysattr_value(usb, "manufacturer");
        const char *product = udev_device_get_sysattr_value(usb, "product");

        char buf[1024];
        sprintf(buf, "%s - %s %s, size %u MB",
            devpath, vendor, product, size/2000);
        udev_device_unref(usb);
        devtab[ndev++] = strdup(buf);
    }

    /* Free the enumerator object */
    udev_enumerate_unref(enumerate);
    udev_unref(udev);

#elif defined(__APPLE__)
    /*
     * Create a list of the devices in the 'IOMedia' class.
     */
    CFMutableDictionaryRef dict = IOServiceMatching(kIOMediaClass);
    if (! dict) {
        printf("Cannot create IO Service dictionary.\n");
        return;
    }

    /*
     * Select devices with attributes Removeable=True and Whole=True.
     */
    CFDictionarySetValue(dict, CFSTR(kIOMediaRemovableKey), kCFBooleanTrue);
    CFDictionarySetValue(dict, CFSTR(kIOMediaWholeKey), kCFBooleanTrue);

    io_iterator_t devices = IO_OBJECT_NULL;
    kern_return_t result = IOServiceGetMatchingServices(kIOMasterPortDefault,
        dict, &devices);
    if (result != KERN_SUCCESS) {
        printf("Cannot find matching IO services.\n");
        return;
    }

    /*
     * For each matching device, print out its information.
     */
    io_object_t device;
    while ((device = IOIteratorNext(devices)) != MACH_PORT_NULL) {
        /*
         * Get device path.
         */
        char devname[1024] = "/dev/r";
        CFStringRef ref = (CFStringRef) IORegistryEntrySearchCFProperty(device,
            kIOServicePlane, CFSTR(kIOBSDNameKey),
            kCFAllocatorDefault, kIORegistryIterateRecursively);
        if (! ref || ! CFStringGetCString(ref, devname + 6,
            sizeof(devname) - 6, kCFStringEncodingUTF8)) {
            /* Cannot get device path. */
            continue;
        }

        /*
         * Get device size in bytes.
         */
        long long size;
        ref = IORegistryEntryCreateCFProperty(device,
            CFSTR(kIOMediaSizeKey), kCFAllocatorDefault, 0);
        if (! ref || ! CFNumberGetValue((CFNumberRef)ref, kCFNumberLongLongType, &size)) {
            /* Cannot get device size. */
            continue;
        }

        /*
         * Get a list of device characteristics.
         */
        CFMutableDictionaryRef dict = (CFMutableDictionaryRef)
            IORegistryEntrySearchCFProperty(device, kIOServicePlane,
            CFSTR(kIOPropertyDeviceCharacteristicsKey),
            kCFAllocatorDefault, kIORegistryIterateParents | kIORegistryIterateRecursively);
        if (! dict) {
            /* Cannot get device characteristics. */
            continue;
        }

        /*
         * Get vendor and product names.
         */
        char vendor[1024], product[1024];
        ref = CFDictionaryGetValue(dict, CFSTR(kIOPropertyVendorNameKey));
        if (! ref || ! CFStringGetCString(ref, vendor,
            sizeof(vendor), kCFStringEncodingUTF8)) {
            /* Cannot get vendor name. */
            continue;
        }
        ref = CFDictionaryGetValue(dict, CFSTR(kIOPropertyProductNameKey));
        if (! ref || ! CFStringGetCString(ref, product,
            sizeof(product), kCFStringEncodingUTF8)) {
            /* Cannot get product name. */
            continue;
        }

        char buf[1024];
        sprintf(buf, "%s - size %u MB, %s %s",
            devname, (unsigned) (size / 1000000), vendor, product);
        IOObjectRelease(device);
        devtab[ndev++] = strdup(buf);
    }

    /* Free the iterator object */
    IOObjectRelease(devices);
#elif defined(MINGW32)
    /*
     * Create a list of the removable USB disk devices.
     */

    /* Get a bitmask of available drives (bit 0 = A:, bit 1 = B:, etc). */
    unsigned drive_mask = GetLogicalDrives();
    int i;

    for (i = 0; drive_mask != 0; i++, drive_mask >>= 1) {
        if (! (drive_mask & 1))
            continue;

        char drive_char = 'A' + i;
        char drive_name[] = "\\\\.\\A:\\";
        drive_name[4] = drive_char;

        int drive_type = GetDriveType(drive_name);
        if (drive_type != DRIVE_REMOVABLE)
            continue;

        drive_name[6] = 0;
        HANDLE h = CreateFile(drive_name, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
            OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        /* Ensure that the drive is actually accessible.
         * Multi-card hubs were reporting "removable" even when empty. */
        DWORD dwOutBytes;
        if (! DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY2,
                NULL, 0, NULL, 0, &dwOutBytes, NULL))
        {
            /* IOCTL_STORAGE_CHECK_VERIFY2 fails on some devices under XP/Vista.
             * Try the other (slower) method, just in case. */
            HANDLE g = CreateFile(drive_name, FILE_READ_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                OPEN_EXISTING, 0, NULL);
            if (g == INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                continue;
            }
            if (! DeviceIoControl(g, IOCTL_STORAGE_CHECK_VERIFY,
                    NULL, 0, NULL, 0, &dwOutBytes, NULL)) {
                CloseHandle(g);
                CloseHandle(h);
                continue;
            }
            CloseHandle(g);
        }

        char sdbuf[sizeof(STORAGE_DEVICE_DESCRIPTOR) + 512 - 1];
        PSTORAGE_DEVICE_DESCRIPTOR sd = (void*) sdbuf;
        sd->Size = sizeof(sdbuf);

        STORAGE_PROPERTY_QUERY query;
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        if (! DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                            &query, sizeof(STORAGE_PROPERTY_QUERY), sd,
                            sd->Size, &dwOutBytes, NULL)) {
            CloseHandle(h);
            continue;
        }
        if (sd->BusType != BusTypeUsb) {
            CloseHandle(h);
            continue;
        }

        /* Get the device number. */
        typedef struct _DEVICE_NUMBER {
            DEVICE_TYPE  DeviceType;
            ULONG  DeviceNumber;
            ULONG  PartitionNumber;
        } DEVICE_NUMBER;
        DEVICE_NUMBER dev_num;
        if (! DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                              NULL, 0, &dev_num, sizeof(DEVICE_NUMBER),
                              &dwOutBytes, NULL)) {
            CloseHandle(h);
            continue;
        }

        DISK_GEOMETRY_EX geom;
        if (! DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                &geom, sizeof(geom), &dwOutBytes, NULL)) {
            CloseHandle(h);
            continue;
        }
        unsigned mbytes = geom.DiskSize.QuadPart / 1000000;
        CloseHandle(h);

        char buf[1024];
        sprintf(buf, "\\\\.\\PhysicalDrive%u - Disk %c: size %u MB",
            (unsigned) dev_num.DeviceNumber, drive_char, mbytes);
        devtab[ndev++] = strdup(buf);
    }
#else
    printf("Don't know how to get the list of CD devices on this system.\n");
#endif

    devtab[ndev] = 0;
}

#ifdef MINGW32
/*
 * Lock a filesystem while writing to the disk.
 * Windows-specific action.
 */
void lock_volume(int volume_id)
{
    char name[] = "\\\\.\\A:";
    name[4] += volume_id;

    /* Open volume. */
    HANDLE h = CreateFile(name, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("Failed to open volume %s\n", name);
        return;
    }

    /* Lock volume. */
    DWORD nbytes;
    if (! DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &nbytes, NULL)) {
        printf("Failed to lock volume %s\n", name);
        CloseHandle(h);
        return;
    }

    /* Unmount volume. */
    if (! DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &nbytes, NULL)) {
        printf("Failed to dismount volume %s\n", name);
    }
}
#endif

/*
 * Ask for a name of the target device.
 */
const char *ask_device()
{
#define MAXDEV 9
    char *devices[MAXDEV + 1];
    char reply[100];
    int ndev;

    get_devices(devices, MAXDEV);
    if (! devices[0]) {
        printf("No removable USB disks avalable.\n");
        quit(0);
    }

    for (;;) {
        printf("\n");
        for (ndev=0; devices[ndev]; ndev++) {
            printf("  %c. %s\n", '1'+ndev, devices[ndev]);
        }
        printf("  q. Cancel\n");

        printf("\nSelect disk device ");
        if (ndev > 1)
            printf("(1-%c, q): ", '0'+ndev);
        else
            printf("(1, q): ");
        fflush(stdout);

        if (! fgets(reply, sizeof(reply), stdin)) {
            quit(0);
        }
        if (*reply == 'q' || *reply == 'Q') {
            printf("Cancelled.\n");
            quit(0);
        }
        if (*reply >= '1' && *reply < '1'+ndev) {
            char *devname = devices[*reply - '1'];
#ifdef MINGW32
            char *q = strchr(devname, ':');
            if (q)
                lock_volume(q[-1] - 'A');
#endif
            char *p = strchr(devname, ' ');
            if (p)
                *p = 0;
            printf("\n");
            return devname;
        }
        printf("\nEnter 1");
        if (ndev > 1)
            printf("...%c", '0'+ndev);
        printf(" to select a device,\n");
        printf("or `Q' to cancel the operation.\n");
    }
}

/*
 * Print a symbol repeated.
 */
void print_symbols(char symbol, int cnt)
{
    while (cnt-- > 0)
        putchar(symbol);
}

/*
 * Advance the progress indicator.
 */
int progress(unsigned step)
{
    ++progress_count;
    if (progress_count % step == 0) {
        putchar('#');
        fflush(stdout);
        return 1;
    }
    return 0;
}

/*
 * Print a data mismatch.
 */
void print_mismatch(char *valid, char *invalid, int nbytes,
    unsigned long long offset)
{
    int i;
    unsigned char byte, expected;

    for (i=0; i<nbytes; i++) {
        expected = valid[i];
        byte = invalid[i];
        if (byte != expected) {
            offset += i;
            printf ("\nError at address 0x");
            if ((offset >> 32) == 0)
                printf ("%X", (unsigned) (offset & 0xffffffff));
            else
                printf ("%X%08X", (unsigned) (offset >> 32),
                    (unsigned) (offset & 0xffffffff));
            printf (": file=0x%02X, disk=0x%02X\n", expected, byte);
            quit(0);
        }
    }
}

/*
 * Open the disk device.
 */
void *disk_open(const char *name)
{
#ifdef MINGW32
    HANDLE h;

    h = CreateFile(name, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Cannot open device %s\n", name);
        fprintf(stderr, "Administrator permissions required.\n");
        quit(0);
    }
    return (void*) h;
#else
    int dest;

    dest = open(name, O_RDWR);
    if (dest < 0) {
        perror(name);
        quit(0);
    }
    return (void*) (intptr_t) dest;
#endif
}

/*
 * Close disk device.
 */
void disk_close(void *dest)
{
#ifdef MINGW32
    CloseHandle((HANDLE) dest);
#else
    close((uintptr_t) dest);
#endif
}

/*
 * Write to the disk device.
 */
void disk_write(void *dest, char *buf, unsigned nbytes)
{
#ifdef MINGW32
    unsigned long nwritten;
    if (! WriteFile((HANDLE) dest, buf, nbytes, &nwritten, NULL)) {
        fprintf(stderr, "%s: Write error\n", device_name);
        quit(0);
    }
#else
    if (write((uintptr_t) dest, buf, nbytes) != nbytes) {
        fprintf(stderr, "%s: Write error\n", device_name);
        quit(0);
    }
#endif
}

/*
 * Read from the disk device.
 */
void disk_read(void *src, char *buf, unsigned nbytes)
{
#ifdef MINGW32
    unsigned long nread;
    if (! ReadFile((HANDLE) src, buf, nbytes, &nread, NULL)) {
        fprintf(stderr, "%s: Write error\n", device_name);
        quit(0);
    }
#else
    if (read((uintptr_t) src, buf, nbytes) != nbytes) {
        fprintf(stderr, "%s: Read error\n", device_name);
        quit(0);
    }
#endif
}

/*
 * Wait until all data are written to the disk device.
 */
void disk_flush(void *dest)
{
#ifdef MINGW32
    FlushFileBuffers((HANDLE) dest);
#else
    fsync((uintptr_t) dest);
#endif
}

/*
 * Copy a contents of binary file to the device.
 */
void write_image(const char *filename, int verify_only)
{
    char buf[32*1024];
    int src, n, progress_len, progress_step;
    void *dest;
    struct stat st;
    off_t nbytes, count;
    struct timeval t0;

    src = open(filename, O_RDONLY | O_BINARY);
    if (src < 0) {
        perror(filename);
        quit(0);
    }
    dest = disk_open(device_name);
    fstat(src, &st);
    nbytes = st.st_size;
    printf("     Source: %s\n", filename);
    printf("Destination: %s\n", device_name);
    printf("       Size: %.1f MB\n", nbytes / 1000000.0);

    /* Compute length of progress indicator. */
    for (progress_step=1; ; progress_step<<=1) {
        progress_len = (nbytes + sizeof(buf) - 1) / sizeof(buf);
        if (progress_len / progress_step < 64) {
            progress_len += progress_step - 1;
            progress_len /= progress_step;
            break;
        }
    }

    progress_count = 0;
    gettimeofday(&t0, 0);
    if (! verify_only) {
        printf("      Write: ");
        print_symbols('.', progress_len);
        print_symbols('\b', progress_len);
        fflush(stdout);
        for (count=0; count<nbytes; count+=sizeof(buf)) {
            /* Read data into buffer. */
            n = nbytes - count;
            if (n > sizeof(buf))
                n = sizeof(buf);
            if (read(src, buf, n) != n) {
                fprintf(stderr, "%s: Read error, n=%d\n", filename, n);
                quit(0);
            }

            /* Write data to the disk. */
            disk_write(dest, buf, n);

            if (progress(progress_step)) {
                /* Flush write buffers. */
                disk_flush(dest);
            }
        }
        printf(" done      \n");
        disk_flush(dest);
    }
    if (verify_only) {
        char buf2[sizeof(buf)];

        printf("     Verify: ");
        print_symbols('.', progress_len);
        print_symbols('\b', progress_len);
        fflush(stdout);
        for (count=0; count<nbytes; count+=sizeof(buf)) {
            /* Read source data. */
            n = nbytes - count;
            if (n > sizeof(buf))
                n = sizeof(buf);
            if (read(src, buf, n) != n) {
                fprintf(stderr, "%s: Read error\n", filename);
                quit(0);
            }

            /* Read destination data. */
            disk_read(dest, buf, n);

            /* Compare. */
            if (memcmp(buf, buf2, n) != 0) {
                fprintf(stderr, "DATA ERROR!\n");
                print_mismatch(buf, buf2, n,
                    lseek(src, 0, SEEK_CUR) - n);
                quit(0);
            }
            progress(progress_step);
        }
        printf(" done       \n");
    }
    close(src);
    disk_close(dest);
    printf("      Speed: %.1f MB/sec\n",
        nbytes / 1000.0 / mseconds_elapsed(&t0));
}

/*
 * Print usage information, then terminate the program.
 */
void usage()
{
    char *devices[MAXDEV + 1];
    int ndev;

    printf("%s\n\n", copyright);
    printf("Usage:\n");
    printf("       sdwriter [-v] [-d device] sdcard.img\n");
    printf("\nArgs:\n");
    printf("       sdcard.img          Binary file with SD card image\n");
    printf("       -v                  Verify only\n");
    printf("       -d device           Use specified disk device\n");
    printf("       -D                  Debug mode\n");
    printf("       -h, --help          Print this help message\n");
    printf("       -V, --version       Print version\n");
    printf("\n");

    get_devices(devices, MAXDEV);
    if (! devices[0]) {
        printf("No target disk devices available.\n");
    } else {
        printf("Available disk devices:\n\n");
        for (ndev=0; devices[ndev]; ndev++) {
            printf("        %s\n", devices[ndev]);
        }
    }
    printf("\n");
    exit(0);
}

int main(int argc, char *argv[])
{
    static const struct option long_options[] = {
        { "help",        0, 0, 'h' },
        { "version",     0, 0, 'V' },
        { NULL,          0, 0, 0 },
    };
    const char *filename;
    int ch;

    printf("SD image writer, Version %s\n", VERSION);
    progname = argv[0];
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGINT, interrupted);
#ifdef __linux__
    signal(SIGHUP, interrupted);
#endif
    signal(SIGTERM, interrupted);

    while ((ch = getopt_long(argc, argv, "vd:DhV", long_options, 0)) != -1)
    {
        switch (ch) {
        case 'v':
            ++verify_only;
            continue;
        case 'd':
            device_name = optarg;
            continue;
        case 'D':
            ++debug_level;
            continue;
        case 'h':
            break;
        case 'V':
            /* Version already printed above. */
            return 0;
        }
        usage();
    }
    argc -= optind;
    argv += optind;
    if (argc != 1)
        usage();
    filename = argv[0];

    printf("%s\n", copyright);

    if (! device_name)
        device_name = ask_device();

    write_image(filename, verify_only);

    quit(1);
    return 0;
}
