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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <linux/fs.h>
#include <inttypes.h>
#include <linux/loop.h>

#include <cutils/android_reboot.h>
#include <bootloader.h>

#include <sparse/sparse.h>
#include "sparse_file.h"
#include "output_file.h"
#include "backed_block.h"
#include "sparse_defs.h"
#include "sparse_format.h"

#include "fastboot.h"
#include "userfastboot.h"
#include "userfastboot_ui.h"
#include "userfastboot_util.h"
#include "userfastboot_fstab.h"

/* make_ext4fs.h can't be included along with linux/ext3_fs.h.
 * This is the only item needed out of the former. */
extern int make_ext4fs(const char *filename, int64_t len,
		const char *mountpoint, struct selabel_handle *sehnd);

void die(void)
{
	pr_error("userfastboot has encountered an unrecoverable problem, exiting!\n");
	mui_set_background(BACKGROUND_ICON_ERROR);
	mui_show_text(1);
	exit(1);
}


void die_errno(const char *s)
{
	pr_perror(s);
	die();
}


void *xmalloc(size_t size)
{
	void *ret = malloc(size);
	if (!ret) {
		pr_error("allocation size: %zd\n", size);
		die_errno("malloc");
	}
	return ret;
}


char *xstrdup(const char *s)
{
	char *ret = strdup(s);
	if (!ret)
		die_errno("strdup");
	return ret;
}


char *xasprintf(const char *fmt, ...)
{
	va_list ap;
	int ret;
	char *out;

	va_start(ap, fmt);
	ret = vasprintf(&out, fmt, ap);
	va_end(ap);

	if (ret < 0)
		die_errno("asprintf");
	return out;
}


void xstring_append_line(char **str, const char *fmt, ...)
{
	va_list ap;
	int ret;
	char *out, *newstr;

	va_start(ap, fmt);
	ret = vasprintf(&out, fmt, ap);
	va_end(ap);

	if (ret < 0)
		die_errno("asprintf");

	if (*str) {
		newstr = xasprintf("%s\n%s", *str, out);
		free(*str);
		free(out);
		*str = newstr;
	} else {
		*str = out;
	}
}


ssize_t robust_write(int fd, const void *buf, size_t count)
{
	const char *pos = buf;
	ssize_t total_written = 0;

	while (count) {
		ssize_t written = write(fd, pos, count);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		count -= written;
		pos += written;
		total_written += written;
	}
	return total_written;
}


static void sparse_file_write_block(struct output_file *out,
		struct backed_block *bb)
{
	switch (backed_block_type(bb)) {
	case BACKED_BLOCK_DATA:
		write_data_chunk(out, backed_block_len(bb), backed_block_data(bb));
		break;
	case BACKED_BLOCK_FILE:
		write_file_chunk(out, backed_block_len(bb),
				backed_block_filename(bb), backed_block_file_offset(bb));
		break;
	case BACKED_BLOCK_FD:
		write_fd_chunk(out, backed_block_len(bb),
				backed_block_fd(bb), backed_block_file_offset(bb));
		break;
	case BACKED_BLOCK_FILL:
		write_fill_chunk(out, backed_block_len(bb),
				backed_block_fill_val(bb));
		break;
	}
}

static unsigned int sparse_count_chunks(struct sparse_file *s)
{
	struct backed_block *bb;
	unsigned int last_block = 0;
	unsigned int chunks = 0;

	for (bb = backed_block_iter_new(s->backed_block_list); bb;
			bb = backed_block_iter_next(bb)) {
		if (backed_block_block(bb) > last_block) {
			/* If there is a gap between chunks, add a skip chunk */
			chunks++;
		}
		chunks++;
		last_block = backed_block_block(bb) +
				DIV_ROUND_UP(backed_block_len(bb), s->block_size);
	}
	if (last_block < DIV_ROUND_UP(s->len, s->block_size)) {
		chunks++;
	}

	return chunks;
}

static int write_all_blocks(struct sparse_file *s, struct output_file *out)
{
	struct backed_block *bb;
	unsigned int last_block = 0;
	int64_t pad;
	unsigned int total_blocks = 0;
	unsigned int count = 0;

	for (bb = backed_block_iter_new(s->backed_block_list); bb;
			bb = backed_block_iter_next(bb))
		total_blocks++;

	mui_show_progress(1.0, 0);

	for (bb = backed_block_iter_new(s->backed_block_list); bb;
			bb = backed_block_iter_next(bb)) {
		mui_set_progress((float)count / (float)total_blocks);
		if (backed_block_block(bb) > last_block) {
			unsigned int blocks = backed_block_block(bb) - last_block;
			write_skip_chunk(out, (int64_t)blocks * s->block_size);
		}
		sparse_file_write_block(out, bb);
		last_block = backed_block_block(bb) +
				DIV_ROUND_UP(backed_block_len(bb), s->block_size);
		count++;
	}

	mui_reset_progress();
	pad = s->len - (int64_t)last_block * s->block_size;
	if (pad < 0) {
		return -1;
	}
	if (pad > 0) {
		write_skip_chunk(out, pad);
	}

	return 0;
}

int named_file_write_ext4_sparse(const char *filename, const char *what)
{
	int infd = -1;
	int outfd = -1;
	int ret = -1;
	struct sparse_file *s;
	int chunks;
	struct output_file *out;

	outfd = open(filename, O_WRONLY);
	if (outfd < 0) {
		pr_error("Coudln't open destination file %s\n", filename);
		goto out;
	}
	infd = open(what, O_RDONLY);
	if (infd < 0) {
		pr_error("Couldn't open sparse input file\n");
		goto out;
	}

	pr_verbose("Importing sparse file data\n");
	s = sparse_file_import(infd, true, false);
	if (!s) {
		pr_error("Couldn't import sparse file data\n");
		goto out;
	}

	pr_verbose("Writing sparse file data\n");

	chunks = sparse_count_chunks(s);
	out = output_file_open_fd(outfd, s->block_size, s->len,
			false, false, chunks, false);
	if (!out)
		die_errno("malloc");

	ret = write_all_blocks(s, out);
	output_file_close(out);

	if (ret < 0)
		pr_error("Couldn't write output file");
	else
		ret = 0;

	pr_verbose("Destroying sparse data stucture\n");
	sparse_file_destroy(s);
	fsync(outfd);
out:
	if (infd >= 0)
		close(infd);
	if (outfd >= 0)
		close(outfd);

	return ret;
}


int named_file_write(const char *filename, const unsigned char *what,
		size_t sz, off_t offset, int append)
{
	int fd, ret, flags;
	size_t sz_orig = sz;
	size_t count = 0;

	flags = O_RDWR | (append ? O_APPEND : (O_CREAT | O_TRUNC));
	if (flags & O_CREAT)
		fd = open(filename, flags, 0600);
	else
		fd = open(filename, flags);
	if (fd < 0) {
		pr_error("file_write: Can't open file %s: %s\n",
				filename, strerror(errno));
		return -1;
	}
	if (offset) {
		if (lseek(fd, offset, SEEK_SET) < 0) {
			pr_perror("lseek");
			close(fd);
			return -1;
		}
	}

	mui_show_progress(1.0, 0);
	pr_verbose("write() %zu bytes to %s\n", sz, filename);

	while (sz) {
		mui_set_progress((float)count / (float)sz_orig);

		ret = write(fd, what, min(sz, 1024U * 1024U));
		if (ret < 0) {
			if (errno != EINTR) {
				mui_reset_progress();
				pr_error("file_write: Failed to write to %s: %s\n",
					filename, strerror(errno));
				close(fd);
				return -1;
			} else {
				continue;
			}
		}
		what += ret;
		sz -= ret;
		count += ret;
	}
	fsync(fd);
	close(fd);
	mui_reset_progress();
	return 0;
}

int mount_partition_device(const char *device, const char *type,
		char *mountpoint, bool readonly)
{
	int ret;

	ret = mkdir(mountpoint, 0777);
	if (ret && errno != EEXIST) {
		pr_perror("mkdir");
		return -1;
	}

	pr_debug("Mounting %s (%s) --> %s\n", device,
			type, mountpoint);
	ret = mount(device, mountpoint, type, readonly ? MS_RDONLY : 0, "");
	if (ret && errno != EBUSY) {
		pr_debug("mount: %s (%s): %s\n", device, type, strerror(errno));
		return -1;
	}
	return 0;
}


int mount_loopback(const char *path, const char *type, char *mountpoint)
{
	int ret, i;
	int file_fd = -1;
	int loop_fd = -1;
	struct loop_info info;
	char tmp[PATH_MAX];

	ret = mkdir(mountpoint, 0777);
	if (ret && errno != EEXIST) {
		pr_perror("mkdir");
		goto out_error;
	}

	file_fd = open(path, O_RDONLY);
	if (file_fd < 0) {
		pr_perror("open");
		goto out_error;
	}

	for (i = 0; ; i++) {
		char tmp[PATH_MAX];

		snprintf(tmp, PATH_MAX, "/dev/block/loop%d", i);
		loop_fd = open(tmp, O_RDONLY);
		if (loop_fd < 0) {
			pr_error("Couldn't open a loop device %s\n", tmp);
			pr_perror("open");
			goto out_error;
		}

		if (ioctl(loop_fd, LOOP_GET_STATUS, &info) < 0 && errno == ENXIO)
			break;
	}

	ret = ioctl(loop_fd, LOOP_SET_FD, file_fd);
	if (ret < 0) {
		pr_perror("LOOP_SET_FD");
		goto out_error;
	}

	ret = mount(tmp, mountpoint, type, MS_RDONLY, NULL);
	if (ret < 0) {
		pr_error("loopback mount failed\n");
		pr_perror("mount");
		ioctl(loop_fd, LOOP_CLR_FD, 0);
		goto out_error;
	}

	close(file_fd);
	return loop_fd;;
out_error:
	if (file_fd >= 0)
		close(file_fd);

	if (loop_fd >= 0)
		close(loop_fd);
	return -1;
}

int unmount_loopback(int loop_fd, const char *mountpoint)
{
	int ret;

	if (umount(mountpoint)) {
		pr_perror("umount");
		return -1;
	}

	ret = ioctl(loop_fd, LOOP_CLR_FD, 0);
	if (ret < 0)
		pr_perror("LOOP_CLR_FD");

	close(loop_fd);

	return (ret < 0);
}

int get_volume_size(struct fstab_rec *vol, uint64_t *sz)
{
	int fd;
	int ret = -1;

	if (vol->length > 0) {
		*sz = vol->length;
		return 0;
	}

	fd = open(vol->blk_device, O_RDONLY);
	if (fd < 0) {
		return -1;
	}

	if (ioctl(fd, BLKGETSIZE64, sz) >= 0) {
		ret = 0;
		*sz += vol->length;
	} else {
		pr_perror("BLKGETSIZE64");
	}
	pr_verbose("size is %" PRIu64 "\n", *sz);
	close(fd);
	return ret;
}


int64_t get_disk_size(const char *disk_name)
{
	int64_t disk_sectors, lba_size;

	if (read_sysfs_int64(&disk_sectors, "/sys/block/%s/size", disk_name)) {
		pr_error("couldn't read %s disk size", disk_name);
		return -1;
	}

	if (read_sysfs_int64(&lba_size, "/sys/block/%s/queue/logical_block_size",
			disk_name)) {
		pr_error("couldn't read %s LBA size", disk_name);
		return -1;
	}

	return disk_sectors * lba_size;
}


int mount_partition(struct fstab_rec *vol, bool readonly)
{
	char *mountpoint;
	int status;
	char *fs_type;

	mountpoint = xasprintf("/mnt/%s", vol->mount_point);
	fs_type = vol->fs_type;
	if (!strcmp(fs_type, "emmc"))
		fs_type = "vfat";

	status = mount_partition_device(vol->blk_device, fs_type, mountpoint, readonly);
	free(mountpoint);

	return status;
}

int unmount_partition(struct fstab_rec *vol)
{
	int ret;
	char *mountpoint = NULL;

	mountpoint = xasprintf("/mnt/%s", vol->mount_point);
	ret = umount(mountpoint);
	free(mountpoint);
	return ret;
}

enum erase_type {
	SECDISCARD,
	DISCARD,
	ZERO
};

/* more or less arbitrary value */
#define ZEROES_ARRAY_SZ	4096U

static int erase_range_zero(int fd, uint64_t start, uint64_t len)
{
	char zeroes[ZEROES_ARRAY_SZ];

	memset(zeroes, 0, ZEROES_ARRAY_SZ);

	if (lseek64(fd, start, SEEK_SET) < 0) {
		pr_perror("lseek64");
		return -1;
	}

	while (len) {
		ssize_t ret;

		ret = write(fd, zeroes, min(len, ZEROES_ARRAY_SZ));
		if (ret < 0) {
			pr_perror("write");
			return -1;
		}
		len -= ret;
	}
	return 0;
}

static int erase_range(int fd, uint64_t start, uint64_t len)
{
	uint64_t range[2];
	int ret;
	static enum erase_type etype = SECDISCARD;

	pr_debug("erasing offset %" PRIu64 " len %" PRIu64 "\n", start, len);
	switch (etype) {
	case SECDISCARD:
		range[0] = start;
		range[1] = len;

		ret = ioctl(fd, BLKSECDISCARD, &range);
		if (ret >= 0)
			break;
		pr_info("BLKSECDISCARD didn't work (%s), trying BLKDISCARD\n",
				strerror(errno));
		etype = DISCARD;
		/* fall through */
	case DISCARD:
		range[0] = start;
		range[1] = len;

		ret = ioctl(fd, BLKDISCARD, &range);
		if (ret >= 0)
			break;
		pr_info("BLKDISCARD didn't work (%s), fall back to zeroing out\n",
				strerror(errno));
		pr_info("This can take a LONG time!\n");
		etype = ZERO;
		/* Fall through */
	case ZERO:
		return erase_range_zero(fd, start, len);
	}

	return 0;
}

static char *get_disk_sysfs(char *node)
{
	struct stat sb;
	if (stat(node, &sb)) {
		pr_perror("stat");
		return NULL;
	}

	return xasprintf("/sys/dev/block/%d:0/", major(sb.st_rdev));
}

#define MAX_INCREMENT 5LL * 1024LL * 1024LL * 1024LL

int erase_partition(struct fstab_rec *vol)
{
	int64_t disk_size;
	int fd;
	int ret = -1;
	int64_t increment;
	int64_t pos;
	int64_t max_bytes;
	char *disk_name = NULL;

	if (!is_valid_blkdev(vol->blk_device)) {
		pr_error("invalid destination node. partition disks?\n");
		return -1;
	}
	get_volume_size(vol, (uint64_t *)&disk_size);
	fd = open(vol->blk_device, O_RDWR);
	if (fd < 0) {
		pr_error("couldn't open block device %s\n", vol->blk_device);
		return -1;
	}

	mui_show_indeterminate_progress();
	/* It would be great if we could do BLKSECDISCARD on small regions
	 * so that we can update the progress bar, but each of these ioctls
	 * has a very long setup/teardown phase which makes the entire operation
	 * much slower if we call multiple times on small areas */
	disk_name = get_disk_sysfs(vol->blk_device);
	if (!disk_name) {
		pr_error("Couldn't get disk major number for %s\n", vol->blk_device);
		goto out;
	}

	if (read_sysfs_int64(&max_bytes, "%s/queue/discard_max_bytes", disk_name)) {
		pr_error("Couldn't read %s/queue/discard_max_bytes, is kernel configured correctly?\n",
				disk_name);
		pr_info("Fallback to manual zero of partition, this can take a LONG time\n");
		ret = erase_range_zero(fd, 0, disk_size);
		mui_show_text(0);
		goto out;
	}
	pr_debug("max bytes: %" PRId64"\n", max_bytes);


	if (max_bytes && disk_size > max_bytes)
		increment = max_bytes;
	else
		increment = disk_size;

	increment = min(increment, MAX_INCREMENT);

	if (increment != disk_size)
		mui_show_progress(1.0, 0);

	pos = 0;
	while (pos < disk_size) {
		mui_set_progress((float)pos / (float)disk_size);
		if (pos + increment > disk_size)
			increment = disk_size - pos;
		if (erase_range(fd, pos, increment)) {
			pr_error("Disk erase operation failed\n");
			goto out;
		}
		pos += increment;
	}
	ret = 0;
out:
	mui_reset_progress();
	free(disk_name);
	fsync(fd);
	close(fd);
	return ret;
}


