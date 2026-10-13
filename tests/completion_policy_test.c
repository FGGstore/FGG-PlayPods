/* Real generated BT dispatcher, mock clock/HCI. No target execution. */
#define main strict_fixture_main
#include "acl_credit_test.c"
#undef main

static void fresh(int capacity)
{
    reset(capacity);
    g_if_head=g_if_count=0;g_assumed=0;g_bt_transport_failed=0;
    memset(g_inflight,0,sizeof g_inflight);
}

int main(void)
{
    /* No mint at exactly 60ms; 61ms allows direct signaling and records reuse. */
    fresh(7);for(int i=0;i<7;i++)assert(send());
    ticks+=60;assert(!bt_can_send() && !send() && sends==7 && !g_assumed);
    ticks++;assert(send() && sends==8 && g_assumed==7 && g_credits==6);
    assert(g_if_count==1 && g_sent==8 && !g_reported);
    assert((long)g_credits_max-(g_sent-g_reported)==-1); /* shadow does not gate */
    long assumed=g_assumed;int credits=g_credits;
    bt_completion_report();assert(g_assumed==assumed && g_credits==credits);
    assert(strstr(logs,"strict-shadow-credits=-1") && strstr(logs,"assumed-reuse=7"));

    /* Timely reports retire FIFO entries without any assumed completion. */
    fresh(7);for(int i=0;i<1000;i++){
        assert(send());ticks+=19;complete(0x20,1);
        assert(g_if_count==0 && g_credits==7 && !g_assumed);
    }
    assert(g_sent==1000 && g_reported==1000);

    /* Sparse missing reports can exceed seven outstanding observations. */
    fresh(7);for(int i=1;i<=1000;i++){
        assert(send());ticks+=19;if(i%40)complete(0x20,1);
    }
    assert(g_sent==1000 && g_reported==975 && g_assumed>0 && bt_can_send());

    /* Explicit hidden occupancy hazard, preserved producer FIFO semantics:
     * first seven assumed, an eighth sent, then a late old report retires
     * the new FIFO entry and restores its allowance. The mock reports USB
     * success; this demonstrates permission, not actual radio capacity. */
    fresh(7);for(int i=0;i<7;i++)assert(send());ticks+=61;assert(send());
    complete(0x20,1);assert(g_reported==1 && g_if_count==0 && g_credits==7);
    assert(g_sent-g_reported==7 && g_assumed==7);
    assert(send());assert(g_sent==9 && g_sent-g_reported==8);
    puts("Known modeled hazard: late reports can retire newer entries and permit overcommit");

    /* Late reports allowed against real submission deficit; allowance capped. */
    fresh(7);for(int i=0;i<7;i++)assert(send());ticks+=61;assert(bt_can_send());
    complete(0x20,7);assert(g_reported==7 && g_assumed==7 && g_credits==7);
    complete(0x20,1);assert(g_acl_flow_failed && g_reported==7 && g_credits==7);
    ticks+=100;assert(!bt_can_send() && !send());

    /* Aggregate excess is atomic even when assumptions happened. */
    fresh(7);assert(send());ticks+=61;assert(send());
    complete(0x20,3);assert(g_acl_flow_failed && !g_reported && g_if_count==1);
    assert(g_credits==6 && g_assumed==1 && !send());

    fresh(1);assert(send());complete(0x21,65535);
    assert(!g_reported && !g_credits && !g_acl_flow_failed);
    const unsigned char bad[]={0x13,5,1,0x20,0};on_event(bad,sizeof bad);
    assert(!g_reported && !g_credits && !g_acl_flow_failed);

    /* Overflow stops before USB and never silently evicts a live record. */
    fresh(65);for(int i=0;i<64;i++)assert(send());
    assert(!send() && sends==64 && g_sent==64 && g_if_count==64);
    assert(g_acl_flow_failed && g_if_head==0 && !g_assumed);
    ticks+=61;assert(!bt_can_send() && !g_assumed && !send());

    fresh(1);fail_send=1;assert(!send() && !g_sent && !g_if_count && g_credits==1);
    fail_send=0;assert(send());ticks+=61;g_disconnected=1;
    assert(!bt_can_send() && !send() && !g_assumed && !g_credits);
    for(int guard=0;guard<3;guard++){
        fresh(1);assert(send());ticks+=61;
        if(guard==0)g_coexistence_conflict=1;
        if(guard==1)g_acl_flow_failed=1;
        if(guard==2)g_bt_transport_failed=1;
        assert(!bt_can_send() && !send() && !g_assumed && !g_credits);
    }
    fresh(1);assert(!bt_send(0x100,NULL,1) && !bt_send(0x100,NULL,-1));
    assert(!bt_send(0x100,(const unsigned char *)"x",1020) && !sends);
    puts("Producer60 threshold, common gate, sparse loss, accounting, overflow and terminal guards passed");
    return 0;
}
