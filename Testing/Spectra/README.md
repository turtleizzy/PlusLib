# Local Spectra / ndi.raw.v1 regressions

This branch records the existing Linux Spectra work on pinned PlusLib `c796218916c2854c3fd8d2286facac8a5821ceb5`. The raw endpoint stays off by default. One serialized BX call produces an immutable snapshot for network and transitional sidecar. The frozen contract is in `frozen/`; it is unchanged. This is an experimental local branch, not a production deployment or an upstream acceptance claim.

The companion ndicapi changes are four independent commits exported under `ndicapi-patches/`. `DEPENDENCIES.json` pins their original base, exact patch hashes and resulting Git tree. This allows one PlusLib fork to carry the complete source change without requiring an additional account-level dependency fork. Do not build against a stock ndicapi that lacks these APIs.

## Reconstruct and build

```sh
python3 -B Testing/Spectra/prepare_ndicapi.py --dest /path/to/new-ndicapi
python3 -B Testing/Spectra/build.py \
  --dependencies /path/to/PlusBuild-bin \
  --ndicapi-source /path/to/new-ndicapi
python3 -B Testing/Spectra/run.py
```

The build helper reuses an existing compatible PlusBuild dependency configuration. It builds this PlusLib and the explicit ndicapi source into ignored directories under `Testing/Spectra`; it does not install, deploy, launch a tracker, or modify a shared dependency. A cold dependency build, PlusApp and Windows/MSVC builds were not performed in the local validation. Upstream's full contribution/dashboard matrix remains unverified.

Optional loopback and guarded PTY tests:

```sh
python3 -B Testing/Spectra/run.py --network
python3 -B Testing/Spectra/run.py --network --rmt-root /path/to/independent/RMT-checkout
```

The optional RMT test imports that checkout read-only. No RMT source is copied or changed. `PLUS_NDI_BASELINE` may point to an existing earlier local candidate to reproduce the cursor bug; without it, the baseline-only comparison is explicitly skipped. A baseline OpenIGTLink dependency that already supports immediate address reuse is also reported as such. Current candidate assertions remain active.

No physical ROMs, live coordinates, recordings, receiver archives, credentials or local logs are versioned. All included data fixtures are synthetic. `build.py` generates opaque zero-filled ROM files for the guarded PTY peer only: they are not tool definitions for a real tracker. The checked-in XML files are templates with placeholder private paths and an example source UUID; generated test copies live under ignored `config/generated`.

## Behavior and configuration

See [docs/ndi-raw-v1.md](../../docs/ndi-raw-v1.md) from the repository root. Live acquisition needs the user's own ROM/configuration and explicit raw enable. Preserve the existing inter-byte pacing (`NDICAPI_SERIAL_WRITE_DELAY_US=1500` in the validated setup), baud/rate and sidecar. Never reconstruct production frames by reading a recorded file.

Initial recovery is explicit (`RawRecoverInitial=true`, default false): preserve the isolated frame; freeze and send the old RECOVERY END; verify reset token, INIT/COMM/configuration/tool-enable/TSTART through the existing command owner; then start a linked new acquisition/session. FIRST has null new/delta values. CRC success, UUID changes, flush or incrementing counters are not recovery evidence.

The cursor regression fixes the requested interpolation time rather than replaying a copied older sample time. Receiver queue capacity and sticky failure rules are unchanged. Optional `BroadcastDiagnosticsIntervalSec` is disabled by default; when enabled, aggregate counts/timings are emitted by the broadcast worker, not the serial producer. Expected invalid-transform interpolation is logged once per continuous episode per buffer; genuine CRC/timestamp/transport/storage failures remain observable.

Validation summaries distinguish software/PTY, transport/storage and physical measurement. The final local hardware short run had both tools OUT_OF_VIEW; complete storage did not imply a valid pose or marker sample. Historical receiver writer stalls/lock contention and older sampling-filter rejects remain unresolved. Original private evidence remains local and is not included in this fork.

The reconstructed portable runner passed 58 PlusLib method executions with 1 optional baseline-only skip, plus 5 XML checks; the companion ndicapi runner passed 67 methods. The full original local C3 suite passed 142 methods before this repository packaging. See `VALIDATION.json` for scope.

`DEPENDENCIES.json` also records the pre-existing compatible dependency source pins and an optional local VTK standard-header patch. It does not certify a cold dependency build.
