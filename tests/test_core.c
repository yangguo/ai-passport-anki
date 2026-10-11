#include "passport_anki.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static pa_collection c;
static void setup(void) { pa_init(&c); assert(pa_add(&c,1,"English","你好","hello")); }
int main(void) {
 setup(); pa_session s; pa_begin(&s,&c,"English"); assert(s.current==0);
 assert(!pa_grade(&c,&s,PA_GOOD)); assert(c.cards[0].stage==PA_NEW);
 assert(pa_flip(&s)); assert(pa_grade(&c,&s,PA_AGAIN));
 assert(!s.revealed && !pa_flip(&s) && !pa_grade(&c,&s,PA_AGAIN));
 assert(c.cards[0].due==1 && c.cards[0].stage==PA_LEARNING);
 assert(pa_next(&c,"English")==PA_NO_CARD); assert(pa_advance(&c,1));
 pa_begin(&s,&c,"English"); assert(pa_flip(&s)); assert(pa_grade(&c,&s,PA_GOOD));
 assert(c.cards[0].due==11); assert(pa_advance(&c,10));
 pa_begin(&s,&c,"English"); assert(pa_flip(&s)); assert(pa_grade(&c,&s,PA_GOOD));
 assert(c.cards[0].stage==PA_REVIEW && c.cards[0].interval==1);
 setup();assert(pa_add(&c,2,"English","q2","a2"));assert(pa_add(&c,3,"English","q3","a3"));
 c.cards[1].stage=PA_REVIEW;c.cards[1].interval=1;
 c.cards[2].stage=PA_LEARNING;c.cards[2].step=1;
 assert(pa_next(&c,"English")==2); /* learning before review before new */
 c.cards[2].stage=PA_REVIEW;c.cards[2].interval=1;
 assert(pa_next(&c,"English")==1); /* equal stage and due uses the smaller ID */
 uint32_t delays[4];
 for(int r=0;r<4;r++) { setup();c.cards[0].stage=PA_REVIEW;c.cards[0].interval=10;
 pa_begin(&s,&c,"English");assert(pa_flip(&s));assert(pa_grade(&c,&s,(pa_rating)r));delays[r]=c.cards[0].due;
 if(r==0) assert(c.cards[0].lapses==1); }
 assert(delays[0]<delays[1] && delays[1]<delays[2] && delays[2]<delays[3]);
 setup();assert(pa_add(&c,2,"Other","a","b"));assert(pa_next(&c,"Missing")==PA_NO_CARD);
 assert(!pa_add(&c,1,"Other","a","b"));assert(!pa_add(&c,3,"","a","b"));
 assert(!pa_add(&c,3,"D","\xc0\xaf","b"));assert(!pa_add(&c,3,"D","\xed\xa0\x80","b"));
 assert(!pa_text_valid("bad\1text", 32));assert(pa_text_valid("line1\nline2", 32));
 assert(!pa_text_valid("\xf4\x90\x80\x80", 32));assert(!pa_text_valid("\xe2\x82", 32));
 assert(!pa_add(&c,0,"D","x","y"));
 char longtext[PA_TEXT_BYTES+1];memset(longtext,'x',sizeof(longtext));longtext[PA_TEXT_BYTES]=0;
 assert(!pa_add(&c,3,"D",longtext,"b"));
 for(uint32_t i=3;i<=PA_MAX_CARDS;i++) assert(pa_add(&c,i,"D","x","y"));
 assert(!pa_add(&c,99,"D","x","y"));
 c.now=UINT32_MAX;assert(!pa_advance(&c,1));pa_begin(&s,&c,"English");assert(pa_flip(&s));
 pa_card before=c.cards[s.current];assert(!pa_grade(&c,&s,PA_GOOD));assert(memcmp(&before,&c.cards[s.current],sizeof(before))==0);
 setup(); int completed=0; bool overflow_seen=false;
 for(int i=0;i<10000;i++) {
   c.now=c.cards[0].due;pa_begin(&s,&c,"English");assert(pa_flip(&s));
   pa_card before=c.cards[0];
   if(!pa_grade(&c,&s,PA_EASY)) {
     assert(memcmp(&before,&c.cards[0],sizeof(before))==0);
     overflow_seen=true;break;
   }
   assert(c.cards[0].interval<=PA_MAX_INTERVAL);completed++;
 }
 assert(overflow_seen && completed>0 && completed<10000);
 setup(); pa_session repeated={.current=0,.revealed=true};
 for(int i=0;i<10000;i++) {
   c.now=0;repeated.current=0;repeated.revealed=true;
   assert(pa_grade(&c,&repeated,PA_EASY));
   assert(c.cards[0].interval<=PA_MAX_INTERVAL);
 }
 assert(c.cards[0].reviews==10000);
 c.cards[0].reviews=UINT16_MAX;repeated.current=0;repeated.revealed=true;
 assert(pa_grade(&c,&repeated,PA_EASY));assert(c.cards[0].reviews==UINT16_MAX);
 puts("core: scheduler, UTF-8, capacity, and safe clock overflow PASS");return 0;
}
