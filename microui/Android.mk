LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := libmicroui
LOCAL_SRC_FILES := events.c graphics.c ui.c resources.c
LOCAL_MODULE_TAGS := optional
LOCAL_C_INCLUDES := external/libpng external/zlib
ifeq ($(TARGET_RECOVERY_PIXEL_FORMAT),"RGBX_8888")
  LOCAL_CFLAGS += -DRECOVERY_RGBX
endif
ifeq ($(TARGET_RECOVERY_PIXEL_FORMAT),"BGRA_8888")
  LOCAL_CFLAGS += -DRECOVERY_BGRA
endif

include $(BUILD_STATIC_LIBRARY)

