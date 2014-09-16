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


#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mount.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <fcntl.h>
#include <stdio.h>
#include <ftw.h>

#include <bootimg.h>
#include <ext4_utils.h>

#include "hashes.h"
#include "userfastboot_util.h"
#include "userfastboot_ui.h"
#include "ext4.h"
#include "keystore.h"

#define BOOT_SIGNATURE_MAX_SIZE  2048

static void report_hash(const char *name, unsigned char *hash)
{
	char *hashstr = xmalloc(SHA_DIGEST_LENGTH * 2 + 1);
	char *pos;
	int i;

	for (i = 0, pos = hashstr; i < SHA_DIGEST_LENGTH; i++, pos += 2)
		snprintf(pos, 3, "%02x", hash[i]);
	fastboot_info("target: /%s", name);
	fastboot_info("hash: %s", hashstr);
	free(hashstr);
}

static int open_partition(const char *ptn)
{
	struct fstab_rec *vol;
	int fd;

	vol = volume_for_name(ptn);
	if (!vol) {
		pr_error("volume %s not found\n", ptn);
		return -1;
	}

	fd = open(vol->blk_device, O_RDONLY);
	if (fd < 0)
		pr_perror("open");
	return fd;
}

#define CHUNK 1024 * 1024

static int hash_fd(int fd, uint64_t len, unsigned char *hash)
{
	unsigned char *blob;
	ssize_t chunklen;
	SHA_CTX sha_ctx;
	int ret = -1;
	uint64_t orig_len = len;

	blob = xmalloc(CHUNK);
	mui_show_progress(1.0, 0);

	if (lseek64(fd, 0, SEEK_SET) < 0) {
		pr_perror("lseek64");
		goto out;
	}

	SHA1_Init(&sha_ctx);

	while (len) {
		mui_set_progress((float)(orig_len - len)/(float)orig_len);
		chunklen = read(fd, blob, min(CHUNK, len));
		if (chunklen < 0) {
			pr_perror("read");
			goto out;
		}
		if (chunklen == 0)
			break;
		SHA1_Update(&sha_ctx, blob, chunklen);
		len -= chunklen;
	}
	if (len) {
		pr_error("short read? remaining %llu\n", len);
                goto out;
        }

	SHA1_Final(hash, &sha_ctx);
	ret = 0;
out:
	mui_reset_progress();

	free(blob);
	return ret;
}

static size_t pagealign(struct boot_img_hdr *hdr, uint32_t blob_size)
{
        uint32_t page_mask = hdr->page_size - 1;
        return (blob_size + page_mask) & (~page_mask);
}


static size_t unsigned_bootimage_size(struct boot_img_hdr *aosp_header)
{
        size_t size;

        size = pagealign(aosp_header, aosp_header->kernel_size) +
               pagealign(aosp_header, aosp_header->ramdisk_size) +
               pagealign(aosp_header, aosp_header->second_size) +
               aosp_header->page_size;

        return size;
}


static ssize_t get_bootimage_len(int fd)
{
	struct boot_img_hdr hdr;
	ssize_t len;
	unsigned char sigbuf[BOOT_SIGNATURE_MAX_SIZE];
	struct boot_signature *bs;

	if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		pr_perror("read fd");
		return -1;
	}

        if (strncmp(BOOT_MAGIC, (char *)hdr.magic, BOOT_MAGIC_SIZE)) {
		pr_error("bad boot magic\n");
                return -1;
	}

	len = unsigned_bootimage_size(&hdr);
	if (len < 0) {
		pr_error("couldn't compute boot image size\n");
		return -1;
	}

	if (lseek64(fd, len, SEEK_SET) < 0) {
		pr_perror("lseek64");
		return -1;
	}

	/* Now try to parse the boot image signature */
	if (read(fd, sigbuf, BOOT_SIGNATURE_MAX_SIZE) != BOOT_SIGNATURE_MAX_SIZE) {
		pr_perror("read sig data");
		return -1;
	}

	bs = get_boot_signature(sigbuf, BOOT_SIGNATURE_MAX_SIZE);
	if (bs) {
		len += bs->total_size;
		free_boot_signature(bs);
	} else {
		pr_debug("boot image doesn't seem to have a signture\n");
	}

	pr_debug("total boot image size %zd\n", len);
	return len;
}


static int ftw_callback(const char *fpath, const struct stat *sb, int typeflag)
{
	int fd;
	unsigned char hash[SHA_DIGEST_LENGTH];

	if (typeflag != FTW_F)
		return 0;

	fd = open(fpath, O_RDONLY);
	if (fd < 0)
		return 0;

	if (!hash_fd(fd, sb->st_size, hash))
		report_hash(fpath + 5, hash);
	close(fd);
	return 0;
}


