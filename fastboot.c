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
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <inttypes.h>

#include <cutils/hashmap.h>

#include "userfastboot.h"
#include "userfastboot_ui.h"
#include "fastboot.h"
#include "userfastboot_util.h"


#define USB_ADB_PATH      "/dev/android_adb"
#define USB_FFS_ADB_PATH  "/dev/usb-ffs/adb/"
#define USB_FFS_ADB_EP(x) USB_FFS_ADB_PATH#x
#define USB_FFS_ADB_EP0   USB_FFS_ADB_EP(ep0)
#define USB_FFS_ADB_OUT   USB_FFS_ADB_EP(ep1)
#define USB_FFS_ADB_IN    USB_FFS_ADB_EP(ep2)


#define ADB_CLASS              0xff
#define ADB_SUBCLASS           0x42
#define FASTBOOT_PROTOCOL      0x3
#define MAGIC_LENGTH 64
#define XFER_MEM_SIZE 4096*1024

#define cpu_to_le16(x)  htole16(x)
#define cpu_to_le32(x)  htole32(x)


#define MAX_PACKET_SIZE_FS	64
#define MAX_PACKET_SIZE_HS	512
#define MAX_PACKET_SIZE_SS	1024
struct io_fds
{
	int read_fp;
	int write_fp;
};
static struct io_fds io;

static const struct {
	struct usb_functionfs_descs_head header;
	struct {
		struct usb_interface_descriptor intf;
		struct usb_endpoint_descriptor_no_audio source;
		struct usb_endpoint_descriptor_no_audio sink;
	} __attribute__((packed)) fs_descs, hs_descs;
} __attribute__((packed)) descriptors = {
	.header = {
		.magic = cpu_to_le32(FUNCTIONFS_DESCRIPTORS_MAGIC),
		.length = cpu_to_le32(sizeof(descriptors)),
		.fs_count = 3,
		.hs_count = 3,
	},
	.fs_descs = {
		.intf = {
			.bLength = sizeof(descriptors.fs_descs.intf),
			.bDescriptorType = USB_DT_INTERFACE,
			.bInterfaceNumber = 0,
			.bNumEndpoints = 2,
			.bInterfaceClass = ADB_CLASS,
			.bInterfaceSubClass = ADB_SUBCLASS,
			.bInterfaceProtocol = FASTBOOT_PROTOCOL,
			.iInterface = 1, /* first string from the provided table */
		},
		.source = {
			.bLength = sizeof(descriptors.fs_descs.source),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 1 | USB_DIR_OUT,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = MAX_PACKET_SIZE_FS,
		},
		.sink = {
			.bLength = sizeof(descriptors.fs_descs.sink),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 2 | USB_DIR_IN,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = MAX_PACKET_SIZE_FS,
		},
	},
	.hs_descs = {
		.intf = {
			.bLength = sizeof(descriptors.hs_descs.intf),
			.bDescriptorType = USB_DT_INTERFACE,
			.bInterfaceNumber = 0,
			.bNumEndpoints = 2,
			.bInterfaceClass = ADB_CLASS,
			.bInterfaceSubClass = ADB_SUBCLASS,
			.bInterfaceProtocol = FASTBOOT_PROTOCOL,
			.iInterface = 1, /* first string from the provided table */
		},
		.source = {
			.bLength = sizeof(descriptors.hs_descs.source),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 1 | USB_DIR_OUT,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = MAX_PACKET_SIZE_HS,
		},
		.sink = {
			.bLength = sizeof(descriptors.hs_descs.sink),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 2 | USB_DIR_IN,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = MAX_PACKET_SIZE_HS,
		},
	},
};

#define STR_INTERFACE_ "FASTBOOT Interface"

static const struct {
	struct usb_functionfs_strings_head header;
	struct {
		__le16 code;
		const char str1[sizeof(STR_INTERFACE_)];
	} __attribute__((packed)) lang0;
} __attribute__((packed)) strings = {
	.header = {
		.magic = cpu_to_le32(FUNCTIONFS_STRINGS_MAGIC),
		.length = cpu_to_le32(sizeof(strings)),
		.str_count = cpu_to_le32(1),
		.lang_count = cpu_to_le32(1),
	},
	.lang0 = {
		cpu_to_le16(0x0409), /* en-us */
		STR_INTERFACE_,
	},
};


struct fastboot_cmd {
	struct fastboot_cmd *next;
	const char *prefix;
	unsigned prefix_len;
	void (*handle) (char *arg, int fd, void *data, unsigned sz);
};

static struct fastboot_cmd *cmdlist;

void fastboot_register(const char *prefix,
		       void (*handle) (char *arg, int fd,
				       void *data, unsigned sz))
{
	struct fastboot_cmd *cmd;
	cmd = xmalloc(sizeof(*cmd));
	cmd->prefix = prefix;
	cmd->prefix_len = strlen(prefix);
	cmd->handle = handle;
	cmd->next = cmdlist;
	cmdlist = cmd;
}

static Hashmap *vars;

void fastboot_publish(char *name, char *value)
{
	pr_verbose("publishing %s=%s\n", name, value);

	hashmapLock(vars);

	if (hashmapContainsKey(vars, name)) {
		pr_verbose("replacing old value\n");
		free(hashmapPut(vars, name, value));
	} else {
		pr_verbose("new value for table\n");
		hashmapPut(vars, xstrdup(name), value);
	}

	hashmapUnlock(vars);
}

char *fastboot_getvar(char *name)
{
	char *ret;

	hashmapLock(vars);
	ret = hashmapGet(vars, name);
	hashmapUnlock(vars);

	return ret;
}

static unsigned char buffer[4096];

static unsigned download_size = 0;
static unsigned long download_max = 0;
static pid_t fastboot_pid;

#define STATE_OFFLINE	0
#define STATE_COMMAND	1
#define STATE_COMPLETE	2
#define STATE_ERROR	3

static unsigned fastboot_state = STATE_OFFLINE;

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

		r = read(io.read_fp, buf, xfer);
		if (r < 0) {
			pr_warning("read");
			goto oops;
		} else if (r == 0) {
			pr_debug("Connection closed\n");
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

static int usb_write(void *_buf, unsigned len)
{
	int r;
	size_t count = 0;
	unsigned char *buf = _buf;

	pr_verbose("usb_write %d\n", len);
	if (fastboot_state == STATE_ERROR)
		goto oops;

	do {
		r = write(io.write_fp, buf + count, len - count);
	if (r < 0) {
		pr_perror("write");
		goto oops;
	}
		 count += r;
	} while (count < len);

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
			count = -1;
			goto out;
		}
		r = write(fd, buf, size);
		if ((r < 0) || ((unsigned int)r != size)) {
			pr_perror("write");
			count = -1;
			goto out;
		}
		len -= size;
		count += size;
		mui_set_progress((float)count / (float)orig_len);
	}
out:
	mui_reset_progress();
	return count;
}

static void fastboot_ack(const char *code, const char *format, va_list ap)
{
	char response[MAGIC_LENGTH];
	char reason[MAGIC_LENGTH];
	int i;

	/* Might be called from a debugging macro. Refuse to do anything
	 * not on the main Fastboot thread */
	if (gettid() != fastboot_pid)
		return;

	if (fastboot_state != STATE_COMMAND)
		return;

	vsnprintf(reason, MAGIC_LENGTH, format, ap);
	/* Nip off trailing newlines */
	for (i = strlen(reason); (i > 0) && reason[i - 1] == '\n'; i--)
		reason[i - 1] = '\0';
	snprintf(response, MAGIC_LENGTH, "%s%s", code, reason);
	pr_debug("ack %s %s\n", code, reason);
	usb_write(response, MAGIC_LENGTH);
}

void fastboot_info(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fastboot_ack("INFO", fmt, ap);
	va_end(ap);
}

void fastboot_fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fastboot_ack("FAIL", fmt, ap);
	va_end(ap);

	fastboot_state = STATE_COMPLETE;
}

void fastboot_okay(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fastboot_ack("OKAY", fmt, ap);
	va_end(ap);

	fastboot_state = STATE_COMPLETE;
}

struct getvar_ctx {
	char **entries;
	int i;
};

/* from the qsort man page */
static int cmpstringp(const void *p1, const void *p2)
{
	return strcmp(* (char * const *) p1, * (char * const *) p2);
}

static bool getvar_all_cb(void *key, void *value, void *context)
{
	char *keystr = key;
	char *valstr = value;
	struct getvar_ctx *ctx = context;

	ctx->entries[ctx->i++] = xasprintf("%s: %s", keystr, valstr);
	return true;
}

static void cmd_getvar(char *arg, int fd, void *data, unsigned sz)
{
	const char *value;

	pr_debug("fastboot: cmd_getvar %s\n", arg);
	if (!strcmp(arg, "all")) {
		struct getvar_ctx ctx;
		int mapsize;
		int i;

		hashmapLock(vars);
		mapsize = hashmapSize(vars);

		ctx.entries = calloc(mapsize, sizeof(char *));
		ctx.i = 0;

		hashmapForEach(vars, getvar_all_cb, &ctx);
		hashmapUnlock(vars);

		qsort(ctx.entries, mapsize, sizeof(char *), cmpstringp);
		for (i = 0; i < mapsize; i++) {
			fastboot_info("%s", ctx.entries[i]);
			free(ctx.entries[i]);
		}
		free(ctx.entries);
		fastboot_okay("");
	} else {
		value = fastboot_getvar(arg);
		if (value) {
			fastboot_okay("%s", value);
		} else {
			fastboot_okay("");
		}
	}
}

static void cmd_download(char *arg, int fd, void *data, unsigned sz)
{
	char response[MAGIC_LENGTH];
	unsigned len;
	int r;

	len = strtoul(arg, NULL, 16);
	pr_debug("fastboot: cmd_download %d bytes\n", len);
	pr_status("Receiving %d bytes\n", len);

	download_size = 0;

	if (len > download_max) {
		fastboot_fail("data too large");
		return;
	}

	sprintf(response, "DATA%08x", len);
	if (usb_write(response, strlen(response)) < 0)
		return;

	r = usb_read_to_file(fd, len);

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
	void *data;

	pr_debug("fastboot: processing commands\n");

again:
	while (fastboot_state != STATE_ERROR) {
		memset(buffer, 0, MAGIC_LENGTH);
		r = usb_read(buffer, MAGIC_LENGTH);
		if (r < 0)
			break;
		buffer[r] = 0;
		pr_debug("fastboot got command: %s\n", buffer);

		for (cmd = cmdlist; cmd; cmd = cmd->next) {
			if (memcmp(buffer, cmd->prefix, cmd->prefix_len))
				continue;
			fastboot_state = STATE_COMMAND;

			fd = open(FASTBOOT_DOWNLOAD_TMP_FILE, O_RDWR | O_CREAT, 0600);
			if (fd < 0) {
				pr_error("fastboot: cannot open temp file: %s\n",
					strerror(errno));
				die();
			}

			if (download_size) {
				struct stat sb;
				if (fstat(fd, &sb)) {
					pr_perror("fstat");
					die();
				}

				if (sb.st_size != download_size) {
					pr_error("size mismatch! (expected %u vs %" PRIu64 ")\n",
							download_size, sb.st_size);
					die();
				}

				data = mmap64(NULL, download_size, PROT_READ, MAP_SHARED, fd, 0);
				if (data == (void*)-1) {
					pr_perror("mmap64");
					die();
				}
				pr_verbose("%u bytes mapped\n", download_size);
			} else {
				pr_verbose("nothing to mmap\n");
				data = NULL;
			}

			pthread_mutex_lock(&action_mutex);
			pr_verbose("enter command handler\n");
			cmd->handle((char *)buffer + cmd->prefix_len,
				    fd, data, download_size);
			pr_verbose("exit command handler\n");
			pthread_mutex_unlock(&action_mutex);

			if (data && munmap(data, download_size)) {
				pr_perror("munmap");
				die();
			}

			if (close(fd)) {
				pr_perror("close");
				die();
			}

			if (data) {
				download_size = 0;
				pr_verbose("deleting temp file\n");

				if (unlink(FASTBOOT_DOWNLOAD_TMP_FILE)) {
					pr_perror("unlink");
					die();
				}
			}

			if (fastboot_state == STATE_COMMAND)
				fastboot_fail("unknown reason");
			else if (fastboot_state == STATE_COMPLETE)
				pr_status("Awaiting commands...\n");
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

	memset(&serv_addr, 0, sizeof(serv_addr));
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

static int enable_ffs = 0;

static int open_usb_fd(void)
{
	io.read_fp = open(USB_ADB_PATH, O_RDWR);
	/* tip to reuse same usb_read() and usb_write() than ffs */
	io.write_fp = io.read_fp;

	return io.read_fp;
}
static int open_usb_ffs(void)
{
	ssize_t ret;
	int control_fp;

	control_fp = open(USB_FFS_ADB_EP0, O_RDWR);
	if (control_fp < 0) {
		pr_info("[ %s: cannot open control endpoint: errno=%d]\n", USB_FFS_ADB_EP0, errno);
		goto err;
	}

	ret = write(control_fp, &descriptors, sizeof(descriptors));
	if (ret < 0) {
		pr_info("[ %s: write descriptors failed: errno=%d ]\n", USB_FFS_ADB_EP0, errno);
		goto err;
	}

	ret = write(control_fp, &strings, sizeof(strings));
	if (ret < 0) {
		pr_info("[ %s: writing strings failed: errno=%d]\n", USB_FFS_ADB_EP0, errno);
		goto err;
	}

	io.read_fp = open(USB_FFS_ADB_OUT, O_RDWR);
	if (io.read_fp < 0) {
		pr_info("[ %s: cannot open bulk-out ep: errno=%d ]\n", USB_FFS_ADB_OUT, errno);
		goto err;
	}

	io.write_fp = open(USB_FFS_ADB_IN, O_RDWR);
	if (io.write_fp < 0) {
		pr_info("[ %s: cannot open bulk-in ep: errno=%d ]\n", USB_FFS_ADB_IN, errno);
		goto err;
	}

	pr_info("Fastboot opened on %s\n", USB_FFS_ADB_PATH);

	close(control_fp);
	control_fp = -1;
	return io.read_fp;

err:
	if (io.write_fp > 0) {
		close(io.write_fp);
		io.write_fp = -1;
	}
	if (io.read_fp > 0) {
		close(io.read_fp);
		io.read_fp = -1;
	}
	if (control_fp > 0) {
		close(control_fp);
		control_fp = -1;
	}

	return -1;
}

/**
 * Opens the file descriptor either using first /dev/android_adb if exists
 * otherwise the ffs one /dev/usb-ffs/adb/
 * */
static int open_usb(void)
{
	int ret = 0;
	static int printed = 0;

	enable_ffs = 0;
	/* first try /dev/android_adb */
	ret = open_usb_fd();

	if (ret < 1) {
    		/* next /dev/usb-ffs/adb */
    		enable_ffs = 1;
		ret = open_usb_ffs();
    	}
	if (!printed) {
		if (ret < 1) {
			pr_info("Can't open ADB device node (%s),"
				" Listening on TCP only.\n",
				strerror(errno));
		} else {
			if (enable_ffs)
				pr_info("Listening on /dev/usb-ffs/adb/...\n");
			else
				pr_info("Listening on /dev/android_adb\n");
		}
		printed = 1;
	}
	return ret;
}

/**
 * Force to close file descriptor used at open_usb()
 * */
void close_iofds(void)
{
	if (io.write_fp > 0) {
		close(io.write_fp);
		io.write_fp = -1;
	}
	if (io.read_fp > 0) {
		if (enable_ffs)
			close(io.read_fp);
		io.read_fp = -1;
	}
}


int fastboot_handler(void)
{
	int usb_fd_idx = 0;
	int tcp_fd_idx = 1;
	int const nfds = 2;

	struct pollfd fds[nfds];

	memset(&fds, 0, sizeof(fds));

	fds[usb_fd_idx].fd = -1;
	fds[tcp_fd_idx].fd = -1;

	for (;;) {
		pr_status("Awaiting commands\n");

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
			fastboot_command_loop();
			close_iofds();
			fds[usb_fd_idx].fd = -1;
		}

		if (fds[tcp_fd_idx].revents & POLLIN) {
			io.read_fp = accept(fds[tcp_fd_idx].fd, NULL, NULL);
			if (io.read_fp < 0)
				pr_error("Accept failure: %s\n", strerror(errno));
			else {
				io.write_fp = io.read_fp;
				fastboot_command_loop();
			}
			close_iofds();
		}
	}
	return 0;
}

static int str_hash(void *key)
{
	return hashmapHash(key, strlen(key));
}

static bool str_equals(void *keyA, void *keyB)
{
	return strcmp(keyA, keyB) == 0;
}

int fastboot_init(unsigned long size)
{
	pr_verbose("fastboot_init()\n");
	download_max = size;
	vars = hashmapCreate(128, str_hash, str_equals);
	fastboot_register("getvar:", cmd_getvar);
	fastboot_register("download:", cmd_download);
	fastboot_publish("max-download-size", xasprintf("0x%lX", download_max));
	fastboot_pid = gettid();

	return 0;
}

/* vim: cindent:noexpandtab:softtabstop=8:shiftwidth=8:noshiftround
 */
