/*
 * Copyright (c) 2014 The Android Open Source Project
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

#include <bootimg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mount.h>

#include <efivar.h>

#include "userfastboot_ui.h"
#include "userfastboot_util.h"

/* We have a vague requirment to do sanity checks on any bootloader
 * update operations. Since the userfastboot boot image is an extension
 * of the bootloader (as least with respect to the functionality it provides)
 * we check boot images too.
 *
 * These checks are very basic but as we get reports of people shooting
 * themselves in the foot we can try to add cases to cover them. We
 * really don't want to get too detailed or inflexible here; just try to
 * make sure unintended images don't get flashed.
 */


/* Make sure this is a valid AOSP boot image */
int bootimage_sanity_checks(unsigned char *data, size_t size)
{
	struct boot_img_hdr *hdr = (struct boot_img_hdr *)data;

	if (size < sizeof(*hdr)) {
		pr_error("image too small for even the boot image header!\n");
		return -1;
	}

	if (memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
		pr_error("bad boot image magic - is this an AOSP-style boot image?\n");
		return -1;
	}

	pr_debug("boot image seems OK\n");
	return 0;
}

/* Make sure this is a valid VFAT EFI System Partition image */
int esp_sanity_checks(const char *path)
{
	struct stat sb;
	int ret = -1;
	int loop_fd;

	loop_fd = mount_loopback(path, "vfat", "/mnt");
	if (loop_fd < 0) {
		pr_error("Couldn't loopback mount bootloader image\n");
		return -1;
	}

	if (efi_variables_supported()) {
		/* At least one of these should be present */
		if (stat("/mnt/EFI/BOOT/bootia32.efi", &sb) &&
		    stat("/mnt/EFI/BOOT/bootx64.efi", &sb)) {
			pr_error("Missing BOOT/EFI loaders!\n");
			goto out;
		}
	} else {
		if (stat("/mnt/isolinux.bin", &sb)) {
			pr_error("Missing BOOT/ISOLINUX loader!\n");
			goto out;
		}
	}

	pr_debug("bootloader image seems OK\n");
	ret = 0;
out:
	if (unmount_loopback(loop_fd, "/mnt")) {
		pr_error("Couldn't un-mount the loopback device\n");
		return -1;
	}
	return ret;
}

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */

