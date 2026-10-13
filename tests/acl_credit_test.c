/* Real dispatcher, synthetic time/HCI; no radio or console calls. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static long ticks=100;
static int fake_clock(clockid_t id,struct timespec *ts)
{ (void)id;ts->tv_sec=ticks/1000;ts->tv_nsec=ticks%1000*1000000;return 0; }
#define clock_gettime fake_clock
#ifndef BT_TEST_SOURCE
#define BT_TEST_SOURCE "../src/bt.c"
#endif
#include BT_TEST_SOURCE
#undef clock_gettime
#ifdef BASELINE_PROBE
int bt_acl_flow_failed(void) { return 0; }
#endif
static int sends,fail_send,opens,closes,commands,inject_drop,tick_calls,frames;
static char logs[4096];
void log_line(const char *fmt,...)
{
    size_t used=strlen(logs);va_list ap;va_start(ap,fmt);
    vsnprintf(logs+used,sizeof logs-used,fmt,ap);va_end(ap);
    used=strlen(logs);if(used+1<sizeof logs){logs[used]='\n';logs[used+1]=0;}
}
void notify(const char *fmt,...) { (void)fmt; }
int hci_open(void) { opens++;return 1; }
void hci_close(void) { closes++; }
int hci_cmd(unsigned op,const void *p,int n)
{
    commands++;
    assert(op==OP_DISCONNECT && n==3 && le16(p)==0x20);
    inject_drop=1;return 1;
}
int hci_acl_send(const unsigned char *p,int n)
{ assert(n==9 && le16(p)==0x2020);sends++;return !fail_send; }
int hci_next_event(unsigned char *p,int n) { (void)p;(void)n;return 0; }
int hci_next_acl(unsigned char *p,int n) { (void)p;(void)n;return 0; }
int hci_pump(int ms)
{
    ticks+=ms;
    if(inject_drop){const unsigned char ev[]={5,4,0,0x20,0,0x16};
        inject_drop=0;on_event(ev,sizeof ev);}
    return 0;
}
static void tick(void) { tick_calls++; }
static void frame(unsigned cid,const unsigned char *p,int n)
{ (void)cid;(void)p;(void)n;frames++; }
static void reset(int credits)
{
    sends=fail_send=opens=closes=commands=inject_drop=tick_calls=0;
    frames=0;g_nchans=0;g_l2need=0;
    logs[0]=0;ticks=100;g_sent=g_reported=0;
    g_credits=g_credits_max=credits;g_acl_mtu=1021;
    g_handle=0x20;g_conn_done=1;g_conn_status=0;g_disconnected=0;
    g_coexistence_conflict=g_setup_failed=0;g_tick=NULL;
#ifdef BASELINE_PROBE
    g_if_head=g_if_count=0;g_assumed=0;
#else
    g_acl_flow_failed=0;
#endif
}
static int send(void) { return bt_send(0x100,(const unsigned char *)"x",1); }
static void complete(unsigned handle,unsigned count)
{
    unsigned char ev[]={0x13,5,1,0,0,0,0};
    put16(ev+3,handle);put16(ev+5,count);on_event(ev,sizeof ev);
}
static void exhaustion(void)
{
    reset(2);assert(send() && send() && sends==2 && !bt_can_send());
    assert(!send() && sends==2 && !g_credits && bt_reports_missing()==2);
    g_credits=-1;assert(!send() && sends==2); /* central signalling gate */
}
static void delayed(void)
{
    reset(1);assert(send());ticks+=60000;
    assert(!bt_can_send() && !g_credits && bt_reports_missing()==1);
    complete(0x21,65535);assert(!bt_can_send() && !g_reported);
    complete(0x20,1);assert(bt_can_send() && g_credits==1 && !bt_reports_missing());
    assert(send() && !bt_can_send());
}
static void excess(void)
{
    reset(2);assert(send());complete(0x20,2);
    assert(g_credits==1 && !g_reported && bt_reports_missing()==1);
    assert(bt_acl_flow_failed() && bt_link_lost() && !g_disconnected);
    assert(!bt_can_send() && !send() && sends==1);
    assert(strstr(logs,"completion count") && strstr(logs,"recovery unverified"));
    complete(0x20,1);assert(!g_reported); /* sticky, no late recovery */
    l2cap_chan ch={.name="test",.scid=0x40};
    g_chans[0]=&ch;g_frame_fns[0]=frame;g_nchans=1;
    const unsigned char acl[]={0x20,0x20,5,0,1,0,0x40,0,42};
    on_acl(acl,sizeof acl);assert(!frames); /* no later channel callback */
    volatile int flag=0;assert(!bt_wait(&flag,10000) && ticks==100);
    g_tick=tick;ticks+=10000;bt_poll(1);assert(!tick_calls);
    assert(!bt_start() && !opens); /* no setup/reopen after ledger loss */
    bt_stop();assert(commands==1 && closes==1 && g_disconnected);
}
static void multi_handle(void)
{
    reset(3);assert(send() && send());
    unsigned char ev[]={0x13,13,3,0x21,0,0xff,0xff,0x20,0,1,0,0x20,0,1,0};
    on_event(ev,sizeof ev);assert(g_credits==3 && g_reported==2 && !bt_acl_flow_failed());
    reset(3);assert(send());on_event(ev,sizeof ev);
    assert(bt_acl_flow_failed() && g_credits==2 && !g_reported); /* atomic validation */
    reset(3);assert(send());ev[1]=9;on_event(ev,11); /* two tuples, truncated */
    assert(!bt_acl_flow_failed() && g_credits==2 && !g_reported);
}
static void submission(void)
{
    reset(1);fail_send=1;assert(!send() && g_credits==1 && !g_sent && !g_reported);
    fail_send=0;assert(send() && !g_credits);complete(0x20,0);assert(!g_reported);
    complete(0x20,1);assert(g_credits==1 && g_reported==1);
    complete(0x20,1);assert(bt_acl_flow_failed() && g_reported==1 && g_credits==1);
    reset(1);assert(send());g_disconnected=1;complete(0x20,1);
    assert(!g_reported && !g_credits && !bt_acl_flow_failed());
    reset(1);g_disconnected=1;assert(!bt_can_send() && !send() && !sends);
    reset(100);for(int i=0;i<100;i++)assert(send()); /* no 64-entry loss */
    assert(!bt_can_send() && bt_reports_missing()==100);
    complete(0x20,100);assert(g_credits==100 && !bt_reports_missing());
}
#ifndef BASELINE_PROBE
static void observations(void)
{
    reset(3);g_events_seen=g_events_rejected=g_completion_events=0;
    g_completion_owned_values=g_completion_foreign_values=0;
    g_completion_owned_tuples=g_completion_foreign_tuples=g_data_block_events=0;
    assert(send());complete(0x21,5);complete(0x20,2); /* excess remains uncredited */
    assert(g_completion_foreign_values==5 && g_completion_owned_values==2);
    assert(g_credits==2 && !g_reported && bt_acl_flow_failed());
    const unsigned char truncated[]={0x13,5,1,0x20,0};on_event(truncated,sizeof truncated);
    const unsigned char block_event[]={0x48,0};on_event(block_event,sizeof block_event);
    int before=sends,cmd_before=commands;bt_completion_report();
    assert(sends==before && commands==cmd_before && g_credits==2 && !g_reported);
    assert(strstr(logs,"events=4 rejected=1 packet-completion-events=2"));
    assert(strstr(logs,"matched-tuples=1 matched-values=2 foreign-tuples=1 foreign-values=5"));
    assert(strstr(logs,"data-block-events-not-decoded=1 sent=1 reconciled=0 awaiting=1 credits=2"));
}
#endif
int main(int argc,char **argv)
{
    if(argc>1){
        if(!strcmp(argv[1],"exhaustion"))exhaustion();
        else if(!strcmp(argv[1],"delayed"))delayed();
        else if(!strcmp(argv[1],"excess"))excess();
        else assert(0);
        return 0;
    }
    exhaustion();delayed();excess();multi_handle();submission();
#ifndef BASELINE_PROBE
    observations();
#endif
    puts("ACL credit exhaustion, elapsed-time non-reuse, owned completion accounting, atomic excess stop and cleanup checks passed");
    return 0;
}
