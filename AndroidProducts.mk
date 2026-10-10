#
# Copyright (C) 2026 The YgorBRxx Project
#
# SPDX-License-Identifier: Apache-2.0
#

# crDroid doesn't run vendorsetup.sh, so source patches are applied here.
# AndroidProducts.mk is parsed for every device on lunch, so only patch
# when fire itself is being configured.
ifneq ($(filter %_fire,$(TARGET_PRODUCT)),)
_fire_patch_status := $(shell bash $(LOCAL_DIR)/patches/apply-patches.sh >&2; echo $$?)
ifneq ($(_fire_patch_status),0)
$(error fire: failed to apply source patches)
endif
endif

PRODUCT_MAKEFILES := \
    $(LOCAL_DIR)/lineage_fire.mk
