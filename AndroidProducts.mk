#
# Copyright (C) 2026 The YgorBRxx Project
#
# SPDX-License-Identifier: Apache-2.0
#

_fire_patch_status := $(shell bash $(LOCAL_DIR)/patches/apply-patches.sh >&2; echo $$?)
ifneq ($(_fire_patch_status),0)
$(error fire: failed to apply source patches)
endif

PRODUCT_MAKEFILES := \
    $(LOCAL_DIR)/lineage_fire.mk
