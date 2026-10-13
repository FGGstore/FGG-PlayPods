# Offline validation

Run `make check-offline` on Linux with GCC, Python 3, AddressSanitizer and UndefinedBehaviorSanitizer. No PS5 SDK or console is required. The target runs the five accepted upstream bounds/capture/session-lock suites, followed by all 63 resident RFC test invocations. Console APIs, Bluetooth and USB operations are mocked; only host test executables run.

## Accepted bounds, capture and locking checks

The five suites cover truncated HCI events and ACL messages, signaling echo/configuration extents, foreign handles, malformed SDP lengths, packet capacity versus negotiated MTU, null/invalid capture boundaries and separate cleanup failures. File-lock contention and process lifetime use real Linux fixtures. The Bluetooth fixture uses designated channel initializers and mocks the RFC's additional output-idle/reporting interfaces; its original bounds assertions remain intact.

The accepted capture parameters, capture error reporting and descriptor-lock implementation are unchanged by this integration. A descriptor lock does not coordinate with older timestamp-lock builds. Linux locking and SDK linkage do not prove PS5 flock semantics or native cleanup/restoration.

## Resident RFC checks

The 63 invocations cover signaling transactions/collisions, authentication boundaries, ownership, terminal faults, startup guidance, one-reconnect gates, main lifecycle, output-idle handling, both receive-rearm policies, completion accounting, active mode and continuous streaming beyond ten simulated minutes. Detailed results and source hashes are written to `build/resident-offline/`.

The RFC changes Bluetooth transport/lifecycle behavior relative to upstream; see [the RFC](docs/RESIDENT-AUDIO-RFC.md) and [hardware summary](docs/hardware-summary.json). These mocks cannot establish radio interoperability, physical buffer availability, native-driver noninterference, audible quality or wireless controller safety.

## Compile only

With an extracted SDK and matching LLVM tools, run:

```sh
LLVM_CONFIG=llvm-config-18 make fgg-playpods.elf \
  BUILD=build/native-rfc PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
```

Do not use `make test`: that is the inherited deployment target. No payload execution or console contact is part of these checks.
