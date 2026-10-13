# RFC: guarded headset startup and one resident reconnect

This review candidate preserves the working local B1 audio policy and includes message/packet bounds, session locking and capture error reporting. It originally branched from upstream `0d35e17635bf14a685f84d64c1f897b01485afa9`. It now incorporates upstream `4b4ddb5` after the focused bounds and capture/session patches were accepted. The merged upstream fixes and their tests are retained; the remaining diff is the experimental resident RFC and its validation/documentation. Original FGG-PlayPods attribution, GPLv3 license and vendored codec remain unchanged.

The owner reports the current milestone works well with PS5 firmware **12.40**, a **Jabra Evolve2 75** and wireless DualSense input. Reliable ordinary startup remains unresolved. The producer documents firmware 11.60 and an Xbox Wireless Headset; these are different validation conditions, and neither set establishes universal compatibility.

## Proposed behavior

- Prepare the saved headset identity before controller polling. Adopt only a fresh successful address-matched connection event; an existing-link result without a verified handle cannot establish ownership. Preserve the stored key on authentication failure and do not automatically replay uncertain connection or disconnect operations.
- Match signaling transactions and channels, handle incoming/outgoing signaling collisions explicitly, and bound discovery replies. Retain authentication/encryption and active-mode checks before audio setup.
- Keep USB OUT storage owned until completion. Bound pending-output waits and stop on sticky transport/accounting failures. Under the B policy, reap completions during OUT waits but rearm receive reads in the normal pump. Receive counts, endpoints, codec and native capture parameters stay fixed.
- Retain the producer's 60 ms assumed-completion policy. Log observed reports, assumed reuse and disconnection retirement separately. Elapsed time **does not prove controller buffers are free**; this policy remains an explicit design risk requiring maintainer review.
- After a successfully observed owned disconnect, allow one passive reconnect in the same process for at most 30 seconds. Require idle local output, a saved key and no sticky fault or ambiguous queued replacement. Reset peer-generation state, not cumulative counters or faults; require a new matching connection event and fresh signaling/media channels. Do not reopen/reconfigure the controller for this reconnect.
- Keep both audio sessions continuous (`STREAM_LIMIT_MS=0`). The 30-second bound governs the passive reconnect wait, not audio duration. A second disconnect ends the process. Session locks coordinate only copies using the descriptor-lock protocol.

## Evidence and limitations

[The sanitized hardware summary](hardware-summary.json) separates owner observations from logged transport counters. One B1 process sustained a reported 69-second first stream, then a 30-second second stream after the owner deliberately switched the headset off/on. The owner confirmed clear sound and wireless input in the first session, confirmed reconnect, and confirmed the second disconnect was another intentional power-off. Post-reconnect fidelity/input and exact game/menu workload were not separately reconfirmed.

The first stream measured about 53.4–53.8 packets/s, compatible with seven 128-sample SBC frames at 48 kHz (53.57 packets/s). Observed output wait maxima were 1 ms. A replacement connection event arrived nine seconds after the first disconnect and audio resumed 13 seconds after it. Counters are submissions/observations, not a measurement of audible fidelity or controller responsiveness.

A prior receive-rearm-during-wait run was choppy and ended after 13 seconds, with one interval at 5.4 packets/s, growing capture overruns and output waits up to 604 ms before the one-second pending-output deadline. However, an earlier run under that policy lasted about 20 minutes. These were not controlled repeated trials with matched workloads: the B result does not establish receive rearming as the universal cause.

A subsequent B1 startup failed on an existing-link result with handle zero. The bounded fresh-event window expired; zero ACL packets were submitted and no unknown handle was disconnected. The owner still reports startup-sequence dependence. Removing it needs verified current connection ownership or a native integration that observes the initial event; guessing a previous handle or repeatedly launching cannot establish ownership.

Shared raw USB reads may consume events intended for the system driver. Filtering foreign traffic after receipt cannot return it. The RFC stops on recognized foreign command replies, but that guard does not prove noninterference, native driver restoration or flawless wireless coexistence. Firmware layout/API assumptions, long gameplay, game startup versus already-running capture, repeated startup/reconnect, lock reuse after exit, and cleanup after native failures remain hardware questions. Do not introduce driver detach/reset, guessed lookup tuples or destructive failure injection based on this RFC.

## Offline checks and compilation

On Linux with GCC, Python 3, ASan and UBSan:

```sh
make check-offline
```

This runs the five accepted upstream bounds/capture/lock suites and 63 resident host invocations against the actual source using mocked Bluetooth/USB/time/capture/encoder calls: transaction reply windows, signaling collisions, authentication boundaries, disconnect ownership, terminal faults, startup guidance, one-reconnect gates, main lifecycle, output-idle checks, both USB OUT-wait receive-rearm policies, completion accounting, active mode and continuous streaming beyond ten simulated minutes. It cannot measure radio interoperability, codec fidelity, native capture timing or input responsiveness. Results and source hashes are written under `build/resident-offline/`.

With an extracted SDK and matching LLVM tools, compile only:

```sh
LLVM_CONFIG=llvm-config-18 make fgg-playpods.elf \
  BUILD=build/native-rfc PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
```

Do not run `make test`: the inherited upstream target deploys. The tested cached SDK v0.43 archive SHA-256 is `a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb`. Compile flags select `HCI_OUT_WAIT_REARM=0` and `STREAM_LIMIT_MS=0`; changing flags requires a fresh object directory.

The normalized candidate compiles with warnings as errors. [Object comparison](object-correspondence.json) shows eight exact working-B1 object matches. Bluetooth's only differing section payloads are `.strtab`/`.symtab`; resolved symbols differ solely in STT_FILE (`bt-timed.c` versus `bt.c`). Main differs in the RFC version string and associated string-symbol positions. This is not a whole-file reproducibility claim: build IDs vary, and this new versioned binary has not been hardware-tested. The owner's installed B1 was left unchanged.

## Review decisions requested

The small bounds and capture/session patches have been accepted upstream and are included as merge ancestry. For this RFC, assess whether the explicit 60 ms fallback and shared-reader architecture are acceptable, how to establish existing-link ownership, and how to integrate one resident reconnect without overstating restoration or reliability. The RFC source is supplied for review; this is not a release recommendation.
