#ifndef _USERFASTBOOT_H_
#define _USERFASTBOOT_H_

#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include <selinux/selinux.h>
#include <selinux/label.h>

extern struct selabel_handle *sehandle;

#include <userfastboot_fstab.h>

#define MEGABYTE	(1024 * 1024)

/* Inspect a volume looking for an automatic SW update. If it's
 * there, provision filesystems and apply it. */
int try_update_sw(struct fstab_rec *vol);

/* global libdiskconfig data structure representing the intended layout of
 * the internal disk, as read from /etc/disk_layout.conf */
extern struct disk_info *disk_info;

/* Serialize all disk operations. Grabbed by fastboot any time it is
 * performing a command, and also any worker thread handlers */
extern pthread_mutex_t action_mutex;

/* If set, apply this update on 'fastboot continue' */
extern char *g_update_location;

#define RECOVERY_FSTAB_LOCATION	"/system/etc/recovery.fstab"
#define USERFASTBOOT_VERSION       "07.03"

/* struct fstab_rec entry in recovery.fstab for the SD card */
#define SDCARD_VOLUME		"/sdcard"

#endif
