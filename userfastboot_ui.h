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

#include <cutils/klog.h>
#include <errno.h>
#include <string.h>
#include "fastboot.h"

#define pr_perror(x)	pr_error("%s failed: %s\n", x, strerror(errno))

#define VERBOSE_DEBUG 0

#define pr_error(...) do { \
	KLOG_ERROR("userfastboot", __VA_ARGS__); \
	mui_print("E: " __VA_ARGS__); \
	fastboot_info(__VA_ARGS__); \
	} while (0)

#define pr_warning(...)		do { \
	mui_print("W: " __VA_ARGS__); \
	KLOG_WARNING("userfastboot", __VA_ARGS__); \
	} while (0)

#define pr_info(...)		do { \
	KLOG_NOTICE("userfastboot", __VA_ARGS__); \
	mui_print(__VA_ARGS__); \
	fastboot_info(__VA_ARGS__); \
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


// Initialize the graphics system.
void mui_init();

// Write a message to the on-screen log shown with Alt-L (also to stderr).
// The screen is small, and users may need to report these messages to support,
// so keep the output short and not too cryptic.
void mui_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void mui_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Writes this text to the top-left corner of the screen. May contain
// multiple lines of information. Multiple calls reset what is written.
void mui_infotext(const char *info);

// Set the icon (normally the only thing visible besides the progress bar).
enum {
	BACKGROUND_ICON_NONE,
	BACKGROUND_ICON_INSTALLING,
	BACKGROUND_ICON_ERROR,
	NUM_BACKGROUND_ICONS
};
void mui_set_background(int icon);

// Show a progress bar and define the scope of the next operation:
//   portion - fraction of the progress bar the next operation will use
//   seconds - expected time interval (progress bar moves at this minimum rate)
void mui_show_progress(float portion, int seconds);
void mui_set_progress(float fraction);  // 0.0 - 1.0 within the defined scope

// Default allocation of progress bar segments to operations
static const int VERIFICATION_PROGRESS_TIME = 60;
static const float VERIFICATION_PROGRESS_FRACTION = 0.25;
static const float DEFAULT_FILES_PROGRESS_FRACTION = 0.4;
static const float DEFAULT_IMAGE_PROGRESS_FRACTION = 0.1;

// Show a rotating "barberpole" for ongoing operations.  Updates automatically.
void mui_show_indeterminate_progress();

// Hide and reset the progress bar.
void mui_reset_progress();

void mui_show_text(int visible);
int mui_text_visible(void);

typedef struct {
	// number of frames in indeterminate progress bar animation
	int indeterminate_frames;

	// number of frames per second to try to maintain when animating
	int update_fps;

	// number of frames in installing animation.  may be zero for a
	// static installation icon.
	int installing_frames;

	// the install icon is animated by drawing images containing the
	// changing part over the base icon.  These specify the
	// coordinates of the upper-left corner.
	int install_overlay_offset_x;
	int install_overlay_offset_y;
} UIParameters;

int mui_start_menu(char** headers, char** items, int initial_selection);
int mui_menu_select(int sel);
void mui_end_menu(void);
int mui_key_pressed(int key);
void mui_clear_key_queue();
int mui_wait_key(void);

#endif

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */
