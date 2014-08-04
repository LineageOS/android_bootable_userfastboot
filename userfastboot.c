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
#define LOG_TAG "userfastboot"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mount.h>

#include <cutils/android_reboot.h>
#include <cutils/klog.h>

#include "aboot.h"
#include "userfastboot_util.h"
#include "userfastboot.h"
#include "fastboot.h"
#include "userfastboot_ui.h"
#include "userfastboot_fstab.h"
#include "network.h"

/* Synchronize operations which touch EMMC. Fastboot holds this any time it
 * executes a command. Threads which touch the disk should do likewise. */
pthread_mutex_t action_mutex = PTHREAD_MUTEX_INITIALIZER;

struct selabel_handle *sehandle;

int main(int argc, char **argv)
{
	struct statfs buf;
	int ret = 0;
	/* Files written only read/writable by root */
	umask(S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

	klog_init();
	klog_set_level(7);

	/* initialize libmicroui */
#ifdef USE_GUI
	KLOG_ERROR("userfastboot", "starting graphics\n");
	mui_init();
	KLOG_ERROR("userfastboot", "graphics OK\n");
#endif
	mui_set_background(BACKGROUND_ICON_INSTALLING);

	struct selinux_opt seopts[] = {
		{ SELABEL_OPT_PATH, "/file_contexts" }
	};

	sehandle = selabel_open(SELABEL_CTX_FILE, seopts, 1);

	if (!sehandle) {
		pr_error("Warning: No file_contexts\n");
	}

	ret = statfs("/tmp", &buf);
	if (!ret) {
		unsigned long size = buf.f_bsize * buf.f_bfree;
		fastboot_init(size);
	} else {
		pr_error("Error when acquiring tmpfs size:-%d\n", errno);
		exit(1);
	}

	load_volume_table();
	aboot_register_commands();
	start_interface_thread();
	fastboot_handler();
	/* Shouldn't get here */
	exit(1);
}

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */

