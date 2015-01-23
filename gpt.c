/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <ctype.h>
#include <stdint.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <inttypes.h>

#include <cutils/hashmap.h>
#include <iniparser.h>
#include <gpt/gpt.h>
#include <efivar.h>

#include "gpt.h"
#include "userfastboot_util.h"
#include "userfastboot_ui.h"
#include "fastboot.h"

#define _unused __attribute__((unused))

static uint64_t round_up_to_multiple(uint64_t val, uint64_t multiple)
{
	uint64_t rem;
	if (!multiple)
		return val;
	rem = val % multiple;
	if (!rem)
		return val;
	else
		return val + multiple - rem;
}


static uint64_t to_unit_ceiling(uint64_t val, uint64_t unit)
{
	return round_up_to_multiple(val, unit) / unit;
}


static uint64_t to_mib(uint64_t val)
{
	return to_unit_ceiling(val, 1 << 20);
}


static uint64_t to_mib_floor(uint64_t val)
{
	return val >> 20;
}


static uint64_t mib_to_lba(struct gpt *gpt, uint64_t mib)
{
	return (mib << 20) / gpt->lba_size;
}


static char* get_pdata(char *name, char *key, dictionary *config)
{
	char keystr[512];

	snprintf(keystr, sizeof(keystr), "partition.%s:%s", name, key);
	return iniparser_getstring(config, keystr, NULL);
}


static bool flags_cb(char *flag, int _unused index, void *context)
{
	uint64_t *flags_ptr = context;
	uint64_t mask;
	bool enable;

	if (flag[0] == '!') {
		flag++;
		enable = false;
	} else {
		enable = true;
	}

	if (!strcmp(flag, "system"))
		mask = GPT_FLAG_SYSTEM;
	else if (!strcmp(flag, "boot"))
		mask = GPT_FLAG_BOOTABLE;
	else if (!strcmp(flag, "ro"))
		mask = GPT_FLAG_READONLY;
	else if (!strcmp(flag, "hidden"))
		mask = GPT_FLAG_HIDDEN;
	else if (!strcmp(flag, "noauto"))
		mask = GPT_FLAG_NO_AUTOMOUNT;
	else {
		pr_error("unknown partition flag '%s'\n", flag);
                return false;
        }

	if (enable)
		*flags_ptr |= mask;
	else
		*flags_ptr &= ~mask;

	return true;
}


static int string_to_type(char *type)
{
	if (!strcmp(type, "esp"))
		return PART_ESP;
	else if (!strcmp(type, "boot"))
		return PART_ANDROID_BOOT;
	else if (!strcmp(type, "recovery"))
		return PART_ANDROID_RECOVERY;
	else if (!strcmp(type, "tertiary"))
		return PART_ANDROID_TERTIARY;
	else if (!strcmp(type, "misc"))
		return PART_ANDROID_MISC;
	else if (!strcmp(type, "metadata"))
		return PART_ANDROID_METADATA;
	else if (!strcmp(type, "linux"))
		return PART_LINUX;
	else if (!strcmp(type, "fat"))
		return PART_MS_DATA;
	else if (!strcmp(type, "swap"))
		return PART_LINUX_SWAP;
	else
		return -1;
}


struct flash_gpt_context {
	struct gpt *gpt;
	dictionary *config;
	uint64_t size_mb;
	uint64_t expand_mb;
	uint64_t next_mb;
	bool found;
	int esp_index;
	char *esp_title;
	char *esp_loader;
};


static bool sumsizes_cb(char *entry, int index _unused, void *data)
{
	struct flash_gpt_context *ctx = (struct flash_gpt_context *)data;
	char *lenstr;
	int64_t len;

	lenstr = get_pdata(entry, "len", ctx->config);
	if (!lenstr) {
		pr_error("Partition %s doesn't specify len\n", entry);
		return false;
	}

	len = atoll(lenstr);
	if (len > 0) {
		ctx->size_mb += len;
	} else {
		if (ctx->found) {
			pr_error("More than one partition with size -1 specified!\n");
			return false;
		} else {
			ctx->found = true;
		}
	}
	return true;
}

static bool create_ptn_cb(char *entry, int i _unused, void *data)
{
	struct flash_gpt_context *ctx;
	int64_t len;
	uint64_t flags;
	char *label, *type, *flagstr, *guidstr;
	struct gpt_entry *ge;
	int type_code;
	uint32_t index;

	ctx = (struct flash_gpt_context *)data;
	label = get_pdata(entry, "label", ctx->config);
	if (!label) {
		pr_error("No label specified for partition %s\n", entry);
		return false;
	}
	if (strlen(label) > 36) {
		pr_error("Label %s is too long for GPT\n", label);
		return false;
	}
	type = get_pdata(entry, "type", ctx->config);
	if (!type) {
		pr_error("no type specified for partition %s\n", entry);
		return false;
	}
	type_code = string_to_type(type);
	if (type_code < 0) {
		pr_error("unknown partition type %s\n", type);
		return false;
	}

	/* sumsizes_cb ensures that this value has been populated */
	len = atoll(get_pdata(entry, "len", ctx->config));
	if (len < 0)
		len = ctx->expand_mb;

	flagstr = get_pdata(entry, "flags", ctx->config);
	flags = 0;
	if (flagstr)
		string_list_iterate(flagstr, flags_cb, &flags);

	pr_verbose("Create partition %s at MiB %" PRIu64 " to %" PRIu64"\n", entry,
			ctx->next_mb, ctx->next_mb + len);
	index = gpt_entry_create(ctx->gpt, label, type_code, flags,
				mib_to_lba(ctx->gpt, ctx->next_mb),
				mib_to_lba(ctx->gpt, ctx->next_mb + len) - 1);
	if (!index) {
		pr_error("Couldn't create partition %s\n", entry);
		return false;
	}

	/* Here we abuse the GPT spec a little. In the bootloader, the EFI
	 * BIOS only has functions to look up partitions by the supposedly
	 * unique part_guid, with no way to look up by name or type guid.
	 * Instead of writing a whole bunch of GPT parsing code in the loader,
	 * just used fixed values for the part_guid if specified in the config
	 * file we passed in */
	guidstr = get_pdata(entry, "guid", ctx->config);
	if (guidstr) {
		ge = gpt_entry_get(index, ctx->gpt);
		if (!ge) {
			pr_error("Internal error creating GPT\n");
			return false;
		}
		if (gpt_string_to_guid(&ge->part_guid, guidstr)) {
			pr_error("GUID '%s' is malformed\n", guidstr);
			return false;
		}
	}

	if (type_code == PART_ESP) {
		if (ctx->esp_index) {
			pr_error("Disk has multiple EFI System Partitions\n");
			return false;
		}
		ctx->esp_loader = get_pdata(entry, "efi_loader", ctx->config);
		if (ctx->esp_loader) {
			ctx->esp_title = get_pdata(entry, "efi_title", ctx->config);
			if (!ctx->esp_title) {
				pr_error("efi_boot specified with no efi_title\n");
				return false;
			}
			ctx->esp_index = index;
			pr_debug("loader %s title %s index %d\n", ctx->esp_loader, ctx->esp_title, index);
		}
	}

	ctx->next_mb += len;
	return true;
}

#define MIN_DATA_PART_SIZE	350 /* CDD section 7.6.1 */

int cmd_flash_gpt(Hashmap *params, int fd, void *data, unsigned sz)
{
	int ret = -1;
	char *device = NULL, *plist, *buf, *conf_device;
	struct flash_gpt_context ctx;
	uint64_t start_lba, end_lba, start_mb, end_mb;
	uint64_t space_available_mb;

	memset(&ctx, 0, sizeof(ctx));

	ctx.config = iniparser_load(FASTBOOT_DOWNLOAD_TMP_FILE);
	if (!ctx.config) {
		pr_error("Couldn't parse GPT config\n");
		return -1;
	}

	conf_device = iniparser_getstring(ctx.config, "base:device", NULL);
	if (!conf_device || !strcmp(conf_device, "auto")) {
		char *disk_name = get_primary_disk_name();
		if(!disk_name) {
			pr_error("Couldn't get primary disk name\n");
			goto out;
		}
		device = xasprintf("/dev/block/%s", disk_name);
		free(disk_name);
	} else {
		device = xstrdup(conf_device);
	}

	plist = iniparser_getstring(ctx.config, "base:partitions", NULL);
	if (!plist) {
		pr_error("Configuration doesn't have a partition list\n");
		goto out;
	}

	ctx.gpt = gpt_init(device);
	if (!ctx.gpt) {
		pr_error("Couldn't init gpt for %s\n", device);
		goto out;
	}

	if (gpt_new(ctx.gpt)) {
		pr_error("Couldn't initialize empty GPT\n");
		goto out_free_gpt;
	}

	pr_debug("Disk %s has %" PRIu64" %d-byte sectors for a total capacity of %"
			PRIu64 " MiB\n", device, ctx.gpt->sectors, ctx.gpt->lba_size,
			to_mib_floor(ctx.gpt->sectors * ctx.gpt->lba_size));

	/* Find out the total size of the partitions specified, so that
	 * if there is a partition with -1 size (typically /data) we
	 * know how large to make it */
	ctx.size_mb = 0;
	ctx.found = false;
	if (string_list_iterate(plist, sumsizes_cb, &ctx)) {
		pr_error("Couldn't sum up partition sizes\n");
		goto out_free_gpt;
	}

	gpt_find_contiguous_free_space(ctx.gpt, &start_lba, &end_lba);
	start_mb = to_mib(start_lba * ctx.gpt->lba_size);
	end_mb = to_mib_floor((end_lba + 1) * ctx.gpt->lba_size);
	space_available_mb = end_mb - start_mb;
	if (space_available_mb < (ctx.size_mb + MIN_DATA_PART_SIZE)) {
		pr_error("insufficient disk space\n");
		goto out_free_gpt;
	}
	ctx.expand_mb = space_available_mb - ctx.size_mb;
	if (ctx.expand_mb && !ctx.found)
		pr_warning("Disk has %" PRIu64 " MiB of unused space!\n", ctx.expand_mb);

	ctx.next_mb = start_mb;
	if (string_list_iterate(plist, create_ptn_cb, &ctx)) {
		pr_error("Failed to create partitions\n");
		goto out_free_gpt;
	}

	/* Dump GPT contents to log */
	buf = gpt_dump_header(ctx.gpt);
	if (buf) {
		pr_debug("%s\n", buf);
		free(buf);
	}
	buf = gpt_dump_pentries(ctx.gpt);
	if (buf) {
		char *buf2, *line;
		for (buf2 = buf; ; buf2 = NULL) {
			line = strtok(buf2, "\n");
			if (!line)
				break;
			pr_debug("%s\n", line);
		}
		free(buf);
	}


	if (gpt_write(ctx.gpt)) {
		pr_error("Couldn't commit new GPT to disk\n");
		goto out_free_gpt;
	}

	if (gpt_sync_ptable(ctx.gpt->device))
		pr_warning("Couldn't re-read GPT, please reboot!\n");
	publish_all_part_data(true);

	if (efi_variables_supported()) {
		if (ctx.esp_index) {
			ret = execute_command("/sbin/efibootmgr -c -d %s -l %s -v -p %d -D %s -L %s",
					ctx.gpt->device, ctx.esp_loader, ctx.esp_index,
					ctx.esp_title, ctx.esp_title);
			if (ret) {
				pr_warning("EFIBOOTMGR failed with exit status %d\n", ret);
				goto out_free_gpt;
			}
		} else {
			pr_warning("Disk has no EFI system partition\n");
		}
	} else {
		pr_debug("Skip calling efiboormgr on non-EFI system\n");
	}
	ret = 0;

out_free_gpt:
	gpt_close(ctx.gpt);
out:
	iniparser_freedict(ctx.config);
	free(device);

	return ret;
}

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */


