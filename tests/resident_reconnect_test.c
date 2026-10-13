/* Actual isolated BT/A2DP, synthetic owned events, USB status and clock. */
#define main historical_main
#include "../build/resident-offline/resident-harness.c"
#undef main
static int inject, fresh_event;
int hci_pump(int ms)
{
    common_hci_pump(ms);
    if(inject==1){inject=0;conn(0,0x21,0);}
    if(inject==2){inject=0;unsigned char e[]={0x13,5,1,0x20,0,1,0};on_event(e,sizeof e);}
    if(fresh_event && ticks>=600) {
        fresh_event=0;conn(0,0x20,1);assert(!g_conn_done);
        conn(0,0x20,0);
        unsigned char a[]={6,3,0,0x20,0},e[]={8,4,0,0x20,0,1};
        on_event(a,sizeof a);on_event(e,sizeof e);
    }
    return 0;
}
static void lost(int status,int foreign)
{unsigned char e[]={5,4,0,0x20,0,0x16};e[2]=(unsigned char)status;e[3]+=foreign;on_event(e,sizeof e);}
int main(int argc,char **argv)
{
    assert(argc==2);owned();g_peer_prepared=1;g_key_path="/synthetic/key";
    g_key.valid=1;memcpy(g_key.addr,g_target,6);memset(g_key.key,0x5a,sizeof g_key.key);
    assert(!a2dp_prepare_reconnect());assert(a2dp_prepare());
    g_sent=3;g_reported=1;g_assumed=1;g_if_count=2;g_credits=5;
    g_auth_done=g_enc_on=1;g_l2len=g_l2need=8;
    g_source_in_use=1;g_av_ready=1;g_fifo_len=20;g_rs_pos=0.5;
    if(!strcmp(argv[1],"unobserved"))g_disconnected=1;
    else if(!strcmp(argv[1],"failed-disconnect"))lost(2,0);
    else if(!strcmp(argv[1],"foreign-disconnect"))lost(0,1);
    else lost(0,0);
    if(!strcmp(argv[1],"out-busy"))output_idle=0;
    if(!strcmp(argv[1],"setup-fault"))g_setup_failed=1;
    if(!strcmp(argv[1],"transport-fault"))g_bt_transport_failed=1;
    if(!strcmp(argv[1],"credit-fault"))g_acl_flow_failed=1;
    if(!strcmp(argv[1],"foreign-command-fault"))g_coexistence_conflict=1;
    if(!strcmp(argv[1],"stopping"))g_stopping=1;
    if(!strcmp(argv[1],"missing-key"))g_key.valid=0;
    if(!strcmp(argv[1],"queued-success"))inject=1;
    if(!strcmp(argv[1],"late-completion"))inject=2;
    int positive=!strcmp(argv[1],"resume") || !strcmp(argv[1],"timeout") ||
                 !strcmp(argv[1],"late-excess") || !strcmp(argv[1],"wait-stop");
    int prepared=bt_prepare_reconnect();assert(prepared==positive);
    assert(!create_requests && !disconnects && !auth_requests);
    if(!prepared) {
        assert(g_sent==3 && g_reported==1 && g_disconnect_retired==0 && g_credits==5);
        assert(!g_reconnect_used && g_source_in_use && g_fifo_len==20);
    } else {
        assert(g_sent==3 && g_reported==1 && g_assumed==1 && g_disconnect_retired==2);
        assert(!bt_reports_missing() && !g_if_count && g_credits==7 && !g_conn_done && !g_handle);
        assert(!g_auth_done && !g_enc_on && !g_l2need && !g_nchans && g_key.valid);
        assert(a2dp_prepare_reconnect() && g_sig.scid==0x44 && g_media.scid==0x46);
        assert(!g_source_in_use && !g_av_ready && !g_fifo_len && !g_rs_pos);
        assert(!a2dp_prepare_reconnect() && !bt_prepare_reconnect());
        if(!strcmp(argv[1],"timeout")) {
            assert(!bt_await_reconnect(30001) && !bt_await_reconnect(0));
            long before=ticks;assert(!bt_await_reconnect(30000) && ticks-before==30000);
            assert(!g_conn_done && !create_requests);
        } else if(!strcmp(argv[1],"wait-stop")) {
            g_bt_transport_failed=1;assert(!bt_await_reconnect(30000) && !g_conn_done);
        } else {
            fresh_event=1;assert(bt_await_reconnect(30000) && g_handle==0x20);
            assert(bt_connect(g_key_path) && !create_requests && !auth_requests);
            incoming(16,0x19,0x800,0);config_ready();assert(g_sig.scid==0x44 && g_sig.dcid==0x800);
            if(!strcmp(argv[1],"late-excess")) {
                g_reported=g_sent-g_disconnect_retired;
                unsigned char e[]={0x13,5,1,0x20,0,2,0};on_event(e,sizeof e);
                assert(g_acl_flow_failed);
            } else {
                assert(!bt_acl_flow_failed() && bt_reports_missing()>=0);
                lost(0,0);assert(!bt_prepare_reconnect());
            }
        }
    }
    printf("Resident reconnect passed: %s\n",argv[1]);return 0;
}
