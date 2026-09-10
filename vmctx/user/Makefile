# Build the vmctx userspace tools.
#
# Static so the binaries run on the Alpine/musl netboot target regardless
# of the build host's libc (a static glibc binary runs on any Linux).
# The kernel UAPI header is shared from ../kernel.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -static -fno-stack-protector -pthread
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
override CPPFLAGS += -I../kernel -DVMCTX_SRC_ID=\"$(SRCID)\"
shell_quote = '$(subst ','"'"',$(1))'

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

# Check content and the effective compile command on every invocation. A
# timestamp prerequisite misses rsync-preserved mtimes and A -> B -> A source
# rollbacks. Publish the executable only after a successful link; a failed
# rebuild must leave the previous executable and its identity together.
$(addprefix $(BUILD)/,$(TOOLS)): $(BUILD)/%: %.c
$(addprefix $(BUILD)/,$(TOOLS) vmremote-local): FORCE | $(BUILD)
	@set -eu; \
	work=$$(mktemp -d "$@.tmp.XXXXXX"); \
	trap 'rm -f "$$work/binary" "$$work/inputs"; rmdir "$$work"' 0; \
	trap 'exit 130' INT; trap 'exit 143' TERM; \
	{ printf '%s\n' '$(SRCID)' $(call shell_quote,CC=$(CC)) \
	    $(call shell_quote,CPPFLAGS=$(CPPFLAGS)) $(call shell_quote,CFLAGS=$(CFLAGS)) \
	    $(call shell_quote,LDFLAGS=$(LDFLAGS)) $(call shell_quote,LDLIBS=$(LDLIBS)); \
	  sha256sum Makefile; $(CC) --version; } > "$$work/inputs"; \
	if [ -x "$@" ] && cmp -s "$$work/inputs" "$@.build-inputs"; then exit 0; fi; \
	printf '%s\n' 'building $@ (source $(SRCID))'; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$$work/binary" $(filter %.c,$^) $(LDLIBS); \
	mv -f "$$work/binary" "$@"; \
	mv -f "$$work/inputs" "$@.build-inputs"

clean:
	rm -f $(addprefix $(BUILD)/,$(TOOLS) vmremote-local) \
	      $(addsuffix .build-inputs,$(addprefix $(BUILD)/,$(TOOLS) vmremote-local))

FORCE:
.PHONY: all clean FORCE

# vmremote maps a home-provided ET_EXEC at its link address (typically
# 0x400000), so vmremote itself must live somewhere else or it would be
# overwritten by the program it is loading.
$(BUILD)/vmremote: override LDFLAGS += -Wl,-Ttext-segment=0x20000000

# AMD loopback uses the source kernel's ABI. Keep this executable separate
# from the Intel destination binary and include the ABI flags in its inputs.
$(BUILD)/vmremote-local: vmremote.c
$(BUILD)/vmremote-local: override CPPFLAGS += -D__NR_vmctx_run=472 -D__NR_vmctx_ctl=473
$(BUILD)/vmremote-local: override LDFLAGS += -Wl,-Ttext-segment=0x20000000

# vmhome materialises the guest's address space in a shadow process, so it must
# not itself occupy those addresses.
$(BUILD)/vmhome: override LDFLAGS += -Wl,-Ttext-segment=0x28000000

print-tools:
	@echo $(TOOLS)

print-srcid:
	@echo $(SRCID)

.PHONY: print-tools print-srcid

