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

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cutils/android_reboot.h>
#include <cutils/klog.h>

#include "microui.h"


#define MAX_COLS 96
#define MAX_ROWS 64

#define CHAR_WIDTH 10
#define CHAR_HEIGHT 18

#define UI_WAIT_KEY_TIMEOUT_SEC    120

UIParameters ui_parameters = {
    6,       // indeterminate progress bar frames
    20,      // fps
    7,       // installation icon frames (0 == static image)
    23, 83,  // installation icon overlay offset
};

static pthread_mutex_t gUpdateMutex = PTHREAD_MUTEX_INITIALIZER;
static gr_surface gBackgroundIcon[NUM_BACKGROUND_ICONS];
static gr_surface *gInstallationOverlay;
static gr_surface *gProgressBarIndeterminate;
static gr_surface gProgressBarEmpty;
static gr_surface gProgressBarFill;

static const struct { gr_surface* surface; const char *name; } BITMAPS[] = {
    { &gBackgroundIcon[BACKGROUND_ICON_INSTALLING], "icon_installing" },
    { &gBackgroundIcon[BACKGROUND_ICON_ERROR],      "icon_error" },
    { &gProgressBarEmpty,               "progress_empty" },
    { &gProgressBarFill,                "progress_fill" },
    { NULL,                             NULL },
};

static int gCurrentIcon = 0;
static int gInstallingFrame = 0;
static int gInit = 0;

static enum ProgressBarType {
    PROGRESSBAR_TYPE_NONE,
    PROGRESSBAR_TYPE_INDETERMINATE,
    PROGRESSBAR_TYPE_NORMAL,
} gProgressBarType = PROGRESSBAR_TYPE_NONE;

// Progress bar scope of current operation
static float gProgressScopeStart = 0, gProgressScopeSize = 0, gProgress = 0;
static double gProgressScopeTime, gProgressScopeDuration;

// Set to 1 when both graphics pages are the same (except for the progress bar)
static int gPagesIdentical = 0;

static char text[MAX_ROWS][MAX_COLS];
static char status[MAX_COLS];
static char infotext[MAX_ROWS][MAX_COLS];
static int info_row = 0;
static int status_modified = 0;
static int text_cols = 0, text_rows = 0;
static int text_col = 0, text_row = 0, text_top = 0;
static int show_text = 0;
static pthread_mutex_t gTextMutex = PTHREAD_MUTEX_INITIALIZER;

static char menu[MAX_ROWS][MAX_COLS];
static int show_menu = 0;
static int menu_top = 0, menu_items = 0, menu_sel = 0;

// Key event input queue
static pthread_mutex_t key_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t key_queue_cond = PTHREAD_COND_INITIALIZER;
static int key_queue[256], key_queue_len = 0;
static volatile char key_pressed[KEY_MAX + 1];

// Return the current time as a double (including fractions of a second).
static double now() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

// Draw the given frame over the installation overlay animation.  The
// background is not cleared or draw with the base icon first; we
// assume that the frame already contains some other frame of the
// animation.  Does nothing if no overlay animation is defined.
// Should only be called with gUpdateMutex locked.
static void draw_install_overlay_locked(int frame) {
    if (gInstallationOverlay == NULL) return;
    gr_surface surface = gInstallationOverlay[frame];
    int iconWidth = gr_get_width(surface);
    int iconHeight = gr_get_height(surface);
    gr_blit(surface, 0, 0, iconWidth, iconHeight,
            ui_parameters.install_overlay_offset_x,
            ui_parameters.install_overlay_offset_y);
}

// Clear the screen and draw the currently selected background icon (if any).
// Should only be called with gUpdateMutex locked.
static void draw_background_locked(int icon)
{
    int i;
    gPagesIdentical = 0;
    gr_color(0, 0, 0, 255);
    gr_fill(0, 0, gr_fb_width(), gr_fb_height());
    status_modified = 1;

    if (!show_text && !show_menu) {
        gr_color(167, 162, 195, 255);
        pthread_mutex_lock(&gTextMutex);
        for (i = 0; i <= info_row; i++)
            gr_text(0, CHAR_HEIGHT * (i + 1), infotext[i]);
        pthread_mutex_unlock(&gTextMutex);
    }

    if (icon) {
        gr_surface surface = gBackgroundIcon[icon];
        int iconWidth = gr_get_width(surface);
        int iconHeight = gr_get_height(surface);
        int iconX = (gr_fb_width() - iconWidth) / 2;
        int iconY = (gr_fb_height() - iconHeight) / 2;
        gr_blit(surface, 0, 0, iconWidth, iconHeight, iconX, iconY);
        if (icon == BACKGROUND_ICON_INSTALLING) {
            draw_install_overlay_locked(gInstallingFrame);
        }
    }
}

static void draw_status_locked()
{
    int iconHeight = gr_get_height(gBackgroundIcon[BACKGROUND_ICON_INSTALLING]);
    int height = gr_get_height(gProgressBarEmpty);
    int fontx, fonty;

    if (show_text || show_menu)
        return;

    pthread_mutex_lock(&gTextMutex);

    if (status_modified) {
        int textwidth = gr_measure(status);
        int dx = (gr_fb_width() - textwidth)/2;
        /* We want the text just below the progress bar */
        int dy = CHAR_HEIGHT + height + (3 * gr_fb_height() + iconHeight) / 4;

        /* Clear any old text in the area */
        gr_color(0, 0, 0, 255);
        gr_fill(0, dy - CHAR_HEIGHT, gr_fb_width(), dy + CHAR_HEIGHT);

        gr_color(187, 221, 230, 255);
        gr_text(dx, dy, status);
        status_modified = 0;
    }

    pthread_mutex_unlock(&gTextMutex);
}

// Draw the progress bar (if any) on the screen.  Does not flip pages.
// Should only be called with gUpdateMutex locked.
static void draw_progress_locked()
{
    draw_status_locked();

    if (gCurrentIcon == BACKGROUND_ICON_INSTALLING) {
        draw_install_overlay_locked(gInstallingFrame);
    }

    if (gProgressBarType != PROGRESSBAR_TYPE_NONE) {
        int iconHeight = gr_get_height(gBackgroundIcon[BACKGROUND_ICON_INSTALLING]);
        int width = gr_get_width(gProgressBarEmpty);
        int height = gr_get_height(gProgressBarEmpty);

        int dx = (gr_fb_width() - width)/2;
        int dy = (3*gr_fb_height() + iconHeight - 2*height)/4;

        // Erase behind the progress bar (in case this was a progress-only update)
        gr_color(0, 0, 0, 255);
        gr_fill(dx, dy, width, height);

        if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL) {
            float progress = gProgressScopeStart + gProgress * gProgressScopeSize;
            int pos = (int) (progress * width);

            if (pos > 0) {
                gr_blit(gProgressBarFill, 0, 0, pos, height, dx, dy);
            }
            if (pos < width-1) {
                gr_blit(gProgressBarEmpty, pos, 0, width-pos, height, dx+pos, dy);
            }
        }

        if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE) {
            static int frame = 0;
            gr_blit(gProgressBarIndeterminate[frame], 0, 0, width, height, dx, dy);
            frame = (frame + 1) % ui_parameters.indeterminate_frames;
        }
    }
}

static void draw_text_line(int row, const char* t) {
  if (t[0] != '\0') {
    gr_text(0, (row+1)*CHAR_HEIGHT-1, t);
  }
}

// Redraw everything on the screen.  Does not flip pages.
// Should only be called with gUpdateMutex locked.
static void draw_screen_locked(void)
{
    int i = 0;

    draw_background_locked(gCurrentIcon);

    if (show_text || show_menu) {
        gr_color(0, 0, 0, 160);
        gr_fill(0, 0, gr_fb_width(), gr_fb_height());
    }

    if (show_menu) {
        gr_color(64, 96, 255, 255);
        gr_fill(0, (menu_top+menu_sel) * CHAR_HEIGHT,
                gr_fb_width(), (menu_top+menu_sel+1)*CHAR_HEIGHT+1);

        for (; i < menu_top + menu_items; ++i) {
            if (i == menu_top + menu_sel) {
                gr_color(255, 255, 255, 255);
                draw_text_line(i, menu[i]);
                gr_color(64, 96, 255, 255);
            } else {
                draw_text_line(i, menu[i]);
            }
        }
        gr_fill(0, i*CHAR_HEIGHT+CHAR_HEIGHT/2-1,
                gr_fb_width(), i*CHAR_HEIGHT+CHAR_HEIGHT/2+1);
        ++i;
    } else if (show_text) {
        gr_color(255, 255, 255, 255);
        pthread_mutex_lock(&gTextMutex);
        for (; i < text_rows; ++i) {
            draw_text_line(i, text[(i+text_top) % text_rows]);
        }
        pthread_mutex_unlock(&gTextMutex);
    } else {
        draw_progress_locked();
    }
}

// Redraw everything on the screen and flip the screen (make it visible).
// Should only be called with gUpdateMutex locked.
static void update_screen_locked(void)
{
    draw_screen_locked();
    gr_flip();
}

// Updates only the progress bar, if possible, otherwise redraws the screen.
// Should only be called with gUpdateMutex locked.
static void update_progress_locked(void)
{
    if (show_text || show_menu)
        return;

    if (!gPagesIdentical) {
        draw_screen_locked();    // Must redraw the whole screen
        gPagesIdentical = 1;
    } else {
        draw_progress_locked();  // Draw only the progress bar and overlays
    }
    gr_flip();
}

static void update_status_locked(void)
{
    draw_status_locked();
    gr_flip();
}

// Keeps the progress bar updated, even when the process is otherwise busy.
static void *progress_thread(void *cookie)
{
    double interval = 1.0 / ui_parameters.update_fps;
    for (;;) {
        double start = now();
        pthread_mutex_lock(&gUpdateMutex);

        int redraw = 0;

        // update the installation animation, if active
        if (gCurrentIcon == BACKGROUND_ICON_INSTALLING &&
            ui_parameters.installing_frames > 0 &&
            !show_text && !show_menu) {
            gInstallingFrame =
                (gInstallingFrame + 1) % ui_parameters.installing_frames;
            redraw = 1;
        }

        // update the progress bar animation, if active
        if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE &&
                !show_text && !show_menu) {
            redraw = 1;
        }

        // move the progress bar forward on timed intervals, if configured
        int duration = gProgressScopeDuration;
        if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL && duration > 0) {
            double elapsed = now() - gProgressScopeTime;
            float progress = 1.0 * elapsed / duration;
            if (progress > 1.0) progress = 1.0;
            if (progress > gProgress) {
                gProgress = progress;
                redraw = 1;
            }
        }

        if (redraw) update_progress_locked();

        pthread_mutex_unlock(&gUpdateMutex);
        double end = now();
        // minimum of 20ms delay between frames
        double delay = interval - (end-start);
        if (delay < 0.02) delay = 0.02;
        usleep((long)(delay * 1000000));
    }
    return NULL;
}

static int rel_sum = 0;

static int input_callback(int fd, short revents, void *data)
{
    struct input_event ev;
    int ret;
    int fake_key = 0;

    ret = ev_get_input(fd, revents, &ev);
    if (ret)
        return -1;

    if (ev.type == EV_SYN) {
        return 0;
    } else if (ev.type == EV_REL) {
        if (ev.code == REL_Y) {
            // accumulate the up or down motion reported by
            // the trackball.  When it exceeds a threshold
            // (positive or negative), fake an up/down
            // key event.
            rel_sum += ev.value;
            if (rel_sum > 3) {
                fake_key = 1;
                ev.type = EV_KEY;
                ev.code = KEY_DOWN;
                ev.value = 1;
                rel_sum = 0;
            } else if (rel_sum < -3) {
                fake_key = 1;
                ev.type = EV_KEY;
                ev.code = KEY_UP;
                ev.value = 1;
                rel_sum = 0;
            }
        }
    } else {
        rel_sum = 0;
    }

    if (ev.type != EV_KEY || ev.code > KEY_MAX)
        return 0;

    pthread_mutex_lock(&key_queue_mutex);
    if (!fake_key) {
        // our "fake" keys only report a key-down event (no
        // key-up), so don't record them in the key_pressed
        // table.
        key_pressed[ev.code] = ev.value;
    }
    const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
    if (ev.value > 0 && key_queue_len < queue_max) {
        key_queue[key_queue_len++] = ev.code;
        pthread_cond_signal(&key_queue_cond);
    }
    pthread_mutex_unlock(&key_queue_mutex);

    return 0;
}

// Reads input events, handles special hot keys, and adds to the key queue.
static void *input_thread(void *cookie)
{
    for (;;) {
        if (!ev_wait(-1))
            ev_dispatch();
    }
    return NULL;
}

void mui_init(void)
{
    gInit = 1;
    gr_init();
    ev_init(input_callback, NULL);

    text_col = text_row = 0;
    text_rows = gr_fb_height() / CHAR_HEIGHT;
    if (text_rows > MAX_ROWS) text_rows = MAX_ROWS;
    text_top = 1;

    text_cols = gr_fb_width() / CHAR_WIDTH;
    if (text_cols > MAX_COLS - 1) text_cols = MAX_COLS - 1;

    int i;
    for (i = 0; BITMAPS[i].name != NULL; ++i) {
        int result = res_create_surface(BITMAPS[i].name, BITMAPS[i].surface);
        if (result < 0) {
            printf("Missing bitmap %s\n(Code %d)\n", BITMAPS[i].name, result);
        }
    }

    gProgressBarIndeterminate = malloc(ui_parameters.indeterminate_frames *
                                       sizeof(gr_surface));
    for (i = 0; i < ui_parameters.indeterminate_frames; ++i) {
        char filename[40];
        // "indeterminate01.png", "indeterminate02.png", ...
        sprintf(filename, "indeterminate%02d", i+1);
        int result = res_create_surface(filename, gProgressBarIndeterminate+i);
        if (result < 0) {
            printf("Missing bitmap %s\n(Code %d)\n", filename, result);
        }
    }

    if (ui_parameters.installing_frames > 0) {
        gInstallationOverlay = malloc(ui_parameters.installing_frames *
                                      sizeof(gr_surface));
        for (i = 0; i < ui_parameters.installing_frames; ++i) {
            char filename[40];
            // "icon_installing_overlay01.png",
            // "icon_installing_overlay02.png", ...
            sprintf(filename, "icon_installing_overlay%02d", i+1);
            int result = res_create_surface(filename, gInstallationOverlay+i);
            if (result < 0) {
                printf("Missing bitmap %s\n(Code %d)\n", filename, result);
            }
        }

        // Adjust the offset to account for the positioning of the
        // base image on the screen.
        if (gBackgroundIcon[BACKGROUND_ICON_INSTALLING] != NULL) {
            gr_surface bg = gBackgroundIcon[BACKGROUND_ICON_INSTALLING];
            ui_parameters.install_overlay_offset_x +=
                (gr_fb_width() - gr_get_width(bg)) / 2;
            ui_parameters.install_overlay_offset_y +=
                (gr_fb_height() - gr_get_height(bg)) / 2;
        }
    } else {
        gInstallationOverlay = NULL;
    }

    pthread_t t;
    pthread_create(&t, NULL, progress_thread, NULL);
    pthread_create(&t, NULL, input_thread, NULL);
}

void mui_set_background(int icon)
{
    if (!gInit)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    gCurrentIcon = icon;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void mui_show_indeterminate_progress()
{
    if (!gInit)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    if (gProgressBarType != PROGRESSBAR_TYPE_INDETERMINATE) {
        gProgressBarType = PROGRESSBAR_TYPE_INDETERMINATE;
        update_progress_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void mui_show_progress(float portion, int seconds)
{
    if (!gInit)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    gProgressBarType = PROGRESSBAR_TYPE_NORMAL;
    gProgressScopeStart += gProgressScopeSize;
    gProgressScopeSize = portion;
    gProgressScopeTime = now();
    gProgressScopeDuration = seconds;
    gProgress = 0;
    update_progress_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void mui_set_progress(float fraction)
{
    if (!gInit)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL && fraction > gProgress) {
        // Skip updates that aren't visibly different.
        int width = gr_get_width(gProgressBarIndeterminate[0]);
        float scale = width * gProgressScopeSize;
        if ((int) (gProgress * scale) != (int) (fraction * scale)) {
            gProgress = fraction;
            update_progress_locked();
        }
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void mui_reset_progress()
{
    if (!gInit)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    gProgressBarType = PROGRESSBAR_TYPE_NONE;
    gProgressScopeStart = gProgressScopeSize = 0;
    gProgressScopeTime = gProgressScopeDuration = 0;
    gProgress = 0;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

static void remove_linefeeds(char *buf)
{
    int end_index;

    end_index = strlen(buf) - 1;

    while (end_index >= 0 && buf[end_index] == '\n') {
        buf[end_index] = '\0';
        end_index--;
    }
}


void mui_status(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, 256, fmt, ap);
    va_end(ap);

    mui_print("%s", buf);

    if (!gInit)
        return;

    remove_linefeeds(buf);

    // This can get called before ui_init(), so be careful.
    pthread_mutex_lock(&gTextMutex);
    strncpy(status, buf, sizeof(status));
    status[sizeof(status) - 1] = '\0';
    status_modified = 1;
    pthread_mutex_unlock(&gTextMutex);

    if (!show_text && !show_menu) {
        pthread_mutex_lock(&gUpdateMutex);
        update_status_locked();
        pthread_mutex_unlock(&gUpdateMutex);
    }
}

void mui_infotext(const char *infodata)
{
    char *saveptr, *str, *idata, *token;

    if (!gInit)
        return;

    idata = strdup(infodata);
    if (!idata)
        return;

    pthread_mutex_lock(&gTextMutex);
    info_row = 0;
    for (str = idata, info_row = 0; info_row < MAX_ROWS; str = NULL, info_row++) {
        token = strtok_r(str, "\n", &saveptr);
        if (!token) {
            infotext[info_row][0] = '\0';
            break;
        }
        strncpy(infotext[info_row], token, MAX_COLS);
	infotext[info_row][MAX_COLS - 1] = '\0';
    }
    pthread_mutex_unlock(&gTextMutex);
    free(idata);

    pthread_mutex_lock(&gUpdateMutex);
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void mui_print(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int chars;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (!gInit)
        return;

    // This can get called before ui_init(), so be careful.
    pthread_mutex_lock(&gTextMutex);
    if (text_rows > 0 && text_cols > 0) {
        char *ptr;
        if (text_col != 0) {
            text_col = 0;
            text_row = (text_row + 1) % text_rows;
            if (text_row == text_top) text_top = (text_top + 1) % text_rows;
        }
        for (ptr = buf; *ptr != '\0'; ++ptr) {
            if (*ptr == '\n' || text_col >= text_cols) {
                text[text_row][text_col] = '\0';
                text_col = 0;
                text_row = (text_row + 1) % text_rows;
                if (text_row == text_top) text_top = (text_top + 1) % text_rows;
            }
            if (*ptr != '\n') text[text_row][text_col++] = *ptr;
        }
        text[text_row][text_col] = '\0';
    }
    pthread_mutex_unlock(&gTextMutex);
    if (show_text) {
        pthread_mutex_lock(&gUpdateMutex);
        update_screen_locked();
        pthread_mutex_unlock(&gUpdateMutex);
    }

}


void mui_start_menu(char** headers, char** items, int initial_selection) {
    int i;
    pthread_mutex_lock(&gUpdateMutex);
    if (text_rows > 0 && text_cols > 0) {
        for (i = 0; i < text_rows; ++i) {
            if (headers[i] == NULL) break;
            strncpy(menu[i], headers[i], text_cols-1);
            menu[i][text_cols-1] = '\0';
        }
        menu_top = i;
        for (; i < text_rows; ++i) {
            if (items[i-menu_top] == NULL) break;
            strncpy(menu[i], items[i-menu_top], text_cols-1);
            menu[i][text_cols-1] = '\0';
        }
        menu_items = i - menu_top;
        show_menu = 1;
        menu_sel = initial_selection;
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

int mui_menu_select(int sel) {
    int old_sel;
    pthread_mutex_lock(&gUpdateMutex);
    if (show_menu > 0) {
        old_sel = menu_sel;
        menu_sel = sel;
        if (menu_sel < 0) menu_sel = menu_items - 1;
        if (menu_sel >= menu_items) menu_sel = 0;
        sel = menu_sel;
        if (menu_sel != old_sel) update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
    return sel;
}

void mui_end_menu() {
    int i;
    pthread_mutex_lock(&gUpdateMutex);
    if (show_menu > 0 && text_rows > 0 && text_cols > 0) {
        show_menu = 0;
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

int mui_text_visible()
{
    pthread_mutex_lock(&gUpdateMutex);
    int visible = show_text;
    pthread_mutex_unlock(&gUpdateMutex);
    return visible;
}


void mui_show_text(int visible)
{
    if (!gInit)
        return;
    pthread_mutex_lock(&gUpdateMutex);
    show_text = visible;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

int mui_wait_key()
{
    pthread_mutex_lock(&key_queue_mutex);

    // Time out after UI_WAIT_KEY_TIMEOUT_SEC
    do {
        struct timeval now;
        struct timespec timeout;
        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec;
        timeout.tv_nsec = now.tv_usec * 1000;
        timeout.tv_sec += UI_WAIT_KEY_TIMEOUT_SEC;

        int rc = 0;
        while (key_queue_len == 0 && rc != ETIMEDOUT) {
            rc = pthread_cond_timedwait(&key_queue_cond, &key_queue_mutex,
                                        &timeout);
        }
    } while (key_queue_len == 0);

    int key = -1;
    if (key_queue_len > 0) {
        key = key_queue[0];
        memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
    }
    pthread_mutex_unlock(&key_queue_mutex);
    return key;
}

int mui_key_pressed(int key)
{
    // This is a volatile static array, don't bother locking
    return key_pressed[key];
}

void mui_clear_key_queue() {
    pthread_mutex_lock(&key_queue_mutex);
    key_queue_len = 0;
    pthread_mutex_unlock(&key_queue_mutex);
}
