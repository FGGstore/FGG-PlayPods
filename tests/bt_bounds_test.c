#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Include the dispatcher to exercise its private parsing boundaries.
 * No USB implementation or console library is linked into this host test. */
#include "../src/bt.c"

static int sends, commands, received;
static unsigned char last_packet[HCI_PKT_MAX];
static int last_length;
static int mock_reply_length, mock_reply_pending;
void log_line(const char *fmt, ...) { (void)fmt; }
void notify(const char *fmt, ...) { (void)fmt; }
int hci_open(void) { return 0; }
void hci_close(void) {}
int hci_output_idle(void) { return 1; }
void hci_receive_report(void) {}
int hci_cmd(unsigned op, const void *p, int n)
{
    (void)p; (void)n; commands++;
    if (op == OP_READ_BUFFER_SIZE && mock_reply_length) mock_reply_pending = 1;
    return 1;
}
int hci_acl_send(const unsigned char *p, int n)
{
    assert(n >= 0 && n <= HCI_PKT_MAX);
    memcpy(last_packet, p, (size_t)n);
    last_length = n;
    sends++;
    return 1;
}
int hci_pump(int ms) { (void)ms; return 0; }
int hci_next_event(unsigned char *p, int n)
{
    if (!mock_reply_pending) return 0;
    assert(n >= mock_reply_length);
    memset(p, 0, (size_t)mock_reply_length);
    p[0] = 0x0e; p[1] = (unsigned char)(mock_reply_length - 2);
    p[2] = 1; put16(p + 3, OP_READ_BUFFER_SIZE);
    mock_reply_pending = 0;
    return mock_reply_length;
}
int hci_next_acl(unsigned char *p, int n) { (void)p; (void)n; return 0; }

static void received_frame(unsigned cid, const unsigned char *p, int n)
{ assert(cid == 0x40 && n == 1 && p[0] == 42); received++; }

static void test_echo(void)
{
    unsigned char frame[HCI_PKT_MAX + 4];
    memset(frame, 42, sizeof frame);
    frame[0] = 0x08; frame[1] = 7; put16(frame + 2, 200);
    on_signaling(frame, 204);
    assert(sends == 1 && last_length == 212);
    assert(memcmp(last_packet + 12, frame + 4, 200) == 0);
    put16(frame + 2, 4);
    on_signaling(frame, 8);
    assert(sends == 2 && last_length == 16);
    assert(last_packet[8] == 0x09 && last_packet[9] == 7);
    assert(memcmp(last_packet + 12, frame + 4, 4) == 0);
    put16(frame + 2, HCI_PKT_MAX);
    on_signaling(frame, sizeof frame);
    assert(sends == 2);  /* larger than the transport capacity */
}

static void test_truncated(void)
{
    static const unsigned char codes[] = {
        0x01, 0x22, 0x2f, 0x04, 0x03, 0x05, 0x06, 0x08,
        0x0e, 0x0f, 0x13, 0x16, 0x17, 0x18, 0x31, 0x33
    };
    static const int minimum[] = {3,17,17,12,13,6,5,6,6,6,7,8,8,25,8,12};
    size_t i;
    int n;
    for (i = 0; i < sizeof codes; i++) {
        for (n = 0; n < minimum[i]; n++) {
            unsigned char *p = calloc((size_t)(n ? n : 1), 1);
            if (n) p[0] = codes[i];
            if (n > 1) p[1] = (unsigned char)(n - 2);
            if (n > 2 && (codes[i] == 0x22 || codes[i] == 0x13)) p[2] = 1;
            on_event(p, n);
            free(p);
        }
    }
    for (n = 0; n < 4; n++) {
        unsigned char *p = calloc((size_t)(n ? n : 1), 1);
        on_acl(p, n);
        free(p);
    }
    for (i = 2; i <= 10; i++) {
        for (n = 0; n < 8; n++) {
            unsigned char *p = calloc((size_t)n + 4, 1);
            p[0] = (unsigned char)i; p[1] = 7; put16(p + 2, (unsigned)n);
            on_signaling(p, n + 4);
            free(p);
        }
    }
}

static void test_ownership_and_valid_acl(void)
{
    unsigned char packet[] = {0x21,0x20,5,0,1,0,0x40,0,42};
    l2cap_chan ch = {.name="test", .scid=0x40, .dcid=0x41,
                     .conn_done=1, .remote_mtu=672};
    g_nchans = 1; g_chans[0] = &ch; g_frame_fns[0] = received_frame;
    g_conn_done = 0;
    on_acl(packet, sizeof packet);
    assert(received == 0);
    g_conn_done = 1; g_conn_status = 0; g_handle = 0x20;
    on_acl(packet, sizeof packet);
    assert(received == 0);
    packet[0] = 0x20;
    on_acl(packet, sizeof packet);
    assert(received == 1);
    packet[4] = 0xff; packet[5] = 0xff;
    on_acl(packet, sizeof packet);
    assert(g_l2need == 0 && received == 1);
}

static void test_configuration(void)
{
    l2cap_chan ch = {.name="test", .scid=0x40, .dcid=0x41,
                     .conn_done=1, .remote_mtu=672};
    unsigned char bad[] = {4,7,6,0,0x40,0,0,0,1,2};
    unsigned char good[] = {4,7,8,0,0x40,0,0,0,1,2,0,2};
    g_nchans = 1; g_chans[0] = &ch;
    sends = 0;
    on_signaling(bad, sizeof bad);
    assert(sends == 0 && ch.remote_mtu == 672 && !ch.cfg_req_done);
    on_signaling(good, sizeof good);
    assert(sends == 1 && ch.remote_mtu == 512 && ch.cfg_req_done);
}

static void test_valid_event(void)
{
    unsigned char inquiry[] = {1,1,0};
    unsigned char complete[] = {0x0e,11,1,5,0x10,0,0x4c,4,0,64,0,0,0};
    g_inq_done = 0;
    on_event(inquiry, sizeof inquiry);
    assert(g_inq_done == 1);
    on_event(complete, sizeof complete);
    assert(g_cc_op == OP_READ_BUFFER_SIZE && g_cc_len == (int)sizeof complete);
}

int main(int argc, char **argv)
{
    g_credits = 64;
    if (argc > 1 && strcmp(argv[1], "echo") == 0) { test_echo(); return 0; }
    if (argc > 1 && strcmp(argv[1], "truncated") == 0) { test_truncated(); return 0; }
    test_echo();
    test_truncated();
    test_ownership_and_valid_acl();
    test_configuration();
    test_valid_event();
    mock_reply_length = 6;
    assert(!setup_controller());
    mock_reply_length = 0;
    assert(!bt_send(1, NULL, -1));
    assert(!bt_send(1, NULL, INT_MAX));
    assert(!sig_send(8, 1, NULL, -1));
    assert(!sig_send(8, 1, NULL, INT_MAX));
    puts("Bluetooth bounds and ownership checks passed (mock transport)");
    return 0;
}
