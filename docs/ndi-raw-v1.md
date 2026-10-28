# Opt-in Spectra raw endpoint (frozen ndi.raw.v1 / minor 0)

Configuration additions are optional; existing raw-disabled TRANSFORM use remains the default. The profile uses standard OpenIGTLink STRING/UTF-8, `NDI_RAW_V1`, header version 1 and timestamp 0. The frozen wire contract and synthetic golden files are in `Testing/Spectra/frozen`.

Example attributes on an NDITracker device (supply private output/ROM paths and a unique stable source UUID):

```xml
EnableMeasuredMarkers="true"
MarkerSidecarDirectory="/private/output"
EnableRawEndpoint="true"
RawProtocol="ndi.raw.v1" RawSchemaMinor="0"
RawSourceId="REPLACE_WITH_STABLE_SOURCE_UUID"
RawPort="18945" RawQueueCapacity="64" RawQueueBytes="16777216"
RawWriteTimeoutMs="500" RawDrainTimeoutMs="2000"
RawRecoverInitial="true"
```

`EnableRawEndpoint` and `RawRecoverInitial` default to false. The schema/minor must be explicitly selected. Generate the source UUID once per independent source, not on each connection. A bind failure does not select another port. This implementation supports one raw subscriber, no history replay and no application hello/ACK. `RawRecoverInitial` selects the verified local initial-recovery procedure; it does not automatically clear arbitrary later device/counter quarantine.

The serial command owner seals one immutable BX snapshot. Sidecar `raw_v1_snapshot_data` equals network `frame.data`; network envelope and exact-payload digest are checked from original network bytes. Legacy `host_poll_sequence` stays continuous across recovery while raw `reply_seq` restarts at 1 in the new acquisition. Polls after a frozen session boundary may continue for TRANSFORM/sidecar outside that session's coverage and must be reported separately.

Do not equate a valid pose with four valid markers, a compact coordinate count with a known slot denominator, or parse/storage acceptance with measurement validity. Unknown marker identity/defined slots, status4/OOV/nonfinite values and invalid raw wire remain represented. Raw fit-error units are UNKNOWN; the old sidecar's `fit_error_mm` spelling is not unit evidence. Host clocks are not exposure timestamps or cross-host synchronization.

Each session has its own sequence, conservation counts, exact sent-frame digest and END. Sender send completion is not receiver durability. Missing END, queue loss or an incomplete old stream remain incomplete after a new session. Stop the source and allow END receipt/durable closure before stopping a receiver when complete session coverage is required. Keep sidecars during transition.

An optional `BroadcastDiagnosticsIntervalSec` attribute on `PlusOpenIGTLinkServer` enables aggregate TRANSFORM send counts, batch sizes, send/retry timing and broadcast-loop gaps. Default 0 is silent. A positive interval such as 5 seconds is intended for correlating source timing with independent receiver queue/writer monitoring; it does not measure receiver disk latency. It adds no producer-side socket/file/JSON operations.

Linux validation used existing compatible dependencies; see `Testing/Spectra/README.md` and `VALIDATION.json`. Physical visibility transitions, longer dual recordings, cold dependency builds, other platforms and the upstream full test matrix remain outside those results.
