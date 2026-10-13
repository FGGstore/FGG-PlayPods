/* Real isolated AVDTP command/parser, synthetic time and peer traffic.
 * Late responses are a model, not a claim about hardware delivery. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static long ticks;
static int fake_clock(clockid_t id,struct timespec *ts)
{(void)id;ts->tv_sec=ticks/1000;ts->tv_nsec=ticks%1000*1000000;return 0;}
#define clock_gettime fake_clock
#include A2DP_TEST_SOURCE
#undef clock_gettime
#include "sdp.h"

static int sends,waits,windows[3],lost,fail_send,peer_sdp,delivered,wrong_delivered;
static unsigned first_label,last_label,last_signal;
static const char *scenario;
void log_line(const char *fmt,...){(void)fmt;}
int bt_send(unsigned cid,const unsigned char *data,int len)
{
    assert(cid==0x800 && len>=2 && !(data[0]&3));
    last_label=data[0]>>4;last_signal=data[1];
    if(!sends)first_label=last_label;
    sends++;return !fail_send;
}
int bt_link_lost(void){return lost;}
int bt_wait(volatile int *flag,int timeout)
{
    assert(waits<3);windows[waits++]=timeout;
    long deadline=ticks+timeout;
    while(!*flag && ticks<deadline && !lost) {
        ticks+=100;
        if(!strcmp(scenario,"loss") && ticks>=1000)lost=1;
        if(!strcmp(scenario,"delayed-sdp") && ticks>=6000 && !peer_sdp) {
            const unsigned char request[]={6,0,1,0,13,0x35,3,0x19,0x11,0x0d,0,0x20,0x35,3,9,0,9,0};
            unsigned char response[512];peer_sdp=sdp_handle(request,sizeof request,response,sizeof response)>0;
        }
        long at=!strcmp(scenario,"immediate")?100:!strcmp(scenario,"near-deadline")?9900:7000;
        if(!strcmp(scenario,"wrong-label") && ticks>=6500 && !wrong_delivered) {
            unsigned char wrong[]={(unsigned char)(((first_label+1)<<4)|2),1,4,8};
            on_signal_frame(g_sig.scid,wrong,sizeof wrong);assert(!*flag);wrong_delivered=1;
        }
        if(!strcmp(scenario,"wrong-signal") && ticks>=6500 && !wrong_delivered) {
            unsigned char wrong[]={(unsigned char)((first_label<<4)|2),2,4,8};
            on_signal_frame(g_sig.scid,wrong,sizeof wrong);assert(!*flag);wrong_delivered=1;
        }
        int reply=!strcmp(scenario,"immediate") || !strcmp(scenario,"late") ||
                  !strcmp(scenario,"delayed-sdp") || !strcmp(scenario,"near-deadline") ||
                  !strcmp(scenario,"wrong-label") || !strcmp(scenario,"wrong-signal");
        if(reply && ticks>=at && !delivered) {
            unsigned char response[]={(unsigned char)((first_label<<4)|2),(unsigned char)last_signal,4,8};
            on_signal_frame(g_sig.scid,response,sizeof response);delivered=1;
        }
    }
    return !lost && *flag;
}

int main(int argc,char **argv)
{
    assert(argc==2);scenario=argv[1];g_sig.dcid=0x800;
    if(!strcmp(scenario,"send-failed"))fail_send=1;
    unsigned signal=!strcmp(scenario,"other-command")?AVDTP_GET_CAPABILITIES:AVDTP_DISCOVER;
    unsigned char seid=4;
    int result=avdtp_cmd((unsigned char)signal,signal==AVDTP_DISCOVER?NULL:&seid,
                       signal==AVDTP_DISCOVER?0:1);
#ifdef BASELINE_WINDOW
    const int first_wait=2000;
#else
    const int first_wait=10000;
#endif
    if(!strcmp(scenario,"send-failed"))assert(result==-1 && sends==1 && !waits && !ticks);
    else if(!strcmp(scenario,"loss"))assert(result==-1 && sends==1 && waits==1 && ticks==1000);
    else if(!strcmp(scenario,"immediate"))assert(result==2 && sends==1 && ticks==100);
    else if(!strcmp(scenario,"other-command"))assert(result==-1 && sends==3 && waits==3 && ticks==6000);
    else if(!strcmp(scenario,"unanswered"))assert(result==-1 && sends==3 && waits==3 && ticks==first_wait+4000);
    else if(first_wait==2000)assert(result==-1 && sends==3 && ticks==6000);
    else {assert(result==2 && sends==1 && waits==1 && ticks<=10000);if(!strcmp(scenario,"delayed-sdp"))assert(peer_sdp);}
    if(waits)assert(windows[0]==(signal==AVDTP_DISCOVER?first_wait:2000));
    for(int i=1;i<waits;i++)assert(windows[i]==2000);
    printf("AVDTP window case=%s result=%d sends=%d elapsed=%ld first-wait=%d sdp=%d\n",
           scenario,result,sends,ticks,first_wait,peer_sdp);return 0;
}
