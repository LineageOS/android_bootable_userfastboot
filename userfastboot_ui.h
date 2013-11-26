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

#define pr_perror(x)	pr_error("%s failed: %s\n", x, strerror(errno))

#define VERBOSE_DEBUG 0

#define pr_error(...) do { \
	mui_print("E: " __VA_ARGS__); \
	mui_set_background(BACKGROUND_ICON_ERROR); \
	mui_show_text(1); \
	} while (0)
#define pr_warning(...)		mui_print("W: " __VA_ARGS__)
#define pr_info(...)		mui_print("I: " __VA_ARGS__)
#if VERBOSE_DEBUG
/* Serial console only */
#define pr_verbose(...)		printf("V: " __VA_ARGS__)
#else
#define pr_verbose(...)		do { } while (0)
#endif
#define pr_debug(...)		mui_print("D: " __VA_ARGS__)


#endif
