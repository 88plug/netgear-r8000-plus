# Out-of-tree Kbuild Makefile for fa_probe.ko
#
# Standard "obj-m" out-of-tree module pattern, built against the kernel
# build tree already prepared inside the bcm53xx/generic OpenWrt SDK
# (Linux 6.12.94, arm_cortex-a9_musl_eabi) - the same SDK/toolchain
# already used in this session to build kmod-brcmfmac.
#
# Usage (builds fa_probe.ko - does NOT install or load it anywhere):
#   make
#   make clean
#
# Override SDK_ROOT/KDIR/TOOLCHAIN_DIR/CROSS_COMPILE on the command line
# if this tree is relocated.

SDK_ROOT      ?= /home/andrew/netgearr8000/openwrt-sdk-25.12.5-bcm53xx-generic_gcc-14.3.0_musl_eabi.Linux-x86_64
KDIR          ?= $(SDK_ROOT)/build_dir/target-arm_cortex-a9_musl_eabi/linux-bcm53xx_generic/linux-6.12.94
TOOLCHAIN_DIR ?= $(SDK_ROOT)/staging_dir/toolchain-arm_cortex-a9_gcc-14.3.0_musl_eabi

# Matches CONFIG_CC_VERSION_TEXT in $(KDIR)/.config, i.e. the exact
# compiler the running kernel itself was built with.
ARCH          ?= arm
CROSS_COMPILE ?= arm-openwrt-linux-muslgnueabi-

PWD := $(shell pwd)
export PATH := $(TOOLCHAIN_DIR)/bin:$(PATH)

obj-m := fa_probe.o fa_bringup.o fa_macc_test.o fa_switch_oobpause_test.o fa_accel.o fa_napt_row_test.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean

.PHONY: all clean
