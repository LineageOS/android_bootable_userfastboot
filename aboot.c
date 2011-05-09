/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name of Google, Inc. nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <pthread.h>
#include <linux/input.h>
#include "debug.h"
#include "device.h"
#include "fastboot.h"

/*
 * MULTITHREADED
 *
 * Pay attention to mutual exclusion on all helper functions.
 */
#define AUTOBOOT_DELAY  8

#define CMD_SYSTEM        "system"
#define CMD_ORIGIN        "tarball_origin"
#define CMD_ORIGIN_ROOT   "root"
#define CMD_ORIGIN_MNT    "mount_point"
#define CMD_BOOT_DEV      "bootdev"
#define CMD_BOOT_DEV_SD   "sd"	// Alias for sdcard
#define CMD_BOOT_DEV_SDCARD "sdcard"
#define CMD_BOOT_DEV_NAND "nand"
#define CMD_BOOT_DEV_USB  "usb"

#define SYSTEM_BUF_SIZ     512	/* For system() and popen() calls. */
#define CONSOLE_BUF_SIZ    400	/* For writes to /dev/console and friends */
#define PARTITION_NAME_SIZ 100	/* Partition names (/mnt/boot) */
#define DEVICE_NAME_SIZ    64	/* Device names (/dev/mmcblk0p1) */
#define MOUNT_POINT_SIZ    50	/* /dev/<whatever> */

/*
 * Global Data
 * No need to mutually exclude  serial_fd or screen_fd.
 */
#ifdef DEVICE_HAS_ttyS0
#define SERIAL_DEV      "/dev/ttyS0"
int serial_fd = -1;
#endif

#define SCREEN_DEV      "/dev/tty0"
int screen_fd = -1;

/*
 * Called from initialization only.
 */
void open_consoles(void)
{
	screen_fd = open(SCREEN_DEV, O_RDWR);
#ifdef DEVICE_HAS_ttyS0
	serial_fd = open(SERIAL_DEV, O_RDWR);
#endif
}

/*
 * Called from multiple threads... but nothing here can break in a serious way.
 * We'll ignore mutual exclusion since the argument is stack based and the
 * writes are unlikely to overlap on output.
 */
void write_to_user(char *fmt, ...)
{
	char buf[CONSOLE_BUF_SIZ];

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (screen_fd >= 0)
		write(screen_fd, buf, strlen(buf));
#ifdef DEVICE_HAS_ttyS0
	if (serial_fd >= 0)
		write(serial_fd, buf, strlen(buf));
#endif
}

//#define TAGS_ADDR     (BASE_ADDR + 0x00000100)
//#define KERNEL_ADDR   (BASE_ADDR + 0x00800000)
//#define RAMDISK_ADDR  (BASE_ADDR + 0x01000000)
//#define SCRATCH_ADDR  (BASE_ADDR + 0x02000000)
//
//static struct udc_device surf_udc_device = {
//      .vendor_id      = 0x18d1,
//      .product_id     = 0x0001,
//      .version_id     = 0x0100,
//      .manufacturer   = "Google",
//      .product        = "Android",
//};

struct ptable {
	char name[64];
	short devNum;
	char fsType[64];
};
#define NUMPART  6
#define BOOT     0
#define DATA     1
#define MEDIA    2
#define RECOVERY 3
#define SYSTEM   4
#define CACHE    5
#define device_format_string_sd   "/dev/mmcblk0p%d"
#define device_format_string_nand "/dev/nda%d"
#define device_format_string_usb  "/dev/sda%d"

struct ptable PartTable[] = {
	[BOOT] = {
		  .name = "boot",
		  .devNum = 1,
		  .fsType = "ext3",
		  },

	[DATA] = {
		  .name = "userdata",
		  .devNum = 2,
		  .fsType = "ext3",
		  },

	[MEDIA] = {
		   .name = "media",
		   .devNum = 3,
		   .fsType = "vfat",
		   },

	[RECOVERY] = {
		      .name = "recovery",
		      .devNum = 5,
		      .fsType = "ext3",
		      },

	[SYSTEM] = {
		    .name = "system",
		    .devNum = 6,
		    .fsType = "ext3",
		    },

	[CACHE] = {
		   .name = "cache",
		   .devNum = 7,
		   .fsType = "ext3",
		   }

};

#define KERNEL  "kernel"
#define KERNEL2 "bzImage"
#define INITRD  "ramdisk.img"
#define INITRD2 "initrd"
#define CMDLINE "cmdline"

#define ROOT  "/mnt/boot/"
#define ROOT2 "/mnt/boot/boot/"

#define MAX_SIZE_OF_SCRATCH (256*1024*1024)
static void *scratch = NULL;

int find_block_partition(const char *partName)
{
	int ptn;

	dprintf(INFO, "%s partName - %s\n", __FUNCTION__, partName);
	for (ptn = 0; ptn < NUMPART; ptn++) {
		if (0 == strncmp(partName,
				 PartTable[ptn].name,
				 strlen(PartTable[ptn].name))) {
			dprintf(INFO, "%s part id = %d\n", PartTable[ptn].name,
				ptn);
			return ptn;
		}
	}
	dprintf(INFO, "partName - %s not found\n", partName);
	return -1;
}

int mount_partition(int ptn)
{
	char buf[PARTITION_NAME_SIZ];
	int i;
	char devName[DEVICE_NAME_SIZ];
	char boot_device[DEVICE_NAME_SIZ];

	i = ptn;
	if (i < 0) {
		dprintf(INFO, "ERROR: Invalid partition number to mount (%d)\n",
			i);
		return 1;
	}
	snprintf(buf, sizeof(buf), "/mnt/%s", PartTable[i].name);
	dprintf(INFO, "%s: mkdir %s\n", __FUNCTION__, buf);
	if (mkdir(buf, 0700) < 0) {
		if (errno != EEXIST) {
			dprintf(INFO,
				"ERROR: Unable to create mount directory: %s\n",
				strerror(errno));
			return 1;
		}
	}

	strcpy(boot_device, fastboot_getvar(CMD_BOOT_DEV));
	if (!strcmp(boot_device, CMD_BOOT_DEV_SDCARD)) {
		sprintf(devName, device_format_string_sd, PartTable[i].devNum);
	} else if (!strcmp(boot_device, CMD_BOOT_DEV_NAND)) {
		sprintf(devName, device_format_string_nand,
			PartTable[i].devNum);
	} else if (!strcmp(boot_device, CMD_BOOT_DEV_USB)) {
		sprintf(devName, device_format_string_usb, PartTable[i].devNum);
	}

	dprintf(INFO, "%s: %s (type: %s)\n", __FUNCTION__, devName,
		PartTable[i].name, PartTable[i].fsType);
	snprintf(buf, sizeof(buf), "/mnt/%s", PartTable[i].name);
	if (mount(devName, buf, PartTable[i].fsType, 0, NULL) < 0) {
		dprintf(INFO, "Unable to mount partition: %s\n",
			strerror(errno));
		return 1;
	}
	return 0;
}

int umount_partition(int ptn)
{
	char buf[PARTITION_NAME_SIZ];
	if (ptn < 0)
		return 1;

	snprintf(buf, sizeof(buf), "/mnt/%s", PartTable[ptn].name);
	dprintf(INFO, "%s: umount %s\n", __FUNCTION__, buf);
	if (umount(buf) < 0) {
		if ((errno != EINVAL) && (errno != ENOENT)) {
			dprintf(INFO, "Unmount of %s failed.\n", buf);
			return 1;
		}
	}

	return 0;

}

int umount_all(void)
{
	int i;
	int found_error = 0;

	for (i = 0; i < NUMPART; i++)
		found_error += umount_partition(i);
	return (found_error ? 1 : 0);
}

/*
 * GLOBAL DATA
 * This variable should be accessed only from the accessors below.
 * Since it is never cleared (only set by the disable function), there
 * is no need for a mutex.
 */
int _autoboot_is_disabled = 0;

void display_disabled_message()
{
	static int message_written = 0;
	if (!message_written) {
		write_to_user
		    ("Autoboot disabled. Use 'fastboot continue' to resume boot\n");
		message_written = 1;
	}
	return;
}

void disable_autoboot(void)
{
	_autoboot_is_disabled = 1;
	display_disabled_message();
}

int autoboot_is_disabled(void)
{
	int rc;

	if (_autoboot_is_disabled) {
		display_disabled_message();
		return 1;
	}

	(void)mount_partition(BOOT);
	rc = (access(ROOT "no_autoboot", R_OK) == 0);
	(void)umount_partition(BOOT);

	if (rc)
		display_disabled_message();

	return (rc);
}

