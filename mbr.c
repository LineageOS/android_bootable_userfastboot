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
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <inttypes.h>

#include <cutils/hashmap.h>

#include "mbr.h"
#include "userfastboot_util.h"
#include "userfastboot_ui.h"
#include "fastboot.h"

#define MBR_CODE_SIZE	440

int cmd_flash_mbr(Hashmap *params, int *fd, unsigned sz)
{
	int ret = -1;
	char *device, *target;
	void *data = NULL;

	target = hashmapGet(params, "target");
	if (target) {
		device = xasprintf("/dev/block/%s", target);
	} else {
		device = xasprintf("/dev/block/%s", get_primary_disk_name());
	}

	if (sz > MBR_CODE_SIZE) {
		pr_error("MBR file cannot be larger than 440 bytes!\n");
		goto out;
	}

	data = mmap64(NULL, sz, PROT_READ, MAP_SHARED, *fd, 0);
	if (data == (void*)-1){
		pr_error("Failed to mmap the file\n");
		goto out;
	}

	pr_debug("Writing %u bytes to %s\n", sz, device);
	ret = named_file_write(device, data, sz, 0, 0);

out:
	free(device);

	return ret;
}

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */
