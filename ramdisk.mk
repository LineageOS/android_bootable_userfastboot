ufb_src_dir := bootable/userfastboot

# TODO It would be nice if we could just specify the modules we actually
# need and have some mechanism to pull in dependencies instead
# of explicitly enumerating everything that goes in
# use something like add-required-deps
#
# At the moment, what we want is a shell, toolbox, and dhcpcd. The rest
# are just supporting modules.
ufb_modules := \
	libcrypto \
	libc \
	libcutils \
	libnetutils \
	libdl \
	liblog \
	libm \
	libstdc++ \
	libselinux \
	libusbhost \
	linker \
	linker64 \
	mksh \
	systembinsh \
	sh \
	toolbox \
	dhcpcd

ifneq ($(TARGET_BUILD_VARIANT),user)
    ufb_modules += su
endif

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
ufb_bin := $(ufb_out)/userfastboot
USERFASTBOOT_ROOT_OUT := $(ufb_out)/root
ufb_data_out := $(USERFASTBOOT_ROOT_OUT)/data
ufb_system_out := $(USERFASTBOOT_ROOT_OUT)/system
ufb_etc_out := $(ufb_system_out)/etc
ufb_initrc := $(ufb_src_dir)/init.rc

efibootmgr := $(PRODUCT_OUT)/efi/efibootmgr

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
		$(ufb_bin) \
		$(efibootmgr) \
		$(INSTALLED_RAMDISK_TARGET) \
		$(MINIGZIP) \
		$(recovery_fstab) \
		$(ufb_initrc) \
		$(USERFASTBOOT_HARDWARE_INITRC) \
		$(ufb_resources_deps) \
		$(ufb_system_files) | $(ACP) \

	$(hide) rm -rf $(USERFASTBOOT_ROOT_OUT)
	$(hide) mkdir -p $(USERFASTBOOT_ROOT_OUT)
	$(hide) mkdir -p $(USERFASTBOOT_ROOT_OUT)/sbin
	$(hide) $(ACP) -f $(ufb_bin) $(USERFASTBOOT_ROOT_OUT)/sbin/userfastboot
	$(hide) $(ACP) -f $(efibootmgr) $(USERFASTBOOT_ROOT_OUT)/sbin/efibootmgr
	$(hide) mkdir -p $(USERFASTBOOT_ROOT_OUT)/data
	$(hide) mkdir -p $(USERFASTBOOT_ROOT_OUT)/mnt
	$(hide) mkdir -p $(ufb_system_out)
	$(hide) mkdir -p $(ufb_system_out)/etc
	$(hide) mkdir -p $(ufb_system_out)/bin
	$(hide) $(ACP) -frd $(TARGET_ROOT_OUT) $(ufb_out)
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

ifneq ($(USERFASTBOOT_2NDBOOTLOADER),)
$(ufb_out)/ufb-second: $(USERFASTBOOT_2NDBOOTLOADER) | $(ACP)
	$(copy-file-to-new-target)

INSTALLED_RADIOIMAGE_TARGET += $(ufb_out)/ufb-second
endif

# Create a standard Android bootimage using the regular kernel and the
# userfastboot ramdisk.
$(USERFASTBOOT_BOOTIMAGE): \
		$(INSTALLED_KERNEL_TARGET) \
		$(USERFASTBOOT_RAMDISK) \
		$(BOARD_KERNEL_CMDLINE_FILE) \
		$(USERFASTBOOT_2NDBOOTLOADER) \
		$(BOOT_SIGNER) \
		$(MKBOOTIMG) \

	$(hide) $(MKBOOTIMG) --kernel $(INSTALLED_KERNEL_TARGET) \
		     $(addprefix --second ,$(USERFASTBOOT_2NDBOOTLOADER)) \
		     --ramdisk $(USERFASTBOOT_RAMDISK) \
		     --cmdline "$(USERFASTBOOT_CMDLINE)" \
		     $(BOARD_MKBOOTIMG_ARGS) \
		     --output $@
	@echo "Created UserFastBoot bootimage: $@"
	$(BOOT_SIGNER) /fastboot $(USERFASTBOOT_BOOTIMAGE) $(PRODUCTS.$(INTERNAL_PRODUCT).PRODUCT_VERITY_SIGNING_KEY) $(USERFASTBOOT_BOOTIMAGE)

.PHONY: userfastboot-ramdisk
userfastboot-ramdisk: $(USERFASTBOOT_RAMDISK)

.PHONY: userfastboot-bootimage
userfastboot-bootimage: $(USERFASTBOOT_BOOTIMAGE)

