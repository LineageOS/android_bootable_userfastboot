ufb_src_dir := bootable/userfastboot

# TODO It would be nice if we could just specify the modules we actually
# need and have some mechanism to pull in dependencies instead
# of explicitly enumerating everything that goes in
# use something like add-required-deps
ufb_modules := \
	libc \
	libcutils \
	libnetutils \
	libdl \
	liblog \
	libm \
	libstdc++ \
	libselinux \
	linker \
	linker64 \
	sh \
	su \
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

ufb_system_files = $(filter $(PRODUCT_OUT)%,$(call module-installed-files,$(ufb_modules)))

ifneq ($(USERFASTBOOT_NO_GUI),true)
ufb_resources_common := $(ufb_src_dir)/res
ufb_resources_deps := $(shell find $(ufb_resources_common) -type f)
endif

# $(1): source base dir
# $(2): target base dir
define userfastboot-copy-files
$(hide) $(foreach srcfile,$(ufb_system_files), \
	destfile=$(patsubst $(1)/%,$(2)/%,$(srcfile)); \
	mkdir -p `dirname $$destfile`; \
	$(ACP) -fdp $(srcfile) $$destfile; \
)
endef

ufb_out := $(PRODUCT_OUT)/userfastboot
USERFASTBOOT_ROOT_OUT := $(ufb_out)/root
ufb_data_out := $(USERFASTBOOT_ROOT_OUT)/data
ufb_system_out := $(USERFASTBOOT_ROOT_OUT)/system
ufb_etc_out := $(ufb_system_out)/etc
ufb_initrc := $(ufb_src_dir)/init.rc

USERFASTBOOT_RAMDISK := $(ufb_out)/ramdisk-fastboot.img.gz
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
		$(ufb_src_dir)/ramdisk.mk \
		$(MKBOOTFS) \
		$(INSTALLED_RAMDISK_TARGET) \
		$(INSTALLED_SYSTEMIMAGE) \
		$(MINIGZIP) \
		$(recovery_fstab) \
		$(ufb_initrc) \
		$(USERFASTBOOT_HARDWARE_INITRC) \
		$(ufb_resources_deps) \
		$(ufb_system_files) \

	$(hide) rm -rf $(USERFASTBOOT_ROOT_OUT)
	$(hide) mkdir -p $(USERFASTBOOT_ROOT_OUT)
	$(hide) mkdir -p $(USERFASTBOOT_ROOT_OUT)/sbin
	$(hide) mkdir -p $(USERFASTBOOT_ROOT_OUT)/data
	$(hide) mkdir -p $(USERFASTBOOT_ROOT_OUT)/mnt
	$(hide) mkdir -p $(ufb_system_out)
	$(hide) mkdir -p $(ufb_system_out)/etc
	$(hide) mkdir -p $(ufb_system_out)/bin
	$(hide) $(ACP) -fr $(TARGET_ROOT_OUT) $(ufb_out)
	$(hide) $(ACP) -f $(ufb_initrc) $(USERFASTBOOT_ROOT_OUT)/init.rc
ifneq ($(strip $(USERFASTBOOT_HARDWARE_INITRC)),)
	$(hide) $(ACP) -f $(USERFASTBOOT_HARDWARE_INITRC) $(USERFASTBOOT_ROOT_OUT)/init.userfastboot.rc
endif
ifneq ($(USERFASTBOOT_NO_GUI),true)
	$(hide) $(ACP) -rf $(ufb_resources_common) $(USERFASTBOOT_ROOT_OUT)/
endif
	$(hide) $(ACP) -f $(recovery_fstab) $(ufb_etc_out)/recovery.fstab
	$(hide) $(call userfastboot-copy-files,$(TARGET_OUT),$(ufb_system_out))
	$(hide) $(MKBOOTFS) $(USERFASTBOOT_ROOT_OUT) | $(MINIGZIP) > $@
	@echo "Created UserFastBoot ramdisk: $@"

USERFASTBOOT_CMDLINE := g_android.fastboot=1
USERFASTBOOT_CMDLINE += $(BOARD_KERNEL_CMDLINE)

$(ufb_out)/ufb-cmdline:
	$(hide) mkdir -p $(dir $@)
	$(hide) echo $(USERFASTBOOT_CMDLINE) > $@

$(ufb_out)/ufb-ramdisk.zip: \
		$(USERFASTBOOT_RAMDISK) \

	$(hide) mkdir -p $(dir $@)
	$(hide) (cd $(USERFASTBOOT_ROOT_OUT) && zip -qry ../$(notdir $@) .)

INSTALLED_RADIOIMAGE_TARGET += $(ufb_out)/ufb-ramdisk.zip $(ufb_out)/ufb-cmdline

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

ifeq ($(TARGET_UEFI_ARCH),i386)
efi_default_name := bootia32.efi
else
efi_default_name := bootx64.efi
endif

ufb_usb_fs_image := $(PRODUCT_OUT)/fastboot-usb.img
ufb_usb_efi_bins := $(GUMMIBOOT_EFI) $(LOCKDOWN_EFI)

ufb_loader_configs := \
	$(ufb_src_dir)/loader/1fastboot.conf \
	$(ufb_src_dir)/loader/2lockdown.conf \
	$(ufb_src_dir)/loader/loader.conf

ifneq ($(TARGET_USE_MOKMANAGER),false)
ufb_usb_efi_bins += $(MOKMANAGER_EFI)
ufb_loader_configs += $(ufb_src_dir)/loader/3mokmanager.conf
endif
ufb_usb_rootfs := $(ufb_out)/usbroot
ufb_usb_efi_dir := $(ufb_usb_rootfs)/EFI/BOOT/
ufb_usb_efi_loader := $(ufb_usb_rootfs)/loader

$(ufb_usb_fs_image): \
		$(ufb_loader_configs) \
		$(ufb_src_dir)/make_vfatfs \
		$(USERFASTBOOT_BOOTIMAGE) \
		$(ufb_usb_efi_bins) \
		$(UEFI_SHIM_EFI) \
		$(ufb_src_dir)/loader/loader.conf \
		| $(ACP) \

	$(hide) rm -rf $(ufb_usb_rootfs)
	$(hide) mkdir -p $(ufb_usb_rootfs)
	$(hide) $(ACP) -f $(USERFASTBOOT_BOOTIMAGE) $(ufb_usb_rootfs)/fastboot.img
	$(hide) mkdir -p $(ufb_usb_efi_dir)
	$(hide) $(ACP) $(UEFI_SHIM_EFI) $(ufb_usb_efi_dir)/$(efi_default_name)
	$(hide) $(ACP) $(ufb_usb_efi_bins) $(ufb_usb_efi_dir)
	$(hide) mkdir -p $(ufb_usb_efi_loader)/entries
	$(hide) $(ACP) $(ufb_src_dir)/loader/loader.conf $(ufb_usb_efi_loader)/
	$(hide) $(ACP) $(ufb_loader_configs) $(ufb_usb_efi_loader)/entries/
	$(hide) $(ufb_src_dir)/make_vfatfs $(ufb_usb_rootfs) $@

.PHONY: userfastboot-usb
userfastboot-usb: $(ufb_usb_fs_image)

# Build this when 'make' is run with no args
droidcore: $(ufb_usb_fs_image)

$(call dist-for-goals,droidcore,$(ufb_usb_fs_image):$(TARGET_PRODUCT)-fastboot-usb-$(FILE_NAME_TAG).img)
