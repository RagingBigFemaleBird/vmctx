// SPDX-License-Identifier: GPL-2.0
/* Growing a shared object must preserve its existing aliases and must never
 * overwrite a different object allocated beside it in the backing store. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#define PAGE 4096
int main(void) {
 int a=memfd_create("grow-a",MFD_CLOEXEC),b=memfd_create("grow-neighbor",MFD_CLOEXEC);
 if(a<0||b<0||ftruncate(a,PAGE)||ftruncate(b,PAGE))return 2;
 uint64_t *first=mmap(NULL,PAGE,PROT_READ|PROT_WRITE,MAP_SHARED,a,0);
 uint64_t *neighbor=mmap(NULL,PAGE,PROT_READ|PROT_WRITE,MAP_SHARED,b,0);
 if(first==MAP_FAILED||neighbor==MAP_FAILED)return 2;
 for(unsigned i=0;i<PAGE/8;i++) {first[i]=0xaaaaaaaa00000000ULL+i;neighbor[i]=0xbbbbbbbb00000000ULL+i;}
 if(ftruncate(a,4*PAGE))return 2;
 uint64_t *middle=mmap(NULL,2*PAGE,PROT_READ|PROT_WRITE,MAP_SHARED,a,PAGE);
 uint64_t *whole=mmap(NULL,4*PAGE,PROT_READ|PROT_WRITE,MAP_SHARED,a,0);
 if(middle==MAP_FAILED||whole==MAP_FAILED)return 2;
 for(unsigned i=0;i<2*PAGE/8;i++)middle[i]=0xcdef000000000000ULL+i;
 for(unsigned i=0;i<PAGE/8;i++) {
  if(whole[i]!=first[i]||neighbor[i]!=0xbbbbbbbb00000000ULL+i)goto fail;
  if(whole[3*PAGE/8+i])goto fail;
 }
 for(unsigned i=0;i<2*PAGE/8;i++)if(whole[PAGE/8+i]!=0xcdef000000000000ULL+i)goto fail;
 whole[1]=0x123456789abcdefULL;if(first[1]!=whole[1])goto fail;
 puts("PASS: shared growth preserves old/new overlapping aliases and neighboring objects");return 0;
fail:
 fputs("FAIL: shared object growth changed alias identity or adjacent object\n",stderr);return 1;
}
