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

#include <errno.h>
#include <fcntl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cutils/android_reboot.h>
#include <cutils/hashmap.h>
#include <cutils/properties.h>

/* from ext4_utils for sparse ext4 images */
#include <sparse_format.h>
#include <sparse/sparse.h>

#include <bootloader.h>
#include "fastboot.h"
#include "userfastboot.h"
#include "userfastboot_util.h"
#include "userfastboot_plugin.h"
#include "userfastboot_ui.h"
#include "gpt.h"

#define CMD_SYSTEM		"system"
#define CMD_SHOWTEXT		"showtext"

struct flash_target {
	char *name;
	Hashmap *params;
};

Hashmap *flash_cmds;
Hashmap *oem_cmds;

static bool strcompare(void *keyA, void *keyB)
{
	return !strcmp(keyA, keyB);
}

static int strhash(void *key)
{
	return hashmapHash(key, strlen((char *)key));
}

static void process_target(char *targetspec, struct flash_target *tgt)
{
	char *params;
	char *paramtoken;
	char *saveptr;
	int no_params = 0;

	params = strchr(targetspec, ':');
	if (!params)
		no_params = 1;
	else {
		*params = '\0';
		params++;
	}

	tgt->name = targetspec;
	pr_verbose("target name: %s\n", targetspec);

	tgt->params = hashmapCreate(8, strhash, strcompare);
	if (!tgt->params) {
		pr_error("Memory allocation failure");
		die();
	}

	if (no_params)
		return;

	for ( ; ; params = NULL) {
		char *argument;

		paramtoken = strtok_r(params, ",", &saveptr);
		if (paramtoken == NULL)
			break;

		argument = strchr(paramtoken, '=');
		if (argument) {
			*argument = '\0';
			argument++;
		}
		pr_verbose("option: '%s' argument: '%s'\n", paramtoken, argument);
		hashmapPut(tgt->params, paramtoken, argument);
	}
}

static int aboot_register_cmd(Hashmap *map, char *key, void *callback)
{
	char *k;

	k = xstrdup(key);
	if (hashmapGet(map, k)) {
		pr_error("key collision '%s'\n", k);
		free(k);
		return -1;
	}
	hashmapPut(map, k, callback);
	pr_verbose("Registered plugin function %p (%s) with table %p\n",
			callback, k, map);
	return 0;
}

int aboot_register_flash_cmd(char *key, flash_func callback)
{
	return aboot_register_cmd(flash_cmds, key, callback);
}

int aboot_register_oem_cmd(char *key, oem_func callback)
{
	return aboot_register_cmd(oem_cmds, key, callback);
}

/* Erase a named partition by creating a new empty partition on top of
 * its device node. No parameters. */