int execute_command(const char *fmt, ...)
{
	int ret = -1;
	va_list ap;
	char *cmd;

	va_start(ap, fmt);
	if (vasprintf(&cmd, fmt, ap) < 0) {
		pr_perror("vasprintf");
		return -1;
	}
	va_end(ap);

	pr_debug("Executing: '%s'\n", cmd);
	ret = system(cmd);

	if (ret < 0) {
		pr_error("Error while trying to execute '%s': %s\n",
			cmd, strerror(errno));
		goto out;
	}
	ret = WEXITSTATUS(ret);
	pr_debug("Done executing '%s' (retval=%d)\n", cmd, ret);
out:
	free(cmd);
	return ret;
}

int execute_command_data(void *data, unsigned sz, const char *fmt, ...)
{
	int ret = -1;
	va_list ap;
	char *cmd;
	FILE *fp;
	size_t bytes_written;

	va_start(ap, fmt);
	if (vasprintf(&cmd, fmt, ap) < 0) {
		pr_perror("vasprintf");
		return -1;
	}
	va_end(ap);

	pr_debug("Executing: '%s'\n", cmd);
	fp = popen(cmd, "w");
	free(cmd);
	if (!fp) {
		pr_perror("popen");
		return -1;
	}

	bytes_written = fwrite(data, 1, sz, fp);
	if (bytes_written != sz) {
		pr_perror("fwrite");
		pclose(fp);
		return -1;
	}

	ret = pclose(fp);
	if (ret < 0) {
		pr_perror("pclose");
		return -1;
	}
	ret = WEXITSTATUS(ret);
	pr_debug("Execution complete, retval=%d\n", ret);

	return ret;
}


int is_valid_blkdev(const char *node)
{
	struct stat statbuf;
	if (stat(node, &statbuf))
		return 0;

	if (!S_ISBLK(statbuf.st_mode))
		return 0;

	return 1;
}


/* Taken from Android init, which also pulls runtime options
 * out of the kernel command line
 * FIXME: params can't have spaces */
void import_kernel_cmdline(void (*callback)(char *name))
{
	char cmdline[1024];
	char *ptr;
	int fd;

	fd = open("/proc/cmdline", O_RDONLY);
	if (fd >= 0) {
		int n = read(fd, cmdline, 1023);
		if (n < 0) n = 0;

		/* get rid of trailing newline, it happens */
		if (n > 0 && cmdline[n-1] == '\n')
			n--;

		cmdline[n] = 0;
		close(fd);
	} else {
		cmdline[0] = 0;
	}

	ptr = cmdline;
	while (ptr && *ptr) {
		char *x = strchr(ptr, ' ');
		if (x != 0)
			*x++ = 0;
		callback(ptr);
		ptr = x;
	}
}

int string_list_iterate(char *stringlist, bool (*cb)(char *entry,
			int index, void *context), void *context)
{
	char *saveptr, *entry, *str;
	int idx = 0;
	char *list;
	int ret = 0;

	if (!stringlist)
		return -1;

	list = xstrdup(stringlist);
	for (str = list; ; str = NULL) {
		entry = strtok_r(str, " \t", &saveptr);
		if (!entry)
			break;
		if (!cb(entry, idx++, context)) {
			ret = -1;
			break;
		}
	}
	free(list);
	return ret;
}

ssize_t robust_read(int fd, void *buf, size_t count, bool short_ok)
{
	unsigned char *pos = buf;
	ssize_t ret;
	ssize_t total = 0;
	do {
		ret = read(fd, pos, count);
		if (ret < 0) {
			if (errno != EINTR) {
				pr_perror("read");
				return -1;
			} else
				continue;
		}
		count -= ret;
		pos += ret;
		total += ret;
	} while (count && !short_ok);
	return total;
}

static char *__read_sysfs(const char *fmt, va_list ap)
{
	int fd;
	char buf[4096];
	char *filename;
	ssize_t bytes_read;

	if (vasprintf(&filename, fmt, ap) < 0) {
		pr_perror("vasprintf");
		return NULL;
	}

	pr_verbose("Opening %s\n", filename);
	fd = open(filename, O_RDONLY);
	free(filename);
	if (fd < 0) {
		pr_perror("open");
		return NULL;
	}

	bytes_read = robust_read(fd, buf, sizeof(buf) - 1, true);
	if (bytes_read < 0)
		return NULL;

	buf[bytes_read] = '\0';
	while (bytes_read && buf[--bytes_read] == '\n')
		buf[bytes_read] = '\0';
	return xstrdup(buf);
}


char *read_sysfs(const char *fmt, ...)
{
	va_list ap;
	char *ret;

	va_start(ap, fmt);
	ret = __read_sysfs(fmt, ap);
	va_end(ap);
	return ret;
}


int read_sysfs_int64(int64_t *val, const char *fmt, ...)
{
	va_list ap;
	char *ret;

	va_start(ap, fmt);
	ret = __read_sysfs(fmt, ap);
	va_end(ap);
	if (!ret)
		return -1;
	*val = atoll(ret);
	free(ret);
	return 0;
}

char *get_dmi_data(const char *node)
{
	char *ret;

	ret = read_sysfs("/sys/devices/virtual/dmi/id/%s", node);
	if (!ret)
		ret = xstrdup("unknown");

	return ret;
}


int copy_bootloader_file(char *filename, void *data, unsigned sz)
{
	struct fstab_rec *vol_bootloader;
	char *destpath = NULL;
	int ret = -1;

	vol_bootloader = volume_for_name("bootloader");
	if (vol_bootloader == NULL) {
		pr_error("/bootloader not defined in fstab\n");
		return -1;

	}
	if (mount_partition(vol_bootloader, false)) {
		pr_error("Couldn't mount bootloader partition!\n");
		return -1;
	}

	destpath = xasprintf("/mnt/bootloader/%s", filename);
	if (named_file_write(destpath, data, sz, 0, 0)) {
		pr_error("Couldn't write image to bootloader partition.\n");
		goto out_unmount;
	}

	ret = 0;
out_unmount:
	unmount_partition(vol_bootloader);
	free(destpath);
	return ret;
}


int update_bcb(char *command)
{
	struct fstab_rec *vol_misc;
	struct bootloader_message bcb;

	vol_misc = volume_for_name("misc");
	if (vol_misc == NULL) {
		pr_error("/misc not defined in fstab\n");
		return -1;
	}

	memset(&bcb, 0, sizeof(bcb));
	strncpy(bcb.command, command, sizeof(bcb.command));
	if (named_file_write(vol_misc->blk_device, (void *)&bcb, sizeof(bcb), 0, 0)) {
		pr_error("Couldn't update BCB!\n");
		return -1;
	}
	return 0;
}


/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */


