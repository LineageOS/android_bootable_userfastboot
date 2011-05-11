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
#include <sys/mount.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <diskconfig/diskconfig.h>

#include "debug.h"
#include "fastboot.h"

/* libdiskconfig data structure representing the intended layout of the
 * internal disk, as read from /etc/disk_layout.conf */
static struct disk_info *dinfo;

#define CMD_SYSTEM		"system"
#define CMD_PARTITION		"partition"

#define SYSTEM_BUF_SIZ     512	/* For system() and popen() calls. */
#define CONSOLE_BUF_SIZ    400	/* For writes to /dev/console and friends */
#define PARTITION_NAME_SIZ 100	/* Partition names (/mnt/boot) */

#define MAX_SIZE_OF_SCRATCH (256*1024*1024)
static void *scratch;

static int execute_command(const char *cmd)
{
	int ret;

	dprintf(SPEW, "Executing: '%s'\n", cmd);
	ret = system(cmd);

	if (ret < 0) {
		dprintf(CRITICAL, "Error while trying to execute '%s': %s\n",
			cmd, strerror(errno));
		return ret;
	}
	ret = WEXITSTATUS(ret);
	dprintf(SPEW, "Done executing '%s' (retval=%d)\n", cmd, ret);

	return ret;
}


static int is_valid_blkdev(const char *node)
{
	struct stat statbuf;
	if (stat(node, &statbuf)) {
		dperror("stat");
		return 0;
	}
	if (!S_ISBLK(statbuf.st_mode)) {
		dprintf(CRITICAL, "%s is not a block device", node);
		return 0;
	}
	return 1;
}


void cmd_reboot(const char *arg, void *data, unsigned sz)
{
	printf("Rebooting...\n");
	fastboot_okay("");
	sync();
	execute_command("reboot");
}

/* Erase a named partition by creating a new empty partition on top of
 * its device node. No parameters. */
