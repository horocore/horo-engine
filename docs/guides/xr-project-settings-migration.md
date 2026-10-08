# XR project-settings schema version 1

The application composition root registers the inert descriptors returned by
`XRProjectSettingDescriptors()` before sealing its shared `ConfigurationSchema`.
Foundation remains the only precedence and provenance authority. Resolve the
committed `ConfigurationSnapshot` through `ResolveXRProjectSettings()`; do not
read project JSON or environment variables from XR consumers.

The portable schema is:

| Key | Values / default |
| --- | --- |
| `xr.schema_version` | Exactly `1`; future versions require migration |
| `xr.enabled` | Boolean; default `false` |
| `xr.runtime_selection` | `0`: system default; `1`: approved developer override |
| `xr.capability_profile` | `0`: projection 1.0; `1`: tracked interaction 1.0 |
| `xr.limits.views` | Exactly `2` |
| `xr.limits.spaces` | `1..256`; default `2` |
| `xr.limits.actions` | `1..512`; default `8` |
| `xr.limits.devices` | `1..64` for projection, `2..64` for interaction; default `2` |

Extra-feature keys are `xr.features.stage_space`, `depth_composition`,
`fixed_foveation`, `refresh_rate`, `visibility_mask`, `controller_presentation`
and `android_standalone` (each uses the `xr.features.` prefix). Their integer
policy values are `0` unrequested, `1` required, `2` optional with explicit disable,
`3` optional with explicit baseline projection, and `4` disabled. Baseline
projection is legal only for depth composition, foveation, refresh rate and
visibility mask. Controller presentation requires the tracked-interaction profile.
These profiles are independent requirement sets, not renderer quality tiers.

Project, packaged-profile, invocation and session inputs are admitted. Environment
and user inputs are rejected by the descriptors. All XR settings require project
reopen, preventing a live setting edit from changing session policy behind native
resources. No runtime path, extension name, device identity, approval grant or
local discovery evidence is persisted in this schema.

Before session preparation, call `AdmitXRProjectSettings()` with the active
configuration revision, retained successful loader preflight, the current host
loader request, renderer/runtime interoperability evidence and exact capability
publication. Application composition supplies renderer compatibility for that
selected tuple; it must re-resolve this evidence whenever the tuple changes.
Project policy cannot authorize a developer override: the host request must carry
explicit development-mode approval. Admission rechecks loader versions, attempt,
composition identities, runtime generation and feature evidence before returning
one complete feature plan. Missing required interaction never downgrades to
projection. Error diagnostics retain the winning source location and setting key.

The policy owns fixed-size feature storage and retains the shared immutable source
snapshot. It remains readable after replacement/shutdown, but revision zero or a
replaced configuration, runtime or capability generation rejects further use.
Projection/admission are load-time boundaries and may allocate errors/provenance;
frame consumers use the existing fixed-work `AdmitXRPlannedFeature()` gate.

`XRProjectSettings.h` belongs to `HoroEngine::XRRuntime`; consumer coverage links
only that target. This additive contract does not migrate existing callers or
activate XR in a host automatically. Downstream setup UI and project adapters must
consume this schema rather than adding another XR policy authority.
