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

#include <ctype.h>
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
#include <inttypes.h>
#include <linux/input.h>
#include <sys/utsname.h>

#include <cutils/android_reboot.h>
#include <cutils/hashmap.h>
#include <cutils/properties.h>

/* from ext4_utils for sparse ext4 images */
#include <sparse_format.h>
#include <sparse/sparse.h>
#include <efivar.h>

#include <bootloader.h>
#include "aboot.h"
#include "fastboot.h"
#include "userfastboot.h"
#include "userfastboot_util.h"
#include "userfastboot_plugin.h"
#include "userfastboot_ui.h"
#include "gpt.h"
#include "mbr.h"
#include "network.h"
#include "sanity.h"
#include "keystore.h"

/* Generated by the makefile, this function defines the
 * register_userfastboot_plugins() function, which calls all the
 * registration functions for device-specific extensions. */
#include "register.inc"

#define CMD_SHOWTEXT		"showtext"
#define CMD_HIDETEXT		"hidetext"

#define CMD_LOCK		"lock"
#define CMD_LOCK_NC		"lock-noconfirm"
#define CMD_UNLOCK		"unlock"
#define CMD_UNLOCK_NC		"unlock-noconfirm"
#define CMD_VERIFIED		"verified"
#define CMD_VERIFIED_NC		"verified-noconfirm"

/* Current device state, set here, affects how bootloader functions */
#define OEM_LOCK_VAR		"OEMLock"
#define OEM_LOCK_UNLOCKED	(1 << 0)
#define OEM_LOCK_VERIFIED	(1 << 1)

/* Boot state as reported by the loader */
#define BOOT_STATE_VAR		"BootState"
#define BOOT_STATE_GREEN	0
#define BOOT_STATE_YELLOW	1
#define BOOT_STATE_ORANGE	2
#define BOOT_STATE_RED		3

#define EFI_GLOBAL_VARIABLE \
	EFI_GUID(0x8BE4DF61, 0x93CA, 0x11d2, 0xAA0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C);
#define SECURE_BOOT_VAR		"SecureBoot"

/* EFI Variable to store user-supplied key store binary data */
#define KEYSTORE_VAR		"KeyStore"

#define LOADER_GUID \
	EFI_GUID(0x4a67b082, 0x0a4c, 0x41cf, 0xb6c7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f);

#define LOADER_VERSION_VAR      "LoaderVersion"

/* Initial list of flash targets that are allowed in VERIFIED state */
static char *default_flash_whitelist[] = {
	"bootloader",
	"boot",
	"system",
	"oem", /* alternate name for vendor */
	"vendor",
	"recovery",
	/* Following three needed even though not specifically listed
	 * since formatting a partition necessitates flashing a sparse
	 * filesystem image */
	"cache",
	"data",
	"userdata",
	NULL
};

/* Initial list of erase targets which are allowed in VERIFIED state */
static char *default_erase_whitelist[] = {
	"cache",
	"data",
	"userdata",
	/* following three needed so we can flash them even though not
	 * specifically listed, they all contain filesystems which can
	 * be sent over as sparse images */
	"system",
	"vendor",
	"oem",
	NULL
};

struct flash_target {
	char *name;
	Hashmap *params;
};

struct cmd_struct {
	void *callback;
	enum device_state min_state;
};

Hashmap *flash_cmds;
Hashmap *oem_cmds;
Hashmap *flash_whitelist;
Hashmap *erase_whitelist;

static char *unlock_headers[] = {
	"**** Unlock bootloader? ****",
	"",
	"If you unlock the bootloader, you will be able to install custom operating",
	"system software on this device and such software will not be verified at boot.",
	"",
	"Changing device state will also delete all personal data from your device",
	"(a 'factory data reset').",
	"",
	"Press the Volume Up/Down to select Yes or No. Then press the Power button.",
	"",
	NULL };

static char *lock_headers[] = {
	"**** Lock bootloader? ****",
	"",
	"If you lock the bootloader, you will prevent the device from having any",
	"custom software flashed until it is again set to 'unlocked' or 'verified'",
	"state.",
	"",
	"Changing device state will also delete all personal data from your device",
	"(a 'factory data reset').",
	"",
	"Press the Volume Up/Down to select Yes or No. Then press the Power button.",
	"",
	NULL };

static char *verified_headers[] = {
	"**** Set bootloader to Verified? ****",
	"",
	"If you set the loader to Verified state, you may flash custom software to",
	"the device and the loader will attempt to verify these custom images against",
	"either the OEM keystore or a keystore supplied by you. Some, but not all",
	"fastboot commands will be available.",
	"",
	"Changing device state will also delete all personal data from your device",
	"(a 'factory data reset').",
	"",
	"Press the Volume Up/Down to select Yes or No. Then press the Power button.",
	"",
	NULL };


static const char *state_to_string(enum device_state ds)
{
	switch (ds) {
	case UNLOCKED:
		return "unlocked";
	case LOCKED:
		return "locked";
	case VERIFIED:
		return "verified";
	}
	/* silence warning, shouldn't get here */
	die();
	return NULL;
}


static enum device_state get_device_state(void)
{
	int ret;
	uint32_t attributes;
	char *data = NULL;
	size_t dsize;
	efi_guid_t fastboot_guid = FASTBOOT_GUID;
	enum device_state state;

	if (!efi_variables_supported()) {
		pr_debug("EFI variables not supported, assuming permanently unlocked non-EFI system\n");
		state = UNLOCKED;
		goto out;
	}

	ret = efi_get_variable(fastboot_guid, OEM_LOCK_VAR, (uint8_t **)&data,
			&dsize, &attributes);
	if (ret || !dsize) {
		pr_debug("Couldn't read OEMLock, assuming locked\n");
		state = LOCKED;
		goto out;
	}

	/* Legacy OEMLock format, used to have string "0" or "1"
	 * for unlocked/locked */
	if (dsize == 2 && data[1] == '\0') {
		if (!strcmp(data, "0")) {
			state = UNLOCKED;
			goto out;
		}
		if (!strcmp(data, "1")) {
			state = LOCKED;
			goto out;
		}
	}

	if (data[0] & OEM_LOCK_UNLOCKED) {
		state = UNLOCKED;
		goto out;
	}

	if (data[0] & OEM_LOCK_VERIFIED) {
		state = VERIFIED;
		goto out;
	}

	state = LOCKED;
out:
	free(data);
	return state;
}


static void fetch_boot_state(void)
{
	int ret;
	uint32_t attributes;
	char *data = NULL;
	size_t dsize;
	efi_guid_t fastboot_guid = FASTBOOT_GUID;
	char *state;

	ret = efi_get_variable(fastboot_guid, BOOT_STATE_VAR, (uint8_t **)&data,
			&dsize, &attributes);
	if (ret || dsize != 1) {
		pr_debug("Couldn't read boot state\n");
		state = "unknown";
	} else {
		switch (data[0]) {
		case BOOT_STATE_GREEN:
			state = "GREEN";
			break;
		case BOOT_STATE_ORANGE:
			state = "ORANGE";
			break;
		case BOOT_STATE_RED:
			state = "RED";
			break;
		case BOOT_STATE_YELLOW:
			state = "YELLOW";
			break;
		default:
			state = "unknown";
		}
	}
	free(data);

	fastboot_publish("boot-state", xstrdup(state));
}


static bool is_secure_boot_enabled(void)
{
	int ret;
	uint32_t attributes;
	char *data = NULL;
	size_t dsize;
	efi_guid_t global_guid = EFI_GLOBAL_VARIABLE;
	bool secure;

	if (!efi_variables_supported()) {
		secure = false;
		goto out;
	}

	ret = efi_get_variable(global_guid, SECURE_BOOT_VAR, (uint8_t **)&data,
			&dsize, &attributes);
	if (ret) {
		pr_debug("Couldn't read SecureBoot\n");
		secure = false;
		goto out;
	}

	if (!dsize) {
		secure = false;
		goto out;
	}

	if (data[0] == 1) {
		secure = true;
		goto out;
	}

	secure = false;
out:
	free(data);
	return secure;
}


static bool confirm_device_state(char *headers[])
{
	int chosen_item = -1;
	int selected = 1;
	bool result = false;

	char *items[] = {
		"Yes: Change device state",
		"No: Cancel",
		NULL };

	mui_clear_key_queue();

	if (mui_start_menu(headers, items, selected)) {
		/* Couldn't start the menu due to no graphics. Just do it. */
		return true;
	}

	fastboot_info("Please confirm the device state action using the UI.");

	while (chosen_item < 0) {
		int key = mui_wait_key();

		pr_debug("got key event %d\n", key);
		switch (key) {
		case -1:
			pr_info("device state prompt timed out\n");
			goto out;

		case KEY_UP:
		case KEY_VOLUMEUP:
			--selected;
			selected = mui_menu_select(selected);
			break;

		case KEY_DOWN:
		case KEY_VOLUMEDOWN:
			++selected;
			selected = mui_menu_select(selected);
			break;

		case KEY_POWER:
		case KEY_ENTER:
			if (selected == 0)
				result = true;
			goto out;
		}
	}
out:
	mui_end_menu();
	return result;
}


