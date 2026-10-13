/* Actual isolated BT/AVDTP candidate; synthetic peer, HCI, time and key state.
 * No console, native API, network or real pairing-key file is accessed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static long ticks=100;
static int clock_fixture(clockid_t id,struct timespec *ts)
{(void)id;ts->tv_sec=ticks/1000;ts->tv_nsec=ticks%1000*1000000;return 0;}
#define clock_gettime clock_fixture
#include "../src/bt.c"
#include "../src/a2dp.c"
#undef clock_gettime
static unsigned char packets[64][HCI_PKT_MAX];
static int lengths[64],sends,auth_requests,create_requests,disconnects,rejects,accepts;
static unsigned pending_op;
static int scenario,phase,fail_send;
void log_line(const char *fmt,...) {(void)fmt;}
void notify(const char *fmt,...) {(void)fmt;}
int hci_open(void) {assert(!"No hardware open allowed");return 0;}
void hci_close(void) {}
void hci_receive_report(void) {}
int hci_next_event(unsigned char *p,int n) {(void)p;(void)n;return 0;}
int hci_next_acl(unsigned char *p,int n) {(void)p;(void)n;return 0;}
int hci_cmd(unsigned op,const void *p,int n)
{
    (void)n;pending_op=op;auth_requests+=op==OP_AUTH_REQUESTED;
    create_requests+=op==OP_CREATE_CONNECTION;disconnects+=op==OP_DISCONNECT;
    rejects+=op==OP_REJECT_CONNECTION;accepts+=op==OP_ACCEPT_CONNECTION;
    if(op==OP_DISCONNECT) assert(le16(p)==0x20);
    return 1;
}
int hci_acl_send(const unsigned char *p,int n)
{
    assert(sends<64 && n<=HCI_PKT_MAX);
    memcpy(packets[sends],p,(size_t)n);lengths[sends++]=n;return !fail_send;
}
static void conn(int status,unsigned handle,int foreign)
{
    unsigned char e[]={3,11,0,0,0,1,2,3,4,5,6,1,0};
    e[2]=(unsigned char)status;put16(e+3,handle);if(foreign)e[5]=99;
    on_event(e,sizeof e);
}
static void sig(unsigned code,unsigned id,const unsigned char *body,int len,int foreign)
{
    unsigned char p[40]={0};assert(len<=28);
    put16(p,(foreign?0x21:0x20)|0x2000);put16(p+2,(unsigned)len+8);
    put16(p+4,(unsigned)len+4);put16(p+6,1);
    p[8]=(unsigned char)code;p[9]=(unsigned char)id;put16(p+10,(unsigned)len);
    memcpy(p+12,body,(size_t)len);on_acl(p,len+12);
}
static void incoming(unsigned id,unsigned psm,unsigned peer,int foreign)
{unsigned char b[4];put16(b,psm);put16(b+2,peer);sig(2,id,b,4,foreign);}
static void conn_rsp(unsigned id,unsigned local,unsigned peer,unsigned result)
{unsigned char b[8]={0};put16(b,peer);put16(b+2,local);put16(b+4,result);sig(3,id,b,8,0);}
static void config_ready(void)
{
    unsigned cfg_id=g_sig.cfg_id;
    unsigned char req[8]={0};put16(req,g_sig.scid);req[4]=1;req[5]=2;put16(req+6,895);
    sig(4,20,req,8,0);
    unsigned char rsp[6]={0};put16(rsp,g_sig.scid);sig(5,cfg_id,rsp,6,0);
}
static void loser_closed(int wrong)
{
    unsigned char b[4];put16(b,g_outgoing_loser.dcid);put16(b+2,g_outgoing_loser.scid);
    sig(7,g_outgoing_loser.close_id+(wrong?1:0),b,4,0);
}
int hci_pump(int ms)
{
    ticks+=ms;
    /* Mock observed completion of all successful synthetic ACL submissions. */
    if(g_sent>g_reported){unsigned char e[]={0x13,5,1,0x20,0,0,0};
        put16(e+5,(unsigned)(g_sent-g_reported));on_event(e,sizeof e);}
    if(pending_op==OP_DISCONNECT){pending_op=0;
        unsigned char request[]={4,10,1,2,3,4,5,6,0,0,0,1};on_event(request,sizeof request);
        conn(0,0x21,0); /* A late success cannot replace cleanup's handle. */
        if(scenario==9)return 0; /* unobserved outcome */
        unsigned char e[]={5,4,0,0x20,0,0x16};e[2]=scenario==8?2:0;
        on_event(e,sizeof e);return 0;}
    if(scenario>=1 && scenario<=6 && phase==0) {
        phase=1;unsigned id=g_sig.conn_id;assert(id);
        incoming(16,0x19,0x800,0);assert(g_sig.scid==0x41);
        conn_rsp(id,0x40,0x501,1);config_ready();
        if(scenario==1)conn_rsp(id,0x40,0,4);
        else if(scenario!=3){
            if(scenario==5)conn_rsp(id,0x77,0x501,0);
            conn_rsp(id,0x40,0x501,0);
        }
        return 0;
    }
    if(scenario>=2 && scenario<=6 && scenario!=3 && scenario!=4 && phase==1) {
        phase=2;loser_closed(1);assert(!g_outgoing_loser.closed);
        if(scenario==6) {
            unsigned char b[4];put16(b,g_outgoing_loser.scid);put16(b+2,g_outgoing_loser.dcid);
            sig(6,70,b,4,0); /* Peer may close the loser before our response. */
        } else loser_closed(0);
    }
    return 0;
}
static void owned(void)
{
    memcpy(g_target,(unsigned char[]){1,2,3,4,5,6},6);
    g_credits=g_credits_max=7;g_acl_mtu=1021;conn(0,0x20,0);
}
static int count_sig(unsigned code)
{int n=0;for(int i=0;i<sends;i++)if(le16(packets[i]+6)==1 && packets[i][8]==code)n++;return n;}
static void audio_command(unsigned char label,unsigned signal,unsigned seid,int len)
{unsigned char d[3]={(unsigned char)(label<<4),(unsigned char)signal,(unsigned char)(seid<<2)};
 hci_pump(0); /* Observe completed prior mock transmissions, not timer credit. */
 on_l2cap_frame(g_sig.scid,d,len);}
int main(int argc,char **argv)
{
    assert(argc==2);owned();
    if(!strcmp(argv[1],"incoming")) {
        assert(a2dp_prepare() && !sends);incoming(16,0x19,0x800,1);assert(!sends);
        incoming(16,0x19,0x800,0);assert(g_sig.conn_done && !g_sig.conn_result && g_sig.scid==0x40);
        assert(le16(packets[0]+16)==0 && le16(packets[0]+12)==0x40);
        config_ready();int before=sends;incoming(16,0x19,0x800,0);
        assert(sends==before+1 && g_sig.cfg_rsp_ok && g_sig.cfg_req_done);
        before=sends;assert(bt_open_channel(&g_sig,0x19,on_signal_frame));assert(sends==before);
        assert(!count_sig(2) && g_sig.remote_mtu==895);
        g_av_label=3;g_av_expected_signal=1;g_av_ready=0;
        audio_command(8,1,0,2);unsigned char *p=packets[sends-1];
        assert(le16(p+6)==0x800 && lengths[sends-1]==12 && p[8]==0x82 && p[9]==1 && p[10]==4 && p[11]==0);
        assert(!g_av_ready);g_source_in_use=1;audio_command(8,1,0,2);assert(packets[sends-1][10]==6);
        audio_command(9,2,1,3);p=packets[sends-1];assert(p[8]==0x92 && lengths[sends-1]==20 && p[18]==2 && p[19]==53);
        audio_command(9,0x0c,1,3);assert(packets[sends-1][8]==0x92);
        audio_command(9,2,2,3);assert(packets[sends-1][8]==0x93 && packets[sends-1][10]==0x12);
        audio_command(9,1,0,3);assert(packets[sends-1][10]==0x11);
        unsigned char wrong[]={0x32,2};on_signal_frame(g_sig.scid,wrong,2);assert(!g_av_ready);
        unsigned char good[]={0x32,1,4,8};on_signal_frame(g_sig.scid,good,4);assert(g_av_ready);
    } else if(!strncmp(argv[1],"collision-",10)) {
        scenario=!strcmp(argv[1],"collision-refused")?1:!strcmp(argv[1],"collision-accepted")?2:
                 !strcmp(argv[1],"collision-unanswered")?3:!strcmp(argv[1],"collision-close-timeout")?4:
                 !strcmp(argv[1],"collision-invalid-cid")?5:!strcmp(argv[1],"collision-peer-close")?6:0;
        assert(scenario && a2dp_prepare());int ok=bt_open_channel(&g_sig,0x19,on_signal_frame);
        assert(ok==(scenario!=3 && scenario!=4));assert(count_sig(2)==1 && g_sig.dcid==0x800);
        assert(count_sig(6)==(scenario==1 || scenario==3?0:1));
        if(!ok) assert(ticks>=10100 && !g_outgoing_loser.closed);
        else assert(g_sig.cfg_rsp_ok && g_sig.cfg_req_done && g_outgoing_loser.closed);
        unsigned old_cid=g_sig.scid;conn_rsp(1,0x40,0x501,0);
        assert(g_sig.scid==old_cid && g_sig.dcid==0x800); /* Late result cannot steal the winner. */
    } else if(!strcmp(argv[1],"invalid-busy")) {
        assert(a2dp_prepare());incoming(16,0x19,0x3f,0);assert(le16(packets[0]+16)==6 && !g_sig.conn_done);
        g_sdp.dcid=0x800;incoming(17,0x19,0x800,0);assert(le16(packets[1]+16)==7 && !g_sig.conn_done);
        g_sdp.dcid=0;incoming(18,0x19,0x800,0);assert(g_sig.dcid==0x800);
        incoming(19,0x19,0x801,0);assert(le16(packets[sends-1]+16)==4 && g_sig.dcid==0x800);
        incoming(20,0x17,0x801,0);assert(le16(packets[sends-1]+16)==2 && g_sig.dcid==0x800);
    } else if(!strcmp(argv[1],"reply-send-failure")) {
        assert(a2dp_prepare());fail_send=1;incoming(16,0x19,0x800,0);
        assert(bt_transport_failed() && !g_sig.conn_done && !bt_open_channel(&g_sig,0x19,on_signal_frame));
    } else if(!strcmp(argv[1],"connection-state")) {
        unsigned char auth[]={6,3,0,0x20,0};on_event(auth,sizeof auth);g_enc_on=1;g_link_mode=2;
        conn(0x22,0,0);assert(g_handle==0x20 && !g_conn_status && g_auth_done && g_enc_on && g_link_mode==2);
        unsigned char status[]={0x0f,4,0x0c,1,5,4};on_event(status,sizeof status);
        assert(!g_conn_status);conn(0,0x20,0);assert(g_auth_done && g_enc_on && g_link_mode==2);
        conn(0,0x21,1);assert(g_handle==0x20 && !bt_transport_failed());
        conn(0,0x21,0);assert(g_handle==0x20 && bt_transport_failed());
    } else if(!strcmp(argv[1],"early-authentication")) {
        g_peer_prepared=1;g_key_path="/synthetic/mock-key";g_key.valid=1;memcpy(g_key.addr,g_target,6);
        unsigned char auth[]={6,3,0,0x20,0};on_event(auth,sizeof auth);g_enc_on=1;
        assert(bt_connect(g_key_path) && !auth_requests && !create_requests && ticks==100);
        g_conn_status=0x0f;conn(0,0x21,0);assert(!g_auth_done && !g_enc_on && g_handle==0x21);
    } else if(!strncmp(argv[1],"cleanup-",8)) {
        scenario=!strcmp(argv[1],"cleanup-failed")?8:!strcmp(argv[1],"cleanup-timeout")?9:7;
        bt_stop();bt_stop();assert(disconnects==1 && rejects==1 && !accepts && g_handle==0x20);
        assert(!bt_prepare_peer("/synthetic/mock-key"));
        assert(g_disconnected==(scenario==7));
        assert(g_disconnect_done==(scenario!=9));
        if(scenario==8) assert(g_disconnect_status==2);
        if(scenario==9) assert(ticks>=2100);
    } else if(!strcmp(argv[1],"matched-channel-close")) {
        assert(a2dp_prepare());incoming(16,0x19,0x800,0);config_ready();
        g_sig.close_id=40;unsigned char b[4];put16(b,0x800);put16(b+2,0x40);
        sig(7,41,b,4,0);assert(!g_sig.closed);put16(b,0x801);sig(7,40,b,4,0);assert(!g_sig.closed);
        put16(b,0x800);sig(7,40,b,4,0);assert(g_sig.closed && !g_sig.close_id);
    } else if(!strcmp(argv[1],"close-once")) {
        assert(a2dp_prepare());incoming(16,0x19,0x800,0);config_ready();
        bt_close_channel(&g_sig);assert(g_sig.close_attempted && !g_sig.closed && count_sig(6)==1);
        bt_close_channel(&g_sig);assert(count_sig(6)==1);
        assert(!bt_open_channel(&g_sig,0x19,on_signal_frame));
    } else assert(!"Unknown fixture");
    printf("Play connection fixture passed: %s\n",argv[1]);return 0;
}
