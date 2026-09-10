/* SPDX-License-Identifier: GPL-2.0 */
/* A private file page is first read-only on the executor. After mprotect
 * grants writes, a source-kernel read must see the executor's new bytes. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    unsigned char seed[4096],observed[4096];
    for(unsigned i=0;i<sizeof(seed);i++)seed[i]=(unsigned char)(i*37+19);
    int fd=memfd_create("readonly-upgrade",MFD_CLOEXEC),channel[2];
    if(fd<0 || write(fd,seed,sizeof(seed))!=(ssize_t)sizeof(seed) || pipe(channel)) {
        perror("setup");return 1;
    }
    volatile unsigned char *page=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fd,0);
    if(page==MAP_FAILED) {perror("mmap");return 1;}
    for(unsigned i=0;i<sizeof(seed);i++)if(page[i]!=seed[i]) {
        fprintf(stderr,"FAIL: initial read at %u got %02x expected %02x\n",i,page[i],seed[i]);return 1;
    }
    if(mprotect((void *)page,4096,PROT_READ|PROT_WRITE)) {perror("mprotect");return 1;}
    for(unsigned i=0;i<sizeof(seed);i++)page[i]=seed[i]^0xa5;
    if(write(channel[1],(const void *)page,4096)!=4096 || read(channel[0],observed,4096)!=4096) {
        perror("source readback");return 1;
    }
    for(unsigned i=0;i<sizeof(seed);i++)if(observed[i]!=(unsigned char)(seed[i]^0xa5)) {
        fprintf(stderr,"FAIL: source read after read-only upgrade at %u got %02x expected %02x (initial %02x, executor %02x)\n",
            i,observed[i],seed[i]^0xa5,seed[i],page[i]);return 1;
    }
    /* MAP_PRIVATE writes must also preserve the original file bytes. */
    if(pread(fd,observed,4096,0)!=4096 || memcmp(seed,observed,4096)) {
        fputs("FAIL: private modification reached original file\n",stderr);return 1;
    }
    munmap((void *)page,4096);close(channel[0]);close(channel[1]);close(fd);
    puts("PASS: read-only private page upgrade preserves source read coherence and private file isolation");
    return 0;
}
