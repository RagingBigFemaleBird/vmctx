/*
 * fm1 -- a private file mapping, which is the case the destination used to
 * intercept: it read the fd argument, made the mapping anonymous locally, and
 * served the file's pages itself. Nothing in the suite covered it, because the
 * only program that did it was the dynamic loader and the loader runs at the
 * source. So: map a file the guest cannot see, and check the bytes against a
 * read of the same descriptor.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdlib.h>

static int bad;

static void one(const char *path, size_t want)
{
	struct stat st;
	static char scratch[65536];
	char *m, *buf;
	size_t len;
	int fd = open(path, O_RDONLY);

	/*
	 * A file this machine does not have is not a result. The list below is
	 * of files that happen to exist on the boxes this has been run on, and
	 * one of them being absent says nothing about the mapping code -- it
	 * used to be counted as a failure, which made fm1 report a fault in
	 * vmctx on any machine without /usr/bin/links installed.
	 */
	if (fd < 0) {
		printf("%-28s skip  not on this machine\n", path);
		return;
	}
	if (fstat(fd, &st) != 0) {
		printf("%-28s FAIL  cannot stat\n", path);
		bad++;
		close(fd);
		return;
	}
	len = (size_t)st.st_size;
	if (len > sizeof(scratch))
		len = sizeof(scratch);
	if (want && len > want)
		len = want;
	m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	if (m == MAP_FAILED) {
		printf("%-28s FAIL  mmap\n", path);
		bad++;
		close(fd);
		return;
	}
	/* Written by the guest before the source fills it. A buffer the guest
	 * has never touched is a separate problem (a forwarded read faulting on
	 * a page nobody has), and this test is about the mapping. */
	buf = scratch;
	memset(buf, 0, len);
	if (pread(fd, buf, len, 0) != (ssize_t)len) {
		printf("%-28s FAIL  pread short\n", path);
		bad++;
	} else if (memcmp(m, buf, len) != 0) {
		size_t i = 0;

		while (i < len && m[i] == buf[i])
			i++;
		printf("%-28s FAIL  differs at +%zu: map %02x read %02x\n",
		       path, i, (unsigned char)m[i], (unsigned char)buf[i]);
		bad++;
	} else {
		printf("%-28s ok    %zu bytes match the file\n", path, len);
	}
	/* And a second mapping at an offset, which is how a loader maps a
	 * segment: the bytes must be the file's at that offset, not the
	 * mapping's own. */
	if (len > 8192) {
		char *m2 = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 4096);

		if (m2 == MAP_FAILED || memcmp(m2, buf + 4096, 4096) != 0) {
			printf("%-28s FAIL  offset mapping wrong\n", path);
			bad++;
		} else {
			printf("%-28s ok    offset 0x1000 matches too\n", path);
		}
	}
	munmap(m, len);
	close(fd);
}

int main(void)
{
	/*
	 * Unbuffered, for the reason the other tests use write(2): a case that
	 * kills the context takes a buffer with it, and how far it got is the
	 * one thing worth knowing.
	 */
	setvbuf(stdout, NULL, _IONBF, 0);
	/* A file smaller than a page, and a file of many pages that is also an
	 * ELF object -- which is what a loader maps and what the removed arm
	 * existed for. Both are the *source's* files; the guest cannot see
	 * either machine's disk except through the calls it makes. */
	one("/etc/hostname", 0);
	/*
	 * The first of these that exists. Naming one file made the case depend
	 * on what happened to be installed; what it actually needs is any ELF
	 * object of more than two pages, and every machine has one.
	 */
	{
		static const char *const elf[] = {
			"/usr/bin/links", "/bin/bash", "/usr/bin/gcc",
			"/bin/ls", NULL
		};
		int i;

		for (i = 0; elf[i]; i++)
			if (access(elf[i], R_OK) == 0)
				break;
		if (elf[i])
			one(elf[i], 65536);
		else
			printf("%-28s skip  no ELF object readable\n", "(elf)");
	}
	printf(bad ? "FAIL\n" : "PASS\n");
	return bad ? 1 : 0;
}