static void cmd_erase(char *part_name, int *fd, unsigned sz)
{
	struct fstab_rec *vol;

	pr_info("%s: %s\n", __func__, part_name);

	vol = volume_for_name(part_name);
	if (vol == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	pr_debug("Erasing %s.\n", part_name);
	if (erase_partition(vol))
		fastboot_fail("Can't erase partition");
	else
		fastboot_okay("");

}

static int cmd_flash_ota_update(Hashmap *params, int *fd, unsigned sz)
{
	struct fstab_rec *cachevol;
	int action = !hashmapContainsKey(params, "noaction");
	int append = hashmapContainsKey(params, "append");
	void *data = NULL;

        cachevol = volume_for_path("/cache");
	if (!cachevol) {
		pr_error("Couldn't find cache partition. Is your recovery.fstab valid?\n");
		return -1;
	}
	if (mount_partition(cachevol)) {
		pr_error("Couldn't mount cache partition\n");
		return -1;
	}

	data = mmap64(NULL, sz, PROT_READ, MAP_SHARED, *fd, 0);
	if (data == (void*)-1){
		pr_error("Failed to mmap the file\n");
		return -1;
	}

	/* Once the update is applied this file is deleted */
	if (named_file_write("/mnt/cache/userfastboot.update.zip",
				data, sz, 0, append)) {
		pr_error("Couldn't write update package to cache partition.\n");
		unmount_partition(cachevol);
		munmap(data, sz);
		return -1;
	}
	unmount_partition(cachevol);
	munmap(data, sz);

	if (action) {
		apply_sw_update("/cache/userfastboot.update.zip", 1);
		return -1;
	}
	return 0;
}

/* Image command. Allows user to send a single file which
 * will be written to a destination location. Typical
 * usage is to write to a disk device node, in order to flash a raw
 * partition, but can be used to write any file.
 *
 * The parameter targetspec can be one of several possibilities:
 *
 * <name> : Look in the flash_cmds table and execute the callback function.
 *          If not found, lookup the named partition in recovery.fstab
 *          and write to its corresponding device node
 *
 * Targetspec may also specify a comma separated list of parameters
 * delimited from the target name by a colon. Each parameter is either
 * a simple string (for flags) or param=value.
 *
 * For flash commands not handled by a plug-in, the following parameters
 * can be set:
 *
 * offset=    : Write the image to the destination at a designated byte offset
 *              from the beginning of the device node. Suffixes "G", "M",
 *              and "K" are recognized.
 */
static void cmd_flash(char *targetspec, int *fd, unsigned sz)
{
	struct flash_target tgt;
	flash_func cb;
	int ret;
        struct fstab_rec *vol;

	off_t offset = 0;
	char *offsetstr;
	void *data = NULL;
	uint32_t magic = 0;

	process_target(targetspec, &tgt);
	pr_verbose("data size %u\n", sz);

	if ( (cb = hashmapGet(flash_cmds, tgt.name)) ) {
		/* Use our table of flash functions registered by platform
		 * specific plugin libraries */
		int cbret;
		cbret = cb(tgt.params, fd, sz);
		if (cbret) {
			pr_error("%s flash failed!\n", tgt.name);
			fastboot_fail(tgt.name);
		} else
			fastboot_okay("");
		goto out;
	} else {
		vol = volume_for_name(tgt.name);
		if (!vol) {
			fastboot_fail(tgt.name);
			goto out;
		}
	}

	if ( (offsetstr = hashmapGet(tgt.params, "offset")) ) {
		off_t multiplier = 1;

		switch (offsetstr[strlen(offsetstr) - 1]) {
		case 'G':
			multiplier *= 1024;
			/* fall through */
		case 'M':
			multiplier *= 1024;
			/* fall through */
		case 'K':
			multiplier *= 1024;
			offsetstr[strlen(offsetstr) - 1] = '\0';
		}

		offset = atol(offsetstr) * multiplier;
	}

	data = mmap64(NULL, sz, PROT_READ, MAP_SHARED, *fd, 0);
	if (data == (void*)-1){
		pr_error("Failed to mmap the file\n");
		goto out_map;
	}

	if (!is_valid_blkdev(vol->blk_device)) {
		fastboot_fail("invalid destination node. partition disks?");
		goto out_map;
	}
	pr_debug("Writing %u bytes to %s at offset: %jd\n",
				sz, vol->blk_device, (intmax_t)offset);

	if (sz >= sizeof(magic))
		memcpy(&magic, data, sizeof(magic));

	if (magic == SPARSE_HEADER_MAGIC) {
		/* If there is enough data to hold the header,
		 * and MAGIC appears in header,
		 * then it is a sparse ext4 image */
		pr_info("Detected sparse header\n");
		ret = named_file_write_ext4_sparse(vol->blk_device, FASTBOOT_DOWNLOAD_TMP_FILE);
	} else {
		ret = named_file_write(vol->blk_device, data, sz, offset, 0);
	}
	pr_verbose("Done writing image\n");
	if (ret) {
		fastboot_fail("Can't write data to target device");
		goto out_map;
	}
	sync();

	pr_debug("wrote %u bytes to %s\n", sz, vol->blk_device);

	fastboot_okay("");
out_map:
	ret = munmap(data, sz);
	if (ret)
		pr_error("Failed to munmap the file\n");
out:
	hashmapFree(tgt.params);
	if (*fd >= 0){
		close(*fd);
		execute_command("rm %s", FASTBOOT_DOWNLOAD_TMP_FILE);
	}
}

static void cmd_oem(char *arg, int *fd, unsigned sz)
{
	char *command, *saveptr, *str1;
	char *argv[MAX_OEM_ARGS];
	int argc = 0;
	oem_func cb;

	pr_verbose("%s: <%s>\n", __FUNCTION__, arg);

	while (*arg == ' ')
		arg++;
	command = xstrdup(arg); /* Can't use strtok() on const strings */

	for (str1 = command; argc < MAX_OEM_ARGS; str1 = NULL) {
		argv[argc] = strtok_r(str1, " \t", &saveptr);
		if (!argv[argc])
			break;
		argc++;
	}
	if (argc == 0) {
		fastboot_fail("empty OEM command");
		goto out;
	}

	if ( (cb = hashmapGet(oem_cmds, argv[0])) ) {
		int ret;

		ret = cb(argc, argv);
		if (ret) {
			pr_error("oem %s command failed, retval = %d\n",
					argv[0], ret);
			fastboot_fail(argv[0]);
		} else
			fastboot_okay("");
	} else if (strcmp(argv[0], CMD_SYSTEM) == 0) {
		int retval;
		arg += strlen(CMD_SYSTEM);
		while (*arg == ' ')
			arg++;
		retval = execute_command("%s", arg);
		if (retval != 0) {
			pr_error("\nfails: %s (return value %d)\n", arg, retval);
			fastboot_fail("OEM system command failed");
		} else {
			pr_verbose("\nsucceeds: %s\n", arg);
			fastboot_okay("");
		}
	} else if (strcmp(argv[0], CMD_SHOWTEXT) == 0) {
		mui_show_text(1);
		fastboot_okay("");
	} else {
		fastboot_fail("unknown OEM command");
	}
out:
	if (command)
		free(command);
	return;
}

static void cmd_boot(char *arg, int *fd, unsigned sz)
{
	/* Copy the boot image to the ESP (bootloader partition)
	 * and set the BCB so that the loader knows to use it */
	struct fstab_rec *vol_bootloader, *vol_misc;
	struct bootloader_message bcb;
	int success = 0;
	void *data;

	vol_bootloader = volume_for_name("bootloader");
	if (vol_bootloader == NULL) {
		fastboot_fail("can't find bootloader partition");
		return;
	}
	vol_misc = volume_for_name("misc");
	if (vol_misc == NULL) {
		fastboot_fail("can't find misc partition");
		return;
	}
	if (mount_partition(vol_bootloader)) {
		fastboot_fail("couldn't mount bootloader partition");
		return;
	}

	data = mmap64(NULL, sz, PROT_READ, MAP_SHARED, *fd, 0);
	if (data == (void*)-1){
		pr_error("Failed to mmap the file\n");
		goto out;
	}

	if (named_file_write("/mnt/bootloader/bootonce.img",
				data, sz, 0, 0)) {
		pr_error("Couldn't write boot image to bootloader partition.\n");
		goto out_unmap;
	}

	memset(&bcb, 0, sizeof(bcb));
	snprintf(bcb.command, sizeof(bcb.command), "bootonce-\\bootonce.img");
	if (named_file_write(vol_misc->blk_device, (void *)&bcb, sizeof(bcb), 0, 0)) {
		pr_error("Couldn't update BCB!\n");
		goto out_unmap;
	}
	success = 1;
out_unmap:
	munmap(data, sz);
out:
	unmount_partition(vol_bootloader);
	if (success) {
		fastboot_okay("");
		pr_info("Booting into supplied image...\n");
		android_reboot(ANDROID_RB_RESTART, 0, 0);
		pr_error("Reboot failed\n");
	}
}

static void cmd_reboot(char *arg, int *fd, unsigned sz)
{
	fastboot_okay("");
	sync();
	pr_info("Rebooting!\n");
	android_reboot(ANDROID_RB_RESTART, 0, 0);
	pr_error("Reboot failed");
}

static void cmd_reboot_bl(char *arg, int *fd, unsigned sz)
{
	fastboot_okay("");
	sync();
	pr_info("Restarting UserFastBoot...\n");
	android_reboot(ANDROID_RB_RESTART2, 0, "fastboot");
	pr_error("Reboot failed");
}

static int start_adbd(int argc, char **argv)
{
	return system("adbd &");
}

static void publish_from_prop(char *key, char *prop, char *dfl)
{
	char val[PROPERTY_VALUE_MAX];
	if (property_get(prop, val, dfl)) {
		char *valcpy = strdup(val);
		if (valcpy) {
			fastboot_publish(key, valcpy);
		}
	}
}

void aboot_register_commands(void)
{
	fastboot_register("oem", cmd_oem);
	fastboot_register("boot", cmd_boot);
	fastboot_register("reboot", cmd_reboot);
	fastboot_register("reboot-bootloader", cmd_reboot_bl);
	fastboot_register("erase:", cmd_erase);
	fastboot_register("flash:", cmd_flash);
	fastboot_register("continue", cmd_reboot);

	fastboot_publish("product", DEVICE_NAME);
	fastboot_publish("kernel", "userfastboot");
	fastboot_publish("version-bootloader", USERFASTBOOT_VERSION);
	fastboot_publish("version-baseband", "unknown");
	publish_from_prop("serialno", "ro.serialno", "unknown");

	flash_cmds = hashmapCreate(8, strhash, strcompare);
	oem_cmds = hashmapCreate(8, strhash, strcompare);
	if (!flash_cmds || !oem_cmds) {
		pr_error("Memory allocation error\n");
		die();
	}
	publish_all_part_data();
	aboot_register_flash_cmd("ota", cmd_flash_ota_update);
	aboot_register_flash_cmd("gpt", cmd_flash_gpt);
	aboot_register_oem_cmd("adbd", start_adbd);
}

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */

