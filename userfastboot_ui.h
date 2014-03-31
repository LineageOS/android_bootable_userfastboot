/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This is a simplified subset of the ui code in bootable/recovery/
 */

#ifndef _USERFASTBOOT_UI_H_
#define _USERFASTBOOT_UI_H_

#include "microui/microui.h"
#include <cutils/klog.h>

#define pr_perror(x)	pr_error("%s failed: %s\n", x, strerror(errno))

#define VERBOSE_DEBUG 0

#define pr_error(...) do { \
	KLOG_ERROR("userfastboot", __VA_ARGS__); \
	mui_print("E: " __VA_ARGS__); \
	mui_set_background(BACKGROUND_ICON_ERROR); \
	mui_show_text(1); \
	} while (0)

#define pr_warning(...)		do { \
	mui_print("W: " __VA_ARGS__); \
	KLOG_WARNING("userfastboot", __VA_ARGS__); \
	} while (0)

#define pr_info(...)		do { \
	KLOG_NOTICE("userfastboot", __VA_ARGS__); \
	mui_print(__VA_ARGS__); \
	} while (0)

#if VERBOSE_DEBUG
/* Serial console only */
#define pr_verbose(...)		KLOG_DEBUG("userfastboot", __VA_ARGS__)
#else
#define pr_verbose(...)		do { } while (0)
#endif

#define pr_debug(...)		KLOG_INFO("userfastboot", __VA_ARGS__)

#define pr_status(...)		do { \
	KLOG_NOTICE("userfastboot", __VA_ARGS__); \
	mui_status(__VA_ARGS__); \
	} while (0)

#define pr_uiinfo(...)		do { \
	KLOG_NOTICE("userfastboot", __VA_ARGS__); \
	mui_infotext(__VA_ARGS__); \
	} while (0);

#endif