/*
 * Read the kernel command line and form/execute the kexec command.
 * We append the androidboot.bootmedia value. This value may also be specified in
 * in the cmdline file. The init process will use the last one found.
 */
int kexec_linux(char *root, char *kernel, char *initrd, char *cmdline)
{
	char buf[SYSTEM_BUF_SIZ], buf2[SYSTEM_BUF_SIZ], *sp;
	const char *bootmedia;
	int fd, rc;

	sprintf(buf2, "%s/%s", root, cmdline);
	fd = open(buf2, O_RDONLY);
	if (fd < 0) {
		write_to_user("ERROR: unable to open %s (%s).\n",
			      strerror(errno));
		return 1;
	}
	if (read(fd, buf2, sizeof(buf2) - 1) <= 0) {
		write_to_user("ERROR: unable to read command line file (%s).\n",
			      strerror(errno));
		return 1;
	}
	buf2[sizeof(buf2) - 1] = 0;
	if (sp = strchr(buf2, '\n'))
		*sp = 0;

	strcat(buf2, " androidboot.bootmedia=");
	bootmedia = fastboot_getvar(CMD_BOOT_DEV);
	if (!strcmp(bootmedia, CMD_BOOT_DEV_USB))
		/* Must match the init.<device>.rc file name */
		bootmedia = "harddisk";
	strcat(buf2, bootmedia);

	snprintf(buf, sizeof(buf),
		 "kexec -f -x %s%s --ramdisk=%s%s --command-line=\"%s\"\n",
		 root, kernel, root, initrd, buf2);
	close(fd);

	dprintf(INFO, "%s: %s\n", __FUNCTION__, buf);
	if (system(buf) < 0) {
		perror(__FUNCTION__);
		return 1;
	}
	return 0;

}

/*
 * Try very hard to locate a triple of [bzImage, initrd, cmdline]. There
 * are several naming conventions and locations to check.
 * This code supports all of:
 *   o Android create-rootfs.sh format
 *   o Android fastboot format
 *   o Moblin image 'dd' format.
 *
 * Called from both threads. It is more less expected to not return,
 * so it is sufficient to do pretty informal mutual exclusion here.
 */
void boot_linux_from_flash_internal(void)
{
	char *kernel, *initrd, *root, *cmdline;
	int fd;
	static int in_boot_linux = 0;

	while (in_boot_linux)
		sleep(1);
	in_boot_linux = 1;

	write_to_user("Attempting to boot from %s.\n",
		      fastboot_getvar(CMD_BOOT_DEV));
	(void)umount_all();
	if (mount_partition(BOOT) != 0) {
		in_boot_linux = 0;
		return;
	}

	root = kernel = initrd = cmdline = NULL;
	if (!access(ROOT KERNEL, R_OK)) {
		root = ROOT;
		kernel = KERNEL;
	} else if (!access(ROOT KERNEL2, R_OK)) {
		root = ROOT;
		kernel = KERNEL2;
	}
	if (!access(ROOT INITRD, R_OK)) {
		initrd = INITRD;
	} else if (!access(ROOT INITRD2, R_OK)) {
		initrd = INITRD2;
	}
	if (!access(ROOT CMDLINE, R_OK)) {
		cmdline = CMDLINE;
	} else if (!access(ROOT CMDLINE, R_OK)) {
		cmdline = CMDLINE;
	}

	if (!root || !initrd || !kernel || !cmdline) {
		root = kernel = initrd = cmdline = NULL;
		if (!access(ROOT2 KERNEL, R_OK)) {
			root = ROOT2;
			kernel = KERNEL;
		} else if (!access(ROOT2 KERNEL2, R_OK)) {
			root = ROOT2;
			kernel = KERNEL2;
		}
		if (!access(ROOT2 INITRD, R_OK)) {
			initrd = INITRD;
		} else if (!access(ROOT2 INITRD2, R_OK)) {
			initrd = INITRD2;
		}
		if (!access(ROOT2 CMDLINE, R_OK)) {
			cmdline = CMDLINE;
		} else if (!access(ROOT2 CMDLINE, R_OK)) {
			cmdline = CMDLINE;
		}
	}

	if (!root || !initrd || !kernel || !cmdline) {
		write_to_user
		    ("ERROR: Could not locate [kernel, initrd, cmdline].\n");
		in_boot_linux = 0;
		return;
	}

	write_to_user("Found: %s [kernel: %s, initrd: %s, cmdline: %s]\n",
		      root, kernel, initrd, cmdline);
	kexec_linux(root, kernel, initrd, cmdline);
	write_to_user("ERROR: Failed to perform Linux boot.\n");
	in_boot_linux = 0;
	return;
}

/*
 * If we can't boot from the default device, try the alternates.
 * Order is important here. We try the external devices before the
 * more internal ones:
 *     default device (or one set by a fastboot command)
 *     SD card
 *     USB card
 *     NAND
 */
void boot_linux_from_flash(void)
{
	const char *default_boot_device;

	/* Attempt to boot to the default device */
	boot_linux_from_flash_internal();

	/* Boot failed. Try each alternate device.
	 * No need to re-try boot on the default device.
	 */
	default_boot_device = fastboot_getvar(CMD_BOOT_DEV);

	if (strcmp(default_boot_device, CMD_BOOT_DEV_SDCARD)) {
		fastboot_publish(CMD_BOOT_DEV, CMD_BOOT_DEV_SDCARD);
		boot_linux_from_flash_internal();
	}
	if (strcmp(default_boot_device, CMD_BOOT_DEV_USB)) {
		fastboot_publish(CMD_BOOT_DEV, CMD_BOOT_DEV_USB);
		boot_linux_from_flash_internal();
	}
	if (strcmp(default_boot_device, CMD_BOOT_DEV_NAND)) {
		fastboot_publish(CMD_BOOT_DEV, CMD_BOOT_DEV_NAND);
		boot_linux_from_flash_internal();
	}

	fastboot_publish(CMD_BOOT_DEV, default_boot_device);

	write_to_user("ERROR: Could not locate [kernel, initrd, cmdline].\n");
	write_to_user("...    Have you used fastboot to load your image?\n");

}

void cmd_reboot(const char *arg, void *data, unsigned sz)
{
	extern int fb_fp;
	extern int enable_fp;

	write_to_user("Rebooting...\n");

	// open the guts of kboot.
	fastboot_okay("");
	close(enable_fp);
	close(fb_fp);

	(void)umount_all();
	sleep(1);
	reboot(0xA1B2C3D4);
}

void cmd_boot(const char *arg, void *data, unsigned sz)
{
	write_to_user("Booting %s.\n", arg);

	// TODO: Teach cmd_boot to boot from a downloaded kernel, bzImage, cmdline
	fastboot_fail("not implemented, boot from flash instead");
}

static int format_partition(int ptn_id)
{
	char devName[DEVICE_NAME_SIZ];
	char buf[SYSTEM_BUF_SIZ];
	char boot_device[DEVICE_NAME_SIZ];
	dprintf(SPEW, "formatting partition %d: %s\n", ptn_id,
		PartTable[ptn_id].name);

	if (umount_partition(ptn_id)) {
		return 1;
	}

	strcpy(boot_device, fastboot_getvar(CMD_BOOT_DEV));
	if (!strcmp(fastboot_getvar(CMD_BOOT_DEV), CMD_BOOT_DEV_SDCARD)) {
		sprintf(devName, device_format_string_sd,
			PartTable[ptn_id].devNum);
	} else if (!strcmp(fastboot_getvar(CMD_BOOT_DEV), CMD_BOOT_DEV_NAND)) {
		sprintf(devName, device_format_string_nand,
			PartTable[ptn_id].devNum);
	} else if (!strcmp(fastboot_getvar(CMD_BOOT_DEV), CMD_BOOT_DEV_USB)) {
		sprintf(devName, device_format_string_usb,
			PartTable[ptn_id].devNum);
	}

	if (!strcmp(PartTable[ptn_id].fsType, "ext3"))
		snprintf(buf, sizeof(buf), "mkfs.%s -L %s %s",
			 PartTable[ptn_id].fsType, PartTable[ptn_id].name,
			 devName);
	else
		snprintf(buf, sizeof(buf), "mkfs.%s -i %s %s",
			 PartTable[ptn_id].fsType, PartTable[ptn_id].name,
			 devName);
	dprintf(INFO, "%s: %s\n", __FUNCTION__, buf);
	if (system(buf) != 0) {
		perror(__FUNCTION__);
		return 1;
	}

	return 0;
}

void cmd_erase(const char *part_name, void *data, unsigned sz)
{
	int ptn_id;

	disable_autoboot();
	write_to_user("Erasing %s.\n", part_name);

	dprintf(INFO, "%s: %s\n", __FUNCTION__, part_name);
	ptn_id = find_block_partition(part_name);
	if (ptn_id < 0) {
		fastboot_fail("unknown partition name");
		return;
	}

	if (format_partition(ptn_id)) {
		fastboot_fail("failed to erase partition");
		return;
	}
	fastboot_okay("");
}

#define BOOT_MAGIC "ANDROID!"
#define BOOT_MAGIC_SIZE 8
#define BOOT_NAME_SIZE 16
#define BOOT_ARGS_SIZE 512

#define PAGE_MASK 2047
#define ROUND_TO_PAGE(x) (((x) + PAGE_MASK) & (~PAGE_MASK))

void cmd_flash(const char *arg, void *data, unsigned sz)
{
	char buf[SYSTEM_BUF_SIZ];
	char mnt_point[MOUNT_POINT_SIZ];
	FILE *fp;
	const char *file;
	int i;
	char *ptn = NULL;
	unsigned extra = 0;

	disable_autoboot();
	write_to_user("Flashing %s.\n", arg);

	dprintf(INFO, "cmd_flash %d bytes to '%s'\n", sz, arg);
	if (*arg == '/') {
		sprintf(mnt_point, "/");
		file = arg;
	} else {
		i = find_block_partition(arg);
		if (i < 0) {
			fastboot_fail("unknown partition name");
			return;
		}
		ptn = PartTable[i].name;
		if (mount_partition(i)) {
			fastboot_fail("mount fail");
			return;
		}

		sprintf(mnt_point, "/mnt/%s", ptn);
		file = arg + strlen(ptn);
		if (file[0] == ':')
			file += 1;	/* skip ':' */
		else
			file = NULL;
	}

// TODO check that boot, and recovery images are TGZ files
//      if (!strcmp(ptn, "boot") || !strcmp(ptn, "recovery")) {
//              if (memcmp((void *)data, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
//                      fastboot_fail("image is not a boot image");
//                      return;
//              }
//      }

	// TODO if boot or recovery I need to crack the compressed
	// ramdisk and kernel.

//      if (!strcmp(ptn, "system") || !strcmp(ptn, "userdata"))
//              extra = 64;
//      else
//              sz = ROUND_TO_PAGE(sz);

	if (file) {
		/* update individual file */
		snprintf(buf, sizeof(buf), "%s/%s", mnt_point, file);
		fp = fopen(buf, "w+");
	} else {
		int origin_is_mntpoint =
		    (strcmp(fastboot_getvar(CMD_ORIGIN), CMD_ORIGIN_MNT) == 0);
		/* update the whole partition */
		snprintf(buf, sizeof(buf), "tar xzf - -C %s",
			 origin_is_mntpoint ? mnt_point : "/mnt");
		fp = popen(buf, "w");
	}

	dprintf(INFO, "%s: %s\n", __FUNCTION__, buf);
	if (fp == NULL) {
		perror("popen or fopen");
		fastboot_fail("fail to open pipe or file");
		return;
	}
	if (sz != fwrite(data, 1, sz, fp)) {
		perror("fwrite");
		fastboot_fail("flash write failure");
		pclose(fp);
		return;
	}
	dprintf(INFO, "wrote %d bytes to '%s'\n", sz, arg);
	pclose(fp);

	if (ptn && umount_partition(i)) {
		fastboot_fail("umount fail");
		return;
	}
	dprintf(INFO, "partition '%s' updated\n", ptn);
	fastboot_okay("");
}

void cmd_continue(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	boot_linux_from_flash();
}

void cmd_oem(const char *arg, void *data, unsigned sz)
{
	const char *command;

	disable_autoboot();
	dprintf(SPEW, "%s: <%s>\n", __FUNCTION__, arg);

	while (*arg == ' ')
		arg++;
	command = arg;

	if (strncmp(command, CMD_SYSTEM, strlen(CMD_SYSTEM)) == 0) {
		arg += strlen(CMD_SYSTEM);
		while (*arg == ' ')
			arg++;
		if (system(arg) != 0) {
			write_to_user("\nfails: %s\n", arg);
			fastboot_fail("OEM system command failed");
		} else {
			write_to_user("\nsucceeds: %s\n", arg);
			fastboot_okay("");
		}

	} else if (strncmp(command, CMD_ORIGIN, strlen(CMD_ORIGIN)) == 0) {
		arg += strlen(CMD_ORIGIN);
		while (*arg == ' ')
			arg++;
		if (strcmp(arg, CMD_ORIGIN_ROOT) == 0) {
			fastboot_publish(CMD_ORIGIN, CMD_ORIGIN_ROOT);
			fastboot_okay("");
		} else if (strcmp(arg, CMD_ORIGIN_MNT) == 0) {
			fastboot_publish(CMD_ORIGIN, CMD_ORIGIN_MNT);
			fastboot_okay("");
		} else {
			fastboot_fail("unknown tarball_origin directory");
		}

	} else if (strncmp(command, CMD_BOOT_DEV, strlen(CMD_BOOT_DEV)) == 0) {
		arg += strlen(CMD_BOOT_DEV);
		while (*arg == ' ')
			arg++;
		if (!strcmp(arg, CMD_BOOT_DEV_SD)
		    || (!strcmp(arg, CMD_BOOT_DEV_SDCARD))) {
			fastboot_publish(CMD_BOOT_DEV, CMD_BOOT_DEV_SDCARD);
			fastboot_okay("");
		} else if (!strcmp(arg, CMD_BOOT_DEV_NAND)) {
			fastboot_publish(CMD_BOOT_DEV, CMD_BOOT_DEV_NAND);
			fastboot_okay("");
		} else if (!strcmp(arg, CMD_BOOT_DEV_USB)) {
			fastboot_publish(CMD_BOOT_DEV, CMD_BOOT_DEV_USB);
			fastboot_okay("");
		} else {
			fastboot_fail("unknown boot device");
		}
	} else {
		fastboot_fail("unknown OEM command");
	}
	return;
}

/*
 * GLOBAL DATA
 * Touched_events could have mutual exclusion... but in practice no bad can come
 * from having it simultaneously read, set and cleared.
 */
#define TOUCHED_PAD     (1)
#define TOUCHED_KEY_ESC (1<<1)
#define TOUCHED_KEY     (1<<2)
volatile int touched_events = 0;

#define EVENT_DEV       "/dev/event%d"
#define EVENT_DEV_MAX   10	/* Max %d for above */

void close_fds(fd_set * fdset)
{
	int i;
	for (i = 0; i < 32; i++) {
		if (FD_ISSET(i, fdset))
			close(i);
	}
}

/*
 * ** THREAD **
 *
 */
void *android_fastboot(void *arg)
{
	fastboot_register("oem", cmd_oem);
	fastboot_register("reboot", cmd_reboot);
	fastboot_register("boot", cmd_boot);
	fastboot_register("erase:", cmd_erase);
	fastboot_register("flash:", cmd_flash);
	fastboot_register("continue", cmd_continue);
#ifdef NAND_BOOT
	fastboot_publish("bootdev", CMD_BOOT_DEV_NAND);
#else
	fastboot_publish("bootdev", CMD_BOOT_DEV_SDCARD);
#endif
#ifdef DEVICE_NAME
	fastboot_publish("product", DEVICE_NAME);
#endif
	fastboot_publish("kernel", "kboot");
	fastboot_publish(CMD_ORIGIN, CMD_ORIGIN_ROOT);

	scratch = malloc(MAX_SIZE_OF_SCRATCH);
	if (scratch == NULL) {
		write_to_user
		    ("ERROR: malloc failed in fastboot. Unable to continue.\n\n");
		return NULL;
	}

	write_to_user("Listening for the fastboot protocol on the USB OTG.\n");
	fastboot_init(scratch, MAX_SIZE_OF_SCRATCH);
	return NULL;
}

/*
 * ** THREAD **
 *
 * Listen for interesting events.
 * Set a (global) flag when an event is found.
 */