static void update_device_state_metadata(void)
{
	enum device_state dstate;

	dstate = get_device_state();

	fastboot_publish("device-state", xstrdup(state_to_string(dstate)));

	switch (dstate) {
	case LOCKED:
		fastboot_publish("unlocked", xstrdup("no"));
		break;
	case UNLOCKED:
	case VERIFIED:
		fastboot_publish("unlocked", xstrdup("yes"));
		break;
	}
}


static int set_device_state(enum device_state device_state,
		bool skip_confirmation)
{
	enum device_state current_state;
	bool must_erase;
	struct fstab_rec *vol;
	uint8_t statevar;
	char **headers = NULL;
	int ret;
	efi_guid_t fastboot_guid = FASTBOOT_GUID;

	current_state = get_device_state();
	if (current_state == device_state) {
		pr_info("Nothing to do.");
		return 0;
	}

	switch (device_state) {
	case LOCKED:
		statevar = 0;
		headers = lock_headers;
		break;
	case UNLOCKED:
		statevar = OEM_LOCK_UNLOCKED;
		headers = unlock_headers;
		break;
	case VERIFIED:
		statevar = OEM_LOCK_VERIFIED;
		headers = verified_headers;
		break;
	}

	vol = volume_for_name("data");
	if (!vol) {
		pr_error("invalid fstab\n");
		return -1;
	}
	must_erase = is_valid_blkdev(vol->blk_device);

	if (must_erase) {
		if (!skip_confirmation && !confirm_device_state(headers))
			return -1;

		pr_status("Userdata erase required, this can take a while...\n");
		fastboot_info("Userdata erase required, this can take a while...\n");

		if (erase_partition(vol)) {
			pr_error("couldn't erase data partition\n");
			return -1;
		}
	}

	ret = efi_set_variable(fastboot_guid, OEM_LOCK_VAR,
			&statevar, sizeof(statevar),
			EFI_VARIABLE_NON_VOLATILE |
			EFI_VARIABLE_RUNTIME_ACCESS |
			EFI_VARIABLE_BOOTSERVICE_ACCESS);
	if (ret) {
		pr_error("Coudn't set OEMLock\n");
		return -1;
	}

	current_state = get_device_state();
	if (current_state != device_state) {
		pr_error("Failed to set device state\n");
		return -1;
	}

	update_device_state_metadata();
	populate_status_info();
	return 0;
}


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
		pr_error("Memory allocation failure\n");
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

static int aboot_register_cmd(Hashmap *map, char *key, void *callback,
		enum device_state min_state)
{
	char *k;
	struct cmd_struct *cs;

	k = xstrdup(key);
	if (hashmapGet(map, k)) {
		pr_error("key collision '%s'\n", k);
		free(k);
		return -1;
	}
	cs = xmalloc(sizeof(*cs));
	cs->callback = callback;
	cs->min_state = min_state;

	hashmapPut(map, k, cs);
	pr_verbose("Registered plugin function %p (%s) with table %p\n",
			callback, k, map);
	return 0;
}

int aboot_register_flash_cmd(char *key, flash_func callback, enum device_state min_state)
{
	int ret;
	ret = aboot_register_cmd(flash_cmds, key, callback, min_state);
	return ret;
}

int aboot_register_oem_cmd(char *key, oem_func callback, enum device_state min_state)
{
	return aboot_register_cmd(oem_cmds, key, callback, min_state);
}


static int set_keystore_data(void *data, unsigned sz)
{
	int ret;
	efi_guid_t fastboot_guid = FASTBOOT_GUID;

	if (sz) {
		struct keystore *ks = get_keystore(data, sz);

		if (!ks) {
			pr_error("keystore data invalid\n");
			return -1;
		}
		dump_keystore(ks);
		free_keystore(ks);
	}

	ret = efi_set_variable(fastboot_guid, KEYSTORE_VAR,
			data, sz,
			EFI_VARIABLE_NON_VOLATILE |
			EFI_VARIABLE_RUNTIME_ACCESS |
			EFI_VARIABLE_BOOTSERVICE_ACCESS);
	if (ret) {
		pr_error("Coudn't modify KeyStore\n");
		return -1;
	}
	return 0;
}


/* Erase a named partition by creating a new empty partition on top of
 * its device node. No parameters. */
