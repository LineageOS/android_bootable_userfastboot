#ifndef DROIDBOOT_DEBUG_H
#define DROIDBOOT_DEBUG_H

#include <stdio.h>

/* debug levels */
#define CRITICAL 0
#define ALWAYS 0
#define INFO 1
#define SPEW 2

#define DEBUG INFO

#if defined(DEBUG)
#define DEBUGLEVEL DEBUG
#else
#define DEBUGLEVEL 0
#endif

#define dperror(n) dprintf(CRITICAL, "%s: %s", n, strerror(errno))

#define dprintf(level, format, ...) do { \
	if ((level) <= DEBUGLEVEL) { \
		printf("droidboot " #level ": " format, ##__VA_ARGS__); \
	} \
} while (0)

#endif