void cmd_erase(const char *part_name, void *data, unsigned sz)
{
	struct part_info *ptn;
	char *pdevice = NULL;
	char *cmd = NULL;

	dprintf(INFO, "%s: %s\n", __func__, part_name);
	ptn = find_part(dinfo, part_name);
	if (ptn == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	printf("Erasing %s.\n", part_name);

	pdevice = find_part_device(dinfo, ptn->name);
	dprintf(SPEW, "destination device: %s\n", pdevice);
	if (!is_valid_blkdev(pdevice)) {
		fastboot_fail("invalid destination node. partition disks?");
		goto out;
	}

	switch (ptn->type) {
	case PC_PART_TYPE_LINUX:
		if (asprintf(&cmd, "/system/bin/make_ext4fs -L %s %s",
					ptn->name, pdevice) < 0) {
			fastboot_fail("memory allocation error");
			cmd = NULL;
			goto out;
		}
		if (execute_command(cmd)) {
			fastboot_fail("make_ext4fs failed");
			goto out;
		}
		break;
	default:
		fastboot_fail("Unsupported partition type");
		goto out;
	}

	fastboot_okay("");
out:
	if (pdevice)
		free(pdevice);
	if (cmd)
		free(cmd);
}

/* Image command. Allows user to send a single gzipped file which
 * will be decompressed and written to a destination location. Typical
 * usage is to write to a disk device node, in order to flash a raw
 * partition, but can be used to write any file.
 *
 * The parameter part_name can be one of several possibilities:
 *
 * "disk" : Write directly to the disk node specified in disk_layout.conf,
 *          whatever it is named there.
 * <name> : Lookup the named partition in disk_layout.conf and write to
 *          its corresponding device node
 */
void cmd_flash(const char *part_name, void *data, unsigned sz)
{
	FILE *fp = NULL;
	char *cmd = NULL;
	int ret;
	char *device;
	char *cmd_base;
	struct part_info *ptn = NULL;
	unsigned char *data_bytes = (unsigned char *)data;
	int free_device = 0;

	if (!strcmp(part_name, "disk")) {
		device = dinfo->device;
	} else {
		free_device = 1;
		device = find_part_device(dinfo, part_name);
		if (!device) {
			fastboot_fail("unknown partition specified");
			return;
		}
		ptn = find_part(dinfo, part_name);
	}

	dprintf(SPEW, "destination device: %s\n", device);
	if (!is_valid_blkdev(device)) {
		fastboot_fail("invalid destination node. partition disks?");
		goto out;
	}

	/* Check for a gzip header, and use gzip to decompress if present.
	 * See http://www.gzip.org/zlib/rfc-gzip.html#file-format */
	if (sz > 2 && data_bytes[0] == 0x1f &&
			data_bytes[1] == 0x8b && data_bytes[3] == 8) {
		cmd_base =
		    "/system/bin/gzip -c -d | /system/bin/dd of=%s bs=8192";
	} else {
		cmd_base = "/system/bin/dd of=%s bs=8192";
	}

	if (asprintf(&cmd, cmd_base, device) < 0) {
		dperror("asprintf");
		cmd = NULL;
		fastboot_fail("memory allocation error");
		goto out;
	}

	dprintf(SPEW, "command: %s\n", cmd);
	fp = popen(cmd, "w");
	if (!fp) {
		dperror("popen");
		fastboot_fail("popen failure");
		goto out;
	}
	free(cmd);
	cmd = NULL;

	if (sz != fwrite(data, 1, sz, fp)) {
		dperror("fwrite");
		fastboot_fail("image write failure");
		goto out;
	}
	pclose(fp);
	fp = NULL;
	sync();

	dprintf(SPEW, "wrote %u bytes to %s\n", sz, device);

	/* Check if we wrote to the base device node. If so,
	 * re-sync the partition table in case we wrote out
	 * a new one */
	if (!strcmp(device, dinfo->device)) {
		int fd = open(device, O_RDWR);
		if (fd < 0) {
			fastboot_fail("could not open device node");
			goto out;
		}
		dprintf(SPEW, "sync partition table\n");
		ioctl(fd, BLKRRPART, NULL);
		close(fd);
	}

	/* If this is an ext partition... */
	if (ptn && ptn->type == PC_PART_TYPE_LINUX) {
		/* Resize the filesystem to fill the partition */
		if (asprintf(&cmd, "/system/bin/resize2fs -F %s",
					device) < 0) {
			cmd = NULL;
			fastboot_fail("memory allocation error");
			goto out;
		}
		if (execute_command(cmd)) {
			fastboot_fail("could not resize filesystem "
					"to fill disk");
			goto out;
		}
		free(cmd);
		cmd = NULL;

		/* run fdisk to make sure the partition is OK */
		if (asprintf(&cmd, "/system/bin/e2fsck -C 0 -fy %s",
					device) < 0) {
			cmd = NULL;
			fastboot_fail("memory allocation error");
			goto out;
		}
		ret = execute_command(cmd);
		if (ret < 0 || ret > 1) {
			/* Return value of 1 is OK */
			fastboot_fail("fsck of filesystem failed");
			goto out;
		}
		free(cmd);
		cmd = NULL;

		/* Set mount count to 1 so that 1st mount on boot doesn't
		 * result in complaints */
		if (asprintf(&cmd, "/system/bin/tune2fs -C 1 %s",
					device) < 0) {
			cmd = NULL;
			fastboot_fail("memory allocation error");
			goto out;
		}
		if (execute_command(cmd)) {
			fastboot_fail("tune2fs failed");
			goto out;
		}
		free(cmd);
		cmd = NULL;
	}

	fastboot_okay("");
out:
	if (fp)
		pclose(fp);
	if (cmd)
		free(cmd);
	if (device && free_device)
		free(device);
}

void cmd_oem(const char *arg, void *data, unsigned sz)
{
	const char *command;
	dprintf(SPEW, "%s: <%s>\n", __FUNCTION__, arg);

	while (*arg == ' ')
		arg++;
	command = arg;

	if (strncmp(command, CMD_SYSTEM, strlen(CMD_SYSTEM)) == 0) {
		int retval;
		arg += strlen(CMD_SYSTEM);
		while (*arg == ' ')
			arg++;
		retval = execute_command(arg);
		if (retval != 0) {
			printf("\nfails: %s (return value %d)\n", arg, retval);
			fastboot_fail("OEM system command failed");
		} else {
			printf("\nsucceeds: %s\n", arg);
			fastboot_okay("");
		}
	} else if (strncmp(command, CMD_PARTITION,
				strlen(CMD_PARTITION)) == 0) {
		dprintf(INFO, "Applying disk configuration\n");
		if (apply_disk_config(dinfo, 0))
			fastboot_fail("apply_disk_config error");
		else
			fastboot_okay("");
	} else {
		fastboot_fail("unknown OEM command");
	}
	return;
}

void cmd_boot(const char *arg, void *data, unsigned sz)
{
	fastboot_fail("'boot' command unimplemented");
}

int main(int argc, char **argv)
{
	char *config_location;

	dprintf(INFO, "DROIDBOOT START\n");
	if (argc > 1)
		config_location = argv[1];
	else
		config_location = "/system/etc/disk_layout.conf";

	dprintf(INFO, "Reading disk layout from %s\n", config_location);
	dinfo = load_diskconfig(config_location, NULL);
	dump_disk_config(dinfo);

	fastboot_register("oem", cmd_oem);
	fastboot_register("boot", cmd_boot);
	fastboot_register("erase:", cmd_erase);
	fastboot_register("flash:", cmd_flash);
	fastboot_register("continue", cmd_reboot);

#ifdef DEVICE_NAME
	fastboot_publish("product", DEVICE_NAME);
#endif
	fastboot_publish("kernel", "droidboot");

	scratch = malloc(MAX_SIZE_OF_SCRATCH);
	if (scratch == NULL) {
		printf
		    ("ERROR: malloc failed in fastboot. Unable to continue.\n\n");
		exit(1);
	}

	printf("Listening for the fastboot protocol on the USB OTG.\n");
	fastboot_init(scratch, MAX_SIZE_OF_SCRATCH);

	/* Shouldn't get here */
	exit(1);
}
