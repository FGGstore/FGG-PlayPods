/* HCI over USB on the PlayStation 5's first HCI interface pair.
 *
 * Hardware descriptors show two HCI-like interface pairs, but do not prove
 * independent radios/state or native reader ownership. No interface is
 * detached here. Safe simultaneous wireless controller operation is unproven.
 *
 * Both readers compete for packets. Packets delivered here do not reach the
 * system's pending read. Filtering foreign handles after receipt does not
 * forward them back; this transport does not provide a safe multiplexer.
 */
#ifndef FGG_HCI_H
#define FGG_HCI_H

/* Controlled comparison only: normal pumps continue to rearm in either mode.
 * The producer's send wait does not rearm IN transfers. Default retains the
 * local servicing fix; neither policy establishes native packet ownership. */
#ifndef HCI_OUT_WAIT_REARM
#define HCI_OUT_WAIT_REARM 1
#endif
#if HCI_OUT_WAIT_REARM != 0 && HCI_OUT_WAIT_REARM != 1
#error HCI_OUT_WAIT_REARM must be 0 or 1
#endif

#define HCI_PKT_MAX 1100

/* Opens the controller alongside the system's driver. Returns 0 on failure. */
int  hci_open(void);
void hci_close(void);
/* Local output transfer completed; no queue mutation or native I/O. */
int hci_output_idle(void);

/* Sends an HCI command. Returns 0 on a transport error. */
int  hci_cmd(unsigned opcode, const void *params, int plen);

/* Sends one complete ACL packet (4-byte header included), waiting up to a
 * second for the previous one to leave. Returns 0 on error or timeout. */
int  hci_acl_send(const unsigned char *pkt, int len);

/* Waits up to timeout_ms for incoming packets and queues them. Returns 1 if
 * a packet is waiting in either queue, 0 on timeout, -1 after traffic loss.
 * A traffic-loss stop is sticky; no new IN reads are armed after it. */
int  hci_pump(int timeout_ms);

/* Pops one queued packet and returns its length, or 0 if none. */
int  hci_next_event(unsigned char *out, int max);
int  hci_next_acl(unsigned char *out, int max);

/* Cumulative local read observations only: does not reap, arm, consume queues
 * or infer traffic delivered to the system's competing reader. */
void hci_receive_report(void);

#endif
