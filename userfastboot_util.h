#ifndef USERFASTBOOT_UTIL_H
#define USERFASTBOOT_UTIL_H

#include <stdbool.h>

#include <diskconfig/diskconfig.h>
#include "userfastboot_fstab.h"

/* File I/O */
int named_file_write(const char *filename, const unsigned char *what,
		size_t sz, off_t offset, int append);
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
int mount_partition_device(const char *device, const char *type,
		char *mountpoint, bool readonly);
int mount_loopback(const char *path, const char *type, char *mountpoint);
int unmount_loopback(int loop_fd, const char *mountpoint);
void import_kernel_cmdline(void (*callback)(char *name));
int is_valid_blkdev(const char *node);
char *read_sysfs(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
int read_sysfs_int64(int64_t *val, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));
char *get_dmi_data(const char *node);
ssize_t robust_read(int fd, void *buf, size_t count, bool short_ok);
ssize_t robust_write(int fd, const void *buf, size_t count);

/* Fails assertion if memory allocations fail */
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
void *xmalloc(size_t size);
void xstring_append_line(char **str, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));

/* struct fstab_rec operations */
int mount_partition(struct fstab_rec *vol, bool readonly);
int erase_partition(struct fstab_rec *vol);
int check_ext_superblock(struct fstab_rec *vol, int *sb_present);
int unmount_partition(struct fstab_rec *vol);
int get_volume_size(struct fstab_rec *vol, uint64_t *sz);
int64_t get_disk_size(const char *disk_name);
int update_bcb(char *command);
int copy_bootloader_file(char *filename, void *data, unsigned sz);

int string_list_iterate(char *stringlist, bool (*cb)(char *entry,
			int index, void *context), void *context);

#ifndef min
#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })
#endif

#ifndef max
#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })
#endif

#endif
