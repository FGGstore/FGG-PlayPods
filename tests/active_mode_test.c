#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static long ticks=100;
static int fake_clock(clockid_t id,struct timespec *ts)
{(void)id;ts->tv_sec=ticks/1000;ts->tv_nsec=ticks%1000*1000000;return 0;}
#define clock_gettime fake_clock
#ifndef BT_TEST_SOURCE
#define BT_TEST_SOURCE "../src/bt.c"
#endif
#include BT_TEST_SOURCE
#undef clock_gettime
static int commands, scenario, delivered;
void log_line(const char *fmt,...){(void)fmt;}
void notify(const char *fmt,...){(void)fmt;}
int hci_open(void){return 0;}
void hci_close(void){}
int hci_cmd(unsigned op,const void *p,int n)
{
    assert(op==0x0804 && n==2 && le16(p)==0x20);
    commands++;return scenario!=6;
}
int hci_acl_send(const unsigned char *p,int n){(void)p;(void)n;return 1;}
int hci_next_event(unsigned char *p,int n){(void)p;(void)n;return 0;}
int hci_next_acl(unsigned char *p,int n){(void)p;(void)n;return 0;}
int hci_pump(int ms)
{
    ticks+=ms;
    if(!delivered++){
        unsigned char mode[]={0x14,6,0,0x20,0,0,0,0};
        unsigned char status[]={0x0f,4,0,1,4,8};
        if(scenario==1)on_event(mode,sizeof mode);
        if(scenario==2){mode[3]=0x21;on_event(mode,sizeof mode);}
        if(scenario==3){status[2]=0x0c;on_event(status,sizeof status);}
        if(scenario==4){mode[2]=0x1f;on_event(mode,sizeof mode);}
        if(scenario==5){unsigned char drop[]={5,4,0,0x20,0,0x16};on_event(drop,sizeof drop);}
        if(scenario==7)on_event(status,sizeof status); /* accepted != completed */
        if(scenario==8)g_handle=0x21;
    }
    return 0;
}
static void reset(int test)
{
    commands=delivered=0;scenario=test;ticks=100;
    g_conn_done=1;g_conn_status=0;g_disconnected=0;g_handle=0x20;
    g_enc_on=1;g_link_mode=2;g_mode_request_pending=0;
}
static void connection(int status,int foreign,unsigned handle,int link_type)
{
    const unsigned char addr[6]={1,2,3,4,5,6};
    memcpy(g_target,addr,6);
    unsigned char ev[]={3,11,0,0,0,1,2,3,4,5,6,1,0};
    ev[2]=(unsigned char)status;put16(ev+3,handle);ev[11]=(unsigned char)link_type;
    if(foreign)ev[5]=99;
    on_event(ev,sizeof ev);
}
static void initial_connection(void)
{
    reset(1);g_conn_done=0;g_enc_on=0;g_link_mode=-1;
    connection(0,0,0x20,1);
    unsigned char enc[]={8,4,0,0x20,0,1};on_event(enc,sizeof enc);
    assert(bt_require_active() && !commands && g_link_mode==0);
}
int main(void)
{
    initial_connection();
#ifdef BASELINE_INITIAL_MODE_PROBE
    return 0;
#else
    /* A repeated successful connection cannot erase a later sniff event. */
    unsigned char sniff[]={0x14,6,0,0x20,0,2,0x36,1};on_event(sniff,sizeof sniff);
    assert(g_link_mode==2 && !g_mode_from_connection);
    connection(0,0,0x20,1);assert(g_link_mode==2 && !g_mode_from_connection);
    assert(bt_require_active() && commands==1);
    for(int test=0;test<4;test++) {
        reset(1);g_conn_done=0;g_enc_on=0;g_link_mode=-1;g_mode_from_connection=0;
        connection(test==0?4:0,test==1,test==2?0xfff:0x20,test==3?0:1);
        assert(g_link_mode==-1 && !g_mode_from_connection && !bt_require_active() && !commands);
    }
    reset(1);assert(bt_require_active());assert(commands==1 && !g_mode_request_pending);
    for(int i=0;i<=8;i++)if(i!=1){
        reset(i);assert(!bt_require_active());assert(commands==1 && !g_mode_request_pending);
        if(i==0||i==2||i==7)assert(ticks==4100);
    }
    reset(1);g_link_mode=0;assert(bt_require_active());assert(!commands);
    reset(1);g_link_mode=-1;assert(!bt_require_active());assert(!commands);
    reset(1);g_link_mode=1;assert(!bt_require_active());assert(!commands);
    reset(1);g_conn_done=0;assert(!bt_require_active());assert(!commands);
    reset(1);g_enc_on=0;assert(!bt_require_active());assert(!commands);
    reset(1);g_disconnected=1;assert(!bt_require_active());assert(!commands);
    reset(1);g_handle=0x0fff;assert(!bt_require_active());assert(!commands);
    puts("Active-mode ownership, success, rejection, timeout, wrong-handle and disconnect tests passed");
#endif
}
