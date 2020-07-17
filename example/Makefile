# This is the main makefile for the filesystem test application.

# _______________________ User overridable configuration _______________________

PROJECT_NAME  ?= fs_demo

VERSION_MAJOR ?= 0
VERSION_MINOR ?= 0
VERSION_PATCH ?= 0
VERSION_DEVEL ?= "-dev"

# Common build options
CFLAGS                  += -Wall -std=c99 -ffunction-sections -fdata-sections -ffreestanding -fsingle-precision-constant -Wstrict-aliasing=0 -D__START=main -D__STARTUP_CLEAR_BSS
CFLAGS                  += -DVTOR_START_LOCATION=$(APP_START) -Wl,--section-start=.text=$(APP_START)
CFLAGS                  += -DUSE_CMSIS_OS2
CFLAGS                  += -DconfigUSE_TICKLESS_IDLE=0 -DconfigEXPECTED_IDLE_TIME_BEFORE_SLEEP=2
LDFLAGS                 += -nostartfiles -Wl,--gc-sections -Wl,--relax -Wl,-Map=$(@:.elf=.map),--cref -Wl,--wrap=atexit -Wl,--undefined=uxTopUsedPriority
LDLIBS                  += -lgcc -lm
INCLUDES                += -Xassembler -I$(BUILD_DIR) -I.

# Set logging verbosity
BASE_LOG_LEVEL          ?= 0xFFFF

# If set, disables asserts and debugging, enables optimization
RELEASE_BUILD           ?= 0

# Enable debug messages
VERBOSE                 ?= 0
# Disable info messages
#SILENT                  ?= 1

# Destination for build results
BUILD_BASE_DIR          ?= build
# Mark the default target
DEFAULT_BUILD_TARGET    ?= $(PROJECT_NAME)

ROOT_DIR                ?= $(abspath .)
ZOO                     ?= $(ROOT_DIR)/../zoo

# Silabs SDK location and version
SILABS_SDKDIR           ?= $(HOME)/SimplicityStudio_v4/developer/sdks/gecko_sdk_suite/v2.7

# Configure how image is programmed to target device
APP_START                = 0
PROGRAM_IMAGE           ?= $(BUILD_DIR)/$(PROJECT_NAME).bin
PROGRAM_DEST_ADDR       ?= $(APP_START)

# Other external tools
HEADEREDIT              ?= headeredit

# Pull in the developer's private configuration overrides for items above
-include Makefile.private

# _______________________ Non-overridable configuration _______________________

BUILD_DIR                = $(BUILD_BASE_DIR)/$(BUILD_TARGET)
BUILDSYSTEM_DIR         := $(ZOO)/thinnect.node-buildsystem/make
PLATFORMS_DIRS          := $(ZOO)/thinnect.node-buildsystem/make $(ZOO)/thinnect.dev-platforms/make
PHONY_GOALS             := all $(PROJECT_NAME) combo clean
TARGETLESS_GOALS        += clean
UUID_APPLICATION        := e4e3db80-c541-4d0f-900a-58c084c88349

VERSION_BIN             := $(shell printf "%02X" $(VERSION_MAJOR))$(shell printf "%02X" $(VERSION_MINOR))$(shell printf "%02X" $(VERSION_PATCH))
VERSION_STR             := "$(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)"$(VERSION_DEVEL)
SW_MAJOR_VERSION        := $(VERSION_MAJOR)
SW_MINOR_VERSION        := $(VERSION_MINOR)
SW_PATCH_VERSION        := $(VERSION_PATCH)
BUILD_TIMESTAMP         := $(shell date '+%s')
IDENT_TIMESTAMP         := $(BUILD_TIMESTAMP)

# NODE_PLATFORM_DIR is used by targets to add components to INCLUDES and SOURCES
NODE_PLATFORM_DIR       := $(ZOO)/thinnect.node-platform

# ______________ Build components - sources and includes _______________________

SOURCES = main.c

# FreeRTOS
FREERTOS_DIR ?= $(ZOO)/FreeRTOS-Kernel
FREERTOS_INC = -I$(FREERTOS_DIR)/include \
               -I$(ZOO)/thinnect.cmsis-freertos/CMSIS_5/CMSIS/RTOS2/Include \
               -I$(ZOO)/thinnect.cmsis-freertos/CMSIS-FreeRTOS/CMSIS/RTOS2/FreeRTOS/Include \
               -I$(ZOO)/thinnect.cmsis-freertos/$(MCU_ARCH)

FREERTOS_SRC = $(wildcard $(FREERTOS_DIR)/*.c) \
               $(ZOO)/thinnect.cmsis-freertos/CMSIS-FreeRTOS/CMSIS/RTOS2/FreeRTOS/Source/cmsis_os2.c

INCLUDES += $(FREERTOS_PORT_INC) $(FREERTOS_INC)
SOURCES += $(FREERTOS_PORT_SRC) $(FREERTOS_SRC)

INCLUDES += -I$(ZOO)/thinnect.cmsis-ext
SOURCES += $(ZOO)/thinnect.cmsis-ext/cmsis_os2_ext.c

# CMSIS_CONFIG_DIR is used to add default CMSIS and FreeRTOS configs to INCLUDES
CMSIS_CONFIG_DIR ?= $(ZOO)/thinnect.cmsis-freertos/$(MCU_ARCH)/config

# Silabs EMLIB, RAIL, radio
INCLUDES += \
    -I$(SILABS_SDKDIR)/hardware/kit/common/drivers \
    -I$(SILABS_SDKDIR)/platform/emlib/inc \
    -I$(SILABS_SDKDIR)/platform/emdrv/sleep/inc

SOURCES += \
    $(SILABS_SDKDIR)/hardware/kit/common/drivers/retargetserial.c \
    $(SILABS_SDKDIR)/hardware/kit/common/drivers/retargetio.c \
    $(SILABS_SDKDIR)/platform/emlib/src/em_system.c \
    $(SILABS_SDKDIR)/platform/emlib/src/em_core.c \
    $(SILABS_SDKDIR)/platform/emlib/src/em_emu.c \
    $(SILABS_SDKDIR)/platform/emlib/src/em_cmu.c \
    $(SILABS_SDKDIR)/platform/emlib/src/em_rmu.c \
    $(SILABS_SDKDIR)/platform/emlib/src/em_gpio.c \
    $(SILABS_SDKDIR)/platform/emlib/src/em_ldma.c \
    $(SILABS_SDKDIR)/platform/emlib/src/em_usart.c \
    $(SILABS_SDKDIR)/platform/emlib/src/em_msc.c \
    $(SILABS_SDKDIR)/platform/emlib/src/em_timer.c \
    $(SILABS_SDKDIR)/platform/emdrv/sleep/src/sleep.c \
    $(NODE_PLATFORM_DIR)/silabs/platform_mutex.c \
    $(NODE_PLATFORM_DIR)/common/spi_flash.c

# logging
#CFLAGS += -DLOGGER_TIMESTAMP
#CFLAGS  += -DLOGGER_FWRITE
#SOURCES += $(NODE_PLATFORM_DIR)/silabs/logger_fwrite.c
#CFLAGS  += -DLOGGER_LDMA_BUFFER_LENGTH=16384
CFLAGS  += -DLOGGER_LDMA
SOURCES += $(NODE_PLATFORM_DIR)/silabs/logger_ldma.c
SOURCES += $(ZOO)/thinnect.lll/logging/loggers_ext.c
INCLUDES += -I$(ZOO)/thinnect.lll/logging

# spiffs
INCLUDES += -I$(ZOO)/pellepl.spiffs/src

SOURCES += $(ZOO)/pellepl.spiffs/src/spiffs_cache.c \
           $(ZOO)/pellepl.spiffs/src/spiffs_check.c \
           $(ZOO)/pellepl.spiffs/src/spiffs_gc.c \
           $(ZOO)/pellepl.spiffs/src/spiffs_hydrogen.c \
           $(ZOO)/pellepl.spiffs/src/spiffs_nucleus.c

# filesystem
INCLUDES += -I$(ROOT_DIR)/..
SOURCES += $(ROOT_DIR)/../fs.c

# Generally useful external tools
INCLUDES += -I$(ZOO)/graphitemaster.incbin

# watchdog, loggers, radio etc...
INCLUDES += -I$(NODE_PLATFORM_DIR)/include
INCLUDES += -I$(NODE_PLATFORM_DIR)/include/silabs
SOURCES += $(NODE_PLATFORM_DIR)/common/sys_panic.c
SOURCES += $(NODE_PLATFORM_DIR)/silabs/retargetspi.c

# ------------------------------------------------------------------------------

# Pull in the grunt work
include $(BUILDSYSTEM_DIR)/Makerules

# Ensure "header.bin" does not exist in the project root dir. It must
# live in the subtree for a given build configuration under "build/xyz".
# Otherwise we risk including the header for a wrong platform.
ifneq ("$(wildcard header.bin)","")
  $(error "Found file [header.bin] in project root directory. Please remove it to avoid accidental header spoofing")
endif

$(call passVarToCpp,CFLAGS,VERSION_MAJOR)
$(call passVarToCpp,CFLAGS,VERSION_MINOR)
$(call passVarToCpp,CFLAGS,VERSION_PATCH)
$(call passVarToCpp,CFLAGS,VERSION_STR)
$(call passVarToCpp,CFLAGS,SW_MAJOR_VERSION)
$(call passVarToCpp,CFLAGS,SW_MINOR_VERSION)
$(call passVarToCpp,CFLAGS,SW_PATCH_VERSION)
$(call passVarToCpp,CFLAGS,IDENT_TIMESTAMP)

$(call passVarToCpp,CFLAGS,DEFAULT_RADIO_CHANNEL)
$(call passVarToCpp,CFLAGS,DEFAULT_PAN_ID)

UUID_APPLICATION_BYTES = $(call uuidToCstr,$(UUID_APPLICATION))
$(call passVarToCpp,CFLAGS,UUID_APPLICATION_BYTES)

$(call passVarToCpp,CFLAGS,BASE_LOG_LEVEL)

# _______________________________ Project rules _______________________________

all: $(DEFAULT_BUILD_TARGET)

# Only the object file calling INCLUDE_BINARY_FILE depends on file header.bin,
# but instead of manually mapping this dependency, it's simpler to add it as a
# dependency to all objects.
$(OBJECTS): $(BUILD_DIR)/header.bin

$(BUILD_DIR)/$(PROJECT_NAME).elf: Makefile | $(BUILD_DIR)

$(BUILD_DIR)/header.bin: Makefile | $(BUILD_DIR)
	$(call pInfo,Creating application headeredit block [$@])
	$(HEADEREDIT) -c -v softtype,1 -v firmaddr,$(APP_START) -v firmsizemax,$(APP_MAX_LEN) \
	    -v version,$(VERSION_STR) -v versionbin,$(VERSION_BIN) \
	    -v uuid,$(UUID_BOARD) -v uuid2,$(UUID_PLATFORM) -v uuid3,$(UUID_APPLICATION) \
	    -v timestamp,$(BUILD_TIMESTAMP) \
	    -v name,$(PROJECT_NAME) \
	    -v size -v crc "$@"

$(BUILD_DIR)/$(PROJECT_NAME).elf: $(OBJECTS)
	$(call pInfo,Linking [$@])
	$(HIDE_CMD)$(CC) $(CFLAGS) $(INCLUDES) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/$(PROJECT_NAME).bin: $(BUILD_DIR)/$(PROJECT_NAME).elf
	$(call pInfo,Exporting [$@])
	$(HIDE_CMD)$(TC_SIZE) --format=Berkeley $<
	$(HIDE_CMD)$(TC_OBJCOPY) --strip-all -O binary "$<" "$@"
	$(HIDE_CMD)$(HEADEREDIT) -C -v size -v crc $@

$(PROJECT_NAME): $(BUILD_DIR)/$(PROJECT_NAME).bin

# _______________________________ Utility rules ________________________________

$(BUILD_DIR):
	$(call pInfo,Creating [$@])
	@mkdir -p "$@"

clean:
	$(call pInfo,Nuking everything in [$(BUILD_BASE_DIR)])
	@-rm -rf "$(BUILD_BASE_DIR)"

.PHONY: $(PHONY_GOALS)