int get_fat_file_hashes(const char *ptn)
{
	struct fstab_rec *vol;
	int fd;
	bool mounted = false;
	int ret = -1;

	pr_status("Hashing files under /%s\n", ptn);

	vol = volume_for_name(ptn);
	if (!vol) {
		pr_error("volume %s not found\n", ptn);
		goto out;
	}

	if (mount_partition(vol, true))
		goto out;
	mounted = true;

	ftw("/mnt/bootloader", ftw_callback, 8);
	ret = 0;
out:
	if (mounted)
		unmount_partition(vol);
	return ret;
}

int get_boot_image_hash(const char *ptn)
{
	char *device;
	struct fstab_rec *vol;
	int fd = -1;
	int ret = -1;
	int64_t len;
	unsigned char hash[SHA_DIGEST_LENGTH];

	pr_status("Hashing boot image /%s\n", ptn);

	fd = open_partition(ptn);
	if (fd < 0)
		goto out;

	len = get_bootimage_len(fd);
	if (len < 0)
		goto out;

	if (hash_fd(fd, len, hash))
		goto out;

	report_hash(ptn, hash);
	ret = 0;
out:
	if (fd >= 0)
		close(fd);
	return ret;
}

/* From system/core/fs_mgr/fs_mgr_verity.c */
#define VERITY_METADATA_SIZE 32768
#define VERITY_METADATA_MAGIC_NUMBER 0xb001b001

/* system/core/include/mincrypt/rsa.h, conflicts with OpenSSL
 * headers else I'd just include it */
#define RSANUMBYTES 256

/* taken from build_verity_tree.cpp */
#define div_round_up(x,y) (((x) + (y) - 1)/(y))

/* taken from build_verity_tree.cpp */
size_t verity_tree_blocks(uint64_t data_size, size_t block_size, size_t hash_size,
                          int level)
{
	size_t level_blocks = div_round_up(data_size, block_size);
	int hashes_per_block = div_round_up(block_size, hash_size);

	do {
		level_blocks = div_round_up(level_blocks, hashes_per_block);
	} while (level--);

	return level_blocks;
}

/* adapted from build_verity_tree.cpp */
uint64_t verity_tree_size(uint64_t data_size)
{
	uint64_t verity_blocks = 0;
	size_t level_blocks;
	int levels = 0;
	const EVP_MD *md = EVP_sha256();
	size_t hash_size = EVP_MD_size(md);
	size_t block_size = 4096;
	uint64_t tree_size;

	do {
		level_blocks = verity_tree_blocks(data_size, block_size,
				hash_size, levels);
		levels++;
		verity_blocks += level_blocks;
	} while (level_blocks > 1);

	tree_size = verity_blocks * block_size;
	pr_debug("verity tree size %llu\n", tree_size);
	return tree_size;
}


int get_ext_image_hash(const char *ptn)
{
	int fd = -1;
	int ret = -1;
	uint64_t len;
	unsigned char hash[SHA_DIGEST_LENGTH];
	unsigned int magic_number;
	int protocol_version;

	pr_status("Hashing ext image /%s\n", ptn);

	fd = open_partition(ptn);
	if (fd < 0)
		goto out;

	if (read_ext(fd, 1)) {
		pr_error("ext image corrupted\n");
		goto out;
	}

	/* info is a libext4_utils global populated by read_ext() */
	len = info.len;

	if (lseek64(fd, len, SEEK_SET) < 0) {
		pr_perror("lseek64");
		goto out;
	}

	if (read(fd, &magic_number, sizeof(magic_number)) !=
			(ssize_t)sizeof(magic_number)) {
		pr_error("can't read verity magic\n");
		goto out;
	}

	if (magic_number != VERITY_METADATA_MAGIC_NUMBER) {
		pr_error("verity magic not found\n");
		goto out;
	}

	if (read(fd, &protocol_version, sizeof(protocol_version)) !=
			(ssize_t)sizeof(protocol_version)) {
		pr_debug("can't read protocol version\n");
		goto out;
	}

	if (protocol_version != 0) {
		pr_error("Unsupported verity protocol version %d\n",
				protocol_version);
		goto out;
	}

	len += verity_tree_size(len) + VERITY_METADATA_SIZE;

	pr_debug("%s filesystem size %lld\n", ptn, len);
	if (hash_fd(fd, len, hash))
		goto out;

	report_hash(ptn, hash);
	ret = 0;
out:
	if (fd >= 0)
		close(fd);
	return ret;
}

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */

