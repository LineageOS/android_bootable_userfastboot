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
#define LOG_TAG "fastboot"

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/mman.h>
#include <errno.h>

#include "userfastboot.h"
#include "userfastboot_ui.h"
#include "fastboot.h"
#include "userfastboot_util.h"

#define MAGIC_LENGTH 64
#define XFER_MEM_SIZE 4096*1024

struct fastboot_cmd {
	struct fastboot_cmd *next;
	const char *prefix;
	unsigned prefix_len;
	void (*handle) (char *arg, int *fd, unsigned sz);
};

struct fastboot_var {
	struct fastboot_var *next;
	const char *name;
	const char *value;
};

static struct fastboot_cmd *cmdlist;

void fastboot_register(const char *prefix,
		       void (*handle) (char *arg, int *fd,
				       unsigned sz))
{
	struct fastboot_cmd *cmd;
	cmd = xmalloc(sizeof(*cmd));
	cmd->prefix = prefix;
	cmd->prefix_len = strlen(prefix);
	cmd->handle = handle;
	cmd->next = cmdlist;
	cmdlist = cmd;
}

static struct fastboot_var *varlist;

void fastboot_publish(const char *name, const char *value)
{
	struct fastboot_var *var;
	pr_verbose("publishing %s=%s\n", name, value);
	var = xmalloc(sizeof(*var));
	var->name = name;
	var->value = value;
	var->next = varlist;
	varlist = var;
}

const char *fastboot_getvar(const char *name)
{
	struct fastboot_var *var;
	for (var = varlist; var; var = var->next)
		if (!strcmp(name, var->name))
			return (var->value);
	return NULL;
}

static unsigned char buffer[4096];

static unsigned download_size = 0;
static unsigned long download_max = 0;

#define STATE_OFFLINE	0
#define STATE_COMMAND	1
#define STATE_COMPLETE	2
#define STATE_ERROR	3

static unsigned fastboot_state = STATE_OFFLINE;
int fb_fp = -1;
int enable_fp;

static int usb_read(void *_buf, unsigned len)
{
	int r = 0;
	unsigned xfer;
	unsigned char *buf = _buf;
	int count = 0;
	unsigned const len_orig = len;

	if (fastboot_state == STATE_ERROR)
		goto oops;

	pr_verbose("usb_read %d\n", len);
	while (len > 0) {
		xfer = (len > 4096) ? 4096 : len;

		r = read(fb_fp, buf, xfer);
		if (r < 0) {
			pr_perror("read");
			goto oops;
		} else if (r == 0) {
			pr_info("Connection closed\n");
			goto oops;
		}

		count += r;
		buf += r;
		len -= r;

		/* Fastboot proto is badly specified. Corner case where it will fail:
		 * file download is MAGIC_LENGTH. Transport has MTU < MAGIC_LENGTH.
		 * Will be necessary to retry after short read, but break below will
		 * prevent it.
		 */
		if (len_orig == MAGIC_LENGTH)
			break;
	}
	pr_verbose("usb_read complete\n");
	return count;

oops:
	fastboot_state = STATE_ERROR;
	return -1;
}

static int usb_write(void *buf, unsigned len)
{
	int r;

	pr_verbose("usb_write %d\n", len);
	if (fastboot_state == STATE_ERROR)
		goto oops;

	r = write(fb_fp, buf, len);
	if (r < 0) {
		pr_perror("write");
		goto oops;
	}

	return r;

oops:
	fastboot_state = STATE_ERROR;
	return -1;
}

static int usb_read_to_file(int fd, unsigned int len)
{
	char buf[XFER_MEM_SIZE];
	int r = 0;
	int count = 0;
	unsigned int orig_len = len;

	lseek64(fd, 0, SEEK_SET);
	mui_show_progress(1.0, 0);
	while (len > 0)
	{
		unsigned int size = (len > XFER_MEM_SIZE) ? XFER_MEM_SIZE : len;
		r = usb_read(buf, size);
		if ((r < 0) || ((unsigned int)r != size)) {
			pr_error("fastboot: usb_read_to_file error only got %d bytes\n", r);
			return -1;
		}
		r = write(fd, buf, size);
		if ((r < 0) || ((unsigned int)r != size)) {
			pr_error("fastboot: usb_read_to_file error only wrote %d bytes to file. Needed:%d\n", r, size);
			return -1;
		}
		len -= size;
		count += size;
		mui_set_progress((float)count / (float)orig_len);
	}

	return count;
}

void fastboot_ack(const char *code, const char *reason)
{
	char response[MAGIC_LENGTH];

	if (fastboot_state != STATE_COMMAND)
		return;

	if (reason == 0)
		reason = "";

	snprintf(response, MAGIC_LENGTH, "%s%s", code, reason);
	fastboot_state = STATE_COMPLETE;

	usb_write(response, strlen(response));

}

void fastboot_fail(const char *reason)
{
	pr_error("ack FAIL %s\n", reason);
	fastboot_ack("FAIL", reason);
}

void fastboot_okay(const char *info)
{
	pr_info("ack OKAY %s\n", info);
	fastboot_ack("OKAY", info);
}

static void cmd_getvar(char *arg, int *fd, unsigned sz)
{
	struct fastboot_var *var;

	pr_debug("fastboot: cmd_getvar %s\n", arg);
	for (var = varlist; var; var = var->next) {
		if (!strcmp(var->name, arg)) {
			fastboot_okay(var->value);
			return;
		}
	}
	fastboot_okay("");
}

static void cmd_download(char *arg, int *fd, unsigned sz)
{
	char response[MAGIC_LENGTH];
	unsigned len;
	int r;

	len = strtoul(arg, NULL, 16);
	pr_debug("fastboot: cmd_download %d bytes\n", len);
	pr_status("Receiving %d bytes", len);

	download_size = 0;

	if (len > download_max) {
		fastboot_fail("data too large");
		return;
	}

	sprintf(response, "DATA%08x", len);
	if (usb_write(response, strlen(response)) < 0)
		return;

	r = usb_read_to_file(*fd, len);

	if ((r < 0) || ((unsigned int)r != len)) {
		pr_error("fastboot: cmd_download error only got %d bytes\n", r);
		fastboot_state = STATE_ERROR;
		return;
	}
	download_size = len;
	fastboot_okay("");
}

static void fastboot_command_loop(void)
{
	struct fastboot_cmd *cmd;
	int r;
	int fd = -1;
	pr_debug("fastboot: processing commands\n");

again:
	while (fastboot_state != STATE_ERROR) {
		memset(buffer, 0, MAGIC_LENGTH);
		r = usb_read(buffer, MAGIC_LENGTH);
		if (r < 0)
			break;
		buffer[r] = 0;
		pr_info("fastboot got command: %s\n", buffer);

		for (cmd = cmdlist; cmd; cmd = cmd->next) {
			if (memcmp(buffer, cmd->prefix, cmd->prefix_len))
				continue;
			fastboot_state = STATE_COMMAND;
			mui_show_indeterminate_progress();

			fd = open(FASTBOOT_DOWNLOAD_TMP_FILE, O_RDWR | O_CREAT, 0600);
			if (fd < 0){
				pr_error("fastboot: command_loop cannot open the temp file errno:%d", errno);
				fastboot_fail("fastboot failed");
			}

			pthread_mutex_lock(&action_mutex);
			cmd->handle((char *)buffer + cmd->prefix_len,
				    &fd, download_size);
			pthread_mutex_unlock(&action_mutex);

                        if (fd >= 0)
				close(fd);

			mui_reset_progress();
			if (fastboot_state == STATE_COMMAND)
				fastboot_fail("unknown reason");
			goto again;
		}
		pr_error("unknown command '%s'\n", buffer);
		fastboot_fail("unknown command");

	}
	fastboot_state = STATE_OFFLINE;
}

static int open_tcp(void)
{
	pr_verbose("Beginning TCP init\n");
	int tcp_fd = -1;
	int portno = 1234;
	struct sockaddr_in serv_addr;

	pr_verbose("Allocating socket\n");
	tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (tcp_fd < 0) {
		pr_error("Socket creation failed: %s\n", strerror(errno));
		return -1;
	}

	memset(&serv_addr, sizeof(serv_addr), 0);
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(portno);
	pr_verbose("Binding socket\n");
	if (bind(tcp_fd, (struct sockaddr *) &serv_addr,
		 sizeof(serv_addr)) < 0) {
		pr_error("Bind failure: %s\n", strerror(errno));
		close(tcp_fd);
		return -1;
	}

	pr_verbose("Listening socket\n");
	if (listen(tcp_fd,5)) {
		pr_error("Listen failure: %s\n", strerror(errno));
		close(tcp_fd);
		return -1;
	}

	pr_info("Listening on TCP port %d\n", portno);
	return tcp_fd;
}

static int open_usb(void)
{
	int usb_fp;
	static int printed = 0;

	usb_fp = open("/dev/android_adb", O_RDWR);

	if (!printed) {
		if (usb_fp < 1)
			pr_info("Can't open ADB device node (%s),"
				" Listening on TCP only.\n",
				strerror(errno));
		else
			pr_info("Listening on /dev/android_adb\n");
		printed = 1;
	}

	return usb_fp;
}

static int fastboot_handler(void *arg)
{
	int usb_fd_idx = 0;
	int tcp_fd_idx = 1;
	int const nfds = 2;
	struct pollfd fds[nfds];

	memset(&fds, sizeof fds, 0);

	fds[usb_fd_idx].fd = -1;
	fds[tcp_fd_idx].fd = -1;

	for (;;) {
		pr_status("Awaiting commands");

		if (fds[usb_fd_idx].fd == -1)
			fds[usb_fd_idx].fd = open_usb();
		if (fds[tcp_fd_idx].fd == -1)
			fds[tcp_fd_idx].fd = open_tcp();

		if (fds[usb_fd_idx].fd >= 0)
			fds[usb_fd_idx].events |= POLLIN;
		if (fds[tcp_fd_idx].fd >= 0)
			fds[tcp_fd_idx].events |= POLLIN;

		while (poll(fds, nfds, -1) == -1) {
			if (errno == EINTR)
				continue;
			pr_error("Poll failed: %s\n", strerror(errno));
			return -1;
		}

		if (fds[usb_fd_idx].revents & POLLIN) {
			fb_fp = fds[usb_fd_idx].fd;
			fastboot_command_loop();
			close(fb_fp);
			fds[usb_fd_idx].fd = -1;
		}

		if (fds[tcp_fd_idx].revents & POLLIN) {
			fb_fp = accept(fds[tcp_fd_idx].fd, NULL, NULL);
			if (fb_fp < 0)
				pr_error("Accept failure: %s\n", strerror(errno));
			else
				fastboot_command_loop();
			close(fb_fp);
		}
		fb_fp = -1;
	}
	return 0;
}

int fastboot_init(unsigned long size)
{
	char download_max_str[30];
	pr_verbose("fastboot_init()\n");
	download_max = size;
	snprintf(download_max_str, sizeof(download_max_str), "%lu\n", download_max);
	fastboot_register("getvar:", cmd_getvar);
	fastboot_register("download:", cmd_download);
	fastboot_publish("version", "0.5");
	fastboot_publish("max-download-size", download_max_str);
	fastboot_handler(NULL);

	return 0;
}
