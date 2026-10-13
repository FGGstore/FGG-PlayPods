/* The Bluetooth link to the headset: controller setup, pairing and
 * reconnection, encryption, L2CAP channels and ACL flow control.
 *
 * Everything runs on one thread: callers pump the controller with bt_poll()
 * or bt_wait(), which dispatch whatever arrived. Only events about the
 * headset are acted on. This does not return consumed system packets or
 * establish safe sharing of controller state or buffer capacity.
 */
#ifndef FGG_BT_H
#define FGG_BT_H

/* One L2CAP channel. `name` and `scid` (our end) are set by the owner. */
typedef struct {
    const char *name;
    unsigned scid, dcid;
    int conn_done, conn_result;
    int cfg_rsp_ok, cfg_req_done, closed, close_attempted;
    unsigned remote_mtu;
    unsigned char conn_id, cfg_id, close_id; /* outstanding transactions */
} l2cap_chan;

/* Receives L2CAP frames on channels opened with bt_open_channel. */
typedef void (*bt_frame_fn)(unsigned cid, const unsigned char *data, int len);

/* Opens the controller and prepares it. Returns 0 on failure. */
int  bt_start(void);
/* Select the saved peer before bt_start can dispatch a reconnect event.
 * Reads only the existing key file; does not open hardware or create a link.
 * Missing keys retain inquiry behavior. A prepared session cannot change peer. */
int bt_prepare_peer(const char *key_path);
/* Sticky after a failed controller-setting request; recovery is not proven. */
int  bt_setup_failed(void);

/* Disconnects the headset if still linked and closes the controller. */
void bt_stop(void);
/* Isolated one-reconnect comparison: observed clean disconnect and idle OUT only. */
int bt_prepare_reconnect(void);
int bt_reconnect_prepared(void);
int bt_await_reconnect(int timeout_ms);

/* Connects to the headset paired before (its key is kept in key_path), or
 * pairs one found in pairing mode, and encrypts the link. Returns 0 on
 * failure, with the reason logged. */
int  bt_connect(const char *key_path);

/* Requires a confirmed encrypted owned link whose recorded state is active.
 * A fresh successful ACL completion establishes its initial active state;
 * later owned Mode Change events replace it. Unobserved native changes remain
 * possible. One bounded Exit Sniff request if sniff was observed; an unknown
 * existing link or other recorded mode still fails closed. */
int bt_require_active(void);

/* Opens an L2CAP channel to `psm` on the headset and configures it. Frames
 * that arrive on it go to `on_frame`. Returns 0 on failure. */
int  bt_open_channel(l2cap_chan *ch, unsigned psm, bt_frame_fn on_frame);
/* Prepare one owned incoming signaling channel before controller polling.
 * Registers routing only; no I/O. Does not accept unsolicited media channels. */
int bt_listen_channel(l2cap_chan *ch, unsigned psm, bt_frame_fn on_frame);
void bt_close_channel(l2cap_chan *ch);

/* Sends one L2CAP frame to the headset's channel `dcid`. */
int  bt_send(unsigned dcid, const unsigned char *data, int len);

/* True while the local observed-credit ledger allows another ACL packet.
 * It does not reserve shared controller buffers from the native owner. */
int  bt_can_send(void);

/* Largest L2CAP frame one ACL packet carries. */
int  bt_max_frame(void);

/* Pumps the controller for up to timeout_ms and dispatches what arrived. */
void bt_poll(int timeout_ms);

/* Pumps until *flag is set, the link drops, or timeout_ms passes. Returns
 * 1 if the flag was set. */
int  bt_wait(volatile int *flag, int timeout_ms);

/* True once disconnected or an ownership/accounting conflict stops the session.
 * This logical stop does not establish physical headset disconnection. */
int  bt_link_lost(void);

/* Sticky stop after a command reply outside this payload's opcode set.
 * It does not prove restoration of the system's controller state. */
int bt_coexistence_conflict(void);

/* Called about once a second while the link layer waits or streams. */
void bt_set_tick(void (*fn)(void));

/* Sticky after owned completions exceed recorded local submissions. */
int bt_acl_flow_failed(void);
/* Sticky after the HCI transport reports lost traffic. */
int bt_transport_failed(void);
int  bt_credits(void);

/* Live submissions minus observed completions and separately recorded retirement
 * after an observed disconnect. Retirement is not a completed-packets report;
 * missing reports do not establish delivery or shared controller availability. */
long bt_reports_missing(void);
/* Observed dispatcher counters only; cannot see packets delivered elsewhere. */
void bt_completion_report(void);

#endif
