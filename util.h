#ifndef UTIL_H
#define UTIL_H

#include <diskconfig/diskconfig.h>

int kexec_linux(char *kernel, char *initrd, char *cmdline);
int is_valid_blkdev(const char *node);
int execute_command(const char *cmd);
int unmount_partition(char *mountpoint);
char *mount_partition(struct part_info *ptn);
void die(void);

#endif
