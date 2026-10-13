/* Host-only events and clock. No console, real key file or native API. */
#include <stdarg.h>
#define main historical_fixture_main
#include "../build/resident-offline/startup-harness.c"
#undef main

static int mode, busy_seen, restart_prompts, startup_prompts, ended_prompts;
static long busy_at;
static void record_notice(const char *fmt, va_list args)
{
    char text[512];
    vsnprintf(text,sizeof text,fmt,args);
    restart_prompts+=strstr(text,"turn the headset off and on now")!=NULL;
    startup_prompts+=strstr(text,"turn it on now if off")!=NULL;
    ended_prompts+=strstr(text,"attempt ended")!=NULL;
}
void notify(const char *fmt,...)
{va_list args;va_start(args,fmt);record_notice(fmt,args);va_end(args);}
static void authenticated_connection(int failed_auth)
{
    conn(0,0x20,0);
    unsigned char auth[]={6,3,0,0x20,0};
    auth[2]=failed_auth?5:0;on_event(auth,sizeof auth);
    unsigned char enc[]={8,4,0,0x20,0,1};on_event(enc,sizeof enc);
}
int hci_pump(int ms)
{
    common_hci_pump(ms);
    if(!busy_seen && ((mode==1 && ticks>=1100) || pending_op==OP_CREATE_CONNECTION)) {
        pending_op=0;
        if(mode<=1) {authenticated_connection(0);busy_seen=1;}
        else {conn(0x0b,0,0);busy_seen=1;busy_at=ticks;}
        return 0;
    }
    if(mode>=2 && busy_seen==1 && ticks>busy_at) {
        /* The restart instruction must precede pumping the recovery window. */
        assert(restart_prompts==1 && !ended_prompts);
        if(mode==4 && ticks>=busy_at+500 && busy_seen==1) {
            conn(0,0x21,1); /* Wireless system device is not ours. */
            assert(!g_conn_done && !g_handle);busy_seen=2;
        }
        if(mode==5 && ticks>=busy_at+500) {g_bt_transport_failed=1;busy_seen=2;}
        if((mode==2 || mode==6 || mode==7) &&
           ticks>=busy_at+(mode==6?9900:2000)) {
            authenticated_connection(mode==7);busy_seen=2;
        }
    }
    return 0;
}
int main(int argc,char **argv)
{
    assert(argc==2);
    mode=!strcmp(argv[1],"immediate")?0:!strcmp(argv[1],"spontaneous")?1:
         !strcmp(argv[1],"restart")?2:!strcmp(argv[1],"timeout")?3:
         !strcmp(argv[1],"foreign")?4:!strcmp(argv[1],"transport-stop")?5:
         !strcmp(argv[1],"near-deadline")?6:!strcmp(argv[1],"auth-failed")?7:-1;
    assert(mode>=0);
    const unsigned char peer[]={1,2,3,4,5,6};
    memcpy(g_target,peer,6);memcpy(g_key.addr,peer,6);
    memset(g_key.key,0x5a,sizeof g_key.key);g_key.valid=1;
    g_peer_prepared=1;g_key_path="/synthetic/mock-key";
    int ok=bt_connect(g_key_path);
    assert(ok==(mode==0 || mode==1 || mode==2 || mode==6));
    assert(startup_prompts==1 && create_requests==(mode==1?0:1));
    assert(restart_prompts==(mode>=2));
    assert(ended_prompts==(mode==3 || mode==4 || mode==5));
    assert(!disconnects && g_key.valid && g_key.key[0]==0x5a);
    if(mode==3 || mode==4)assert(ticks-busy_at==10000 && !g_conn_done && !g_handle);
    if(mode==5)assert(ticks-busy_at<10000 && bt_transport_failed());
    if(ok)assert(g_conn_done && !g_conn_status && g_handle==0x20 && g_enc_on);
    printf("Startup fixture passed: %s; creates=%d prompts=%d elapsed=%ld\n",
           argv[1],create_requests,restart_prompts,ticks-100);
    return 0;
}
