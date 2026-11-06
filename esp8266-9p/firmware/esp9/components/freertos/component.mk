# freertos component override: the Plan 9 kernel shim replaces
# the FreeRTOS kernel. Headers still come from the SDK's real
# freertos component (declarations only); no kernel code from
# it is compiled or linked.
COMPONENT_ADD_INCLUDEDIRS := include include/freertos include/freertos/private
COMPONENT_SRCDIRS := .
CFLAGS += -I$(PROJECT_PATH)/../kernel
