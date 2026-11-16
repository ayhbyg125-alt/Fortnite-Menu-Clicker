# Library, directory, and output settings
PROJ_NAME := $(shell basename $(CURDIR))
SDK       := $(abspath ../../SDK)
OO_PS4_TOOLCHAIN ?= $(abspath ../../OpenOrbis/PS4Toolchain)

# Compiler & Linker
CC      := clang
AS      := clang
LD      := ld.lld
OBJCOPY := llvm-objcopy

# Directories
SRC_DIR   := source
BUILD_DIR := build
OUT_DIR   := $(abspath ../../bin/plugins/prx_final)
TARGET    := $(OUT_DIR)/$(PROJ_NAME).prx

# Source & Object Files
CFILES   := $(wildcard $(SRC_DIR)/*.c)
SFILES   := $(wildcard $(SRC_DIR)/*.s)
OBJS     := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(CFILES)) $(patsubst $(SRC_DIR)/%.s, $(BUILD_DIR)/%.o, $(SFILES))

# Compiler & Linker Flags
CFLAGS   := -cc1 -triple x86_64-pc-freebsd-elf -munwind-tables -I"$(OO_PS4_TOOLCHAIN)/include" -I"$(SDK)/include" -Iinclude -O2 -Wall -fcolor-diagnostics -std=gnu11 -fPIC -D__ORBIS__
ASFLAGS  := -target x86_64-pc-freebsd-elf -fPIC
LDFLAGS  := -m elf_x86_64 -pie --script "$(OO_PS4_TOOLCHAIN)/link.x" -e _init --eh-frame-hdr -L"$(OO_PS4_TOOLCHAIN)/lib" -L"$(SDK)"
LIBS     := -lSceLibcInternal -lGoldHEN_Hook -lkernel -lSceSysmodule -lSceSystemService -lScePad -lSceVideoOut

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(OUT_DIR)
	$(LD) $(LDFLAGS) $(OBJS) $(LIBS) -o $(OUT_DIR)/$(PROJ_NAME)
	$(OO_PS4_TOOLCHAIN)/bin/linux/create-fself -in=$(OUT_DIR)/$(PROJ_NAME) --out=$(TARGET) --lib=$(OUT_DIR)/$(PROJ_NAME).prx --paid=0x3800000000000011

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -emit-obj $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.s
	@mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)/$(PROJ_NAME) $(TARGET)
