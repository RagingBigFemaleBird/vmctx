# Build the vmctx userspace tools.
#
# Static so the binaries run on the Alpine/musl netboot target regardless
# of the build host's libc (a static glibc binary runs on any Linux).
# The kernel UAPI header is shared from ../kernel.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -static -fno-stack-protector -pthread -I../kernel
BUILD   ?= ../build

# A fingerprint of the sources this binary is built from, compiled into it and
# printed at start-up by both halves.
#
# Timestamps cannot do this job here. rsync preserves mtimes, so a source just
# synced from home looks OLDER than the binary next to it, make decides there
# is nothing to do, and the run silently measures the previous change. Content
# is the only honest answer, so the id is a hash of the content.
SRCID := $(shell cat $(sort $(wildcard *.c *.h ../kernel/*.h)) 2>/dev/null | \
                 sha256sum | cut -c1-12)
CFLAGS += -DVMCTX_SRC_ID=\"$(SRCID)\"

# startvm, vmenter, vmcat and vmproc are deliberately absent.
#
# Each of them ran a guest whose syscalls the LOCAL kernel dispatched, with no
# monitor anywhere. That mode no longer exists: a context's syscalls belong to
# the machine that owns the program, so the module answers an unserviced
# syscall with -EIO rather than running it here. The four tools therefore died
# on their first call (#UD -> -EIO -> exit 251, 0 syscalls, 0 faults), and a
# tool that advertises a mode the design has removed is worse than no tool.
# The supported way to run a program is the vmhome/vmremote pair.
TOOLS = dumpvm vmselftest vmmon vmremote vmhome vmmig \
        vmthreads vmbench vmtake vmsys

all: $(addprefix $(BUILD)/,$(TOOLS))

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%: %.c common.h vmrproto.h ../kernel/vmctx_uapi.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(addprefix $(BUILD)/,$(TOOLS))

.PHONY: all clean

# vmremote maps a home-provided ET_EXEC at its link address (typically
# 0x400000), so vmremote itself must live somewhere else or it would be
# overwritten by the program it is loading.
$(BUILD)/vmremote: CFLAGS += -Wl,-Ttext-segment=0x20000000

# vmhome materialises the guest's address space in a shadow process, so it must
# not itself occupy those addresses.
$(BUILD)/vmhome: CFLAGS += -Wl,-Ttext-segment=0x28000000

print-tools:
	@echo $(TOOLS)

print-srcid:
	@echo $(SRCID)

.PHONY: print-tools print-srcid
