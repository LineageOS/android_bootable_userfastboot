LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

ifeq ($(TARGET_USE_DROIDBOOT),true)

LOCAL_SRC_FILES := \
	aboot.c \
	fastboot.c \
	util.c \
	droidboot.c \
	manage_device.c \
	update_osip.c

LOCAL_CFLAGS := -DDEVICE_NAME=\"$(TARGET_DEVICE)\" \
	-W -Wall -Wno-unused-parameter -Werror

LOCAL_MODULE := droidboot
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libdiskconfig

include $(BUILD_EXECUTABLE)

endif # TARGET_USE_DROIDBOOT
