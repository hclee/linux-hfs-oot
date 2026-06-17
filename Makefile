# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build Makefile for HFS & HFS+

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

export CONFIG_HFS_FS = m
export CONFIG_HFSPLUS_FS = m
export CONFIG_HFS_KUNIT_TEST = m
export CONFIG_HFSPLUS_KUNIT_TEST = m

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
