LOCAL_PATH := $(call my-dir)

# TODO It would be nice if we could just specify the modules we actually
# need and have some mechanism to pull in dependencies instead
# of explicitly enumerating everything that goes in
# use something like add-required-deps
userfastboot_modules := \
	libc \
	libcutils \
	libnetutils \
	libdl \
	liblog \
	libm \
	libstdc++ \
	libselinux \
	linker \
	sh \
	toolbox \
	libext2fs \
	libext2_com_err \
	libext2_e2p \
	libext2_blkid \
	libext2_uuid \
	libext2_profile \
	libext4_utils \
	libsparse \
	libusbhost \
	libz \
	resize2fs \
	tune2fs \
	e2fsck \
	gzip \
	userfastboot \
	netcfg \
	init.utilitynet.sh \
	dhcpcd \

userfastboot_system_files = $(filter $(PRODUCT_OUT)%,$(call module-installed-files,$(userfastboot_modules)))

ifneq ($(USERFASTBOOT_NO_GUI),true)
userfastboot_resources_common := $(LOCAL_PATH)/res
userfastboot_resources_deps := $(shell find $(userfastboot_resources_common) -type f)
endif

# $(1): source base dir
# $(2): target base dir
define userfastboot-copy-files
$(hide) $(foreach srcfile,$(userfastboot_system_files), \
	destfile=$(patsubst $(1)/%,$(2)/%,$(srcfile)); \
	mkdir -p `dirname $$destfile`; \
	$(ACP) -fdp $(srcfile) $$destfile; \
)
endef

userfastboot_out := $(PRODUCT_OUT)/userfastboot
USERFASTBOOT_ROOT_OUT := $(userfastboot_out)/root
userfastboot_data_out := $(USERFASTBOOT_ROOT_OUT)/data
userfastboot_system_out := $(USERFASTBOOT_ROOT_OUT)/system
userfastboot_etc_out := $(userfastboot_system_out)/etc
userfastboot_initrc := $(LOCAL_PATH)/init.rc

USERFASTBOOT_RAMDISK := $(userfastboot_out)/ramdisk-fastboot.img.gz
USERFASTBOOT_BOOTIMAGE := $(PRODUCT_OUT)/fastboot.img

# Used by UserFastBoot to know what device the SD card is on for OTA
ifdef TARGET_RECOVERY_FSTAB
recovery_fstab := $(TARGET_RECOVERY_FSTAB)
else
recovery_fstab := $(TARGET_DEVICE_DIR)/fstab
endif

# NOTE: You'll need to pass g_android.fastboot=1 on the kernel command line
# so that the ADB driver exports the right protocol
$(USERFASTBOOT_RAMDISK): \
		$(LOCAL_PATH)/ramdisk.mk \
		$(MKBOOTFS) \
		$(INSTALLED_RAMDISK_TARGET) \
		$(INSTALLED_SYSTEMIMAGE) \
		$(MINIGZIP) \
		$(recovery_fstab) \
		$(userfastboot_initrc) \
		$(USERFASTBOOT_HARDWARE_INITRC) \
		$(userfastboot_resources_deps) \
		$(userfastboot_system_files) \

	$(hide) rm -rf $(USERFASTBOOT_ROOT_OUT)
	$(hide) mkdir -p $(USERFASTBOOT_ROOT_OUT)
	$(hide) mkdir -p $(USERFASTBOOT_ROOT_OUT)/sbin
	$(hide) mkdir -p $(USERFASTBOOT_ROOT_OUT)/data
	$(hide) mkdir -p $(USERFASTBOOT_ROOT_OUT)/mnt
	$(hide) mkdir -p $(userfastboot_system_out)
	$(hide) mkdir -p $(userfastboot_system_out)/etc
	$(hide) mkdir -p $(userfastboot_system_out)/bin
	$(hide) $(ACP) -fr $(TARGET_ROOT_OUT) $(userfastboot_out)
	$(hide) $(ACP) -f $(userfastboot_initrc) $(USERFASTBOOT_ROOT_OUT)/init.rc
ifneq ($(strip $(USERFASTBOOT_HARDWARE_INITRC)),)
	$(hide) $(ACP) -f $(USERFASTBOOT_HARDWARE_INITRC) $(USERFASTBOOT_ROOT_OUT)/init.userfastboot.rc
endif
ifneq ($(USERFASTBOOT_NO_GUI),true)
	$(hide) $(ACP) -rf $(userfastboot_resources_common) $(USERFASTBOOT_ROOT_OUT)/
endif
	$(hide) $(ACP) -f $(recovery_fstab) $(userfastboot_etc_out)/recovery.fstab
	$(hide) $(call userfastboot-copy-files,$(TARGET_OUT),$(userfastboot_system_out))
	$(hide) $(MKBOOTFS) $(USERFASTBOOT_ROOT_OUT) | $(MINIGZIP) > $@
	@echo "Created UserFastBoot ramdisk: $@"

USERFASTBOOT_CMDLINE := g_android.fastboot=1
USERFASTBOOT_CMDLINE += $(BOARD_KERNEL_CMDLINE)

# Create a standard Android bootimage using the regular kernel and the
# userfastboot ramdisk.
$(USERFASTBOOT_BOOTIMAGE): \
		$(INSTALLED_KERNEL_TARGET) \
		$(USERFASTBOOT_RAMDISK) \
		$(BOARD_KERNEL_CMDLINE_FILE) \
		$(MKBOOTIMG) \

	$(hide) $(MKBOOTIMG) --kernel $(INSTALLED_KERNEL_TARGET) \
		     --ramdisk $(USERFASTBOOT_RAMDISK) \
		     --cmdline "$(USERFASTBOOT_CMDLINE)" \
		     $(BOARD_MKBOOTIMG_ARGS) \
		     --output $@
	@echo "Created UserFastBoot bootimage: $@"

.PHONY: userfastboot-ramdisk
userfastboot-ramdisk: $(USERFASTBOOT_RAMDISK)

.PHONY: userfastboot-bootimage
userfastboot-bootimage: $(USERFASTBOOT_BOOTIMAGE)

