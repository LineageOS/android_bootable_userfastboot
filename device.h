/*
 * Decompose the target device name into a set a capabilities.
 */


#ifdef DEVICE_ivydale
# define DEVICE_HAS_KEYPAD
# define DEVICE_HAS_ttyS0
# define CHECK_ESC_ON_TTYS0
#endif

#ifdef DEVICE_mrst_ref
# define DEVICE_HAS_ttyS0
# define CHECK_ESC_ON_TTYS0
#endif
