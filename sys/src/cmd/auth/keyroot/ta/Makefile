# keyroot probe TA.
#
# Build against an OP-TEE 3.13 TA dev kit and sign with the default key:
#
#   make TA_DEV_KIT_DIR=/path/to/export-ta_arm64 \
#        CROSS_COMPILE=aarch64-buildroot-linux-gnu- \
#        TA_SIGN_KEY=/path/to/export-ta_arm64/keys/default_ta.pem
#
# The signed result is 6b1d7c2e-5a41-4f0b-9c7e-2d1a8e6f3b90.ta.

BINARY = 6b1d7c2e-5a41-4f0b-9c7e-2d1a8e6f3b90

CFG_TEE_TA_LOG_LEVEL ?= 2
CPPFLAGS += -DCFG_TEE_TA_LOG_LEVEL=$(CFG_TEE_TA_LOG_LEVEL)

-include $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk

ifeq ($(wildcard $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk), )
clean:
	@echo 'TA_DEV_KIT_DIR is not set or does not contain mk/ta_dev_kit.mk'
endif