# Real monitor helpers exercised with native sockets/cache entries; no vmctx
# kernel or remote machine is needed. Always compile against the current code.
check-host: | $(BUILD)
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/diagnostic-controls.XXXXXX"); \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$$work/monitor-diagnostics" ../tests/control/monitor-diagnostics.c $(LDLIBS); \
	for mode in inherit saturation sink-error; do \
	  python3 ../tests/owned-run.py --report "$$work/$$mode.json" --timeout 20 "$$work/monitor-diagnostics" "$$mode"; \
	done
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/host-controls.XXXXXX"); \
	trap 'rm -rf "$$work"' 0; \
	trap 'exit 130' INT; trap 'exit 143' TERM; \
	for control in page-reply memory-reply page-receipt source-bound-helpers source-transfer ack-episode page-landing socket-send retained-range cpu-wire shared-pages child-pages source-snapshot settle-failures monitor-maps page-transfers fault-dispatch execution-fds execution-mm execution-target memory-identity source-memory-target; do \
	  $(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
	    -o "$$work/$$control" "../tests/control/$$control.c" $(LDLIBS); \
	  "$$work/$$control"; \
	  if [ "$$control" = source-bound-helpers ]; then "$$work/$$control" bulk; fi; \
	done; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
	  -o "$$work/source-record" ../tests/control/source-record.c $(LDLIBS); \
	for mode in monotonic terminal errors allocation history; do "$$work/source-record" "$$mode"; done
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/permission-controls.XXXXXX"); \
	trap 'rm -rf "$$work"' 0; \
	trap 'exit 130' INT; trap 'exit 143' TERM; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
	  -o "$$work/write-permissions" ../tests/control/write-permissions.c $(LDLIBS); \
	for mode in recall cow-denied; do "$$work/write-permissions" "$$mode"; done
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/lifetime-controls.XXXXXX"); \
	for control in snapshot-spent permission-exit mapping-retirement; do \
	  $(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$$work/$$control" "../tests/control/$$control.c" $(LDLIBS); \
	done; \
	for mode in spent owed missing; do \
	  python3 ../tests/owned-run.py --report "$$work/snapshot-$$mode.json" --timeout 20 "$$work/snapshot-spent" "$$mode"; \
	done; \
	for mode in replace diagnostic; do \
	  python3 ../tests/owned-run.py --report "$$work/mapping-$$mode.json" --timeout 20 "$$work/mapping-retirement" "$$mode"; \
	done; \
	for mode in ended live owner error; do \
	  python3 ../tests/owned-run.py --report "$$work/permission-$$mode.json" --timeout 20 "$$work/permission-exit" "$$mode"; \
	done
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/admission-controls.XXXXXX"); \
	trap 'rm -rf "$$work"' 0; \
	trap 'exit 130' INT; trap 'exit 143' TERM; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
	  -o "$$work/source-admission-binding" ../tests/control/source-admission-binding.c $(LDLIBS); \
	for mode in binding-drift args-drift construction publication birth completion exec; do "$$work/source-admission-binding" "$$mode"; done
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/claim-controls.XXXXXX"); \
	trap 'rm -rf "$$work"' 0; \
	trap 'exit 130' INT; trap 'exit 143' TERM; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$$work/claim-landing" ../tests/control/claim-landing.c $(LDLIBS); \
	for mode in claim mm-identity; do "$$work/claim-landing" "$$mode"; done
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/ack-controls.XXXXXX"); \
	trap 'rm -rf "$$work"' 0; \
	trap 'exit 130' INT; trap 'exit 143' TERM; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$$work/ack-confirm" ../tests/control/ack-confirm.c $(LDLIBS); \
	for mode in accepted eof rejected wrong-target payload; do "$$work/ack-confirm" "$$mode"; done
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/snapshot-controls.XXXXXX"); \
	trap 'rm -rf "$$work"' 0; \
	trap 'exit 130' INT; trap 'exit 143' TERM; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$$work/snapshot-readonly" ../tests/control/snapshot-readonly.c $(LDLIBS); \
	for mode in readonly executable shared; do "$$work/snapshot-readonly" "$$mode"; done
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/storage-controls.XXXXXX"); \
	trap 'rm -rf "$$work"' 0; \
	trap 'exit 130' INT; trap 'exit 143' TERM; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
	  -o "$$work/execution-storage" ../tests/control/execution-storage.c $(LDLIBS); \
	for mode in discard preview region landing; do "$$work/execution-storage" "$$mode"; done
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/journal-controls.XXXXXX"); \
	trap 'rm -rf "$$work"' 0; \
	trap 'exit 130' INT; trap 'exit 143' TERM; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
	  -o "$$work/access-journal" ../tests/control/access-journal.c $(LDLIBS); \
	for mode in callback callback-newer concurrent malformed failed-ack; do "$$work/access-journal" "$$mode"; done
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/exception-controls.XXXXXX"); \
	trap 'rm -rf "$$work"' 0; \
	trap 'exit 130' INT; trap 'exit 143' TERM; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$$work/source-exception" ../tests/control/source-exception.c $(LDLIBS); \
	for mode in runtime setcpu ended getcpu delayed live malformed handler drift; do "$$work/source-exception" "$$mode"; done; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$$work/executor-exception" ../tests/control/executor-exception.c $(LDLIBS); \
	for mode in ended handler legal wrong-mm wrong-task bad-ended bad-status payload stop-failed; do "$$work/executor-exception" "$$mode"; done
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/origin-controls.XXXXXX"); \
	trap 'rm -rf "$$work"' 0; \
	trap 'exit 130' INT; trap 'exit 143' TERM; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$$work/fault-page-origin" ../tests/control/fault-page-origin.c $(LDLIBS); \
	for mode in local-cow local-cow-region source source-region source-object source-region-object source-loan-map watched-local watched-region watched-region-exec watched-local-write watched-region-write source-no-landing source-region-no-landing; do "$$work/fault-page-origin" "$$mode"; done

.PHONY: check-host

# Actual executor tables, compiled once and isolated per case. Keep each run's
# binary, output, and owned-process cleanup record for diagnosis and replay.
TABLE_CONTROLS = mm-cache-identity cow-collisions cow-capacity cow-retirement \
        cow-alloc-failure cow-concurrent execution-descriptors execution-object-stdio \
        acknowledgement-transport transfer-old-target transfer-busy-gets transfer-busy-cow transfer-busy-get \
        mapping-effects construction-capacity construction-partial construction-overlap \
        construction-alloc-failure construction-concurrent loan-delete loan-identity \
        installed-delete installed-identity installed-normalize installed-capacity \
        installed-collisions installed-alloc-failure installed-scale installed-retirement \
        installed-concurrent loan-collisions loan-capacity loan-concurrent \
        loan-alloc-failure loan-snapshot transfer-exclusion loan-scale

check-tables: | $(BUILD)
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/table-controls.XXXXXX"); \
	printf 'table control artifacts: %s\n' "$$work"; \
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections $(LDFLAGS) \
	  -Wl,--gc-sections -o "$$work/page-tables" ../tests/control/page-tables.c $(LDLIBS); \
	sha256sum "$$work/page-tables" > "$$work/binary.sha256"; \
	for control in $(TABLE_CONTROLS); do \
	  if python3 ../tests/owned-run.py --report "$$work/$$control.json" --timeout 45 \
	       "$$work/page-tables" "$$control" > "$$work/$$control.log" 2>&1; then \
	    cat "$$work/$$control.log"; \
	  else cat "$$work/$$control.log"; exit 1; fi; \
	done

.PHONY: check-tables

# Real shared slots and wire acknowledgements, including a private decoy at
# the same guest address. A successful landing must update every shared alias.
check-shared-settlement: | $(BUILD)
	@set -eu; \
	work=$$(mktemp -d "$(BUILD)/shared-settlement-controls.XXXXXX"); \
	printf 'shared settlement artifacts: %s\n' "$$work"; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$$work/settle-shared" ../tests/control/settle-shared.c $(LDLIBS); \
	for mode in bytes absent hole short readonly replaced read-slot read-hole; do \
	  python3 ../tests/owned-run.py --report "$$work/$$mode.json" --timeout 20 "$$work/settle-shared" "$$mode"; \
	done

check-host: check-shared-settlement
.PHONY: check-shared-settlement
