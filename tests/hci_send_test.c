/* Compile the actual transport with deterministic time and USB calls. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
static int mock_ioctl(int fd, unsigned long request, ...);
static int mock_clock(clockid_t id, struct timespec *ts);
static int mock_sleep(useconds_t us);
#define ioctl mock_ioctl
#define clock_gettime mock_clock
#define usleep mock_sleep
#include "../src/hci.c"
#undef ioctl
#undef clock_gettime
#undef usleep

static long clock_ms, complete_at, submit_delay, reap_delay;
static int starts, fail_start, report_count, incoming_pending, read_starts;
static char report[1024];
static int continuous_completions, fail_read, complete_calls;
void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (strncmp(fmt, "usb timing:", 11) == 0) {
        vsnprintf(report, sizeof report, fmt, ap);
        report_count++;
    }
    va_end(ap);
}
static int mock_clock(clockid_t id, struct timespec *ts)
{
    (void)id;
    ts->tv_sec = clock_ms / 1000;
    ts->tv_nsec = clock_ms % 1000 * 1000000;
    return 0;
}
static int mock_sleep(useconds_t us) { (void)us; clock_ms++; return 0; }
static int mock_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == USB_FS_COMPLETE) {
        complete_calls++;
        clock_ms += reap_delay;
        if (continuous_completions) {
            clock_ms++;
            ((struct usb_fs_complete *)arg)->ep_index=0;
            return 0;
        }
        if (incoming_pending) {
            incoming_pending=0;
            ((struct usb_fs_complete *)arg)->ep_index=0;
            return 0;
        }
        if (complete_at >= 0 && clock_ms >= complete_at && g_pending[IX_ACL_OUT]) {
            ((struct usb_fs_complete *)arg)->ep_index = IX_ACL_OUT;
            g_eps[IX_ACL_OUT].aFrames=1;
            return 0;
        }
        errno = EBUSY;
        return -1;
    }
    assert(request == USB_FS_START);
    if (((struct usb_fs_start *)arg)->ep_index == 0) {
        read_starts++;
        if(fail_read){errno=EIO;return -1;}
        return 0;
    }
    assert(((struct usb_fs_start *)arg)->ep_index == IX_ACL_OUT);
    starts++;
    clock_ms += submit_delay;
    if (fail_start) { errno = EIO; return -1; }
    return 0;
}
static void reset(void)
{
    memset(g_pending, 0, sizeof g_pending);
    memset(g_eps, 0, sizeof g_eps);
    memset(g_buf, 0x5a, sizeof g_buf);
    memset(&g_wait_time, 0, sizeof g_wait_time);
    memset(&g_reap_time, 0, sizeof g_reap_time);
    memset(&g_submit_time, 0, sizeof g_submit_time);
    g_send_timeouts = g_submit_failures = 0;
    g_send_report_at = clock_ms = 100;
    g_transport_failed=0;
    g_event_reads=g_acl_reads=incoming_pending=read_starts=0;
    continuous_completions=fail_read=complete_calls=0;
    g_out_timed=0;g_out_completed=g_out_elapsed=g_out_max=0;
    g_out_errors=g_out_short=g_reads_rearmed=0;
    complete_at = -1;
    submit_delay = reap_delay = starts = fail_start = report_count = 0;
}
int main(void)
{
    const unsigned char packet[] = {1,2,3,4};
    reset();
    assert(hci_acl_send(packet, sizeof packet));
    assert(starts == 1 && g_pending[IX_ACL_OUT]);
    assert(!memcmp(g_buf[IX_ACL_OUT], packet, sizeof packet));
    assert(g_wait_time.total == 0 && g_submit_time.calls == 1);

    reset();
    g_pending[IX_ACL_OUT] = 1;
    complete_at = 480; /* 380 ms wait, then 7 ms submission */
    submit_delay = 7;
    assert(hci_acl_send(packet, sizeof packet));
    assert(g_wait_time.total == 380 && g_submit_time.total == 7);
    assert(starts == 1);

    reset();
    g_pending[IX_ACL_OUT] = 1;
    complete_at = 110;
    reap_delay = 10;
    assert(hci_acl_send(packet, sizeof packet));
    assert(g_reap_time.total == 20 && g_wait_time.total == 20);

    reset();
    g_pending[IX_ACL_OUT] = 1;
    assert(!hci_acl_send(packet, sizeof packet));
    assert(starts == 0 && g_pending[IX_ACL_OUT]);
    assert(g_buf[IX_ACL_OUT][0] == 0x5a);
    assert(g_send_timeouts == 1 && g_wait_time.total == 1000);

    reset();
    fail_start = 1;
    assert(!hci_acl_send(packet, sizeof packet));
    assert(starts == 1 && !g_pending[IX_ACL_OUT]);
    assert(g_submit_failures == 1);

    reset();
    assert(!hci_acl_send(packet, -1));
    assert(!hci_acl_send(NULL, 1));
    assert(!hci_acl_send(packet, XFER_BUF + 1));
    assert(starts == 0 && g_buf[IX_ACL_OUT][0] == 0x5a);

    reset();
    clock_ms += 5000;
    submit_delay = 17;
    assert(hci_acl_send(packet, sizeof packet));
    assert(report_count == 1);
    assert(strstr(report, "submit 1 calls 17 ms total 17 max"));
    assert(g_submit_time.calls == 0 && g_wait_time.calls == 0);
    reset();
    g_event_reads=1;g_pending[0]=1;g_pending[IX_ACL_OUT]=1;
    incoming_pending=1;complete_at=110;
    assert(hci_acl_send(packet,sizeof packet));
    assert(read_starts==HCI_OUT_WAIT_REARM && g_pending[0]==HCI_OUT_WAIT_REARM);
    /* Both profiles retain receive arming in the normal pump. */
    assert(!hci_pump(1));
    assert(read_starts==1 && g_pending[0]);
    reset();
    g_event_reads=1;g_pending[IX_ACL_OUT]=1;fail_read=1;complete_at=110;
    if (HCI_OUT_WAIT_REARM) assert(!hci_acl_send(packet,sizeof packet));
    else {
        assert(hci_acl_send(packet,sizeof packet));
        assert(!read_starts && !g_transport_failed);
        assert(hci_pump(1)==-1);
    }
    assert(read_starts==1 && !g_pending[0] && g_transport_failed);
    assert(!hci_acl_send(packet,sizeof packet) && read_starts==1); /* sticky, no retry */

    reset();continuous_completions=1;
    assert(reap());assert(complete_calls==N_XFERS);
    long before=clock_ms;
    assert(!hci_pump(1));assert(clock_ms-before==N_XFERS);

    reset();
    assert(hci_acl_send(packet,sizeof packet));
    complete_at=clock_ms+380;clock_ms=complete_at;
    assert(reap());
    assert(g_out_completed==1 && g_out_elapsed==380 && g_out_max==380);
    assert(!g_out_errors && !g_out_short && !g_pending[IX_ACL_OUT]);
    assert(hci_acl_send(packet,sizeof packet));
    g_eps[IX_ACL_OUT].status=7;clock_ms++;
    assert(reap());assert(g_out_errors==1 && g_transport_failed);
    assert(!hci_acl_send(packet,sizeof packet));
    reset();
    assert(hci_acl_send(packet,sizeof packet));
    complete_at=clock_ms;
    g_buflen[IX_ACL_OUT][0]=2;clock_ms++;
    assert(reap());assert(g_out_short==1 && g_transport_failed);
    assert(!hci_acl_send(packet,sizeof packet));
    /* Actual receive splitter, with native completions modeled as data. */
    reset();g_acl_reads=1;memset(&g_acl,0,sizeof g_acl);
    g_acl_read_completions=g_acl_read_bytes=g_acl_read_errors=0;
    g_acl_read_zero=g_acl_read_partial=g_acl_read_invalid=g_acl_packets_queued=0;
    const unsigned char incoming[]={0x20,0x20,6,0,2,0,0x40,0,0x12,1};
    memcpy(g_buf[EVENT_READS],incoming,sizeof incoming);
    g_eps[EVENT_READS].aFrames=1;g_buflen[EVENT_READS][0]=sizeof incoming;
    on_complete(EVENT_READS);
    assert(g_acl_read_completions==1 && g_acl_packets_queued==1 && g_acl.count==1);
    g_buflen[EVENT_READS][0]=2;on_complete(EVENT_READS);
    g_buflen[EVENT_READS][0]=5;on_complete(EVENT_READS);
    assert(g_acl_read_partial==2 && g_acl_packets_queued==1);
    g_eps[EVENT_READS].status=7;on_complete(EVENT_READS);
    assert(g_acl_read_errors==1);
    g_eps[EVENT_READS].status=0;g_eps[EVENT_READS].aFrames=0;on_complete(EVENT_READS);
    assert(g_acl_read_zero==1);
    g_eps[EVENT_READS].aFrames=1;g_buflen[EVENT_READS][0]=XFER_BUF+1;on_complete(EVENT_READS);
    assert(g_acl_read_invalid==1 && g_acl_packets_queued==1);
    assert(g_acl_read_bytes==sizeof incoming+7);
    printf("HCI send ownership, timing, failures and receive accounting passed (OUT wait rearm=%d)\n",HCI_OUT_WAIT_REARM);
    return 0;
}