void *android_touched(void *arg)
{
	int select_ret, rv;
	int fd, i;
	int max_fd = -1;
	struct timeval timeout;
	fd_set fds, rfds;

	char devname[] = EVENT_DEV "NNNNN";	/* Make sure that the buffer is large enough */
	FD_ZERO(&rfds);

	for (i = 0; i <= EVENT_DEV_MAX; i++) {
		snprintf(devname, sizeof(devname), EVENT_DEV, i);
		fd = open(devname, O_RDONLY);
		if (fd < 0) {
			dprintf(INFO, "Unable to open %s. fd=%d, errno=%d\n",
				devname, fd, errno);
			continue;
		}
		dprintf(INFO, "Opened %s. fd=%d\n", devname, fd);

		max_fd = (fd > max_fd ? fd : max_fd);
		FD_SET(fd, &rfds);
	}

	if (max_fd < 0) {
		write_to_user("ERROR: Unable to open the touch device.\n\n");
		pthread_exit((void *)NULL);
	}

	while (1) {
		struct input_event event;

		fds = rfds;
		select_ret = select(max_fd + 1, &fds, NULL, NULL, NULL);
		dprintf(SPEW, "select returns %d (errno=%d) \n", select_ret,
			errno);

		for (i = 0; i <= max_fd; i++) {
			if (FD_ISSET(i, &fds)) {
				int deb;
				/* read the event */
				if ((deb =
				     read(i, &event,
					  sizeof(event))) != sizeof(event)) {
					dprintf(INFO,
						"Unable to read event from fd=%d, deb=%d, errno=%d\n",
						i, deb, errno);
					continue;
				}

				dprintf(SPEW,
					"read from fd=%d. Event type: %x, code: %x, value: %x\n",
					i, event.type, event.code, event.value);

				switch (event.type) {
				case EV_SYN:
					/* ignore */
					continue;

				case EV_KEY:
					switch (event.code) {
					case KEY_DOT:
						/* This is very likely from the MRST keypad on a
						 * device (such as AAVA) that does not have a keypad.
						 * Ignore the event.
						 */
						continue;

					case KEY_ESC:
						touched_events |=
						    TOUCHED_KEY_ESC;
						continue;

					default:
						touched_events |= TOUCHED_KEY;
						continue;
					}

				case EV_ABS:
				case EV_REL:
					/* Mouse or touchscreen */
					touched_events |= TOUCHED_PAD;
					continue;

				default:
					dprintf(INFO, "Unknown event\n");
					continue;
				}
			}
		}
	}
 /*NOTREACHED*/}

/*
 * ** THREAD **
 *
 */
void *android_autoboot(void *arg)
{
	int sleep_time = (int)arg;

	if (autoboot_is_disabled())
		pthread_exit((void *)NULL);

	write_to_user("Autobooting in %d seconds...\n", sleep_time);

	sleep(sleep_time);
	if (!autoboot_is_disabled())
		boot_linux_from_flash();

	pthread_exit((void *)NULL);
}

/*
 * ** MAIN THREAD **
 *
 */
void android_boot(void)
{
	int rv;
	pthread_t thr;
	pthread_attr_t atr;

	if (pthread_attr_init(&atr) != 0) {
		write_to_user
		    ("ERROR: Unable to set fastboot thread attribute.\n");
		if (!autoboot_is_disabled())
			boot_linux_from_flash();
	} else {
		if (pthread_create(&thr, &atr, android_fastboot, NULL) != 0)
			write_to_user
			    ("ERROR: Unable to create fastboot thread.\n");
		if (pthread_create
		    (&thr, &atr, android_autoboot, (void *)AUTOBOOT_DELAY) != 0)
			write_to_user
			    ("ERROR: Unable to create autoboot thread.\n");
		if (pthread_create(&thr, &atr, android_touched, NULL) != 0)
			write_to_user
			    ("ERROR: Unable to create autoboot thread.\n");
	}

	while (1) {
		sleep(1);
		if (touched_events & TOUCHED_KEY_ESC) {
			write_to_user("Terminating aboot.\n");
			exit(0);
		} else if (touched_events & (TOUCHED_PAD | TOUCHED_KEY)) {
			disable_autoboot();
		}
		touched_events = 0;
	}
}

int main(int c, char *argp)
{
	open_consoles();
#ifdef DEVICE_HAS_KEYPAD
	write_to_user
	    ("To terminate aboot: Hit ESC on your internal keyboard.\n");
#endif
	write_to_user("To disable autoboot either:\n");
	write_to_user(" - Execute a fastboot command (other than getvar).\n");
	write_to_user
	    (" - Place the file 'no_autoboot' on your boot partition.\n");
	write_to_user(" - Stroke the touch screen\n");
	write_to_user(" - Press any key or button\n");
	android_boot();
	exit(1);
}
