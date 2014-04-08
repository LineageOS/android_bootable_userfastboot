LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

ifeq ($(TARGET_USE_USERFASTBOOT),true)

LOCAL_SRC_FILES := \
	aboot.c \
	fastboot.c \
	util.c \
	userfastboot.c \
	fstab.c \
	gpt.c \
	network.c

LOCAL_CFLAGS := -DDEVICE_NAME=\"$(TARGET_BOOTLOADER_BOARD_NAME)\" \
	-W -Wall -Wno-unused-parameter -Werror

LOCAL_MODULE := userfastboot
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(PRODUCT_OUT)/userfastboot
LOCAL_UNSTRIPPED_PATH := $(PRODUCT_OUT)/userfastboot/debug
LOCAL_SHARED_LIBRARIES := liblog libext4_utils libz libcutils
LOCAL_STATIC_LIBRARIES += libsparse_static libmicroui libpng libpixelflinger_static \
			  libselinux libfs_mgr libenc libstdc++ libiniparser libgpt_static \
			  libefivar

LOCAL_STATIC_LIBRARIES += $(TARGET_USERFASTBOOT_LIBS) $(TARGET_USERFASTBOOT_EXTRA_LIBS)
LOCAL_C_INCLUDES += external/zlib \
		    external/libpng \
		    external/iniparser/src \
		    external/efivar/src \
		    bootable/userfastboot/microui \
		    bootable/userfastboot/libgpt/include \
		    bootable/iago/include \
		    bootable/recovery \
		    system/core/libsparse \
		    system/core/fs_mgr/include \
		    system/core/libsparse/include \

# Each library in TARGET_USERFASTBOOT_LIBS should have a function
# named "<libname>_init()".  Here we emit a little C function that
# gets #included by aboot.c.  It calls all those registration
# functions.

# Devices can also add libraries to TARGET_USERFASTBOOT_EXTRA_LIBS.
# These libs are also linked in with userfastboot, but we don't try to call
# any sort of registration function for these.  Use this variable for
# any subsidiary static libraries required for your registered
# extension libs.

inc := $(call intermediates-dir-for,PACKAGING,userfastboot_extensions)/register.inc

# During the first pass of reading the makefiles, we dump the list of
# extension libs to a temp file, then copy that to the ".list" file if
# it is different than the existing .list (if any).  The register.inc
# file then uses the .list as a prerequisite, so it is only rebuilt
# (and aboot.o recompiled) when the list of extension libs changes.

junk := $(shell mkdir -p $(dir $(inc));\
	        echo $(TARGET_USERFASTBOOT_LIBS) > $(inc).temp;\
	        diff -q $(inc).temp $(inc).list 2>/dev/null || cp -f $(inc).temp $(inc).list)

$(inc) : libs := $(TARGET_USERFASTBOOT_LIBS)
$(inc) : $(inc).list $(LOCAL_PATH)/Android.mk
	$(hide) mkdir -p $(dir $@)
	$(hide) echo "" > $@
	$(hide) $(foreach lib,$(libs), echo -e "extern void $(lib)_init(void);\n" >> $@;)
	$(hide) echo "void register_userfastboot_plugins() {" >> $@
	$(hide) $(foreach lib,$(libs),echo "  $(lib)_init();" >> $@;)
	$(hide) echo "}" >> $@

$(call intermediates-dir-for,EXECUTABLES,userfastboot)/aboot.o : $(inc)
LOCAL_C_INCLUDES += $(dir $(inc))

ifneq ($(USERFASTBOOT_NO_GUI),true)
LOCAL_CFLAGS += -DUSE_GUI
endif

ifeq ($(USERFASTBOOT_SKIP_FSCK),true)
LOCAL_CFLAGS += -DSKIP_FSCK
endif

include $(BUILD_EXECUTABLE)

endif # TARGET_USE_USERFASTBOOT

include bootable/userfastboot/microui/Android.mk
include bootable/userfastboot/libgpt/Android.mk

