#include "hci.h"
#include "log.h"
#include "util.h"

#include <sys/ioctl.h>
#include <sys/types.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define BT_NODE "/dev/ugen0.2"

/* Owner's Usblayout2 descriptor report confirms these interface/endpoint
 * groups. It does not establish independent radio state, addresses, or which
 * function the native system owns. Audio uses the first interface pair.
 *
 *   iface 0/1: events 0x82, ACL in 0x81, ACL out 0x01   (used here)
 *   iface 2/3: events 0x85, ACL in 0x84, ACL out 0x04   (ownership unknown)
 *
 * Commands for function 0 go on the control endpoint. */
#define EP_EVENT   0x82
#define EP_ACL_IN  0x81
#define EP_ACL_OUT 0x01

/* Read counts and buffer sizes are inherited from upstream. Native reader
 * count and scheduling are not established on this firmware. Competing IN
 * reads do not preserve delivery to the system's owner or form a multiplexer. */
#define EVENT_READS 48
#define ACL_READS   16
#define EVENT_BUF   260
#define XFER_BUF    HCI_PKT_MAX

#define IX_ACL_OUT  (EVENT_READS + ACL_READS)
#define N_XFERS     (IX_ACL_OUT + 1)

#define QUEUE_LEN 32

typedef struct {
    unsigned char data[QUEUE_LEN][HCI_PKT_MAX];
    int len[QUEUE_LEN];
    int head, count;
} pkt_queue;

static int g_fd = -1;
/* Sticky for this process: traffic loss cannot be repaired by reopening USB. */
static int g_transport_failed;
static void transport_fail(const char *reason)
{
    if (!g_transport_failed)
        log_line("hci: transport integrity lost (%s); stopping session, recovery unverified", reason);
    g_transport_failed = 1;
}
static struct usb_fs_endpoint g_eps[N_XFERS];
static void *g_bufptr[N_XFERS][1];
static uint32_t g_buflen[N_XFERS][1];
static unsigned char g_buf[N_XFERS][XFER_BUF];
static int g_pending[N_XFERS];
static long g_read_started_at[N_XFERS];
static int g_event_reads, g_acl_reads;     /* how many actually opened */

static pkt_queue g_events, g_acl;
static long g_out_started_at, g_out_completed, g_out_elapsed, g_out_max;
static long g_out_errors, g_out_short, g_reads_rearmed;
static unsigned g_out_requested;
static int g_out_timed;
/* Receive diagnostics count transport observations, never packet contents. */
static unsigned long g_acl_read_completions, g_acl_read_bytes, g_acl_read_errors;
static unsigned long g_acl_read_zero, g_acl_read_partial, g_acl_read_invalid;
static unsigned long g_acl_packets_queued;
static unsigned long g_event_read_completions, g_event_read_bytes;
static unsigned long g_event_read_errors, g_event_read_zero, g_event_packets_queued;
static unsigned long g_event_starts, g_acl_starts;
static long g_last_event_completion, g_last_acl_completion;


static int queue_push(pkt_queue *q, const unsigned char *p, int len)
{
    int slot;

    if (len <= 0 || len > HCI_PKT_MAX || !p) {
        transport_fail("invalid queued packet extent");
        return 0;
    }
    if (q->count == QUEUE_LEN) {
        transport_fail(q == &g_events ? "event receive queue full" : "ACL receive queue full");
        return 0; /* Preserve the existing order; do not silently replace data. */
    }
    slot = (q->head + q->count) % QUEUE_LEN;
    memcpy(q->data[slot], p, (size_t)len);
    q->len[slot] = len;
    q->count++;
    return 1;
}

static int queue_pop(pkt_queue *q, unsigned char *out, int max)
{
    int n;

    if (q->count == 0) return 0;
    n = q->len[q->head];
    if (!out || n > max) {
        transport_fail("receive output buffer too small");
        return 0;
    }
    memcpy(out, q->data[q->head], (size_t)n);
    q->head = (q->head + 1) % QUEUE_LEN;
    q->count--;
    return n;
}

int hci_next_event(unsigned char *out, int max) { return queue_pop(&g_events, out, max); }
int hci_next_acl(unsigned char *out, int max)   { return queue_pop(&g_acl, out, max); }

static int is_event_read(int ix) { return ix < EVENT_READS; }

static int ep_open(int index, int addr, int bufsize)
{
    struct usb_fs_open op;

    memset(&op, 0, sizeof op);
    op.max_bufsize = (uint32_t)bufsize;
    op.max_frames = 1;
    op.ep_index = (uint8_t)index;
    op.ep_no = (uint8_t)addr;
    if (ioctl(g_fd, USB_FS_OPEN, &op) != 0) {
        log_line("hci: USB_FS_OPEN %#x (transfer %d) errno=%d", addr, index, errno);
        return 0;
    }
    if (index == IX_ACL_OUT)
        log_line("usb out: ep=%#x max-packet=%u buffer=%u frames=%u flags=%u timeout=%u",
                 addr, op.max_packet_length, op.max_bufsize, op.max_frames,
                 g_eps[index].flags, g_eps[index].timeout);
    return 1;
}

static int ep_start(int index, int length)
{
    struct usb_fs_start start;

    g_buflen[index][0] = (uint32_t)length;
    g_eps[index].nFrames = 1;
    g_eps[index].aFrames = 0;
    g_eps[index].status = 0;

    memset(&start, 0, sizeof start);
    start.ep_index = (uint8_t)index;
    if (ioctl(g_fd, USB_FS_START, &start) != 0) {
        log_line("hci: USB_FS_START %d errno=%d", index, errno);
        return 0;
    }
    g_pending[index] = 1;
    if (index == IX_ACL_OUT) {
        g_out_started_at = now_ms();
        g_out_requested = (unsigned)length;
        g_out_timed = 1;
    } else {
        g_reads_rearmed++;
        g_read_started_at[index] = now_ms();
        if (is_event_read(index)) g_event_starts++;
        else g_acl_starts++;
    }
    return 1;
}

/* Restart only completed IN transfers. No Bluetooth dispatch here: it can
 * itself send packets, so dispatching inside an OUT wait would recurse. */
static int rearm_reads(void)
{
    if (g_transport_failed) return 0;
    for (int i = 0; i < g_event_reads; i++)
        if (!g_pending[i] && !ep_start(i, EVENT_BUF)) {
            transport_fail("event read restart failed");
            return 0;
        }
    for (int i = EVENT_READS; i < EVENT_READS + g_acl_reads; i++)
        if (!g_pending[i] && !ep_start(i, XFER_BUF)) {
            transport_fail("ACL read restart failed");
            return 0;
        }
    return 1;
}

/* Opens up to `want` reads at indexes first.. on `addr`. A refusal after the
 * first only leaves fewer reads. Returns how many opened. */
static int open_reads(int first, int want, int addr, int bufsize)
{
    int i;

    for (i = 0; i < want; i++)
        if (!ep_open(first + i, addr, bufsize)) break;
    return i;
}

int hci_open(void)
{
    struct usb_fs_init init;
    int i;

    if (g_transport_failed) return 0;
    g_acl_read_completions = g_acl_read_bytes = g_acl_read_errors = 0;
    g_acl_read_zero = g_acl_read_partial = g_acl_read_invalid = 0;
    g_acl_packets_queued = 0;
    g_event_read_completions = g_event_read_bytes = 0;
    g_event_read_errors = g_event_read_zero = g_event_packets_queued = 0;
    g_event_starts = g_acl_starts = 0;
    g_last_event_completion = g_last_acl_completion = 0;
    memset(g_read_started_at, 0, sizeof g_read_started_at);
    g_fd = open(BT_NODE, O_RDWR);
    if (g_fd < 0) {
        log_line("hci: open %s errno=%d", BT_NODE, errno);
        return 0;
    }

    memset(g_eps, 0, sizeof g_eps);
    for (i = 0; i < N_XFERS; i++) {
        g_bufptr[i][0] = g_buf[i];
        g_buflen[i][0] = XFER_BUF;
        g_eps[i].ppBuffer = g_bufptr[i];
        g_eps[i].pLength = g_buflen[i];
        g_eps[i].nFrames = 1;
        g_eps[i].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
    }
    /* Bulk OUT must end a packet that is an exact multiple of the max packet
     * size with a zero-length packet. */
    g_eps[IX_ACL_OUT].flags = USB_FS_FLAG_FORCE_SHORT;

    memset(&init, 0, sizeof init);
    init.pEndpoints = g_eps;
    init.ep_index_max = N_XFERS;
    if (ioctl(g_fd, USB_FS_INIT, &init) != 0) {
        log_line("hci: USB_FS_INIT errno=%d", errno);
        hci_close();
        return 0;
    }

    /* The OUT transfer first, so it is never the one left out. */
    if (!ep_open(IX_ACL_OUT, EP_ACL_OUT, XFER_BUF)) {
        hci_close();
        return 0;
    }
    g_event_reads = open_reads(0, EVENT_READS, EP_EVENT, EVENT_BUF);
    g_acl_reads = open_reads(EVENT_READS, ACL_READS, EP_ACL_IN, XFER_BUF);
    if (g_event_reads < 1 || g_acl_reads < 1) {
        hci_close();
        return 0;
    }

    memset(g_pending, 0, sizeof g_pending);
    g_out_timed = 0;
    g_out_completed = g_out_elapsed = g_out_max = 0;
    g_out_errors = g_out_short = g_reads_rearmed = 0;
    memset(&g_events, 0, sizeof g_events);
    memset(&g_acl, 0, sizeof g_acl);

    log_line("hci: controller open alongside the system, %d event and %d ACL reads",
             g_event_reads, g_acl_reads);
    return 1;
}

void hci_close(void)
{
    struct usb_fs_uninit un;

    if (g_fd < 0) return;
    hci_receive_report();
    log_line("usb rx: acl-completions=%lu bytes=%lu errors=%lu zero-frame=%lu "
             "partial-reads=%lu invalid-length=%lu queued=%lu remaining=%d",
             g_acl_read_completions, g_acl_read_bytes, g_acl_read_errors,
             g_acl_read_zero, g_acl_read_partial, g_acl_read_invalid,
             g_acl_packets_queued, g_acl.count);
    memset(&un, 0, sizeof un);
    ioctl(g_fd, USB_FS_UNINIT, &un);
    close(g_fd);
    g_fd = -1;
}

int hci_cmd(unsigned opcode, const void *params, int plen)
{
    struct usb_ctl_request req;
    unsigned char pkt[3 + 255];

    if (opcode > 0xffff || plen < 0 || plen > 255 || (plen > 0 && !params)) {
        log_line("hci: invalid command opcode/parameters; not submitted");
        return 0;
    }
    pkt[0] = (unsigned char)(opcode & 0xFF);
    pkt[1] = (unsigned char)(opcode >> 8);
    pkt[2] = (unsigned char)plen;
    if (plen > 0) memcpy(pkt + 3, params, (size_t)plen);

    memset(&req, 0, sizeof req);
    req.ucr_data = pkt;
    req.ucr_request.bmRequestType = UT_WRITE_CLASS_DEVICE;
    req.ucr_request.bRequest = 0;
    USETW(req.ucr_request.wValue, 0);
    USETW(req.ucr_request.wIndex, 0);
    USETW(req.ucr_request.wLength, 3 + plen);

    if (ioctl(g_fd, USB_DO_REQUEST, &req) != 0) {
        log_line("hci: command %#06x failed, errno=%d", opcode, errno);
        return 0;
    }
    return 1;
}

/* Whole HCI packets have a two-byte event header or four-byte ACL header.
 * An incomplete remainder establishes a framing loss, not who consumed it.
 * Without a single ordered owner, do not guess continuation across reads. */
static void split_packets(int index)
{
    const unsigned char *p = g_buf[index];
    int n = (int)g_buflen[index][0], off = 0;
    int event = is_event_read(index);

    if (n < 0 || n > XFER_BUF) {
        if (!event) g_acl_read_invalid++;
        log_line("hci: invalid receive completion length on %s endpoint", event ? "event" : "ACL");
        transport_fail("invalid receive completion length");
        return;
    }
    for (;;) {
        int hdr = event ? 2 : 4, need;

        if (off + hdr > n) break;
        need = event ? hdr + p[off + 1] : hdr + (int)le16(p + off + 2);
        if (off + need > n) break;
        if (!queue_push(event ? &g_events : &g_acl, p + off, need)) return;
        if (!event) g_acl_packets_queued++;
        else g_event_packets_queued++;
        off += need;
    }
    if (off != n) {
        if (!event) g_acl_read_partial++;
        transport_fail("incomplete received HCI packet");
    }
}

static void on_complete(int index)
{
    g_pending[index] = 0;
    if (index == IX_ACL_OUT) {
        if (g_out_timed) {
            long elapsed = now_ms() - g_out_started_at;
            if (elapsed < 0) elapsed = 0;
            g_out_completed++;
            g_out_elapsed += elapsed;
            if (elapsed > g_out_max) g_out_max = elapsed;
            if (g_eps[index].status != 0) {
                g_out_errors++;
                transport_fail("ACL output completion error");
            } else if (g_eps[index].aFrames != 1 || g_buflen[index][0] != g_out_requested) {
                g_out_short++;
                log_line("hci: ACL output frames=%u bytes=%u requested=%u",
                         g_eps[index].aFrames, (unsigned)g_buflen[index][0], g_out_requested);
                transport_fail("incomplete ACL output completion");
            }
            g_out_timed = 0;
        }
        if (g_eps[index].status != 0)
            log_line("hci: ACL out status=%u", (unsigned)g_eps[index].status);
        return;
    }
    if (index < g_event_reads) {
        g_event_read_completions++;
        g_last_event_completion = now_ms();
        if (g_eps[index].status != 0) g_event_read_errors++;
        else if (g_eps[index].aFrames == 0) g_event_read_zero++;
        else if (g_buflen[index][0] <= XFER_BUF) g_event_read_bytes += g_buflen[index][0];
    } else if (index >= EVENT_READS && index < EVENT_READS + g_acl_reads) {
        g_acl_read_completions++;
        g_last_acl_completion = now_ms();
        if (g_eps[index].status != 0) g_acl_read_errors++;
        else if (g_eps[index].aFrames == 0) g_acl_read_zero++;
        else if (g_buflen[index][0] <= XFER_BUF) g_acl_read_bytes += g_buflen[index][0];
    }
    if (g_eps[index].status != 0) {
        log_line("hci: receive completion index=%d status=%u", index, g_eps[index].status);
        transport_fail("receive completion error");
        return;
    }
    if (g_eps[index].aFrames == 0) return;
    split_packets(index);
}

/* g_pending is the local successful-start/not-yet-reaped state, not a native
 * queue query or proof of exclusive ownership. Ages never trigger recovery. */
void hci_receive_report(void)
{
    long now = now_ms(), event_oldest = 0, acl_oldest = 0;
    int event_pending = 0, acl_pending = 0;
    for (int i = 0; i < g_event_reads; i++) {
        if (!g_pending[i]) continue;
        long age = now >= g_read_started_at[i] ? now - g_read_started_at[i] : 0;
        event_pending++;
        if (age > event_oldest) event_oldest = age;
    }
    for (int i = EVENT_READS; i < EVENT_READS + g_acl_reads; i++) {
        if (!g_pending[i]) continue;
        long age = now >= g_read_started_at[i] ? now - g_read_started_at[i] : 0;
        acl_pending++;
        if (age > acl_oldest) acl_oldest = age;
    }
    long event_idle = g_event_read_completions && now >= g_last_event_completion ?
                      now - g_last_event_completion : -1;
    long acl_idle = g_acl_read_completions && now >= g_last_acl_completion ?
                    now - g_last_acl_completion : -1;
    log_line("usb read state: event-starts=%lu completions=%lu bytes=%lu errors=%lu "
             "zero-frames=%lu packets=%lu pending=%d/%d oldest-ms=%ld idle-ms=%ld queued=%d; "
             "acl-starts=%lu completions=%lu pending=%d/%d oldest-ms=%ld idle-ms=%ld queued=%d; lost=%d",
             g_event_starts,g_event_read_completions,g_event_read_bytes,
             g_event_read_errors,g_event_read_zero,g_event_packets_queued,
             event_pending,g_event_reads,event_oldest,event_idle,g_events.count,
             g_acl_starts,g_acl_read_completions,acl_pending,g_acl_reads,
             acl_oldest,acl_idle,g_acl.count,g_transport_failed);
}

/* Handles every finished transfer. Returns 1 if there was any. */
static int reap(void)
{
    struct usb_fs_complete comp;
    int got = 0;

    /* Bound one drain so caller deadlines are checked even under a stream
     * of completions. At most one batch of the configured transfer slots. */
    for (int drained = 0; drained < N_XFERS; drained++) {
        memset(&comp, 0, sizeof comp);
        if (ioctl(g_fd, USB_FS_COMPLETE, &comp) != 0) {
            if (errno != EBUSY && errno != EAGAIN && errno != EINTR) {
                log_line("hci: USB_FS_COMPLETE errno=%d", errno);
                transport_fail("USB completion query failed");
            }
            break;
        }
        if (comp.ep_index < N_XFERS) on_complete((int)comp.ep_index);
        else transport_fail("invalid USB completion index");
        got = 1;
    }
    return got;
}

int hci_pump(int timeout_ms)
{
    long deadline = now_ms() + timeout_ms;

    for (;;) {
        if (!g_transport_failed) rearm_reads();
        reap(); /* Existing pending reads may still finish owned cleanup. */
        if (g_transport_failed) {
            /* Bounded cleanup waits must not spin if no pending read finishes. */
            if (timeout_ms > 0 && !g_events.count && !g_acl.count) usleep(1000);
            return -1;
        }
        if (g_events.count || g_acl.count) return 1;
        if (now_ms() >= deadline) return 0;
        usleep(1000);
    }
}

/* Diagnostic intervals include signalling and media sends. Reap time is a
 * subset of wait time; the two totals must not be added together. */
typedef struct { long calls, total, max; } send_time;
static send_time g_wait_time, g_reap_time, g_submit_time;
static long g_send_report_at, g_send_timeouts, g_submit_failures;

static void send_record(send_time *s, long elapsed)
{
    if (elapsed < 0) elapsed = 0;
    s->calls++;
    s->total += elapsed;
    if (elapsed > s->max) s->max = elapsed;
}

static void send_report(void)
{
    long now = now_ms();
    if (now - g_send_report_at < 5000) return;
    log_line("usb timing: wait %ld calls %ld ms total %ld max; "
             "reap-in-wait %ld calls %ld ms total %ld max; "
             "submit %ld calls %ld ms total %ld max; timeouts %ld failures %ld",
             g_wait_time.calls, g_wait_time.total, g_wait_time.max,
             g_reap_time.calls, g_reap_time.total, g_reap_time.max,
             g_submit_time.calls, g_submit_time.total, g_submit_time.max,
             g_send_timeouts, g_submit_failures);
    log_line("usb completion: count=%ld age-total=%ld ms age-max=%ld ms errors=%ld "
             "short=%ld reads-started=%ld pending-out=%d",
             g_out_completed, g_out_elapsed, g_out_max, g_out_errors,
             g_out_short, g_reads_rearmed, g_pending[IX_ACL_OUT]);
    g_out_completed = g_out_elapsed = g_out_max = 0;
    g_out_errors = g_out_short = g_reads_rearmed = 0;
    memset(&g_wait_time, 0, sizeof g_wait_time);
    memset(&g_reap_time, 0, sizeof g_reap_time);
    memset(&g_submit_time, 0, sizeof g_submit_time);
    g_send_timeouts = g_submit_failures = 0;
    g_send_report_at = now;
}

int hci_acl_send(const unsigned char *pkt, int len)
{
    long before = now_ms(), deadline = before + 1000, stage;
    int result;

    if (g_transport_failed) return 0;
    if (!g_send_report_at) g_send_report_at = before;
    if (len < 0 || len > XFER_BUF || (len > 0 && !pkt)) {
        log_line("hci: invalid packet length or buffer (%d bytes)", len);
        return 0;
    }
    while (g_pending[IX_ACL_OUT]) {
        stage = now_ms();
        reap();
        send_record(&g_reap_time, now_ms() - stage);
        /* A failed restart stops this transport; do not retry or submit OUT. */
        if (HCI_OUT_WAIT_REARM) rearm_reads();
        if (g_transport_failed) return 0;
        if (!g_pending[IX_ACL_OUT]) break;
        if (now_ms() >= deadline) {
            send_record(&g_wait_time, now_ms() - before);
            g_send_timeouts++;
            log_line("hci: ACL out still busy after 1 s");
            send_report();
            return 0;
        }
        usleep(500);
    }
    send_record(&g_wait_time, now_ms() - before);
    if (len > 0) memcpy(g_buf[IX_ACL_OUT], pkt, (size_t)len);
    stage = now_ms();
    result = ep_start(IX_ACL_OUT, len);
    send_record(&g_submit_time, now_ms() - stage);
    if (!result) {
        g_submit_failures++;
        transport_fail("ACL output submission failed");
    }
    send_report();
    return result;
}

/* Local transfer state only; not an exclusive controller-buffer claim. */
int hci_output_idle(void)
{
    return g_fd>=0 && !g_transport_failed && !g_pending[IX_ACL_OUT];
}
