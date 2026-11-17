# Native Audio Device Qualification

This is the result matrix and recording format for AUD-007. The required test
families and ownership rules are defined in the
[delivery ledger](../architecture/runtime/audio-device-delivery.md#hardware-qualification-protocol)
and [Audio Architecture](../architecture/runtime/audio-architecture.md).
Each recorded result belongs to one exact source snapshot, host, native peer,
endpoint class and workload. A result from another combination does not fill a cell.

## Current matrix

All cells below are **unqualified**. Native source and fixture coverage exist;
compiled native execution and physical-device evidence have not been recorded
for this delivery. Windows and Linux rows must be expanded for each architecture
declared by the product being qualified. The two macOS architectures are separate
required lanes under the delivery protocol.

| Native peer and host | Speakers | Wired headphones | USB output | Bluetooth output |
|---|---|---|---|---|
| WASAPI / Windows / declared product architecture | Unqualified | Unqualified | Unqualified | Unqualified |
| Core Audio / macOS / arm64 | Unqualified | Unqualified | Unqualified | Unqualified |
| Core Audio / macOS / x86_64 | Unqualified | Unqualified | Unqualified | Unqualified |
| PipeWire / Linux / declared product architecture | Unqualified | Unqualified | Unqualified | Unqualified |

Every physical cell requires all applicable test families below. Record each
family separately rather than combining a successful Open with an untested
recovery or shutdown path.

| Test family | Required evidence |
|---|---|
| Discovery and selection | Complete catalogs, stable unchanged IDs, correct role defaults, explicit selection, missing/stale selection rejection and bounded long/localized labels. |
| Negotiation and diagnostics | Preserved requested tuple and bounds; actual rate, semantic layout, native/Horo periods and fresh timing provenance after reconfiguration. |
| Steady playback | Actual selected native output, matching Ready/activation, finite PCM, workload and observation window, callback allocation/free/deadline and underrun observations. |
| Migration and recovery | Default changes, unplug/replug, invalidation, available Bluetooth route/rate changes, service restart, explicit switch and one terminal per transaction. Preserve/Restart requires actual voice position and execution evidence. |
| Focus and suspension | Focus/minimize, suspend/resume and interruption policy; closed producer admission, new clock/discontinuity evidence and bounded resumption. |
| Retirement and rollback | Failure/cancellation at acquisition boundaries, stale/late facts, saturated/pending completions, deadline expiration, repeated shutdown and actual native reader/listener retirement before release. |

Use a separate diagnostic record for Null, virtual sinks and redirected endpoints.
Those runs can establish their own behavior; they do not fill a physical cell in
the matrix. Installed driver names or operating-system device status alone do not
establish an active endpoint or an executed Horo native lifecycle.

WASAPI, Core Audio, PipeWire and enumerated SDL3 endpoints now report `Unknown`
when their discovery path supplies no native evidence distinguishing physical
from virtual routing. The SDL3 logical system-default endpoint remains `Virtual`;
Null remains `Headless`. An unknown endpoint remains selectable, but does not fill
a physical cell. Verify the actual route independently for qualification; a
friendly name or successful Open cannot establish its class. Native-evidence
classification and execution of the new discovery regressions remain outstanding.

## Recording a run

Copy [the blank record](audio-device-qualification-record.template.json) into the
ignored build/report location used for that run. Record source commit plus the
content manifest for any uncommitted inputs. Record the actual OS/build,
architecture, build configuration, SDK/compiler and native API/library version.
Use a privacy-safe endpoint description and class rather than serial numbers or
raw native identifiers. The record and referenced diagnostics belong to the
actual running composition, including the host's explicit selected backend.

For a physical matrix cell, retain separate results for default and explicit
selection. Record the requested and actual tuples before and after every change.
Record one switch operation identity, its old/new device epochs, configured
playback policy, actual source cursor/execution state, native terminal and overall
playback outcome. Record fresh sample-clock generation/discontinuity and the first
new callback anchor before claiming producer resumption. A copied policy value or
backend activation alone does not prove preserved or restarted playback.

Record render-core progress separately from native packet submission and queued
device progress. A canonical block can still have retained, unsubmitted PCM when
the core publishes its hold boundary. Include unequal core/native packet sizes
in the migration cases, and verify the declared handling of that retained fade
tail. Device loss needs an explicit discontinuity/loss policy. A core cursor,
native buffer submission and physical audible position are distinct evidence.

For timing, retain quality, source, units, sample count and observation window.
Missing measurements remain `null`; zero is a measurement only when actually
observed. Buffer preferences and software estimates remain separate from actual
native values and physical loopback measurements. Latency components are summed
only when their owning contracts establish that they are disjoint.

For teardown, retain the exact native Stop/Close results and reader/listener
retirement evidence. Pending, cancellation and timeout alone cannot prove release.
A failure-injection case may correctly retain an island; its record must explain
the retained outcome and the final owned-process/native cleanup evidence rather
than calling it a clean shutdown.

## Result interpretation

Use these per-family states in the record:

| State | Meaning |
|---|---|
| Missing | The case has not been executed or its evidence is absent. |
| Unavailable | The required host, endpoint, permission or observation facility was unavailable; record the reason. |
| Unsupported | An explicit rejection was observed for a declared unsupported mode; record the expected contract and typed result. |
| Failed | Executed behavior contradicted the expected contract or required evidence was inconsistent. |
| Passed | The exact recorded case executed and its required assertions and evidence were verified. |

A physical cell is qualified only after its required positive families pass and
its required rejection/failure cases match their declared contracts. Missing or
Unavailable cases remain gaps. Unsupported mirroring/aggregation must reject
explicitly; this negative-case success does not qualify synchronized output.
Factory/header tests establish composition boundaries; shared primitive/Null tests
establish their recorded contract scope. Hardware qualification additionally
requires actual native endpoint and recovery execution.

Reconfiguration that changes the source, binary, OS, native library, endpoint
format or tested workload requires fresh relevant evidence or an explicit bounded
applicability argument. Attach only the evidence supporting the claimed scope.
The current delivery remains unqualified until actual run records replace the
missing evidence; this blank template records no pass result.
