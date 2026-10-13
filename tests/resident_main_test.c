/* Main lifecycle with all console operations mocked. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define main payload_main
#include "../src/main.c"
#undef main
static int mode, opens, closes, starts, connects, streams, prepares, waits, stops, releases, restarts;
int log_open(const char *d,const char *p){(void)d;(void)p;return 1;}
void log_close(void){}
void log_line(const char *fmt,...){(void)fmt;}
void notify(const char *fmt,...){(void)fmt;}
int session_lock_take(const char *p,int *fd){(void)p;*fd=42;return 1;}
void session_lock_release(int *fd){assert(*fd==42);releases++;}
int capture_open(void){opens++;return 1;}
void capture_close(void){closes++;}
int capture_restart(void){assert(waits==1 && streams==1);restarts++;return mode==4?0:1;}
int bt_prepare_peer(const char *p){(void)p;return 1;}
int a2dp_prepare(void){return 1;}
int bt_start(void){starts++;return 1;}
int bt_connect(const char *p){(void)p;connects++;return mode!=5 || connects==1;}
int a2dp_start(void){return mode!=6 || connects==1;}
int a2dp_stream(int c){assert(c==(streams==1 && mode==4?0:1));streams++;return 1;}
int bt_prepare_reconnect(void){assert(streams==1);prepares++;return mode!=1;}
int a2dp_prepare_reconnect(void){return mode!=2;}
int bt_await_reconnect(int ms){assert(ms==30000 && prepares==1);waits++;return mode!=3;}
void a2dp_stop(void){stops++;}
void bt_stop(void){stops++;}
int bt_setup_failed(void){return 0;}
int bt_coexistence_conflict(void){return 0;}
int bt_acl_flow_failed(void){return 0;}
int bt_transport_failed(void){return 0;}
int main(int argc,char **argv)
{
    assert(argc==2);mode=!strcmp(argv[1],"no-rearm")?1:!strcmp(argv[1],"audio-rearm-failed")?2:
      !strcmp(argv[1],"no-peer")?3:!strcmp(argv[1],"capture-not-ready")?4:
      !strcmp(argv[1],"second-auth-failed")?5:!strcmp(argv[1],"second-audio-failed")?6:0;
    payload_main();assert(opens==1 && closes==1 && starts==1 && prepares==1 && stops==2 && releases==1);
    assert(streams==((mode==0 || mode==4)?2:1));assert(restarts==((mode==0 || mode==4)?1:0));
    assert(waits==(mode==1 || mode==2?0:1));assert(connects==(mode<=3 && mode!=0?1:2));
    printf("Resident main passed: %s\n",argv[1]);return 0;
}
