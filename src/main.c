/* FGG-PlayPods -- the PlayStation 5's sound on a Bluetooth headset.
 *
 * The PS5 has no Bluetooth audio: its own headsets use a USB dongle. This
 * payload plays the console's sound, game and system alike, on an ordinary
 * Bluetooth headset, through the console's own Bluetooth chip. It shares
 * endpoints with the system; wireless controller coexistence is unverified.
 *
 *   capture.c  the console's audio, as Remote Play hears it
 *   hci.c      USB transport using the chip's first interface pair
 *   bt.c       pairing, the link to the headset, L2CAP, flow control
 *   sdp.c      the service record a headset looks up
 *   a2dp.c     AVDTP, SBC encoding and the stream itself
 *   main.c     the lock and the session
 */
#include "a2dp.h"
#include "bt.h"
#include "capture.h"
#include "hci.h"
#include "log.h"
#include "session_lock.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#ifndef PLAYPODS_VERSION
#define PLAYPODS_VERSION "1.0-contribution-resident-rfc"
#endif
#ifndef STREAM_LIMIT_MS
#define STREAM_LIMIT_MS 0
#endif
#if STREAM_LIMIT_MS != 0
#error continuous play candidate requires STREAM_LIMIT_MS=0
#endif

#define STATE_DIR "/data/fgg-playpods"
#define LOG_PATH  STATE_DIR "/playpods.log"
#define LOCK_PATH STATE_DIR "/playpods.lock"
#ifndef PLAYPODS_KEY_PATH
#define PLAYPODS_KEY_PATH STATE_DIR "/headset.key"
#endif
#define KEY_PATH PLAYPODS_KEY_PATH

int main(void)
{
    int capturing, streamed = 0, lock_fd = -1, locked;

    if (!log_open(STATE_DIR, LOG_PATH)) return 1;
    log_line("========================================");
    log_line("FGG-PlayPods %s", PLAYPODS_VERSION);
#ifdef PLAYPODS_PROFILE_DESCRIPTION
    log_line("device comparison: %s; key-path=%s", PLAYPODS_PROFILE_DESCRIPTION, KEY_PATH);
#endif
    log_line("play profile: out-wait-rearm=%d no duration timer; producer60 completion policy and terminal guards retained; connection fixes enabled",
             HCI_OUT_WAIT_REARM);

    locked = session_lock_take(LOCK_PATH, &lock_fd);
    if (locked != 1) {
        notify(locked == 0 ? "FGG-PlayPods: already running" :
               "FGG-PlayPods: cannot acquire session lock - see the log");
        log_close();
        return 1;
    }

    /* Capture first: if it cannot work, Bluetooth is never touched. */
    capturing = capture_open();
    if (capturing < 0) {
        notify("FGG-PlayPods: audio capture unavailable - see the log");
        goto out;
    }

    if (!bt_prepare_peer(KEY_PATH)) {
        notify("FGG-PlayPods: cannot prepare headset identity - see the log");
        capture_close();
        goto out;
    }
    if (!a2dp_prepare()) {
        notify("FGG-PlayPods: cannot prepare audio signaling - see the log");
        capture_close();
        goto out;
    }
    if (!bt_start()) {
        if (bt_setup_failed())
            notify("FGG-PlayPods: Bluetooth setup stopped; controller recovery unverified - see the log");
        else if (!bt_coexistence_conflict() && !bt_acl_flow_failed() && !bt_transport_failed())
            notify("FGG-PlayPods: the Bluetooth controller did not answer - see the log");
        capture_close();
        goto out;
    }

    if (bt_connect(KEY_PATH) && a2dp_start()) {
        notify("PlayPods resident comparison: continuous audio; one reconnect available");
        streamed = a2dp_stream(capturing);
        if(streamed && bt_prepare_reconnect() && a2dp_prepare_reconnect()) {
            notify("PlayPods: still listening for 30 seconds; turn the headset on to resume; do not relaunch");
            if(bt_await_reconnect(30000) && bt_connect(KEY_PATH) && a2dp_start()) {
                capturing=capture_restart();
                notify("PlayPods: reconnected in the same payload; switch headset off to stop");
                streamed |= a2dp_stream(capturing);
            }
        }
    }
    else if (!bt_coexistence_conflict() && !bt_acl_flow_failed() && !bt_transport_failed())
        notify("FGG-PlayPods: could not connect to the headset - see the log");

    a2dp_stop();
    bt_stop();
    capture_close();

    if (streamed && !bt_coexistence_conflict() && !bt_acl_flow_failed() && !bt_transport_failed()) notify("FGG-PlayPods: stopped");

out:
    if (bt_transport_failed())
        notify("FGG-PlayPods: Bluetooth traffic lost; audio stopped - see the log");
    if (bt_acl_flow_failed())
        notify("FGG-PlayPods: Bluetooth buffer accounting lost; audio stopped - see the log");
    if (bt_coexistence_conflict())
        notify("FGG-PlayPods: shared Bluetooth activity detected; audio stopped");
    log_line("FGG-PlayPods done");
    session_lock_release(&lock_fd);
    log_close();
    return 0;
}
