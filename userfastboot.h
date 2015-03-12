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

/* Serialize all disk operations. Grabbed by fastboot any time it is
 * performing a command, and also any worker thread handlers */
extern pthread_mutex_t action_mutex;

#define RECOVERY_FSTAB_LOCATION	          "/system/etc/recovery.fstab"
#define USERFASTBOOT_VERSION_NUMBER       "08.07"
#if defined(USER)
#define USERFASTBOOT_VARIANT              ""
#elif defined(USERDEBUG)
#define USERFASTBOOT_VARIANT              "-userdebug"
#else
#define USERFASTBOOT_VARIANT              "-eng"
#endif
#define USERFASTBOOT_VERSION    USERFASTBOOT_VERSION_NUMBER USERFASTBOOT_VARIANT

#define FASTBOOT_GUID \
	EFI_GUID(0x1ac80a82, 0x4f0c, 0x456b, 0x9a99, 0xde, 0xbe, 0xb4, 0x31, 0xfc, 0xc1);

#endif
