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
	network.c \
	ui.c \
	sanity.c \
	keystore.c \
	asn1.c \
	hashes.c

LOCAL_CFLAGS := -DDEVICE_NAME=\"$(TARGET_BOOTLOADER_BOARD_NAME)\" \
	-W -Wall -Wextra -Wno-unused-parameter -Wno-format-zero-length -Werror

LOCAL_MODULE := userfastboot-$(TARGET_BUILD_VARIANT)
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(PRODUCT_OUT)/userfastboot
LOCAL_UNSTRIPPED_PATH := $(PRODUCT_OUT)/userfastboot/debug
LOCAL_STATIC_LIBRARIES := libc liblog libz libm libcutils \
			  libsparse_static libminui libpng \
			  libselinux libfs_mgr libstdc++ libiniparser \
			  libgpt_static libefivar libcrypto_static \
			  libext4_utils_static
LOCAL_MODULE_STEM := userfastboot

LOCAL_FORCE_STATIC_EXECUTABLE := true

LOCAL_STATIC_LIBRARIES += $(TARGET_USERFASTBOOT_LIBS) $(TARGET_USERFASTBOOT_EXTRA_LIBS)
LOCAL_C_INCLUDES += external/libpng \
		    external/iniparser/src \
		    external/efivar/src \
		    external/openssl/include \
		    bootable/userfastboot/microui \
		    bootable/userfastboot/libgpt/include \
		    bootable/recovery \
		    system/core/libsparse \
		    system/core/mkbootimg \
		    system/core/fs_mgr/include \
		    system/core/libsparse/include \
		    system/extras/ext4_utils

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

$(call intermediates-dir-for,EXECUTABLES,userfastboot-$(TARGET_BUILD_VARIANT),,,$(TARGET_PREFER_32_BIT))/aboot.o : $(inc)
LOCAL_C_INCLUDES += $(dir $(inc))

ifneq ($(USERFASTBOOT_NO_GUI),true)
    LOCAL_CFLAGS += -DUSE_GUI
endif
ifeq ($(TARGET_BUILD_VARIANT),user)
    LOCAL_CFLAGS += -DUSER -DUSERDEBUG
endif
ifeq ($(TARGET_BUILD_VARIANT),userdebug)
    LOCAL_CFLAGS += -DUSERDEBUG
endif
include $(BUILD_EXECUTABLE)

endif # TARGET_USE_USERFASTBOOT
##################################
include $(CLEAR_VARS)

FORCE_PERMISSIVE_TO_UNCONFINED:=true

ifeq ($(TARGET_BUILD_VARIANT),user)
  # User builds are always forced unconfined+enforcing
  FORCE_PERMISSIVE_TO_UNCONFINED:=true
endif

# SELinux policy version.
# Must be <= /selinux/policyvers reported by the Android kernel.
# Must be within the compatibility range reported by checkpolicy -V.
POLICYVERS ?= 26

MLS_SENS=1
MLS_CATS=1024
SEPOLICY_PATH=external/sepolicy

# Quick edge case error detection for BOARD_SEPOLICY_REPLACE.
# Builds the singular path for each replace file.
sepolicy_replace_paths :=
$(foreach pf, $(BOARD_SEPOLICY_REPLACE), \
  $(if $(filter $(pf), $(BOARD_SEPOLICY_UNION)), \
    $(error Ambiguous request for sepolicy $(pf). Appears in both \
      BOARD_SEPOLICY_REPLACE and BOARD_SEPOLICY_UNION), \
  ) \
  $(eval _paths := $(filter-out $(BOARD_SEPOLICY_IGNORE), \
  $(wildcard $(addsuffix /$(pf), $(BOARD_SEPOLICY_DIRS))))) \
  $(eval _occurrences := $(words $(_paths))) \
  $(if $(filter 0,$(_occurrences)), \
    $(error No sepolicy file found for $(pf) in $(BOARD_SEPOLICY_DIRS)), \
  ) \
  $(if $(filter 1, $(_occurrences)), \
    $(eval sepolicy_replace_paths += $(_paths)), \
    $(error Multiple occurrences of replace file $(pf) in $(_paths)) \
  ) \
  $(if $(filter 0, $(words $(wildcard $(addsuffix /$(pf), $(SEPOLICY_PATH))))), \
    $(error Specified the sepolicy file $(pf) in BOARD_SEPOLICY_REPLACE, \
      but none found in $(SEPOLICY_PATH)), \
  ) \
)


# Builds paths for all requested policy files w.r.t
# both BOARD_SEPOLICY_REPLACE and BOARD_SEPOLICY_UNION
# product variables.
# $(1): the set of policy name paths to build
build_policy = $(foreach type, $(1), \
  $(filter-out $(BOARD_SEPOLICY_IGNORE), \
    $(foreach expanded_type, $(notdir $(wildcard $(addsuffix /$(type), $(SEPOLICY_PATH)))), \
      $(if $(filter $(expanded_type), $(BOARD_SEPOLICY_REPLACE)), \
        $(wildcard $(addsuffix $(expanded_type), $(sort $(dir $(sepolicy_replace_paths))))), \
        $(SEPOLICY_PATH)/$(expanded_type) \
      ) \
    ) \
    $(foreach union_policy, $(wildcard $(addsuffix /$(type), $(BOARD_SEPOLICY_DIRS))), \
      $(if $(filter $(notdir $(union_policy)), $(BOARD_SEPOLICY_UNION)), \
        $(union_policy), \
      ) \
    ) \
  ) \
)

sepolicy_build_files := security_classes \
                        initial_sids \
                        access_vectors \
                        global_macros \
                        mls_macros \
                        mls \
                        policy_capabilities \
                        te_macros \
                        attributes \
                        *.te \
                        roles \
                        users \
                        initial_sid_contexts \
                        fs_use \
                        genfs_contexts \
                        port_contexts

LOCAL_MODULE := sepolicy.userfastboot
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_TAGS := eng

include $(BUILD_SYSTEM)/base_rules.mk
userfastboot_sepolicy := $(call intermediates-dir-for,ETC,sepolicy.userfastboot)/sepolicy.userfastboot
sepolicy_policy_userfastboot.conf := $(intermediates)/policy_userfastboot.conf
$(sepolicy_policy_userfastboot.conf): PRIVATE_MLS_SENS := $(MLS_SENS)
$(sepolicy_policy_userfastboot.conf): PRIVATE_MLS_CATS := $(MLS_CATS)
$(sepolicy_policy_userfastboot.conf) : $(call build_policy, $(sepolicy_build_files))
	@mkdir -p $(dir $@)
	$(hide) m4 -D mls_num_sens=$(PRIVATE_MLS_SENS) -D mls_num_cats=$(PRIVATE_MLS_CATS) \
		-D target_build_variant=$(TARGET_BUILD_VARIANT) \
		-D force_permissive_to_unconfined=$(FORCE_PERMISSIVE_TO_UNCONFINED) \
		-D target_userfastboot=true \
		-s $^ > $@

$(LOCAL_BUILT_MODULE) : $(sepolicy_policy_userfastboot.conf) $(HOST_OUT_EXECUTABLES)/checkpolicy
	@mkdir -p $(dir $@)
	$(hide) $(HOST_OUT_EXECUTABLES)/checkpolicy -M -c $(POLICYVERS) -o $@ $<

built_sepolicy_userfastboot := $(LOCAL_BUILT_MODULE)
sepolicy_policy_userfastboot.conf :=

##################################
include $(CLEAR_VARS)


include bootable/userfastboot/libgpt/Android.mk

