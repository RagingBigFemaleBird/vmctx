// SPDX-License-Identifier: GPL-2.0
/*
 * pk1 — a read of the program's memory must never CREATE the program's memory.
 *
 * NOT IN THE SUITE, AND HONEST ABOUT WHY. This builds the geometry exactly and
 * the clamp fires on all 200 rounds, so the shape is right -- but it PASSES with
 * the clamp switched off too, which means it does not discriminate and would go
 * green if the fix were reverted. It is kept as the record of the geometry and
 * run by hand. What it is missing is whatever lets the spill INSTALL a page in
 * firefox and not here: the kernel refuses a foreign read of an absent
 * anonymous page (vmctx_foreign_anon_fault, reached only from
 * do_anonymous_page) and that refusal catches this and not that. Measured here:
 * "CTL 7 ... INSTALLED IT" is 0 across a whole pk1 run and non-zero in firefox.
 * Finish that and this becomes a real test.
 *
 * The owner reads the program's address space with VMCTX_CTL_PEEK, which is
 * access_process_vm() underneath. The task that calls it is not the context, so
 * its fault is NOT redirected to the monitor: when the address is absent, the
 * local kernel simply installs a page. For an address whose current contents
 * live on the other machine that is not a read at all -- it invents a page here,
 * and the ownership record goes on saying the other machine holds it. Both
 * machines then have the page and neither is told, which is the one state this
 * whole protocol exists to prevent.
 *
 * Every caller of the peek knows this and guards: "read only when the page is
 * already here". The guard checks the address it was handed. What it cannot
 * check, not knowing the length, is the page AFTER that address -- and a read of
 * a path or a struct that begins near the end of a page reaches into the next
 * one. So a peek of a page this side legitimately holds silently manufactures
 * the page beside it.
 *
 * That is not a corner. It is what killed firefox: the loader's forwarded
 * openat() had its path 0x30 bytes below a page boundary, the trace's 96-byte
 * read of that path spilled into the next page -- the guest's live stack, just
 * handed over -- and invented it here. The very next forwarded read(2) put the
 * library header into the invented page, the guest read its own copy, and the
 * loader rejected a valid library:
 *
 *   CTL 7 over 0x7fffffff8fd0+96 ... presence 0 -> 1 <<< THIS CALL INSTALLED IT
 *   read(0x7, 0x7fffffff91e8, 0x340) -> 832
 *   -> libthai.so.0: invalid ELF header
 *
 * The geometry has to be the loader's, not merely similar, and the first version
 * of this test taught that the hard way: two fresh mmap()ed pages reproduce
 * nothing. A page neither machine has ever held is ABSENT rather than the
 * guest's, so the owner's touch of it faults, is redirected to the monitor, and
 * pulls the guest's bytes -- the protocol working exactly as it should. The
 * damage needs a page the guest is the established holder of, which on the
 * loader's path means THE STACK: pages that ping-pong, so that at the moment of
 * the spill the guest holds the current copy and the owner holds none.
 *
 * So this runs the loader's own shape, on the stack, in a loop:
 *
 *   1. A page boundary inside a large stack array. The path goes 48 bytes BELOW
 *      it, the read buffer 0x1e8 ABOVE it -- the offsets firefox died at.
 *   2. The buffer page is stamped, so the guest is its holder.
 *   3. A forwarded openat() of the path, which must fail. What matters is only
 *      that the owner reads the path afterwards, and whether that read stops at
 *      the end of the page it checked.
 *   4. A forwarded read(2) of known bytes into the buffer, performed by the
 *      owner.
 *   5. The guest reads the buffer back and must see the file's bytes.
 *
 * Round after round, because the state that does the damage is one phase of a
 * ping-pong and a single pass can land in the other. Any round that fails is a
 * failure: the owner wrote the bytes into a page it had invented for itself
 * while this side kept its own -- the firefox failure with the library replaced
 * by a byte pattern.
 *
 * The clamp has no switch: the arm that proved it was deleted with the proof
 * kept, so there is no compiled-in way to put the spill back.
 * Native this passes trivially: there is one address space and nothing to
 * invent.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>

#define PG	4096UL
#define FILL	0x5a		/* what the guest puts in B */
#define MARK	0xc3		/* what the file holds, and what must come back */
#define NMARK	64

#define ROUNDS	200

int main(void)
{
	/*
	 * Three pages of stack, so that a page boundary certainly falls inside
	 * and both the path and the buffer are ordinary stack addresses -- the
	 * mapping the loader uses, and the one that ping-pongs.
	 */
	volatile char big[3 * PG];
	uintptr_t base;
	char *path;
	unsigned char *buf;
	char tmpl[] = "/tmp/pk1XXXXXX";
	unsigned char want[NMARK];
	int fd, bad = 0, i, r, worst = -1;
	ssize_t got;

	/*
	 * The file the forwarded read will draw from. Written and closed before
	 * anything else happens, so its contents are not in question.
	 */
	fd = mkstemp(tmpl);
	if (fd < 0) {
		perror("mkstemp");
		return 2;
	}
	memset(want, MARK, sizeof(want));
	if (write(fd, want, sizeof(want)) != (ssize_t)sizeof(want)) {
		perror("write");
		return 2;
	}
	close(fd);

	/*
	 * The loader's geometry: the path 48 bytes below a page boundary (so a
	 * 96-byte read of it runs 48 bytes past), the buffer 0x1e8 above it, in
	 * the page that read would reach.
	 */
	base = ((uintptr_t)big + PG - 1) & ~(PG - 1);
	path = (char *)(base - 48);
	buf  = (unsigned char *)(base + 0x1e8);
	fprintf(stderr, "pk1: boundary=0x%lx path=%p buf=%p\n",
		(unsigned long)base, (void *)path, (void *)buf);

	for (r = 0; r < ROUNDS; r++) {
		/*
		 * Stamp the buffer page, so this side is its holder and its
		 * current bytes exist only here. Written through a volatile
		 * pointer so the compiler cannot decide the stores are dead
		 * because the read below overwrites them -- the whole point is
		 * that the stores and the read may land in different copies.
		 */
		for (i = 0; i < NMARK; i++)
			((volatile unsigned char *)buf)[i] = FILL;

		/*
		 * The forwarded call whose argument sits below the boundary. It
		 * must fail; the open is only a way to make the owner read the
		 * string, which is what may spill into the page above.
		 */
		snprintf(path, 48, "/nonexistent-pk1-%d", (int)getpid());
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			fprintf(stderr, "pk1: %s exists, which it must not\n",
				path);
			close(fd);
			return 2;
		}

		/*
		 * Now put known bytes into the buffer through the owner. read(2)
		 * is performed in the owner's address space; for this side to
		 * see the result, the owner must be writing the same page this
		 * side holds -- true unless the peek above invented one.
		 */
		fd = open(tmpl, O_RDONLY);
		if (fd < 0) {
			perror("open");
			return 2;
		}
		got = read(fd, buf, NMARK);
		close(fd);
		if (got != NMARK) {
			fprintf(stderr, "pk1: read returned %zd, wanted %d\n",
				got, NMARK);
			unlink(tmpl);
			return 2;
		}

		for (i = 0; i < NMARK; i++)
			if (((volatile unsigned char *)buf)[i] != MARK) {
				bad++;
				if (worst < 0)
					worst = r;
				break;
			}
	}
	unlink(tmpl);

	if (bad) {
		fprintf(stderr, "pk1: FAIL -- %d of %d rounds never saw the bytes "
			"the owner read into this page (first at round %d, "
			"buf[0]=0x%02x; 0x%02x is this side's own stamp, so the "
			"owner read into a page it had invented for itself "
			"while this side kept its own)\n",
			bad, ROUNDS, worst, buf[0], FILL);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
