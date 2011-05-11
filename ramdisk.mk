LOCAL_PATH := $(call my-dir)

# TODO It would be nice if we could just specify the modules we actually
# need and have some mechanism to pull in dependencies instead
# of explicitly enumerating everything that goes in
# use something like add-required-deps
droidboot_modules := \
	libc \
	libcutils \
	libdl \
	liblog \
	libm \
	libstdc++ \
	linker \
	ash \
	systembinsh \
	toolbox \
	logcat \
	strace \
	netcfg \
	libdiskconfig \
	libext2fs \
	libext2_com_err \
	libext2_e2p \
	libext2_blkid \
	libext2_uuid \
	libext2_profile \
	libext4_utils \
	libusbhost \
	libssl \
	libz \
	resize2fs \
	tune2fs \
	mke2fs \
	e2fsck \
	make_ext4fs \
	editdisklbl \
	nc \
	scp \
	ssh \
	tcpdump \
	dosfstools \
	gzip \
	droidboot

droidboot_system_files = $(call module-installed-files,$(droidboot_modules))

# $(1): source base dir
# $(2): target base dir
define droidboot-copy-files
$(hide) $(foreach srcfile,$(droidboot_system_files), \
	destfile=$(patsubst $(1)/%,$(2)/%,$(srcfile)); \
	mkdir -p `dirname $$destfile`; \
	echo "Copy $(srcfile) $$destfile"; \
	cp -fR $(srcfile) $$destfile; \
)
endef

droidboot_out := $(PRODUCT_OUT)/droidboot
droidboot_root_out := $(droidboot_out)/root
droidboot_data_out := $(droidboot_root_out)/data
droidboot_system_out := $(droidboot_root_out)/system
droidboot_etc_out := $(droidboot_system_out)/etc
droidboot_initrc := $(LOCAL_PATH)/init.rc

DROIDBOOT_RAMDISK := $(droidboot_out)/ramdisk-droidboot.img.gz

# NOTE: You'll need to pass f_adb.fastboot=1 on the kernel command line
# so that the ADB driver exports the right protocol
$(DROIDBOOT_RAMDISK): \
		$(LOCAL_PATH)/ramdisk.mk \
		$(MKBOOTFS) \
		$(TARGET_DISK_LAYOUT_CONFIG) \
		$(INSTALLED_RAMDISK_TARGET) \
		$(INSTALLED_SYSTEMIMAGE) \
		$(droidboot_initrc) \
		$(droidboot_system_files)
	$(info Creating Droidboot ramdisk $@)
	rm -rf $(droidboot_root_out)
	mkdir -p $(droidboot_root_out)
	mkdir -p $(droidboot_root_out)/sbin
	mkdir -p $(droidboot_root_out)/data
	mkdir -p $(droidboot_root_out)/mnt
	mkdir -p $(droidboot_system_out)
	mkdir -p $(droidboot_system_out)/etc
	mkdir -p $(droidboot_system_out)/bin
	cp -fR $(TARGET_ROOT_OUT) $(droidboot_out)
	rm -f $(droidboot_root_out)/init*.rc
	cp -f $(droidboot_initrc) $(droidboot_root_out)
	$(call droidboot-copy-files,$(TARGET_OUT),$(droidboot_system_out))
	cp -f $(TARGET_DISK_LAYOUT_CONFIG) $(droidboot_etc_out)/disk_layout.conf
	$(MKBOOTFS) $(droidboot_root_out) | gzip > $@


.PHONY: droidboot-ramdisk
droidboot-ramdisk: $(DROIDBOOT_RAMDISK)
