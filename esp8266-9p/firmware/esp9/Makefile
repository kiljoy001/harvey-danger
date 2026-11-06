# esp9: Plan 9-shaped 9P server on the kernel shim
# (RTOS-generation Wi-Fi blobs hosted by p9 + freertos_shim).
PROJECT_NAME := esp9

# Clean-room objects (B24 esf_buf, B31 rate_control,
# B36 trc, B37 pp, B38 pm, B39 if_hwctrl, B40 lmac): link ours directly so the linker satisfies
# their symbols from them and never pulls the blob
# members out of libpp.a (all archives sit in one
# --start-group, so archive order cannot do this).
EXTRA_LDFLAGS += $(BUILD_DIR_BASE)/freertos/esf_buf.o \
	$(BUILD_DIR_BASE)/freertos/pp_esf.o \
	$(BUILD_DIR_BASE)/freertos/pp_lldesc.o \
	$(BUILD_DIR_BASE)/freertos/rate_control.o \
	$(BUILD_DIR_BASE)/freertos/trc.o \
	$(BUILD_DIR_BASE)/freertos/trc_tables.o \
	$(BUILD_DIR_BASE)/freertos/pp.o \
	$(BUILD_DIR_BASE)/freertos/pm.o \
	$(BUILD_DIR_BASE)/freertos/if_hwctrl.o \


include $(IDF_PATH)/make/project.mk
