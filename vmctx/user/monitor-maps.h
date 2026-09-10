/* SPDX-License-Identifier: GPL-2.0 */
/* The ownership table is monitor metadata. Other vmctx objects must never be
 * inherited through the monitor's mm when it creates an execution context. */
#ifndef VMCTX_MONITOR_MAPS_H
#define VMCTX_MONITOR_MAPS_H
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static int monitor_guest_maps(int metadata_fd, FILE *diagnostics)
{
	struct stat metadata;
	if (fstat(metadata_fd, &metadata)) return -1;
	FILE *maps = fopen("/proc/self/maps", "r");
	if (!maps) return -1;
	char *line = NULL;
	size_t capacity = 0;
	int count = 0, error = 0;
	while (getline(&line, &capacity, maps) >= 0) {
		unsigned dev_major, dev_minor;
		unsigned long long inode;
		if (!strstr(line, "/memfd:vmctx")) continue;
		if (sscanf(line, "%*s %*s %*s %x:%x %llu",
			   &dev_major, &dev_minor, &inode) != 3) {
			error = EPROTO; break;
		}
		/* Match the retained object, never its freely chosen name. */
		if (metadata.st_ino == inode && major(metadata.st_dev) == dev_major &&
		    minor(metadata.st_dev) == dev_minor) continue;
		count++;
		if (diagnostics)
			fprintf(diagnostics, "[vmremote] forbidden guest backing in monitor mm: %s", line);
	}
	if (!error && ferror(maps)) error = errno ? errno : EIO;
	free(line);
	fclose(maps);
	if (error) { errno = error; return -1; }
	return count;
}
#endif
