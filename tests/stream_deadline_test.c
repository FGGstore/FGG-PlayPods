/* Actual stream loop; deterministic clock, capture, encoder and BT fixtures.
 * This verifies duration boundaries, not codec fidelity or PS5 native timing. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static int fixture_clock(clockid_t id,struct timespec *ts);
#define clock_gettime fixture_clock
#ifndef A2DP_TEST_SOURCE
#define A2DP_TEST_SOURCE "../src/a2dp.c"
#endif
#include A2DP_TEST_SOURCE
#undef clock_gettime
#ifndef A2DP_TEST_MAX_LIMIT_MS
#define A2DP_TEST_MAX_LIMIT_MS 30000
#endif

static long clock_ms, link_drop_at;
static int limit, send_calls, read_calls, poll_calls, encode_calls;
static int init_calls, finish_calls, restart_calls, duration_logs;
static int credit, continuous_capture, read_delay, encode_delay, send_delay;
static int encode_failure, capture_error, poll_records;

static int fixture_clock(clockid_t id,struct timespec *ts)
{
    (void)id;ts->tv_sec=clock_ms/1000;ts->tv_nsec=(clock_ms%1000)*1000000;
    return 0;
}
void log_line(const char *fmt,...)
{
    if (strstr(fmt,"duration reached")) duration_logs++;
}
void notify(const char *fmt,...) { (void)fmt; }
int sbc_init(sbc_t *s,unsigned long flags)
{ (void)flags;memset(s,0,sizeof *s);init_calls++;return 0; }
void sbc_finish(sbc_t *s) { (void)s;finish_calls++; }
size_t sbc_get_codesize(sbc_t *s) { (void)s;return 512; }
size_t sbc_get_frame_length(sbc_t *s) { (void)s;return 119; }
ssize_t sbc_encode(sbc_t *s,const void *in,size_t inlen,void *out,size_t outlen,ssize_t *written)
{
    (void)s;(void)in;assert(inlen==512 && outlen==119);
    assert(!limit || clock_ms<limit);
    encode_calls++;clock_ms+=encode_delay;
    if(encode_failure){*written=0;return -1;}
    memset(out,0x5a,119);*written=119;return 512;
}
int capture_read(float *buf,size_t size)
{
    assert(size==CAPTURE_RECORD);assert(!limit || clock_ms<limit);
    read_calls++;clock_ms+=read_delay;
    if(capture_error) return -1;
    if(!continuous_capture && !poll_records) return 0;
    poll_records=0;memset(buf,0,size);return (int)size;
}
int capture_restart(void) { restart_calls++;return 0; }
long capture_overruns(void) { return 0; }
int bt_max_frame(void) { return 1021; }
int bt_link_lost(void) { return link_drop_at>=0 && clock_ms>=link_drop_at; }
int bt_can_send(void) { return credit; }
int bt_credits(void) { return credit?7:0; }
long bt_reports_missing(void) { return credit?0:7; }
void bt_completion_report(void) {}
void hci_receive_report(void) {}
void bt_poll(int timeout)
{
    assert(timeout>0);poll_calls++;clock_ms+=timeout;poll_records=1;
}
int bt_send(unsigned cid,const unsigned char *data,int len)
{
    assert(cid==0x42 && len==RTP_HEADER+7*119);
    assert(data[0]==0x80 && data[12]==7);
    assert(!limit || clock_ms<limit);send_calls++;clock_ms+=send_delay;return 1;
}
static void reset(int duration)
{
    clock_ms=0;link_drop_at=-1;limit=duration;
    send_calls=read_calls=poll_calls=encode_calls=0;
    init_calls=finish_calls=restart_calls=duration_logs=0;
    credit=1;continuous_capture=read_delay=encode_delay=send_delay=0;
    encode_failure=capture_error=0;poll_records=1;
    g_fifo_len=0;g_rs_pos=0;memset(g_rs_prev,0,sizeof g_rs_prev);
    memset(&g_sc,0,sizeof g_sc);g_sc.rate=48000;
    g_media.closed=0;g_media.dcid=0x42;g_media.remote_mtu=895;
}
static void check_finished(void)
{
    assert(init_calls==1 && finish_calls==1);
    assert(duration_logs==1 && clock_ms>=limit);
}
int main(void)
{
    reset(10);
    assert(!a2dp_stream_bounded(1,0));
    assert(!a2dp_stream_bounded(1,-1));
    assert(!a2dp_stream_bounded(1,A2DP_TEST_MAX_LIMIT_MS+1));
    assert(!init_calls && !read_calls && !send_calls);

    reset(30);credit=0;
    assert(!a2dp_stream_bounded(1,limit));check_finished();
    assert(!send_calls && !encode_calls && !restart_calls && poll_calls==10);
    assert(clock_ms==30); /* Missing credits cannot extend the test forever. */

    reset(30);continuous_capture=1;read_delay=1;
    assert(!a2dp_stream_bounded(1,limit));check_finished();
    assert(read_calls==30 && !poll_calls && !send_calls);

    reset(30);read_delay=40;
    assert(!a2dp_stream_bounded(1,limit));check_finished();
    assert(read_calls==1 && !encode_calls && !send_calls && clock_ms==40);

    reset(30);encode_delay=40;
    assert(!a2dp_stream_bounded(1,limit));check_finished();
    assert(encode_calls==1 && !send_calls && clock_ms==40);

    reset(30);send_delay=40;
    assert(a2dp_stream_bounded(1,limit));check_finished();
    assert(send_calls==1 && clock_ms==41);
    /* Native-call latency can overrun the bound; no next packet is sent. */

    reset(30);
    assert(a2dp_stream_bounded(1,limit));check_finished();
    assert(send_calls>1 && !restart_calls && clock_ms==30);

    reset(30);link_drop_at=5;
    assert(a2dp_stream_bounded(1,limit));
    assert(clock_ms==5 && !duration_logs && finish_calls==1);

    reset(30);encode_failure=1;
    assert(!a2dp_stream_bounded(1,limit));
    assert(finish_calls==1 && !send_calls && !duration_logs);

    reset(30);capture_error=1;
    assert(a2dp_stream_bounded(1,limit));check_finished();
    assert(read_calls==1 && !restart_calls);

    reset(0);link_drop_at=5;
    assert(a2dp_stream(1));
    assert(clock_ms==5 && finish_calls==1 && !duration_logs);
    puts("Bounded stream: stall, endless capture, slow native/encode/send, normal stop, loss and errors passed (mocked)");
    return 0;
}
