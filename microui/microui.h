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

#ifndef _MINUI_H_
#define _MINUI_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* gr_surface;
typedef unsigned short gr_pixel;

int gr_init(void);
void gr_exit(void);

int gr_fb_width(void);
int gr_fb_height(void);
gr_pixel *gr_fb_data(void);
void gr_flip(void);
void gr_fb_blank(bool blank);

void gr_color(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void gr_fill(int x, int y, int w, int h);
int gr_text(int x, int y, const char *s);
int gr_measure(const char *s);
void gr_font_size(int *x, int *y);

void gr_blit(gr_surface source, int sx, int sy, int w, int h, int dx, int dy);
unsigned int gr_get_width(gr_surface surface);
unsigned int gr_get_height(gr_surface surface);

// input event structure, include <linux/input.h> for the definition.
// see http://www.mjmwired.net/kernel/Documentation/input/ for info.
struct input_event;

typedef int (*ev_callback)(int fd, short revents, void *data);
typedef int (*ev_set_key_callback)(int code, int value, void *data);

int ev_init(ev_callback input_cb, void *data);
void ev_exit(void);
int ev_add_fd(int fd, ev_callback cb, void *data);
int ev_sync_key_state(ev_set_key_callback set_key_cb, void *data);

/* timeout has the same semantics as for poll
 *    0 : don't block
 *  < 0 : block forever
 *  > 0 : block for 'timeout' milliseconds
 */
int ev_wait(int timeout);

int ev_get_input(int fd, short revents, struct input_event *ev);
void ev_dispatch(void);

// Resources

// Returns 0 if no error, else negative.
int res_create_surface(const char* name, gr_surface* pSurface);
void res_free_surface(gr_surface surface);


// Initialize the graphics system.
void mui_init();

// Write a message to the on-screen log shown with Alt-L (also to stderr).
// The screen is small, and users may need to report these messages to support,
// so keep the output short and not too cryptic.
void mui_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void mui_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void mui_infotext(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

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

#ifdef __cplusplus
}
#endif

#endif