static void cmd_erase(char *part_name, int fd, void *data, unsigned sz)
{
	struct fstab_rec *vol;
	enum device_state current_state;

	current_state = get_device_state();
	if (current_state == LOCKED) {
		fastboot_fail("bootloader must not be locked");
		return;
	}

	if (current_state == VERIFIED &&
			!hashmapContainsKey(erase_whitelist, part_name)) {
		fastboot_fail("can't erase this in 'verified' state");
		return;
	}

	if (!strcmp(part_name, "keystore")) {
		if (set_keystore_data(NULL, 0))
			fastboot_fail("couldn't erase keystore");
		else
			fastboot_okay("");
		return;
	}

	vol = volume_for_name(part_name);
	if (vol == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	pr_status("Erasing %s, this can take a while...\n", part_name);
	if (erase_partition(vol))
		fastboot_fail("Can't erase partition");
	else
		fastboot_okay("");

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
 */
static void cmd_flash(char *targetspec, int fd, void *data, unsigned sz)
{
	struct flash_target tgt;
	flash_func cb;
	struct cmd_struct *cs;
	int ret;
        struct fstab_rec *vol;
	uint64_t vsize;
	uint32_t magic = 0;
	enum device_state current_state;

	process_target(targetspec, &tgt);

	current_state = get_device_state();

	pr_verbose("data size %u\n", sz);
	pr_status("Flashing %s\n", targetspec);

	if ( (cs = hashmapGet(flash_cmds, tgt.name)) ) {
		int cbret;

		/* Use our table of flash functions registered by platform
		 * specific plugin libraries */
		if (current_state < cs->min_state) {
			fastboot_fail("command not allowed in this device state");
			goto out;
		}

		cb = (flash_func)cs->callback;

		cbret = cb(tgt.params, fd, data, sz);
		if (cbret) {
			pr_error("%s flash failed!\n", tgt.name);
			fastboot_fail("%s", tgt.name);
		} else
			fastboot_okay("");
		goto out;
	}

	if (current_state == LOCKED) {
		fastboot_fail("Bootloader must not be locked");
		goto out;
	}

	if (current_state == VERIFIED &&
			!hashmapContainsKey(flash_whitelist, tgt.name)) {
		fastboot_fail("can't flash this partition in VERIFIED state");
		goto out;
	}

	vol = volume_for_name(tgt.name);
	if (!vol) {
		fastboot_fail("%s", tgt.name);
		goto out;
	}

	if (!is_valid_blkdev(vol->blk_device)) {
		fastboot_fail("invalid destination node. partition disks?");
		goto out;
	}
	if (get_volume_size(vol, &vsize)) {
		fastboot_fail("couldn't get volume size");
		goto out;
	}

	if (!strcmp(targetspec, "fastboot") ||
	    !strcmp(targetspec, "recovery") ||
	    !strcmp(targetspec, "boot")) {
		if (bootimage_sanity_checks(data, sz)) {
			fastboot_fail("malformed AOSP boot image, refusing to flash!");
			goto out;
		}
	}

	if (!strcmp(targetspec, "bootloader")) {
		if (esp_sanity_checks(FASTBOOT_DOWNLOAD_TMP_FILE)) {
			fastboot_fail("malformed bootloader image");
			goto out;
		}
	}

	pr_debug("target '%s' volume size: %" PRIu64 " MiB\n", targetspec, vsize >> 20);

	if (sz >= sizeof(magic))
		memcpy(&magic, data, sizeof(magic));

	if (magic == SPARSE_HEADER_MAGIC) {
		/* If there is enough data to hold the header,
		 * and MAGIC appears in header,
		 * then it is a sparse ext4 image */
		struct sparse_header *sh = (struct sparse_header *)data;
		uint64_t totalsize = (uint64_t)sh->blk_sz * (uint64_t)sh->total_blks;
		pr_debug("Detected sparse header, total size %" PRIu64 " MiB\n",
				totalsize >> 20);
		if (totalsize > vsize) {
			pr_error("need %" PRIu64 " bytes, have %" PRIu64 " available\n",
					totalsize, vsize);
			fastboot_fail("target partition too small!");
			goto out;
		}
		ret = named_file_write_ext4_sparse(vol->blk_device, FASTBOOT_DOWNLOAD_TMP_FILE);
	} else {
		if (sz > vsize) {
			pr_error("need %d, %" PRIu64 " available\n",
					sz, vsize);
			fastboot_fail("target partition too small!");
			goto out;
		}
		pr_debug("Writing %u MiB to %s\n", sz >> 20, vol->blk_device);
		ret = named_file_write(vol->blk_device, data, sz, 0, 0);
	}
	pr_verbose("Done writing image\n");
	if (ret) {
		fastboot_fail("Can't write data to target device");
		goto out;
	}
	sync();

	pr_debug("wrote %u bytes to %s\n", sz, vol->blk_device);

	fastboot_okay("");
out:
	hashmapFree(tgt.params);
}

static int parse_state_cmd(char *cmd, enum device_state *state, bool *confirm)
{
	if (!strcmp(cmd, CMD_UNLOCK)) {
		*confirm = true;
		*state = UNLOCKED;
		return 0;
	}

	if (!strcmp(cmd, CMD_LOCK)) {
		*confirm = true;
		*state = LOCKED;
		return 0;
	}

	if (!strcmp(cmd, CMD_VERIFIED)) {
		*confirm = true;
		*state = VERIFIED;
		return 0;
	}

	if (!strcmp(cmd, CMD_UNLOCK_NC)) {
		*confirm = false;
		*state = UNLOCKED;
		return 0;
	}

	if (!strcmp(cmd, CMD_LOCK_NC)) {
		*confirm = false;
		*state = LOCKED;
		return 0;
	}

	if (!strcmp(cmd, CMD_VERIFIED_NC)) {
		*confirm = false;
		*state = VERIFIED;
		return 0;
	}

	return -1;
}


static void cmd_oem(char *arg, int fd, void *data, unsigned sz)
{
	char *command, *saveptr, *str1;
	char *argv[MAX_OEM_ARGS];
	int argc = 0;
	enum device_state device_state, new_state;
	bool confirm;
	struct cmd_struct *cs;
	int ret;

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

	/* Check if user sent one of various state transition commands */
	if (!parse_state_cmd(argv[0], &new_state, &confirm)) {
		if (set_device_state(new_state, !confirm))
			fastboot_fail("couldn't change state");
		else
			fastboot_okay("");
		goto out;
	}

	cs = hashmapGet(oem_cmds, argv[0]);
	if (!cs) {
		fastboot_fail("unknown OEM command");
		goto out;
	}

	device_state = get_device_state();
	if (device_state < cs->min_state) {
		fastboot_fail("command not allowed in this device state");
		goto out;
	}

	ret = ((oem_func)cs->callback)(argc, argv);
	if (ret) {
		pr_error("oem %s command failed, retval = %d\n",
				argv[0], ret);
		fastboot_fail("%s", argv[0]);
	} else {
		fastboot_okay("");
	}
out:
	if (command)
		free(command);
	return;
}




static void cmd_boot(char *arg, int fd, void *data, unsigned sz)
{
	if (get_device_state() != UNLOCKED) {
		fastboot_fail("bootloader must be unlocked");
		return;
	}

	pr_status("Preparing boot image");
	if (copy_bootloader_file("bootonce.img", data, sz)) {
		fastboot_fail("couldn't stage boot image");
		return;
	}

	if (update_bcb("bootonce-\\bootonce.img")) {
		fastboot_fail("couldn't update bootloader control block");
		return;
	}

	pr_info("Booting into supplied image...\n");
	fastboot_okay("");
	close_iofds();
	android_reboot(ANDROID_RB_RESTART, 0, 0);
	pr_error("Reboot failed\n");
}


static int cmd_flash_efirun(Hashmap *params, int fs, void *data, unsigned sz)
{
	pr_status("Preparing EFI binary");

	if (copy_bootloader_file("bootonce.efi", data, sz)) {
		pr_error("couldn't stage efi binary");
		return -1;
	}

	if (update_bcb("bootonce-\\bootonce.efi")) {
		pr_error("couldn't update bootloader control block");
		return -1;
	}

	pr_info("Running EFI program...\n");
	fastboot_okay("");
	close_iofds();
	android_reboot(ANDROID_RB_RESTART2, 0, "bootloader");
	pr_error("Reboot failed\n");
	return -1;
}


static int cmd_flash_sfu(Hashmap *params, int fd, void *data, unsigned sz)
{
	pr_status("Preparing capsule update");
	if (copy_bootloader_file("BIOSUPDATE.fv", data, sz)) {
		pr_error("couldn't stage capsule");
		return -1;
	}

	fastboot_info("SFU capsule will be applied on next reboot");
	return 0;
}

static bool parse_oemvar_guid_line(char *line, efi_guid_t *g)
{
	unsigned int a, b, c, d, e[6];
	if (sscanf(line, " GUID = %8x-%4x-%4x-%4x-%2x%2x%2x%2x%2x%2x ",
		  &a, &b, &c, &d, &e[0], &e[1], &e[2], &e[3], &e[4], &e[5])
	    == 10)
	{
		g->a = a; g->b = b;
		g->c = c; g->d = (d >> 8) | (d << 8);
		g->e[0] = e[0]; g->e[1] = e[1];
		g->e[2] = e[2]; g->e[3] = e[3];
		g->e[4] = e[4]; g->e[5] = e[5];
		return true;
	}
	return false;
}

/* Implements modify-in-place "URL-like" escaping: "%[0-9a-fA-F]{2}"
 * converts to the specified byte; no other modifications are
 * performed (including "+" for space!).  Returns the number of output
 * bytes */
static size_t unescape_oemvar_val(char *val)
{
	char *p = val, *out = val;
	unsigned int byte;
	while (*p) {
		if (p[0] != '%') {
			*out++ = *p++;
			continue;
		}

		if (sscanf(p, "%%%2x", &byte) == 1) {
			*out++ = byte;
			p += 3;
		} else {
			*out++ = *p++;
		}
	}
	return out - val;
}

static int cmd_flash_oemvars(Hashmap *params, int fd, void *data, unsigned sz)
{
	int ret = -1;
	char *buf, *line, *eol, *var, *val, *p;
	size_t vallen;
	efi_guid_t curr_guid = FASTBOOT_GUID;

	pr_info("Parsing and setting values from oemvars file\n");

	/* extra byte so we can always terminate the last line */
	buf = malloc(sz+1);
	if (!buf)
		return ret;
	if (robust_read(fd, buf, sz, false) != (ssize_t)sz) {
		free(buf);
		return ret;
	}
	buf[sz] = 0;

	for (line = buf; line - buf < (ssize_t)sz; line = eol+1) {
		/* Detect line and terminate */
		eol = strchr(line, '\n');
		if (!eol)
			eol = line + strlen(line);
		*eol = 0;

		/* Snip comments */
		if ((p = strchr(line, '#')))
			*p = 0;

		/* Snip trailing whitespace for sanity */
		p = line + strlen(line);
		while (p > line && isspace(*(p-1)))
			*(--p) = 0;

		/* GUID line syntax */
		if (parse_oemvar_guid_line(line, &curr_guid))
			continue;

		/* Variable definition? */
		while (*line && isspace(*line)) line++;
		var = line;
		val = NULL;
		while (*line && !isspace(*line)) line++;
		if (*line) {
			*line++ = 0;
			while (*line && isspace(*line)) line++;
			val = line;
		}
		if (*var && val && *val) {
			vallen = unescape_oemvar_val(val);
			pr_info("Setting oemvar: %s\n", var);
			ret = efi_set_variable(curr_guid, var,
					       (uint8_t *)val, vallen,
					       EFI_VARIABLE_NON_VOLATILE |
					       EFI_VARIABLE_RUNTIME_ACCESS |
					       EFI_VARIABLE_BOOTSERVICE_ACCESS);
		}
	}

	free(buf);
	return ret;
}

static int cmd_flash_keystore(Hashmap *params, int fd, void *data,
		unsigned sz)
{
	return set_keystore_data(data, sz);
}


static void cmd_reboot(char *arg, int fd, void *data, unsigned sz)
{
	fastboot_okay("");
	sync();
	close_iofds();
	pr_info("Rebooting!\n");
	android_reboot(ANDROID_RB_RESTART, 0, 0);
	pr_error("Reboot failed\n");
}


static void cmd_reboot_bl(char *arg, int fd, void *data, unsigned sz)
{
	fastboot_okay("");
	sync();
	close_iofds();
	pr_info("Restarting UserFastBoot...\n");
	android_reboot(ANDROID_RB_RESTART2, 0, "bootloader");
	pr_error("Reboot failed\n");
}


static int start_adbd(int argc, char **argv)
{
	return system("adbd &");
}


#define CHUNK	1024LL * 1024LL
static int garbage_disk(int argc, char **argv)
{
	char disk_path[PATH_MAX];
	char *disk_name = NULL;
	int ifd = -1;
	int ofd = -1;
	char *buf = NULL;
	int64_t remaining_disk, disk_size;
	int ret = -1;

	if (argc == 2)
		disk_name = xstrdup(argv[1]);
	else
		disk_name = get_primary_disk_name();

	if (!disk_name)
		goto out;

	snprintf(disk_path, sizeof(disk_path), "/dev/block/%s", disk_name);

	ofd = open(disk_path, O_WRONLY);
	if (ofd < 0) {
		pr_perror("open");
		pr_error("open %s node\n", disk_path);
		goto out;
	}

	remaining_disk = disk_size = get_disk_size(disk_name);

	pr_status("Trashing %s contents...this can take a while", disk_name);

	/* Get a big blob of pseudo-random data to write over and over again */
	buf = xmalloc(CHUNK);
	ifd = open("/dev/urandom", O_RDONLY);
	if (ifd < 0) {
		pr_perror("open /dev/urandom");
		goto out;
	}

	if (robust_read(ifd, buf, CHUNK, false) != CHUNK) {
		pr_error("couldn't read /dev/urandom\n");
		goto out;
	}

	mui_show_progress(1.0, 0);
	while (remaining_disk) {
		ssize_t written, to_write;

		mui_set_progress((float)(disk_size - remaining_disk) / (float)disk_size);

		to_write = (CHUNK > disk_size) ? disk_size : CHUNK;
		written = robust_write(ofd, buf, to_write);
		if (written < 0) {
			pr_error("couldn't write to the disk\n");
			goto out;
		}
		remaining_disk -= written;
	}
	ret = 0;
out:
	if (ifd >= 0)
		close(ifd);
	if (ofd >= 0)
		close(ofd);
	mui_reset_progress();
	free(disk_name);
	free(buf);

	return ret;
}

static int set_efi_var(int argc, char **argv)
{
	int ret;
	efi_guid_t fastboot_guid = FASTBOOT_GUID;
	size_t datalen;
	uint16_t *data, *d_pos;
	char *o_pos;

	if (argc != 3) {
		pr_error("incorrect number of parameters");
		return -1;
	}

	if (strlen(argv[1]) > 128) {
		pr_error("pathologically long variable name");
		return -1;
	}

	/* up-convert the data to a 16-bit string as that is what EFI generally uses */
	datalen = (strlen(argv[2]) + 1) * 2;
	if (datalen > 256) { // value is arbitray but should be more than enough
		pr_error("pathologically long data string");
		return -1;
	}
	data = xmalloc(datalen);
	d_pos = data;
	o_pos = argv[2];
	while (*o_pos)
		*d_pos++ = *o_pos++;
	*d_pos = 0;

	pr_debug("Setting '%s' to value '%s'\n", argv[1], argv[2]);
	ret = efi_set_variable(fastboot_guid, argv[1],
			(uint8_t *)data, datalen,
			EFI_VARIABLE_NON_VOLATILE |
			EFI_VARIABLE_RUNTIME_ACCESS |
			EFI_VARIABLE_BOOTSERVICE_ACCESS);
	free(data);
	if (ret)
		pr_error("Couldn't set '%s' EFI variable to '%s'\n",
				argv[1], argv[2]);
	return ret;
}


static int oem_reboot_cmd(int argc, char **argv)
{
	if (argc != 2) {
		pr_error("incorrect number of parameters");
		return -1;
	}

	pr_info("Rebooting into %s...\n", argv[1]);
	fastboot_okay("");
	close_iofds();
	android_reboot(ANDROID_RB_RESTART2, 0, argv[1]);
	/* Shouldn't get here */
	return -1;
}


static int oem_hidetext(int argc, char **argv)
{
	mui_set_background(BACKGROUND_ICON_INSTALLING);
	mui_show_text(0);
	return 0;
}


static int oem_showtext(int argc, char **argv)
{
	mui_show_text(1);
	return 0;
}


static void publish_from_prop(char *key, char *prop, char *dfl)
{
	char val[PROPERTY_VALUE_MAX];
	if (property_get(prop, val, dfl))
		fastboot_publish(key, xstrdup(val));
}


void populate_status_info(void)
{
	char *interface_info;
	char *infostring;

	pr_debug("updating status text\n");
	interface_info = get_network_interface_status();

	infostring = xasprintf("Userfastboot for %s\n \n"
		     "      bootloader: %s\n"
		     "          kernel: %s\n"
		     "        firmware: %s\n"
		     "           board: %s\n"
		     "        serialno: %s\n"
		     "    device state: %s\n"
		     "UEFI secure boot: %s\n"
		     "      boot state: %s\n"
		     " \n%s",
		     fastboot_getvar("product"),
		     fastboot_getvar("version-bootloader"),
		     fastboot_getvar("kernel"),
		     fastboot_getvar("firmware"),
		     fastboot_getvar("board"),
		     fastboot_getvar("serialno"),
		     fastboot_getvar("device-state"),
		     fastboot_getvar("secureboot"),
		     fastboot_getvar("boot-state"),
		     interface_info);
	pr_debug("%s", infostring);
	mui_infotext(infostring);
	free(infostring);
}


static Hashmap *init_hashmap_list(char ** init_list)
{
	char ** pos;
	Hashmap *m;

	m = hashmapCreate(8, strhash, strcompare);
	if (!m)
		return NULL;

	for (pos = init_list; *pos; pos++)
		hashmapPut(m, *pos, NULL);

	return m;
}


static char *get_loader_version(void)
{
	int ret;
	efi_guid_t loader_guid = LOADER_GUID;
	uint16_t *data16, *pos16;
	char *data, *pos, *version;
	size_t dsize;
	uint32_t attributes;

	ret = efi_get_variable(loader_guid, LOADER_VERSION_VAR, (uint8_t **)&data16,
			&dsize, &attributes);
	if (ret || !dsize || dsize % 2 != 0)
		return xstrdup("unknown+userfastboot-" USERFASTBOOT_VERSION);

	/* poor-man's 8 bit char conversion */
	data = xmalloc((dsize / 2) + 1);
	pos = data;
	pos16 = data16;
	while (pos16 < (data16 + (dsize / 2))) {
		*pos = (char)*pos16;
		if (!*pos16)
			break;
		pos++;
		pos16++;
	}
	data[dsize / 2] = '\0';

	version = xasprintf("%s+userfastboot-%s", data, USERFASTBOOT_VERSION);
	free(data);
	free(data16);
	return version;
}


void aboot_register_commands(void)
{
	char *bios_vendor, *bios_version, *bios_string;
	char *board_vendor, *board_version, *board_name, *board_string;
	struct utsname uts;

	fastboot_register("oem", cmd_oem);
	fastboot_register("reboot", cmd_reboot);
	fastboot_register("reboot-bootloader", cmd_reboot_bl);
	fastboot_register("continue", cmd_reboot);

	fastboot_publish("product", xstrdup(DEVICE_NAME));
	fastboot_publish("product-name", get_dmi_data("product_name"));
	fastboot_publish("version-bootloader", get_loader_version());
	fastboot_publish("version-baseband", xstrdup("N/A"));
	publish_from_prop("serialno", "ro.serialno", "unknown");

	flash_cmds = hashmapCreate(8, strhash, strcompare);
	oem_cmds = hashmapCreate(8, strhash, strcompare);
	flash_whitelist = init_hashmap_list(default_flash_whitelist);
	erase_whitelist = init_hashmap_list(default_erase_whitelist);
	if (!flash_cmds || !oem_cmds || !flash_whitelist || !erase_whitelist) {
		pr_error("Memory allocation error\n");
		die();
	}
	publish_all_part_data(false);

	/* Currently we don't require signatures on images
	 * XXX need to reconcile this with Verifiedbootflow.pdf */
	fastboot_publish("secure", xstrdup("no"));

	fastboot_publish("secureboot", xstrdup(is_secure_boot_enabled() ? "yes" : "no"));

	bios_vendor = get_dmi_data("bios_vendor");
	bios_version = get_dmi_data("bios_version");
	bios_string = xasprintf("%s %s", bios_vendor, bios_version);
	fastboot_publish("firmware", bios_string);
	free(bios_vendor);
	free(bios_version);

	board_vendor = get_dmi_data("board_vendor");
	board_version = get_dmi_data("board_version");
	board_name = get_dmi_data("board_name");
	board_string = xasprintf("%s %s %s", board_vendor, board_name, board_version);
	fastboot_publish("board", board_string);
	free(board_vendor);
	free(board_version);
	free(board_name);

	if (!uname(&uts))
		fastboot_publish("kernel", xasprintf("%s %s %s",
				uts.release, uts.version, uts.machine));
	else
		fastboot_publish("kernel", xstrdup("unknown"));


	/* At this time we don't have a special 'charge mode',
	 * which is entered when power is applied.
	 * if later we do, we need to implement a
	 * 'fastboot oem off-mode-charge 0' which bypasses
	 * charge mode and boots the device normally as
	 * if the user pressed the power button */
	fastboot_publish("off-mode-charge", xstrdup("0"));

	fastboot_register("boot", cmd_boot);
	fastboot_register("erase:", cmd_erase);
	fastboot_register("flash:", cmd_flash);

	aboot_register_flash_cmd("gpt", cmd_flash_gpt, UNLOCKED);
	aboot_register_flash_cmd("mbr", cmd_flash_mbr, UNLOCKED);
	aboot_register_flash_cmd("sfu", cmd_flash_sfu, VERIFIED);
	aboot_register_flash_cmd("oemvars", cmd_flash_oemvars, UNLOCKED);
	aboot_register_flash_cmd("keystore", cmd_flash_keystore, UNLOCKED);
	aboot_register_flash_cmd("efirun", cmd_flash_efirun, UNLOCKED);

	aboot_register_oem_cmd("adbd", start_adbd, UNLOCKED);
	aboot_register_oem_cmd("garbage-disk", garbage_disk, UNLOCKED);
	aboot_register_oem_cmd("setvar", set_efi_var, UNLOCKED);
	aboot_register_oem_cmd("reboot", oem_reboot_cmd, LOCKED);
	aboot_register_oem_cmd("showtext", oem_showtext, LOCKED);
	aboot_register_oem_cmd("hidetext", oem_hidetext, LOCKED);

	register_userfastboot_plugins();

	fetch_boot_state();
	update_device_state_metadata();
}

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */

