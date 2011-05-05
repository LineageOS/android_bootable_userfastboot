#include <stdio.h>

/* debug levels */
#define CRITICAL 0
#define ALWAYS 0
#define INFO 1
#define SPEW 2

//#define DEBUG SPEW

#if defined(DEBUG)
#define DEBUGLEVEL DEBUG
#else
#define DEBUGLEVEL 0
#endif


#define dprintf(level, x...) do { if ((level) <= DEBUGLEVEL) { write_to_user(x); } } while (0)

