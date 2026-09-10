// SPDX-License-Identifier: GPL-2.0
/* Adjacent one-byte source writes must preserve each other and guest stores,
 * including locked anonymous memory used by secure allocators. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
 for(unsigned locked=0;locked<2;locked++) {
  unsigned char *p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  int fd[2];if(p==MAP_FAILED||pipe(fd)||(locked&&mlock(p,4096)))return 2;
  memset(p,0xa5,4096);if(write(fd[1],"pw\n",3)!=3)return 2;close(fd[1]);
  for(unsigned i=0;i<3;i++) {
   ssize_t r=read(fd[0],p+8+i,1);
   if(r!=1||p[8+i]!=(unsigned char)"pw\n"[i]||memcmp(p+8,"pw\n",i+1)||p[7]!=0xa5||p[9+i]!=0xa5) {
    printf("FAIL: byte read locked=%u i=%u returned=%zd bytes=%02x:%02x:%02x:%02x:%02x\n",locked,i,r,p[7],p[8],p[9],p[10],p[11]);return 1;
   }
  }
  if(read(fd[0],p+11,1)!=0||p[11]!=0xa5)return 1;
  close(fd[0]);munmap(p,4096);
 }
 puts("PASS: adjacent byte reads preserve source writes and guest stores in locked/unlocked memory");return 0;
}
