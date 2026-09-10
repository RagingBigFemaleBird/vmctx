/*
 * Do forwarded syscalls run with the guest's credentials?
 *
 * This is the question bwrap answers by accident. bwrap unshares a user
 * namespace and is then nobody, so a path only the owner can traverse becomes
 * unreachable and it gives up -- which is the whole reason the browser gets past
 * its image loader natively. If the same check succeeds under vmctx, the sandbox
 * proceeds down a path that native never takes, and the difference is not
 * bwrap's: it is that a permission check was answered by a process holding
 * different credentials from the one that asked.
 *
 * unshare is forwarded, so the shadow enters the new user namespace; the guest
 * task keeps the credentials it had on the machine running it. Whichever of those
 * answers a check decides the result, so this asks for the same file twice and
 * prints what each attempt got. Run it natively and under vmctx: the two must
 * agree.
 *
 * Note what the file test does *not* prove. Natively, /etc/shadow stays readable
 * after the unshare, and that is correct rather than surprising: in a user
 * namespace with no mapping written, an unmapped owner appears as the overflow
 * uid, which is also what this process now is, so the owner bits apply. The line
 * that carries the information is the euid, and the result of the unshare itself.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>

#define SECRET "/etc/shadow"		/* root-only; 640 root:shadow */

static const char *try_read(const char *path)
{
	static char msg[96];
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		snprintf(msg, sizeof(msg), "refused (%s)", strerror(errno));
		return msg;
	}
	close(fd);
	return "allowed";
}

int main(void)
{
	uid_t before = geteuid();

	printf("as euid %d, %s is %s\n", (int)before, SECRET, try_read(SECRET));
	fflush(stdout);

	if (unshare(CLONE_NEWUSER) < 0) {
		/*
		 * Not a skip: on this path the guest cannot have a user
		 * namespace at all, which is a difference from native and the
		 * thing this test is for. EINVAL here means the side that ran
		 * the call had more than one thread -- the kernel refuses
		 * CLONE_NEWUSER to a threaded process, and a shadow always has
		 * a fault thread.
		 */
		printf("UNSHARE-REFUSED CLONE_NEWUSER: %s%s\n",
		       strerror(errno),
		       errno == EINVAL ? " (the caller was multi-threaded)" : "");
		fflush(stdout);
		return 0;
	}
	printf("after unshare(CLONE_NEWUSER), euid is %d, %s is %s\n",
	       (int)geteuid(), SECRET, try_read(SECRET));

	/*
	 * The verdict in one line, so the two machines can be compared without
	 * reading uids: inside a fresh user namespace with no mapping written,
	 * nothing privileged may be read.
	 */
	if (geteuid() == before && before == 0)
		printf("CREDS-UNCHANGED still euid 0 inside a new user namespace\n");
	else
		printf("CREDS-DROPPED euid %d inside the new user namespace\n",
		       (int)geteuid());
	fflush(stdout);
	return 0;
}
