# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build Makefile for HFS & HFS+

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Include kernel configuration to check for KUnit support
-include $(KDIR)/include/config/auto.conf

export CONFIG_HFS_FS = m
export CONFIG_HFSPLUS_FS = m

# Only enable KUnit tests if the target kernel has CONFIG_KUNIT set (y or m)
ifneq ($(filter y m,$(CONFIG_KUNIT)),)
export CONFIG_HFS_KUNIT_TEST = m
export CONFIG_HFSPLUS_KUNIT_TEST = m
endif

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
