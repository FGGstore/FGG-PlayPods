#include "bt.h"
#include "hci.h"
#include "log.h"
#include "sdp.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* HCI opcodes: OGF << 10 | OCF. */
#define OP_INQUIRY              0x0401
#define OP_INQUIRY_CANCEL       0x0402
#define OP_CREATE_CONNECTION    0x0405
#define OP_DISCONNECT           0x0406
#define OP_ACCEPT_CONNECTION    0x0409
#define OP_REJECT_CONNECTION    0x040A
#define OP_LINK_KEY_REPLY       0x040B
#define OP_LINK_KEY_NEG_REPLY   0x040C
#define OP_PIN_CODE_REPLY       0x040D
#define OP_AUTH_REQUESTED       0x0411
#define OP_SET_ENCRYPTION       0x0413
#define OP_IO_CAP_REPLY         0x042B
#define OP_USER_CONFIRM_REPLY   0x042C
#define OP_SET_EVENT_MASK       0x0C01
#define OP_WRITE_LOCAL_NAME     0x0C13
#define OP_WRITE_SCAN_ENABLE    0x0C1A
#define OP_WRITE_CLASS_OF_DEV   0x0C24
#define OP_WRITE_INQUIRY_MODE   0x0C45
#define OP_WRITE_SSP_MODE       0x0C56
#define OP_READ_BUFFER_SIZE     0x1005
#define OP_EXIT_SNIFF_MODE       0x0804

#define CID_SIGNALING 0x0001
#define PSM_SDP       0x0001
#define SDP_CID       0x0050        /* our end of the headset's SDP channel */

#define MAX_CHANNELS  4

/* DIAGNOSTIC: producer-style >60 ms assumed reuse, not proof that
 * controller buffers are free. Observed reports remain separate.
 * Controller capacity is not a native-owner reservation. */

typedef struct {
    int valid;
    unsigned char addr[6];
    unsigned char key[16];
    unsigned char type;
} link_key;

/* ---- state, all written by the dispatcher ------------------------------ */

static const char *g_key_path;
static link_key g_key;
static int g_peer_prepared;

static unsigned g_cc_op;
static unsigned char g_cc[HCI_PKT_MAX];
static int g_cc_len;

static int g_inq_done, g_found;
static unsigned char g_target[6];
static unsigned char g_target_psrm;
static unsigned g_target_clock;

static int g_conn_done, g_conn_status;
static unsigned g_handle;
static int g_auth_done, g_auth_status;
static int g_enc_status, g_enc_on;
static int g_enc_event_seen, g_enc_event_enabled;
static unsigned g_connection_status_trace, g_sdp_trace;
static int g_disconnected;
static int g_reconnect_used, g_reconnect_blocked;
static long g_disconnect_retired;
static int g_stopping, g_disconnect_done, g_disconnect_status;
/* Sticky for this process. Foreign command replies demonstrate another owner
 * using these shared endpoints; they cannot be returned to its driver here. */
static int g_coexistence_conflict;
/* A failed setting request may have changed shared controller state. Never
 * reopen and replay setup in this process after that uncertain boundary. */
static int g_setup_failed;
/* Sticky if completion counts cannot be reconciled with local submissions. */
static int g_acl_flow_failed;
static int g_bt_transport_failed;
static int session_stopped(void)
{
    return g_coexistence_conflict || g_acl_flow_failed || g_bt_transport_failed;
}
/* A newly established ACL link starts active; later owned Mode Change events
 * replace that initial state. Neither observation proves exclusive delivery. */
static int g_link_mode = -1;
static int g_mode_from_connection;
static int g_mode_request_pending, g_mode_request_status, g_mode_change_error;

static unsigned char g_sig_id = 1;
static l2cap_chan *g_chans[MAX_CHANNELS];
static bt_frame_fn g_frame_fns[MAX_CHANNELS];
static int g_nchans;
static l2cap_chan *g_incoming;
static unsigned g_incoming_psm;
static unsigned char g_incoming_request_id;
/* A single outgoing request superseded by incoming signaling. Its original
 * local CID remains reserved. Any successful loser is closed once and must
 * be observed closed before media starts; uncertain outcomes stop the session. */
static struct {
    unsigned char conn_id, close_id;
    unsigned scid, dcid;
    int active, closed;
} g_outgoing_loser;
static l2cap_chan g_sdp = { .name="sdp", .scid=SDP_CID, .remote_mtu=672 };

static unsigned char g_l2buf[4096];
static int g_l2len, g_l2need;
/* Header-only diagnostics. No link keys, audio samples or packet bodies. */
static unsigned g_sig_tx_trace, g_sig_rx_trace, g_frame_trace;
static unsigned long g_acl_observed, g_acl_owned, g_acl_invalid;
static unsigned long g_l2_completed, g_l2_unknown;

static unsigned g_acl_mtu;
static int g_credits, g_credits_max;
static long g_sent, g_reported;     /* ACL packets, and their reports */
/* Producer FIFO semantics, with bounded storage and no silent eviction. */
#define INFLIGHT_MAX 64
#define INFLIGHT_MS 60
static long g_inflight[INFLIGHT_MAX];
static int g_if_head, g_if_count;
static long g_assumed;
static long g_system_replies;
static unsigned long g_events_seen, g_events_rejected, g_completion_events;
static unsigned long g_completion_owned_values, g_completion_foreign_values;
static unsigned long g_completion_owned_tuples, g_completion_foreign_tuples;
static unsigned long g_data_block_events;

static void (*g_tick)(void);
static long g_last_tick;

/* ---- helpers ----------------------------------------------------------- */

static const char *addr_str(const unsigned char *a)
{
    static char buf[4][18];
    static int which;
    char *out = buf[which++ & 3];

    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             a[5], a[4], a[3], a[2], a[1], a[0]);
    return out;
}

static void key_load(void)
{
    FILE *f = fopen(g_key_path, "rb");
    unsigned char rec[23];

    memset(&g_key, 0, sizeof g_key);
    if (!f) return;
    if (fread(rec, 1, sizeof rec, f) == sizeof rec) {
        memcpy(g_key.addr, rec, 6);
        memcpy(g_key.key, rec + 6, 16);
        g_key.type = rec[22];
        g_key.valid = 1;
    }
    fclose(f);
}

static void key_save(void)
{
    FILE *f = fopen(g_key_path, "wb");
    unsigned char rec[23];

    if (!f) {
        log_line("link key: cannot write %s errno=%d", g_key_path, errno);
        return;
    }
    memcpy(rec, g_key.addr, 6);
    memcpy(rec + 6, g_key.key, 16);
    rec[22] = g_key.type;
    fwrite(rec, 1, sizeof rec, f);
    fclose(f);
}

static l2cap_chan *chan_by_scid(unsigned cid)
{
    int i;

    if (cid == SDP_CID) return &g_sdp;
    for (i = 0; i < g_nchans; i++)
        if (g_chans[i]->scid == cid) return g_chans[i];
    return NULL;
}

/* Match responses by their outstanding transaction, not a guessed channel.
 * Failed Connection Responses need not contain valid CIDs (Core 3.A 4.3). */
static l2cap_chan *conn_by_id(unsigned id)
{
    if (!id) return NULL;
    for (int i = 0; i < g_nchans; i++)
        if (g_chans[i]->conn_id == id) return g_chans[i];
    return NULL;
}

static unsigned char next_sig_id(void)
{
    for (;;) {
        unsigned char id = g_sig_id++;
        int used = !id || g_sdp.cfg_id == id ||
                   g_outgoing_loser.conn_id == id || g_outgoing_loser.close_id == id;
        for (int i = 0; i < g_nchans; i++)
            if (g_chans[i]->conn_id == id || g_chans[i]->cfg_id == id ||
                g_chans[i]->close_id == id) used = 1;
        if (!used) return id;
    }
}

static int remote_cid_used(unsigned cid, const l2cap_chan *except)
{
    if (&g_sdp != except && !g_sdp.closed && g_sdp.dcid == cid) return 1;
    for (int i = 0; i < g_nchans; i++)
        if (g_chans[i] != except && !g_chans[i]->closed && g_chans[i]->dcid == cid)
            return 1;
    return 0;
}

/* The controller is shared with the system, which runs its own links on it
 * (the DualSense reconnects through it). Only events about the headset are
 * acted on. Dropping a foreign packet here cannot return it to the system's
 * reader; filtering after receipt does not establish noninterference. */
static int is_ours_addr(const unsigned char *a)
{
    return memcmp(a, g_target, 6) == 0;
}

static int is_ours_handle(const unsigned char *h)
{
    return g_conn_done && g_conn_status == 0 && (le16(h) & 0x0FFF) == g_handle;
}

/* Opcodes this payload sends. A reply to any other belongs to the system's
 * driver, which normally leaves this controller alone; it is logged, since
 * such a reply never reaches the command's owner. */
static int is_our_opcode(unsigned op)
{
    static const unsigned ours[] = {
        OP_INQUIRY, OP_INQUIRY_CANCEL, OP_CREATE_CONNECTION, OP_DISCONNECT,
        OP_ACCEPT_CONNECTION, OP_REJECT_CONNECTION, OP_LINK_KEY_REPLY, OP_LINK_KEY_NEG_REPLY,
        OP_PIN_CODE_REPLY, OP_AUTH_REQUESTED, OP_SET_ENCRYPTION,
        OP_IO_CAP_REPLY, OP_USER_CONFIRM_REPLY, OP_SET_EVENT_MASK,
        OP_WRITE_LOCAL_NAME, OP_WRITE_SCAN_ENABLE, OP_WRITE_CLASS_OF_DEV,
        OP_WRITE_INQUIRY_MODE, OP_WRITE_SSP_MODE, OP_READ_BUFFER_SIZE,
        OP_EXIT_SNIFF_MODE,
    };
    size_t i;

    if (op == 0) return 1;              /* no-op: command credits only */
    for (i = 0; i < sizeof ours / sizeof ours[0]; i++)
        if (ours[i] == op) return 1;
    return 0;
}

static void system_reply(unsigned op, unsigned status)
{
    if (g_system_replies++ < 50)
        log_line("reply to the system's command %#06x (status %#04x)", op, status);
    if (!g_coexistence_conflict) {
        g_coexistence_conflict = 1;
        log_line("coexistence: foreign command reply op=%#06x status=%#04x; "
                 "stopping audio, controller recovery unverified", op, status);
    }
}

/* ---- ACL flow control -------------------------------------------------- */

static void inflight_done(int n)
{
    while (n-- > 0 && g_if_count > 0) {
        g_if_head = (g_if_head + 1) % INFLIGHT_MAX;
        g_if_count--;
    }
}

static void inflight_expire(void)
{
    long now = now_ms();
    while (g_if_count > 0 && now - g_inflight[g_if_head] > INFLIGHT_MS) {
        g_if_head = (g_if_head + 1) % INFLIGHT_MAX;
        g_if_count--;
        g_assumed++;
        if (g_credits < g_credits_max) g_credits++;
    }
}

static int inflight_room(void)
{
    if (g_if_count < INFLIGHT_MAX) return 1;
    g_acl_flow_failed = 1;
    log_line("acl diagnostic: inflight FIFO full; stopping before USB submission");
    return 0;
}

int bt_can_send(void)
{
    if (session_stopped() || g_disconnected) return 0;
    inflight_expire();
    return g_credits > 0;
}

int  bt_credits(void)              { return g_credits; }
long bt_reports_missing(void)      { return g_sent - g_reported - g_disconnect_retired; }
int  bt_acl_flow_failed(void)      { return g_acl_flow_failed; }
int  bt_transport_failed(void)     { return g_bt_transport_failed; }
int  bt_link_lost(void)            { return g_disconnected || session_stopped(); }
int  bt_coexistence_conflict(void) { return g_coexistence_conflict; }
void bt_set_tick(void (*fn)(void)) { g_tick = fn; }

void bt_completion_report(void)
{
    log_line("acl observations: events=%lu rejected=%lu packet-completion-events=%lu "
             "matched-tuples=%lu matched-values=%lu foreign-tuples=%lu foreign-values=%lu "
             "data-block-events-not-decoded=%lu sent=%ld reconciled=%ld disconnect-retired=%ld awaiting=%ld credits=%d",
             g_events_seen,g_events_rejected,g_completion_events,
             g_completion_owned_tuples,g_completion_owned_values,
             g_completion_foreign_tuples,g_completion_foreign_values,g_data_block_events,
             g_sent,g_reported,g_disconnect_retired,bt_reports_missing(),g_credits);
    log_line("completion policy: producer60ms assumed-reuse=%ld observed=%ld "
             "report-deficit=%ld strict-shadow-credits=%ld tracked=%d allowance=%d; "
             "assumed+observed are not distinct completions",
             g_assumed,g_reported,bt_reports_missing(),
             (long)g_credits_max-bt_reports_missing(),g_if_count,g_credits);
}

int bt_max_frame(void)
{
    return g_acl_mtu ? (int)g_acl_mtu - 4 : HCI_PKT_MAX - 8;
}

/* ---- L2CAP output ------------------------------------------------------ */

int bt_send(unsigned dcid, const unsigned char *data, int len)
{
    unsigned char pkt[HCI_PKT_MAX];

    if (session_stopped() || g_disconnected) return 0;
    if (len < 0 || len > (int)sizeof pkt - 8 ||
        (len > 0 && !data) ||
        (g_acl_mtu && (unsigned)len + 4 > g_acl_mtu)) {
        log_line("l2cap: %d-byte frame too large to send unfragmented", len);
        return 0;
    }
    /* Adaptation: expire before the common gate, including direct signaling.
     * Retain packet bounds, sticky stops and pre-submission FIFO capacity. */
    if (!bt_can_send()) {
        log_line("acl diagnostic: no send allowance; frame not submitted");
        return 0;
    }
    if (!inflight_room()) return 0;
    put16(pkt, (g_handle & 0x0FFF) | 0x2000);   /* first, auto-flushable */
    put16(pkt + 2, (unsigned)(4 + len));
    put16(pkt + 4, (unsigned)len);
    put16(pkt + 6, dcid);
    if (len > 0) memcpy(pkt + 8, data, (size_t)len);
    if (!hci_acl_send(pkt, 8 + len)) return 0;
    g_credits--;
    g_sent++;
    g_inflight[(g_if_head + g_if_count) % INFLIGHT_MAX] = now_ms();
    g_if_count++;
    return 1;
}

static void trace_signaling(const char *direction, unsigned *counter,
                            unsigned code, unsigned id,
                            const unsigned char *data, int len)
{
    unsigned index = (*counter)++;
    if (index == 64) log_line("l2cap %s: trace limit reached", direction);
    if (index >= 64) return;
    log_line("l2cap %s: code=%#04x id=%u bytes=%d", direction, code, id, len);
    if (code == 0x04 && len >= 4)
        log_line("l2cap %s config-request: cid=%#04x flags=%#04x",
                 direction, le16(data), le16(data + 2));
    if (code == 0x05 && len >= 6)
        log_line("l2cap %s config-response: cid=%#04x flags=%#04x result=%u",
                 direction, le16(data), le16(data + 2), le16(data + 4));
    int start = code == 0x04 ? 4 : code == 0x05 ? 6 : len;
    for (int i = start, options = 0; i < len && options < 8; options++) {
        if (len - i < 2 || data[i + 1] > len - i - 2) {
            log_line("l2cap %s option: truncated", direction);
            break;
        }
        unsigned type = data[i] & 0x7F, bytes = data[i + 1];
        log_line("l2cap %s option: type=%u hint=%u bytes=%u",
                 direction, type, data[i] >> 7, bytes);
        if ((type == 1 || type == 2) && bytes == 2)
            log_line("l2cap %s option: %s=%u", direction,
                     type == 1 ? "mtu" : "flush-timeout", le16(data + i + 2));
        if (type == 4 && bytes == 9)
            log_line("l2cap %s option: retransmission-mode=%u", direction, data[i + 2]);
        if (type == 5 && bytes == 1)
            log_line("l2cap %s option: fcs=%u", direction, data[i + 2]);
        i += 2 + bytes;
    }
}

static int sig_send(unsigned char code, unsigned char id,
                    const unsigned char *data, int len)
{
    unsigned char p[HCI_PKT_MAX - 8];

    if (len < 0 || len > (int)sizeof p - 4 || (len > 0 && !data)) {
        log_line("l2cap: signaling response of %d bytes exceeds its buffer", len);
        return 0;
    }
    p[0] = code;
    p[1] = id;
    put16(p + 2, (unsigned)len);
    if (len > 0) memcpy(p + 4, data, (size_t)len);
    trace_signaling("tx", &g_sig_tx_trace, code, id, data, len);
    int sent = bt_send(CID_SIGNALING, p, 4 + len);
    if (!sent) log_line("l2cap tx: submission failed code=%#04x id=%u", code, id);
    return sent;
}

/* ---- inbound: HCI events ----------------------------------------------- */

static void on_inquiry_record(const unsigned char *r, const unsigned char *eir,
                              int eirlen)
{
    unsigned cod = (unsigned)r[8] | (unsigned)r[9] << 8 | (unsigned)r[10] << 16;
    char name[64] = "";
    int i = 0;

    while (eir && i + 1 < eirlen) {
        int flen = eir[i];
        if (flen == 0 || i + 1 + flen > eirlen) break;
        if (eir[i + 1] == 0x08 || eir[i + 1] == 0x09) {
            int n = flen - 1;
            if (n > (int)sizeof name - 1) n = (int)sizeof name - 1;
            memcpy(name, eir + i + 2, (size_t)n);
            name[n] = '\0';
        }
        i += 1 + flen;
    }

    /* Major device class 4: audio/video. */
    if (!g_found && ((cod >> 8) & 0x1F) == 4
#ifdef PLAYPODS_INQUIRY_ACCEPT
        && PLAYPODS_INQUIRY_ACCEPT(name)
#endif
       ) {
        g_found = 1;
        memcpy(g_target, r, 6);
        g_target_psrm = r[6];
        g_target_clock = le16(r + 11) | 0x8000;
        log_line("found %s '%s' (class %06x)", addr_str(r), name, cod);
    }
}

static void on_event(const unsigned char *ev, int len)
{
    unsigned char p[32];
    int minimum = 2;

    g_events_seen++;
    if (len < 2 || len != 2 + ev[1]) {g_events_rejected++;return;}
    switch (ev[0]) {
    case 0x01: minimum = 3; break;
    case 0x22:
        if (len < 3) {g_events_rejected++;return;}
        minimum = 3 + 14 * ev[2]; break;
    case 0x2F: minimum = 17; break;
    case 0x04: minimum = 12; break;
    case 0x03: minimum = 13; break;
    case 0x05: case 0x08: case 0x0E: case 0x0F: minimum = 6; break;
    case 0x06: minimum = 5; break;
    case 0x14: minimum = 8; break;
    case 0x13:
        if (len < 3) {g_events_rejected++;return;}
        minimum = 3 + 4 * ev[2]; break;
    case 0x16: case 0x17: case 0x31: minimum = 8; break;
    case 0x18: minimum = 25; break;
    case 0x33: minimum = 12; break;
    default: break;
    }
    if (len < minimum) {
        g_events_rejected++;
        log_line("hci: truncated event %#04x (%d bytes)", ev[0], len);
        return;
    }
    if(ev[0]==0x13)g_completion_events++;
    if(ev[0]==0x48)g_data_block_events++; /* presence only, not block accounting */
    /* After a conflict, only the owned disconnect can finish cleanup. Never
     * answer pairing/channel requests or resume this session from later data. */
    if (session_stopped() && ev[0] != 0x05) return;

    switch (ev[0]) {
    case 0x01:  /* Inquiry Complete */
        g_inq_done = 1;
        break;

    case 0x22:  /* Inquiry Result with RSSI */
    {
        int i;
        for (i = 0; i < ev[2]; i++) on_inquiry_record(ev + 3 + i * 14, NULL, 0);
        break;
    }

    case 0x2F:  /* Extended Inquiry Result */
        on_inquiry_record(ev + 3, ev + 17, len - 17);
        break;

    case 0x04:  /* Connection Request */
        if (!is_ours_addr(ev + 2) || ev[11] != 1) break;   /* the system's */
        if (g_stopping || (g_conn_done && g_conn_status==0 && !g_disconnected)) {
            memcpy(p,ev+2,6);p[6]=0x0d; /* reject: limited resources */
            log_line("connection: rejecting another owned peer request during %s",
                     g_stopping ? "cleanup" : "an existing link");
            hci_cmd(OP_REJECT_CONNECTION,p,7);
            break;
        }
        log_line("headset connecting to us by itself");
        memcpy(p, ev + 2, 6);
        p[6] = 0x01;    /* remain peripheral: no role switch to fail */
        hci_cmd(OP_ACCEPT_CONNECTION, p, 7);
        break;

    case 0x03:  /* Connection Complete */
        if (!is_ours_addr(ev + 5)) break;
        if (g_stopping || g_disconnected) {
            if(g_disconnected && !g_stopping && ev[2]==0)g_reconnect_blocked=1;
            log_line("connection: completion during terminal cleanup/loss not adopted (status=%#04x handle=%#05x)",
                     ev[2],le16(ev+3)&0x0fff);
            break;
        }
        if (ev[2]==0 && (ev[11]!=1 || (le16(ev+3)&0x0fff)>0x0eff)) {
            log_line("connection: invalid successful ACL connection event; not adopted");
            break;
        }
        if (ev[2]!=0 && g_conn_done && g_conn_status==0) {
            log_line("connection: late failure %#04x does not replace owned handle=%#05x",ev[2],g_handle);
            break;
        }
        if (ev[2]==0 && g_conn_done && g_conn_status==0 &&
            !g_disconnected && g_handle==(le16(ev+3)&0x0fff)) {
            /* A repeated completion does not supersede a later mode event. */
            log_line("connection: duplicate owned completion; existing mode retained");
            break;
        }
        if (ev[2]==0 && g_conn_done && g_conn_status==0) {
            log_line("connection: different successful handle not adopted; owned link retained, session stopped");
            g_bt_transport_failed=1;
            break;
        }
        if (ev[2]==0) {
            g_auth_done=g_auth_status=g_enc_on=g_enc_status=g_enc_event_seen=g_enc_event_enabled=0;
        }
        g_mode_from_connection = ev[2]==0 && !(g_conn_done && g_conn_status==0);
        g_link_mode = g_mode_from_connection ? 0 : -1;
        g_conn_status = ev[2];
        g_handle = le16(ev + 3) & 0x0FFF;
        g_conn_done = 1;
        log_line("connection complete: status %#04x handle %#05x", ev[2], g_handle);
        if (g_mode_from_connection)
            log_line("link mode: initial active state from new ACL connection; later native changes may be unobserved");
        break;

    case 0x05:  /* Disconnection Complete */
        if (!is_ours_handle(ev + 3)) break;
        log_line("disconnected: status %#04x reason %#04x", ev[2], ev[5]);
        g_disconnect_done=1;g_disconnect_status=ev[2];
        if (ev[2]!=0) {
            log_line("connection: failed disconnection event; owned link loss unconfirmed");
            break;
        }
        g_disconnected = 1;
        g_link_mode = -1;
        g_mode_from_connection = 0;
        break;

    case 0x06:  /* Authentication Complete */
        if (!is_ours_handle(ev + 3)) break;
        g_auth_status = ev[2];
        g_auth_done = 1;
        log_line("authentication complete: status %#04x", ev[2]);
        break;

    case 0x08:  /* Encryption Change */
        if (!is_ours_handle(ev + 3)) break;
        g_enc_status = ev[2];
        g_enc_event_seen=1;g_enc_event_enabled=ev[5];
        if (ev[2] == 0) g_enc_on = ev[5];
        log_line("encryption change: status %#04x enabled %u", ev[2], ev[5]);
        break;

    case 0x0E:  /* Command Complete */
        if (!is_our_opcode(le16(ev + 3))) {
            system_reply(le16(ev + 3), ev[5]);
            break;
        }
        g_cc_op = le16(ev + 3);
        g_cc_len = len < (int)sizeof g_cc ? len : (int)sizeof g_cc;
        memcpy(g_cc, ev, (size_t)g_cc_len);
        break;

    case 0x0F:  /* Command Status */
        if (g_connection_status_trace<32 &&
            (le16(ev+4)==OP_CREATE_CONNECTION || le16(ev+4)==OP_AUTH_REQUESTED ||
             le16(ev+4)==OP_SET_ENCRYPTION || le16(ev+4)==OP_DISCONNECT)) {
            /* Status has no handle or request ID: observation is not proof
             * that a shared controller reply belongs to our submission. */
            g_connection_status_trace++;
            log_line("connection command-status observation: opcode=%#06x status=%#04x; ownership not encoded",le16(ev+4),ev[2]);
        }
        if (g_mode_request_pending && le16(ev + 4) == OP_EXIT_SNIFF_MODE) {
            g_mode_request_status = ev[2];
            log_line("active mode: Exit Sniff command status=%#04x", ev[2]);
        }
        if (!is_our_opcode(le16(ev + 4))) {
            system_reply(le16(ev + 4), ev[2]);
            break;
        }
        if (ev[2] != 0) {
            unsigned op = le16(ev + 4);
            if (op == OP_CREATE_CONNECTION) {
                if (g_conn_done && g_conn_status==0 && !g_disconnected)
                    log_line("connection: failed Create Connection status %#04x does not replace owned link",ev[2]);
                else {g_conn_status = ev[2];g_conn_done = 1;}
            } else if (op == OP_AUTH_REQUESTED) {
                g_auth_status = ev[2];
                g_auth_done = 1;
            }
        }
        break;

    case 0x14:  /* Observe here; requests are made outside the dispatcher. */
        if (!is_ours_handle(ev + 3) || g_disconnected) break;
        if (ev[2] == 0) {g_link_mode = ev[5];g_mode_from_connection = 0;}
        else if (g_mode_request_pending) g_mode_change_error = ev[2];
        log_line("link mode: status=%#04x mode=%u interval-slots=%u",
                 ev[2], ev[5], le16(ev + 6));
        break;

    case 0x13:  /* Number Of Completed Packets */
    {
        /* Validate the entire event before returning any local credits.
         * Repeated owned handles are included in the aggregate. Foreign
         * completions cannot establish availability for this ledger. */
        int done = 0;
        if (g_disconnected) {
            for(int i=0;i<ev[2];i++)
                if(is_ours_handle(ev+3+i*4) && le16(ev+5+i*4))g_reconnect_blocked=1;
            break;
        }
        for (int i = 0; i < ev[2]; i++) {
            unsigned count=le16(ev+5+i*4);
            if (is_ours_handle(ev + 3 + i * 4)) {
                g_completion_owned_tuples++;
                g_completion_owned_values+=count;
                done += (int)count;
            } else {
                g_completion_foreign_tuples++;
                g_completion_foreign_values+=count;
            }
        }
        if (done > bt_reports_missing()) {
            g_acl_flow_failed = 1;
            log_line("acl: completion count %d exceeds local outstanding %ld; "
                     "stopping session, controller recovery unverified",
                     done, bt_reports_missing());
            break;
        }
        g_credits += done;
        g_reported += done;
        inflight_done(done);
        if (g_credits > g_credits_max) g_credits = g_credits_max;
        break;
    }

    case 0x16:  /* PIN Code Request: legacy pairing, try 0000 */
        if (!is_ours_addr(ev + 2)) break;
        log_line("PIN code requested, replying 0000");
        memset(p, 0, sizeof p);
        memcpy(p, ev + 2, 6);
        p[6] = 4;
        memcpy(p + 7, "0000", 4);
        hci_cmd(OP_PIN_CODE_REPLY, p, 23);
        break;

    case 0x17:  /* Link Key Request */
        if (!is_ours_addr(ev + 2)) break;
        if (g_key.valid && memcmp(g_key.addr, ev + 2, 6) == 0) {
            memcpy(p, ev + 2, 6);
            memcpy(p + 6, g_key.key, 16);
            hci_cmd(OP_LINK_KEY_REPLY, p, 22);
        } else {
            log_line("no stored key: pairing");
            hci_cmd(OP_LINK_KEY_NEG_REPLY, ev + 2, 6);
        }
        break;

    case 0x18:  /* Link Key Notification */
        if (!is_ours_addr(ev + 2)) break;
        memcpy(g_key.addr, ev + 2, 6);
        memcpy(g_key.key, ev + 8, 16);
        g_key.type = ev[24];
        g_key.valid = 1;
        key_save();
        log_line("paired with %s, key saved", addr_str(ev + 2));
        break;

    case 0x31:  /* IO Capability Request */
        if (!is_ours_addr(ev + 2)) break;
        memcpy(p, ev + 2, 6);
        p[6] = 0x03;    /* NoInputNoOutput */
        p[7] = 0x00;    /* no OOB data */
        p[8] = 0x04;    /* general bonding, MITM not required */
        hci_cmd(OP_IO_CAP_REPLY, p, 9);
        break;

    case 0x33:  /* User Confirmation Request */
        if (!is_ours_addr(ev + 2)) break;
        hci_cmd(OP_USER_CONFIRM_REPLY, ev + 2, 6);
        break;

    case 0xFF:  /* Vendor-specific event; meaning has not been established. */
    {
        char hex[3 * 32 + 1];
        int i, n = 0;

        for (i = 0; i < len && i < 32; i++)
            n += snprintf(hex + n, sizeof hex - (size_t)n, "%02x ", ev[i]);
        log_line("chip vendor event (%d bytes): %s", len, hex);
        break;
    }

    default:
        break;
    }
}

/* ---- inbound: L2CAP ---------------------------------------------------- */

static void on_signaling(const unsigned char *d, int len)
{
    while (len >= 4) {
        unsigned char code = d[0], id = d[1];
        int clen = (int)le16(d + 2);
        const unsigned char *c = d + 4;
        unsigned char r[16];
        l2cap_chan *ch;
        int i, minimum = 0;

        if (4 + clen > len) break;
        switch (code) {
        case 0x02: case 0x04: case 0x06: case 0x07: minimum = 4; break;
        case 0x03: minimum = 8; break;
        case 0x05: minimum = 6; break;
        case 0x0A: minimum = 2; break;
        default: break;
        }
        if (clen < minimum) {
            log_line("l2cap: truncated signaling command %#04x (%d bytes)", code, clen);
            return;
        }
        trace_signaling("rx", &g_sig_rx_trace, code, id, c, clen);
        if (code == 0x04) {
            /* Validate every option before changing channel state. */
            for (i = 4; i < clen; ) {
                if (clen - i < 2 || c[i + 1] > clen - i - 2) {
                    log_line("l2cap: truncated configuration option");
                    return;
                }
                if ((c[i] & 0x7F) == 0x01 && c[i + 1] != 2) return;
                i += 2 + c[i + 1];
            }
        }

        switch (code) {
        case 0x02:  /* Connection Request from the headset */
            if (g_incoming && le16(c)==g_incoming_psm) {
                unsigned peer=le16(c+2),result=0;
                ch=g_incoming;
                if (!id) break;
                if (g_stopping) result=4;
                else if (peer<0x40) result=6; /* invalid source CID */
                else if (ch->conn_done && !ch->closed && ch->dcid==peer &&
                         id==g_incoming_request_id) {
                    /* Repeat the same response without resetting configuration. */
                    put16(r,ch->scid);put16(r+2,peer);put16(r+4,0);put16(r+6,0);
                    if (!sig_send(0x03,id,r,8)) g_bt_transport_failed=1;
                    break;
                } else if (remote_cid_used(peer,NULL)) result=7; /* allocated */
                else if (ch->conn_done || g_outgoing_loser.active) result=4;
                if (!result && ch->conn_id) {
                    /* Give incoming signaling its own CID, preserving the
                     * outstanding outgoing CID until its outcome is known. */
                    unsigned fresh=0x40;
                    while (fresh<SDP_CID && (fresh==0x42 || chan_by_scid(fresh))) fresh++;
                    if (fresh==SDP_CID) result=4;
                    else {
                        g_outgoing_loser.active=1;
                        g_outgoing_loser.conn_id=ch->conn_id;
                        g_outgoing_loser.scid=ch->scid;
                        ch->conn_id=0;ch->scid=fresh;
                        log_line("l2cap: incoming signaling wins simultaneous opening; outgoing id=%u cid=%#04x remains tracked",
                                 g_outgoing_loser.conn_id,g_outgoing_loser.scid);
                    }
                }
                put16(r,result?0:ch->scid);put16(r+2,peer);
                put16(r+4,result);put16(r+6,0);
                if (!sig_send(0x03,id,r,8)) {g_bt_transport_failed=1;break;}
                if (result) {
                    log_line("l2cap: incoming signaling refused result=%u",result);
                    break;
                }
                ch->dcid=peer;ch->conn_done=1;ch->conn_result=0;
                ch->cfg_req_done=ch->cfg_rsp_ok=ch->closed=0;
                ch->remote_mtu=672;g_incoming_request_id=id;
                put16(r,peer);put16(r+2,0);ch->cfg_id=next_sig_id();
                if (!sig_send(0x04,ch->cfg_id,r,4)) g_bt_transport_failed=1;
                log_line("l2cap: accepted incoming signaling id=%u scid=%#04x dcid=%#04x",id,ch->scid,peer);
                break;
            }
            if (le16(c) == PSM_SDP) {
                /* It looks up our service record: accept, configure. */
                g_sdp.dcid = le16(c + 2);
                g_sdp.conn_done = 1;
                g_sdp.closed = 0;
                g_sdp.cfg_rsp_ok = g_sdp.cfg_req_done = 0;
                put16(r, SDP_CID);
                put16(r + 2, g_sdp.dcid);
                put16(r + 4, 0);        /* success */
                put16(r + 6, 0);
                sig_send(0x03, id, r, 8);
                put16(r, g_sdp.dcid);
                put16(r + 2, 0);
                g_sdp.cfg_id = next_sig_id();
                sig_send(0x04, g_sdp.cfg_id, r, 4);
                break;
            }
            log_line("l2cap: incoming PSM %#04x not supported", le16(c));
            /* Anything else (AVRCP, for one) is not offered. */
            put16(r, 0);
            put16(r + 2, le16(c + 2));
            put16(r + 4, 0x0002);       /* PSM not supported */
            put16(r + 6, 0);
            sig_send(0x03, id, r, 8);
            break;

        case 0x03:  /* Connection Response */
            if (id && id==g_outgoing_loser.conn_id) {
                if (le16(c+4)==1) break;
                if (le16(c+4)==0) {
                    if (le16(c+2)!=g_outgoing_loser.scid || le16(c)<0x40 ||
                        remote_cid_used(le16(c),NULL)) {
                        log_line("l2cap: invalid superseded success CIDs; awaiting valid response");
                        break;
                    }
                    g_outgoing_loser.dcid=le16(c);
                    g_outgoing_loser.conn_id=0;
                    g_outgoing_loser.close_id=next_sig_id();
                    put16(r,g_outgoing_loser.dcid);put16(r+2,g_outgoing_loser.scid);
                    log_line("l2cap: closing successful superseded signaling once scid=%#04x dcid=%#04x",
                             g_outgoing_loser.scid,g_outgoing_loser.dcid);
                    if (!sig_send(0x06,g_outgoing_loser.close_id,r,4)) g_bt_transport_failed=1;
                } else {
                    log_line("l2cap: superseded outgoing request refused result=%u; incoming retained",le16(c+4));
                    g_outgoing_loser.conn_id=0;g_outgoing_loser.closed=1;
                }
                break;
            }
            ch = conn_by_id(id);
            if (!ch) {
                log_line("l2cap: ignored unmatched connection response id=%u", id);
                break;
            }
            log_line("l2cap: %s response id=%u result=%u status=%u scid=%#04x dcid=%#04x",
                     ch->name, id, le16(c + 4), le16(c + 6), le16(c + 2), le16(c));
            if (le16(c + 4) == 1) break; /* pending: CIDs not valid yet */
            if (le16(c + 4) == 0 &&
                (le16(c + 2) != ch->scid || le16(c) < 0x40 ||
                 remote_cid_used(le16(c), ch))) {
                log_line("l2cap: %s invalid success CIDs; awaiting valid response", ch->name);
                break;
            }
            ch->conn_result = (int)le16(c + 4);
            ch->dcid = ch->conn_result == 0 ? le16(c) : 0;
            ch->conn_id = 0;
            ch->conn_done = 1;
            break;

        case 0x04:  /* Configuration Request */
            ch = chan_by_scid(le16(c));
            if (ch && ch->conn_done && ch->conn_result==0 && ch->dcid && !ch->closed) {
                for (i = 4; i + 2 <= clen; i += 2 + c[i + 1]) {
                    if ((c[i] & 0x7F) == 0x01 && c[i + 1] == 2)
                        ch->remote_mtu = le16(c + i + 2);
                }
                put16(r, ch->dcid);
                put16(r + 2, 0);        /* flags */
                put16(r + 4, 0);        /* success */
                if (!sig_send(0x05,id,r,6)) {g_bt_transport_failed=1;break;}
                ch->cfg_req_done = 1;
            }
            break;

        case 0x05:  /* Configuration Response */
            ch = chan_by_scid(le16(c));
            if (!ch || !id || id != ch->cfg_id) {
                log_line("l2cap: ignored unmatched configuration response id=%u scid=%#04x",
                         id, le16(c));
                break;
            }
            if (le16(c + 4) == 0 && !(le16(c + 2) & 1)) {
                ch->cfg_rsp_ok = 1;
                ch->cfg_id = 0;
            }
            break;

        case 0x06:  /* Disconnection Request */
            if (g_outgoing_loser.active && le16(c)==g_outgoing_loser.scid &&
                g_outgoing_loser.dcid && le16(c+2)==g_outgoing_loser.dcid) {
                if (!sig_send(0x07,id,c,4)) g_bt_transport_failed=1;
                g_outgoing_loser.closed=1;g_outgoing_loser.close_id=0;
                break;
            }
            ch = chan_by_scid(le16(c));
            if (!ch || !ch->dcid || le16(c+2)!=ch->dcid) {
                log_line("l2cap: unmatched disconnection request ignored");
                break;
            }
            if (!sig_send(0x07, id, c, 4)) g_bt_transport_failed=1;
            ch->closed = 1;ch->close_id=0;
            if (ch == &g_sdp) g_sdp.dcid = 0;   /* it may open another later */
            break;

        case 0x07:  /* Disconnection Response */
            if (id && id==g_outgoing_loser.close_id &&
                le16(c)==g_outgoing_loser.dcid && le16(c+2)==g_outgoing_loser.scid) {
                g_outgoing_loser.close_id=0;g_outgoing_loser.closed=1;
                break;
            }
            ch = chan_by_scid(le16(c + 2));
            if (ch && id && id==ch->close_id && le16(c)==ch->dcid) {
                ch->closed = 1;ch->close_id=0;
            } else log_line("l2cap: unmatched disconnection response ignored");
            break;

        case 0x08:  /* Echo Request */
            sig_send(0x09, id, c, clen);
            break;

        case 0x0A:  /* Information Request */
        {
            unsigned type = le16(c);
            memset(r, 0, sizeof r);
            put16(r, type);
            if (type == 2) {                /* extended features: none */
                sig_send(0x0B, id, r, 8);
            } else if (type == 3) {         /* fixed channels: signaling */
                r[4] = 0x02;
                sig_send(0x0B, id, r, 12);
            } else {
                put16(r + 2, 1);            /* not supported */
                sig_send(0x0B, id, r, 4);
            }
            break;
        }

        default:
            break;
        }

        d += 4 + clen;
        len -= 4 + clen;
    }
}

/* Protocol-only SDP trace: at most 16 request/response pairs, first 64
 * bytes each. Never called on HCI keys, media or foreign-handle ACL traffic. */
static void trace_sdp_pdu(const char *direction, unsigned serial,
                          const unsigned char *data, int len)
{
    static const char digits[]="0123456789abcdef";
    char hex[129];int shown=len<64?len:64;
    if(shown<0)shown=0;
    for(int i=0;i<shown;i++) {
        hex[i*2]=digits[data[i]>>4];hex[i*2+1]=digits[data[i]&15];
    }
    hex[shown*2]=0;
    log_line("sdp trace: %s serial=%u bytes=%d shown=%d truncated=%d hex=%s",
             direction,serial,len,shown,len>shown,hex);
}

static void on_l2cap_frame(unsigned cid, const unsigned char *d, int len)
{
    int i;

    g_l2_completed++;
    int known = cid == CID_SIGNALING || cid == SDP_CID || chan_by_scid(cid) != NULL;
    if (!known) g_l2_unknown++;
    if (g_frame_trace < 32) {
        log_line("l2cap frame: cid=%#04x bytes=%d known=%d", cid, len, known);
        g_frame_trace++;
    } else if (g_frame_trace == 32) {
        log_line("l2cap frame: trace limit reached");
        g_frame_trace++;
    }
    if (cid == CID_SIGNALING) {
        on_signaling(d, len);
    } else if (cid == SDP_CID) {
        unsigned char rsp[512];
        unsigned serial=g_sdp_trace<17 ? ++g_sdp_trace : 18;
        if(serial<=16)trace_sdp_pdu("request",serial,d,len);
        else if(serial==17)log_line("sdp trace: pair limit reached");
        int n = sdp_handle(d, len, rsp, (int)sizeof rsp);
        int submitted=0;
        if(n>0 && g_sdp.dcid)submitted=bt_send(g_sdp.dcid,rsp,n);
        if(serial<=16) {
            if(n>0)trace_sdp_pdu("response",serial,rsp,n);
            log_line("sdp trace: result serial=%u dcid=%#04x response-bytes=%d submitted=%d",
                     serial,g_sdp.dcid,n,submitted);
        }
    } else {
        for (i = 0; i < g_nchans; i++)
            if (g_chans[i]->scid == cid && g_frame_fns[i]) g_frame_fns[i](cid, d, len);
    }
}

static void on_acl(const unsigned char *pkt, int len)
{
    unsigned hdr;
    int pb, dlen;
    const unsigned char *d;

    if (session_stopped() || g_disconnected) return;
    g_acl_observed++;
    if (len < 4) { g_acl_invalid++; return; }
    if (!is_ours_handle(pkt)) return;
    g_acl_owned++;
    hdr = le16(pkt);
    pb = (int)((hdr >> 12) & 3);
    dlen = (int)le16(pkt + 2);
    d = pkt + 4;
    if (dlen != len - 4) { g_acl_invalid++; return; }

    if (pb == 1) {                              /* continuation */
        if (g_l2need == 0) return;
        if (g_l2len + dlen > (int)sizeof g_l2buf) {
            g_l2need = 0;
            return;
        }
        memcpy(g_l2buf + g_l2len, d, (size_t)dlen);
        g_l2len += dlen;
    } else {                                    /* start of an L2CAP frame */
        if (dlen < 4 || dlen > (int)sizeof g_l2buf) return;
        if (le16(d) > sizeof g_l2buf - 4) {
            g_l2need = 0;
            return;
        }
        memcpy(g_l2buf, d, (size_t)dlen);
        g_l2len = dlen;
        g_l2need = 4 + (int)le16(d);
    }

    if (g_l2need && g_l2len >= g_l2need) {
        on_l2cap_frame(le16(g_l2buf + 2), g_l2buf + 4, g_l2need - 4);
        g_l2need = 0;
    }
}

/* ---- pumping and waiting ----------------------------------------------- */

void bt_poll(int timeout_ms)
{
    unsigned char buf[HCI_PKT_MAX];
    int n;

    if (hci_pump(timeout_ms) < 0 && !g_bt_transport_failed) {
        g_bt_transport_failed = 1;
        log_line("bluetooth: transport loss; session stopped, controller recovery unverified");
    }
    while ((n = hci_next_event(buf, (int)sizeof buf)) > 0) on_event(buf, n);
    while ((n = hci_next_acl(buf, (int)sizeof buf)) > 0) on_acl(buf, n);

    if (!session_stopped() && g_tick && now_ms() - g_last_tick >= 1000) {
        g_last_tick = now_ms();
        g_tick();
    }
}

int bt_wait(volatile int *flag, int timeout_ms)
{
    long deadline = now_ms() + timeout_ms;

    if (session_stopped() && flag != &g_disconnected) return 0;
    while (!*flag) {
        long left = deadline - now_ms();
        if (left <= 0) return 0;
        bt_poll(left > 100 ? 100 : (int)left);
        if (session_stopped() && flag != &g_disconnected) return 0;
        if (g_disconnected && flag != &g_disconnected) return 0;
    }
    return 1;
}

/* Read Buffer Size can be retried. Setting requests are attempted once:
 * absence of a reply does not establish that the first write was unapplied. */
static int hci_sync(unsigned opcode, const void *params, int plen)
{
    int attempt;
    int setting = opcode != OP_READ_BUFFER_SIZE;
    int attempts = setting ? 1 : 3;

    if (session_stopped() || g_setup_failed) return 0;
    for (attempt = 1; attempt <= attempts; attempt++) {
        long deadline = now_ms() + (setting || attempt == 3 ? 3000 : 1000);

        g_cc_op = 0;
        if (!hci_cmd(opcode, params, plen)) {
            if (setting) g_setup_failed = 1;
            log_line("setup command %#06x: submission failed%s", opcode,
                     setting ? "; setting outcome/recovery unverified, no retry" : "");
            return 0;
        }
        while (!session_stopped() && g_cc_op != opcode && now_ms() < deadline)
            bt_poll(50);
        if (session_stopped()) return 0;
        if (g_cc_op == opcode) {
            if (g_cc_len > 5 && g_cc[5] == 0) return 1;
            if (setting) g_setup_failed = 1;
            if (g_cc_len > 5)
                log_line("setup command %#06x: rejected status=%#04x%s", opcode, g_cc[5],
                         setting ? "; prior settings/recovery unverified, no retry" : "");
            else
                log_line("setup command %#06x: truncated reply%s", opcode,
                         setting ? "; setting outcome/recovery unverified, no retry" : "");
            return 0;
        }
    }
    if (setting) g_setup_failed = 1;
    log_line("command %#06x: no reply%s", opcode,
             setting ? "; setting outcome/recovery unverified, no retry" : "");
    return 0;
}

/* ---- controller -------------------------------------------------------- */

/* Prepares the controller. No reset: the system's driver has it set up, and
 * a reset would pull that state from under it. */
static int setup_controller(void)
{
    static const unsigned char mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF,
                                           0xFF, 0xFF, 0xFF, 0x3F };
    static const unsigned char one[1] = { 1 };
    static const unsigned char two[1] = { 2 };
    /* Major class computer, minor desktop: an ordinary audio source. */
    static const unsigned char cod[3] = { 0x04, 0x01, 0x00 };
    unsigned char name[248];

    if (!hci_sync(OP_READ_BUFFER_SIZE, NULL, 0)) return 0;
    if (g_cc_len < 13) {
        log_line("controller: truncated Read Buffer Size response (%d bytes)", g_cc_len);
        return 0;
    }
    g_acl_mtu = le16(g_cc + 6);
    g_credits = g_credits_max = (int)le16(g_cc + 9);
    log_line("controller: %d ACL buffers of %u bytes", g_credits, g_acl_mtu);

    if (!hci_sync(OP_SET_EVENT_MASK, mask, (int)sizeof mask)) return 0;
    if (!hci_sync(OP_WRITE_SSP_MODE, one, 1)) return 0;
    if (!hci_sync(OP_WRITE_INQUIRY_MODE, two, 1)) return 0;
    if (!hci_sync(OP_WRITE_CLASS_OF_DEV, cod, 3)) return 0;

    memset(name, 0, sizeof name);
    memcpy(name, "FGG-PlayPods", 12);
    if (!hci_sync(OP_WRITE_LOCAL_NAME, name, (int)sizeof name)) return 0;

    /* Page scan on, inquiry scan off: a paired headset reconnects to its
     * source by itself, and must be able to reach us to do it. */
    if (!hci_sync(OP_WRITE_SCAN_ENABLE, two, 1)) return 0;
    return !session_stopped();
}

int bt_start(void)
{
    int attempt;

    /* Right after a previous session the controller can take a moment to
     * answer; closing and trying again a little later works. */
    if (session_stopped() || g_setup_failed) return 0;
    for (attempt = 1; attempt <= 4; attempt++) {
        if (!hci_open()) return 0;
        if (setup_controller()) return 1;
        hci_close();
        if (session_stopped() || g_setup_failed) return 0;
        log_line("controller not answering (attempt %d), retrying in 3 s", attempt);
        sleep(3);
    }
    return 0;
}

int bt_setup_failed(void) { return g_setup_failed; }

int bt_prepare_peer(const char *key_path)
{
    if (g_stopping || session_stopped() || g_setup_failed || !key_path || !*key_path) return 0;
    /* Never reload state or replace a peer after events have selected it. */
    if (g_peer_prepared) return strcmp(g_key_path,key_path)==0;
    if (g_conn_done || g_found) return 0;
    g_key_path=key_path;
    key_load();
    if (g_key.valid) {
        memcpy(g_target,g_key.addr,sizeof g_target);
        g_target_psrm=1;
        g_target_clock=0;
    }
    g_peer_prepared=1;
    log_line("connection: peer prepared before controller polling; saved-key=%d",g_key.valid);
    return 1;
}

void bt_stop(void)
{
    unsigned char r[3];
    if (g_stopping) return;
    g_stopping=1;

    if (g_conn_done && g_conn_status == 0 && !g_disconnected) {
        /* One attempt on this observed handle. A timeout or failed completion
         * is uncertain, not permission to replay or adopt another connection. */
        put16(r,g_handle);r[2]=0x13;g_disconnect_done=0;
        log_line("connection cleanup: disconnect handle=%#05x attempt=1",g_handle);
        if (!hci_cmd(OP_DISCONNECT,r,3))
            log_line("connection cleanup: disconnect submission failed; outcome uncertain, no retry");
        else {
            /* bt_wait can return zero on the same poll that observes link
             * loss. The owned completion flag establishes this outcome. */
            bt_wait(&g_disconnect_done,2000);
            if(!g_disconnect_done)
                log_line("connection cleanup: no completed disconnect event; outcome uncertain, no retry");
            else if(g_disconnect_status!=0)
                log_line("connection cleanup: disconnect failed status=%#04x; no retry",g_disconnect_status);
            else log_line("connection cleanup: successful owned disconnect observed");
        }
    }
    log_line("l2cap summary: acl-observed=%lu owned=%lu invalid=%lu frames=%lu unknown-cid=%lu",
             g_acl_observed, g_acl_owned, g_acl_invalid, g_l2_completed, g_l2_unknown);
    log_line("connection cleanup: known-link=%d disconnect-observed=%d",
             g_conn_done && g_conn_status == 0, g_disconnected);
    bt_completion_report();
    hci_close();
}

/* One passive reconnect only. No controller reopen/settings replay. */
int bt_prepare_reconnect(void)
{
    if(g_reconnect_used || g_stopping || session_stopped() || g_setup_failed ||
       !g_key.valid || !g_conn_done || g_conn_status!=0 || !g_disconnected ||
       !g_disconnect_done || g_disconnect_status!=0 || !hci_output_idle()) {
        log_line("resident: reconnect not eligible; no state reset");
        return 0;
    }
    /* Drain already completed local queues while the old link remains lost.
     * A queued replacement success or post-disconnect completion is ambiguous. */
    bt_poll(0);
    if(session_stopped() || g_reconnect_blocked || !hci_output_idle()) {
        log_line("resident: reconnect boundary uncertain; no state reset");
        return 0;
    }
    long retired=bt_reports_missing();
    if(retired<0) {g_acl_flow_failed=1;return 0;}
    log_line("resident: observed disconnect handle=%#05x; retiring %ld unreported "
             "submissions under HCI disconnection rule; not completed reports",g_handle,retired);
    g_disconnect_retired+=retired;
    g_if_head=g_if_count=0;g_credits=g_credits_max;
    /* Peer/generation state only. Sticky transport/setup/conflict faults and
     * cumulative sent/observed/assumed accounting are never cleared. */
    g_conn_done=g_conn_status=g_handle=0;
    g_auth_done=g_auth_status=g_enc_status=g_enc_on=0;
    g_enc_event_seen=g_enc_event_enabled=0;
    g_disconnected=g_disconnect_done=g_disconnect_status=0;
    g_link_mode=-1;g_mode_from_connection=0;
    g_mode_request_pending=g_mode_request_status=g_mode_change_error=0;
    g_cc_op=0;g_cc_len=0;g_tick=NULL;
    g_nchans=0;memset(g_chans,0,sizeof g_chans);memset(g_frame_fns,0,sizeof g_frame_fns);
    g_incoming=NULL;g_incoming_psm=g_incoming_request_id=0;
    memset(&g_outgoing_loser,0,sizeof g_outgoing_loser);
    g_sdp=(l2cap_chan){.name="sdp",.scid=SDP_CID,.remote_mtu=672};
    g_l2len=g_l2need=0;
    g_reconnect_used=1;
    log_line("resident: generation 2 prepared; no page command; waiting for a fresh owned event");
    return 1;
}

int bt_reconnect_prepared(void)
{
    return g_reconnect_used && !g_stopping && !session_stopped() && !g_disconnected;
}

int bt_await_reconnect(int timeout_ms)
{
    if(!g_reconnect_used || timeout_ms<1 || timeout_ms>30000 || g_stopping ||
       session_stopped() || g_disconnected)return 0;
    int ok=bt_wait(&g_conn_done,timeout_ms) && g_conn_status==0;
    log_line("resident: fresh owned event %s",ok?"observed":"not established; no retry");
    return ok;
}

/* ---- connection -------------------------------------------------------- */

static int find_headset(void)
{
    static const unsigned char inquiry[5] = { 0x33, 0x8B, 0x9E, 0x08, 0x00 };
    long deadline;

    if (g_key.valid) {
        memcpy(g_target, g_key.addr, 6);
        g_target_psrm = 0x01;
        g_target_clock = 0;
        log_line("headset paired before: %s", addr_str(g_target));
        return 1;
    }

    notify("FGG-PlayPods: searching - put the headset in pairing mode");
    log_line("searching for a headset in pairing mode");
    g_found = 0;
    g_inq_done = 0;
    if (!hci_cmd(OP_INQUIRY, inquiry, (int)sizeof inquiry)) return 0;

    deadline = now_ms() + 15000;
    while (!session_stopped() && !g_found && !g_inq_done && now_ms() < deadline)
        bt_poll(100);
    if (session_stopped()) return 0;
    if (!g_inq_done) {
        hci_cmd(OP_INQUIRY_CANCEL, NULL, 0);
        bt_poll(200);
    }
    if (!g_found) {
        log_line("no headset in pairing mode found");
        return 0;
    }
    return 1;
}

int bt_connect(const char *key_path)
{
    unsigned char p[16];
    long deadline;

    if (session_stopped()) return 0;
    if (!g_peer_prepared) {
        /* Compatibility for callers that have not started dispatch yet. */
        if (!bt_prepare_peer(key_path)) return 0;
    } else if (!key_path || strcmp(g_key_path,key_path)!=0) {
        log_line("connection: prepared peer path changed; stopped");
        return 0;
    }
    if (!find_headset()) return 0;

    memcpy(p, g_target, 6);
    put16(p + 6, 0xCC18);           /* DM1..DH5 */
    p[8] = g_target_psrm;
    p[9] = 0;
    put16(p + 10, g_target_clock);
    p[12] = 1;                      /* allow role switch */

    notify("FGG-PlayPods: connecting to the headset; turn it on now if off");
    if (g_disconnected) {
        log_line("connection: observed peer already disconnected; no automatic reconnect");
        return 0;
    }
    /* An owned successful connection may have arrived during setup. Keep its
     * handle, authentication/encryption and mode observations. Failed events
     * do not establish an owned link and cannot be adopted. */
    if (!(g_conn_done && g_conn_status==0)) g_conn_done=0;

    /* A paired headset that is switched on reconnects by itself; give it a
     * moment before paging it. */
    if (g_key.valid && bt_wait(&g_conn_done, 3000) && g_conn_status == 0) {
        log_line("headset connected by itself");
    } else {
        if (session_stopped()) return 0;
        g_conn_done = 0;
        if (!hci_cmd(OP_CREATE_CONNECTION, p, 13)) return 0;
        if (!bt_wait(&g_conn_done, 20000)) {
            log_line("connection: no answer");
            return 0;
        }
        if (g_conn_status == 0x0B) {
            /* Already linked: the headset is completing its own
             * reconnection, or a link from an earlier session remains whose
             * handle we never learned. Other handles on this controller can
             * be the system's, so nothing is disconnected blindly. */
            g_conn_done = 0;
            log_line("connection: existing link has no owned handle; "
                     "listening for a fresh connection for up to 10000 ms");
            notify("FGG-PlayPods: existing headset link; turn the headset off "
                   "and on now; do not launch again");
            if (!bt_wait(&g_conn_done, 10000)) {
                log_line("connection: controller reported existing link; "
                         "no matching successful connection event received");
                notify("FGG-PlayPods: attempt ended; turn the headset off, launch once, "
                       "then turn it on at the connecting message");
                return 0;
            }
        }
    }
    if (g_conn_status != 0) {
        log_line("connection failed: status %#04x%s", g_conn_status,
                 g_conn_status == 0x04 ? " (headset off, out of range, or "
                                         "connected to another device)" : "");
        return 0;
    }

    put16(p, g_handle);
    if (!g_auth_done) {
        log_line("authentication: requesting on owned handle=%#05x",g_handle);
        if (!hci_cmd(OP_AUTH_REQUESTED, p, 2)) return 0;
    } else log_line("authentication: retaining observed result for owned handle=%#05x",g_handle);
    if (!bt_wait(&g_auth_done, 30000)) {
        log_line("authentication: no answer");
        return 0;
    }
    if (g_auth_status != 0) {
        log_line("authentication failed: status %#04x", g_auth_status);
        if (g_key.valid) {
            log_line("stored key preserved after authentication failure; no deletion or automatic retry");
        }
        return 0;
    }

    /* A headset that reconnected by itself starts encryption by itself, and
     * asking at the same moment collides (status 0x2f). Give it the chance
     * first, and accept a failed request as long as encryption ends up on. */
    bt_wait(&g_enc_on, 2000);
    if (session_stopped()) return 0;
    if (!g_enc_on) {
        put16(p, g_handle);
        p[2] = 1;
        log_line("encryption: submitting enable request on owned handle=%#05x",g_handle);
        if (!hci_cmd(OP_SET_ENCRYPTION, p, 3)) return 0;
    }
    deadline = now_ms() + 8000;
    while (!g_enc_on && !bt_link_lost() && now_ms() < deadline) bt_poll(100);
    if (session_stopped()) return 0;
    if (!g_enc_on) {
        if(g_enc_event_seen)
            log_line("encryption: no observed enabled state before deadline; last observed event status=%#04x enabled=%d",g_enc_status,g_enc_event_enabled);
        else log_line("encryption: no Encryption Change event observed before deadline; status unknown");
        return 0;
    }
    log_line("link encrypted");
    return 1;
}

/* One request on the confirmed headset ACL link, never a controller-wide
 * policy change. Command acceptance alone is not mode-change completion. */
int bt_require_active(void)
{
    unsigned char p[2];
    unsigned handle = g_handle;
    long deadline;
    int ok = 0;

    if (!g_conn_done || g_conn_status != 0 || bt_link_lost() || !g_enc_on ||
        handle > 0x0EFF) {
        log_line("active mode: no confirmed encrypted headset link");
        return 0;
    }
    if (g_link_mode == 0) {
        log_line("active mode: recorded active on handle=%#05x source=%s", handle,
                 g_mode_from_connection ? "new ACL connection" : "owned Mode Change");
        return 1;
    }
    if (g_link_mode != 2) {
        log_line("active mode: current mode=%d not confirmed sniff; stopping before audio", g_link_mode);
        return 0;
    }

    put16(p, handle);
    g_mode_request_status = -1;
    g_mode_change_error = 0;
    g_mode_request_pending = 1;
    deadline = now_ms() + 4000;
    log_line("active mode: requesting Exit Sniff on handle=%#05x", handle);
    if (!hci_cmd(OP_EXIT_SNIFF_MODE, p, sizeof p)) {
        log_line("active mode: command submission failed");
        goto out;
    }
    for (;;) {
        if (bt_link_lost() || !g_conn_done || g_conn_status != 0 ||
            g_handle != handle || !g_enc_on) {
            log_line("active mode: headset link changed or disconnected");
            break;
        }
        if (g_mode_request_status > 0 || g_mode_change_error) {
            log_line("active mode: failed command-status=%d mode-status=%d",
                     g_mode_request_status, g_mode_change_error);
            break;
        }
        if (g_link_mode == 0) {
            log_line("active mode: confirmed active on handle=%#05x", handle);
            ok = 1;
            break;
        }
        if (now_ms() >= deadline) {
            log_line("active mode: no confirmed active-mode event within 4 s; command-status=%d",
                     g_mode_request_status);
            break;
        }
        bt_poll(50);
    }
out:
    g_mode_request_pending = 0;
    return ok;
}

/* ---- channels ---------------------------------------------------------- */

int bt_listen_channel(l2cap_chan *ch,unsigned psm,bt_frame_fn on_frame)
{
    if (!ch || !on_frame || psm!=0x19 || ch->scid<0x40 || ch->scid==SDP_CID ||
        g_stopping || session_stopped() ||
        (g_incoming && (g_incoming!=ch || g_incoming_psm!=psm))) return 0;
    l2cap_chan *existing=chan_by_scid(ch->scid);
    if ((existing && existing!=ch) || (!existing && g_nchans>=MAX_CHANNELS)) return 0;
    if (!existing) {
        g_chans[g_nchans]=ch;g_frame_fns[g_nchans]=on_frame;g_nchans++;
    }
    g_incoming=ch;g_incoming_psm=psm;
    log_line("l2cap: prepared incoming signaling psm=%#04x scid=%#04x",psm,ch->scid);
    return 1;
}

int bt_open_channel(l2cap_chan *ch, unsigned psm, bt_frame_fn on_frame)
{
    unsigned char r[8];
    long deadline, resend = 0;

    if (g_stopping || session_stopped() || ch->closed || ch->close_attempted) return 0;
    l2cap_chan *registered = chan_by_scid(ch->scid);
    if ((registered && registered != ch) ||
        (!registered && g_nchans >= MAX_CHANNELS)) {
        log_line("l2cap: cannot register %s channel", ch->name);
        return 0;
    }
    if (!registered) {
        g_chans[g_nchans] = ch;
        g_frame_fns[g_nchans] = on_frame;
        g_nchans++;
    }

    /* Incoming signaling may already have connected and configured while
     * authentication/encryption were being pumped. Preserve that transaction. */
    if (!ch->conn_done) {
    put16(r, psm);
    put16(r + 2, ch->scid);
    ch->conn_done = ch->conn_result = 0;
    ch->dcid = 0;
    ch->cfg_rsp_ok = ch->cfg_req_done = ch->closed = 0;
    ch->cfg_id = 0;
    ch->remote_mtu = 672;
    ch->conn_id = next_sig_id();
    log_line("l2cap: opening %s psm=%#04x scid=%#04x id=%u",
             ch->name, psm, ch->scid, ch->conn_id);
    if (!sig_send(0x02, ch->conn_id, r, 4)) {
        ch->conn_id = 0;
        return 0;
    }
    } else log_line("l2cap: retaining %s channel established by peer",ch->name);
    if (!bt_wait(&ch->conn_done, 10000)) {
        ch->conn_id = 0;
        log_line("l2cap: %s connection %s", ch->name,
                 g_bt_transport_failed ? "stopped by transport loss" :
                 g_acl_flow_failed ? "stopped by ACL accounting guard" :
                 g_coexistence_conflict ? "stopped by coexistence guard" :
                 g_disconnected ? "link lost" : "timed out");
        return 0;
    }
    if (ch->conn_result != 0) {
        log_line("l2cap: %s connection refused (%d)%s", ch->name, ch->conn_result,
                 ch->conn_result == 4 ? " (peer reports no resources available)" : "");
        return 0;
    }

    /* Preserve the existing bounded same-ID configuration retransmission.
     * Missing replies do not establish exclusive or competing reception. */
    if (!ch->cfg_rsp_ok && !ch->cfg_id) ch->cfg_id = next_sig_id();
    else if (ch->cfg_id) resend=now_ms()+1500;
    deadline = now_ms() + 10000;
    while (!(ch->cfg_rsp_ok && ch->cfg_req_done) ||
           (ch==g_incoming && g_outgoing_loser.active && !g_outgoing_loser.closed)) {
        if (now_ms() >= deadline || bt_link_lost() || ch->closed) {
            ch->cfg_id = 0;
            log_line("l2cap: %s configuration/superseded-channel cleanup did not finish; no automatic reopen", ch->name);
            return 0;
        }
        if (!ch->cfg_rsp_ok && now_ms() >= resend) {
            put16(r, ch->dcid);
            put16(r + 2, 0);
            if (!sig_send(0x04, ch->cfg_id, r, 4)) {
                ch->cfg_id = 0;
                return 0;
            }
            resend = now_ms() + 1500;
        }
        bt_poll(100);
    }
    log_line("l2cap: %s ready scid=%#04x dcid=%#04x peer-request=%d our-response=%d remote-mtu=%u",
             ch->name, ch->scid, ch->dcid, ch->cfg_req_done, ch->cfg_rsp_ok, ch->remote_mtu);
    return !session_stopped();
}

void bt_close_channel(l2cap_chan *ch)
{
    unsigned char r[4];

    if (!ch->dcid || ch->closed || ch->close_attempted || bt_link_lost()) return;
    ch->close_attempted=1;
    put16(r, ch->dcid);
    put16(r + 2, ch->scid);
    ch->close_id=next_sig_id();
    if (!sig_send(0x06,ch->close_id,r,4)) {
        ch->close_id=0;
        log_line("l2cap: %s close submission failed; outcome uncertain, no retry",ch->name);
        return;
    }
    if (!bt_wait(&ch->closed,2000))
        log_line("l2cap: %s close unconfirmed; no retry",ch->name);
    ch->close_id=0;
}
