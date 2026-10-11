#include "passport_anki.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static pa_collection a,b,original;
static uint8_t buf[PA_SNAPSHOT_MAX];
static void rewrite_crc(size_t n) {
 uint32_t crc=pa_crc32(buf,n-4);
 buf[n-4]=(uint8_t)crc;buf[n-3]=(uint8_t)(crc>>8);
 buf[n-2]=(uint8_t)(crc>>16);buf[n-1]=(uint8_t)(crc>>24);
}
static void put_u32(size_t offset,uint32_t value) {
 buf[offset]=(uint8_t)value;buf[offset+1]=(uint8_t)(value>>8);
 buf[offset+2]=(uint8_t)(value>>16);buf[offset+3]=(uint8_t)(value>>24);
}
int main(void) {
 pa_init(&a);assert(pa_add(&a,1,"语言","你好","hello"));
 a.now=123;a.cards[0].stage=PA_REVIEW;a.cards[0].interval=30;a.cards[0].due=456;
 size_t n=pa_encode(&a,buf,sizeof(buf));assert(n>0);assert(pa_decode(&b,buf,n));
 assert(pa_encode(&a,buf,n)==n);
 assert(b.now==123 && b.count==1 && b.cards[0].due==456);assert(strcmp(b.cards[0].front,"你好")==0);
 original=b;for(size_t i=0;i<n;i++) {assert(!pa_decode(&b,buf,i));assert(memcmp(&b,&original,sizeof(b))==0);}
 for(size_t i=0;i<n;i++) { buf[i]^=1;assert(!pa_decode(&b,buf,n));assert(memcmp(&b,&original,sizeof(b))==0);buf[i]^=1; }
 assert(pa_encode(&a,buf,1)==0); assert(pa_crc32((const uint8_t *)"123456789",9)==0xcbf43926u);
 assert(memcmp(buf,"PAK1",4)==0);
 buf[28]=99;rewrite_crc(n);assert(!pa_decode(&b,buf,n));assert(memcmp(&b,&original,sizeof(b))==0);
 buf[28]=PA_REVIEW;rewrite_crc(n);
 buf[29]=1;rewrite_crc(n);assert(!pa_decode(&b,buf,n));assert(memcmp(&b,&original,sizeof(b))==0);
 buf[29]=0;rewrite_crc(n);
 buf[22]=0;buf[23]=0;rewrite_crc(n);assert(!pa_decode(&b,buf,n));assert(memcmp(&b,&original,sizeof(b))==0);
 buf[22]=(uint8_t)2500;buf[23]=(uint8_t)(2500>>8);rewrite_crc(n);
 pa_init(&a);for(uint32_t i=1;i<=PA_MAX_CARDS;i++) assert(pa_add(&a,i,"D","a","b"));
 n=pa_encode(&a,buf,sizeof(buf));assert(n && pa_decode(&b,buf,n) && b.count==PA_MAX_CARDS);
 put_u32(39,1);rewrite_crc(n);assert(!pa_decode(&b,buf,n));assert(b.count==PA_MAX_CARDS);
 put_u32(39,2);rewrite_crc(n);assert(pa_decode(&b,buf,n));
 a.cards[1].id=a.cards[0].id;assert(pa_encode(&a,buf,sizeof(buf))==0);
 puts("codec: roundtrip, every truncation, every byte corruption, atomic decode PASS");return 0;
}
