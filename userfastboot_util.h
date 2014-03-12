#ifndef USERFASTBOOT_UTIL_H
#define USERFASTBOOT_UTIL_H

#include <diskconfig/diskconfig.h>
#include "userfastboot_fstab.h"

/* File I/O */
int named_file_write(const char *filename, const unsigned char *what,
		size_t sz, off_t offset, int append);
int named_file_write_decompress_gzip(const char *filename,
		unsigned char *what, size_t sz, off_t offset, int append);
int named_file_write_ext4_sparse(const char *filename, const char *what);

/* Attribute specification and -Werror prevents most security shenanigans with
 * these functions */
int execute_command(const char *fmt, ...) __attribute__((format(printf,1,2)));
int execute_command_data(void *data, unsigned sz, const char *fmt, ...)
		__attribute__((format(printf,3,4)));

/* Misc utility functions */
void die(void);
void die_errno(const char *s);
void apply_sw_update(const char *location, int send_fb_ok);
int mount_partition_device(const char *device, const char *type, char *mountpoint);
void import_kernel_cmdline(void (*callback)(char *name));
int is_valid_blkdev(const char *node);

/* Fails assertion if memory allocations fail */
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...);
void *xmalloc(size_t size);

/* struct fstab_rec operations */
int mount_partition(struct fstab_rec *vol);
int erase_partition(struct fstab_rec *vol);
int check_ext_superblock(struct fstab_rec *vol, int *sb_present);
int unmount_partition(struct fstab_rec *vol);
int get_volume_size(struct fstab_rec *vol, uint64_t *sz);

#endif
