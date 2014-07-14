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

#ifndef __APP_FASTBOOT_H
#define __APP_FASTBOOT_H
#define FASTBOOT_DOWNLOAD_TMP_FILE "/tmp/fstboot.img"

/* Initialize fastboot protocol */
int fastboot_init(unsigned long size);

/* Begin listening for fastboot commands. Does not return except on fatal errors */
int fastboot_handler(void);

/* register a command handler 
 * - command handlers will be called if their prefix matches
 * - they are expected to call fastboot_okay() or fastboot_fail()
 *   to indicate success/failure before returning
 */
void fastboot_register(const char *prefix,
                       void (*handle)(char *arg, int *fd, unsigned size));

/* Fetch the value of a fastboot_publish variable */
char *fastboot_getvar(char *name);

/* only callable from within a command handler */
void fastboot_info(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
void fastboot_fail(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
void fastboot_okay(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

/* Takes ownership of the value pointer, may be freed at any time. Do not
 * use a constant string! xstrdup() is your friend.
 * It uses a copy of the name pointer, can be a constant string or something
 * on the heap; free it after publishing if you need to */
void fastboot_publish(char *name, char *value);

/**
 * Force to close file descriptor used at open_usb()
 * */
void close_iofds(void);

#endif

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */
