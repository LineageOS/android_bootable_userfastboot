#ifndef DROIDBOOT_DEBUG_H
#define DROIDBOOT_DEBUG_H

#include <stdio.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>

/* debug levels */
#define CRITICAL 0
#define ALWAYS 0
#define INFO 1
#define SPEW 2

/* #define DEBUG SPEW */

/* prevent garbled debug messages from multiple threads outputting messages
 * simultaneously. defined in droidboot.c */
extern pthread_mutex_t debug_mutex;

#if defined(DEBUG)
#define DEBUGLEVEL DEBUG
#define dprintf(level, format, ...) do { \
	if ((level) <= DEBUGLEVEL) { \
		pthread_mutex_lock(&debug_mutex); \
		printf("%s:%d " #level ": " format, __func__, __LINE__, \
			##__VA_ARGS__); \
		pthread_mutex_unlock(&debug_mutex); \
	} \
} while (0)
#else
#define DEBUGLEVEL 0
#define dprintf(level, format, ...) do { \
	if ((level) <= DEBUGLEVEL) { \
		pthread_mutex_lock(&debug_mutex); \
		printf("droidboot: " format, ##__VA_ARGS__); \
		pthread_mutex_unlock(&debug_mutex); \
	} \
} while (0)
#endif

#define dperror(n) dprintf(CRITICAL, "%s: %s\n", n, strerror(errno))


#endif
