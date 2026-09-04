# Out-of-tree build of the vmctx module against the LAN-boot kernel tree.
# The kernel is at ../../src/linux-6.18.35 (same tree the booted image came
# from), so the resulting vmctx.ko has a matching vermagic.

obj-m += vmctx.o

KDIR ?= $(abspath $(CURDIR)/../../src/linux-6.18.35)

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
