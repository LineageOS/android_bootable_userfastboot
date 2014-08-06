/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (C) 2011 Intel Corporation
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

#include <errno.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <inttypes.h>
#include <regex.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/fs.h>

#include <cutils/properties.h>
#include <fs_mgr.h>

#include "fastboot.h"
#include "userfastboot_fstab.h"
#include "userfastboot.h"
#include "userfastboot_ui.h"
#include "userfastboot_util.h"

#define DISK_MATCH_REGEX    "^[.]+|(ram|loop)[0-9]+|mmcblk[0-9]+(rpmb|boot[0-9]+)$"

static struct fstab *fstab = NULL;

void load_volume_table()
{
	int i;
	int ret;

	fstab = fs_mgr_read_fstab("/etc/recovery.fstab");
	if (!fstab) {
		pr_error("failed to read /etc/recovery.fstab\n");
		return;
	}

	ret = fs_mgr_add_entry(fstab, "/tmp", "ramdisk", "ramdisk");
	if (ret < 0) {
		pr_error("failed to add /tmp entry to fstab\n");
		fs_mgr_free_fstab(fstab);
		fstab = NULL;
		return;
	}

	pr_debug("recovery filesystem table\n");
	pr_debug("=========================\n");
	for (i = 0; i < fstab->num_entries; ++i) {
		struct fstab_rec *v = &fstab->recs[i];
		pr_debug("  %d %s %s %s %lld\n", i, v->mount_point, v->fs_type,
			 v->blk_device, v->length);
	}
	printf("\n");
}

struct fstab_rec *volume_for_path(const char *path)
{
	return fs_mgr_get_entry_for_mount_point(fstab, path);
}

struct fstab_rec *volume_for_name(const char *name)
{
	char *pat;
	struct fstab_rec *vol;

	/* Historical: it's /data in recovery.fstab, but some fastboot
	 * options (such as -w) expect it to be called userdata */
	if (!strcmp("userdata", name))
		name = "data";

	/* recovery.fstab entries are all prefixed with '/' */
	pat = xasprintf("/%s", name);
	vol = volume_for_path(pat);
	free(pat);
	return vol;
}

char *get_primary_disk_name(void)
{
	DIR *dir;
	int64_t largest = 0;
	char *ret = NULL;
	regex_t diskreg;

	dir = opendir("/sys/block");
	if (!dir) {
		pr_error("opendir");
		return NULL;
	}

	if (regcomp(&diskreg, DISK_MATCH_REGEX, REG_EXTENDED | REG_NOSUB)) {
		pr_perror("regcomp");
		die();
	}

	while (1) {
		struct dirent *dp;
		int64_t removable, disk_size;
		char *devtype;

		dp = readdir(dir);
		if (!dp)
			break;

		if (!regexec(&diskreg, dp->d_name, 0, NULL, 0)) {
			pr_verbose("Skipping %s\n", dp->d_name);
			continue;
		}

		if (read_sysfs_int64(&removable, "/sys/block/%s/removable",
					dp->d_name))
			continue;

		if (removable) {
			pr_verbose("%s is removable, skipping\n", dp->d_name);
			continue;
		}

		/*
		 * SD cards may sometimes also have removable set to 0.
		 * So skip if it is SD card.
		 */
		devtype = read_sysfs("/sys/block/%s/device/type", dp->d_name);
		if (devtype && !strcmp(devtype, "SD")) {
			pr_verbose("%s is of type %s, skipping\n",
				dp->d_name, devtype);
			free(devtype);
			continue;
		}
		free(devtype);
		disk_size = get_disk_size(dp->d_name);
		pr_debug("%s --> %" PRId64 "M\n", dp->d_name, disk_size >> 20);
		if (disk_size > largest) {
			largest = disk_size;
			if (ret)
				free(ret);
			ret = xstrdup(dp->d_name);
		}
	}

	return ret;
}


static void publish_part_data(bool wait, struct fstab_rec *v, char *name)
{
	char *buf;
	uint64_t size;
	struct stat sb;
	int ctr = 15;

	/* Keep trying for ctr seconds for the device node to show up.
	 * ueventd may be busy creating the node */
	while (wait && ctr-- && stat(v->blk_device, &sb)) {
		pr_debug("waiting for %s\n", v->blk_device);
		sleep(1);
	}

	buf = xasprintf("partition-type:%s", name);
	fastboot_publish(buf, xstrdup(v->fs_type));
	free(buf);

	buf = xasprintf("partition-size:%s", name);
	if (get_volume_size(v, &size)) {
		if (wait)
			pr_error("Couldn't get %s volume size\n", name);
		fastboot_publish(buf, xstrdup("0x0"));
	} else {
		fastboot_publish(buf, xasprintf("0x%" PRIx64, size));
	}
	free(buf);
}

void publish_all_part_data(bool wait)
{
	int i;
	for (i = 0; i < fstab->num_entries; i++) {
		struct fstab_rec *v = &fstab->recs[i];

		/* Don't care about sd card slot and it may not even
		 * be there, Skip anything that begins with /sdcard.
		 * skip the fake /tmp entry too */
		if (!strncmp("/sdcard", v->mount_point, 7) ||
				!strcmp("auto", v->mount_point) ||
				!strcmp("/tmp", v->mount_point))
			continue;

		publish_part_data(wait, v, v->mount_point + 1);
		/* Historical */
		if (!strcmp("/data", v->mount_point))
			publish_part_data(wait, v, "userdata");
	}
}

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */

