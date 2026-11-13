# ADR-182: Advanced Gameplay AI Provider Authority

- **Status**: Proposed
- **Date**: 2026-11-10
- **Scope**: Optional planning, inference and conversational Gameplay AI providers
- **Issue**: [GAI-007.1](https://github.com/horocore/horo-engine/issues/1384)
- **Jira**: [HORO-1384](https://horo-engine.atlassian.net/browse/HORO-1384)
- **Parent**: [GAI-007](https://github.com/horocore/horo-engine/issues/1223)
- **Related**: [ADR-021](021-gameplay-ai-ownership-scheduling-and-behavior-boundary.md), [ADR-022](022-ai-fixed-tick-order-authority-and-simulation-budget.md), [ADR-025](025-ai-decision-assets-and-gameplay-behavior-boundary.md), [ADR-178](178-application-security-primitive-and-signature-baseline.md)
- **Normative documents**: [Navigation and AI Architecture](../architecture/runtime/navigation-and-ai-architecture.md), [Package System](../architecture/packages/package-system.md), [Application Security](../architecture/security/application-security.md)

## Context

Gameplay AI schedules and applies authoritative decisions at existing fixed-tick
boundaries. Optional planners and models introduce different execution,
compatibility, disclosure and latency properties. The core decision pipeline
needs an explicit contract for these contributions before it can rely on them
in shipped content. Editor agent tools and immersive authoring keep their own
policy and host composition.

## Decision

### Admission and compatibility

The host composes advanced providers by exact contribution identity and locked
package version. Admission verifies the package's publisher, artifact digest,
contribution manifest, compatible request/result versions, target support, and
the project-granted capabilities. Native and script contributions use their
existing package and backend-extension admission owners. A descriptor is
metadata until the host explicitly activates the provider.

Each contribution declares its execution domain, required immutable inputs,
maximum request and result sizes, work and concurrency limits, output schema,
and support for cancellation and deterministic ticks. A cooked dependency
records the admitted provider and schema identities. Missing or incompatible
dependencies retain inspectable authored data and fail typed admission. An
authored fallback may select a specific built-in branch after an enumerated
failure. Fallback participates in the same plan validation and budgets.

Core BT/HSM and utility decisions remain usable with every advanced provider
disabled. A target may omit the provider and its runtime dependencies entirely.

### Request and result authority

Only the owning authoritative simulation admits work for an active controller.
Each request carries the exact agent and Scene generations, plan identity,
provider activation generation, frozen input revisions, deadline, and scoped
cancellation. Providers return bounded proposals such as stable action IDs and
typed parameters. They receive immutable input values. The provider cannot use
the request to acquire Scene mutation, blackboard write, Editor/MCP, replication,
or network authority.

At the existing owner safe point, the decision runtime checks the proposal's
schema, capabilities, input and provider generations, deadline, and allowed
action set. The usual Gameplay owners validate live entity targets and execution
preconditions before applying any intent. Malformed, stale, cancelled, and
unauthorized proposals yield distinct typed outcomes. Neither late completion
nor a retried callback can apply a second intent. Teardown revokes admission,
cancels owned requests, and drains or retires their provider leases before code
or model data is unloaded.

### Deterministic and adaptive execution

Deterministic admission requires pinned inputs and artifacts, bounded work,
declared tick publication, stable ordering, and a qualified numeric contract.
A remote answer arriving on its service clock is ineligible. A local inference
provider declares its model digest, preprocessing schema, execution profile,
and reproducibility limits. Adaptive providers use bounded async work and
explicit deadlines. Pending work holds its reservation until completion or
retirement. Budget exhaustion follows the authored wait or fallback policy.

### Remote disclosure

A remote request requires an explicit project and product grant for the exact
provider, destination and data classes. The host constructs a bounded projection
from the provider's declared input fields and the grant. Only that projection
leaves the process. Secrets, raw captures, unrelated entities and unrestricted
blackboard or project state stay with their owners. Provider credentials remain
host-side. Redacted diagnostic evidence records identities, counts and outcome
codes. Retaining raw inputs or outputs requires a separate capture policy.

## Consequences

Provider availability can vary across targets without changing the core graph
contract. Provider failures surface at a typed decision boundary. Runtime,
authoring and save consumers can correlate an outcome without retaining live
provider or task handles. Compatibility, authority, disclosure, cancellation
and deterministic publication require executable evidence before a concrete
provider is qualified.

Qualification must exercise a provider-free core composition, a malformed or
revoked proposal through the actual intent boundary, and a remote test adapter
that observes the exact admitted projection. The package and backend-extension
prerequisites are tracked by [PKG-001](https://github.com/horocore/horo-engine/issues/34)
and [EXT-002](https://github.com/horocore/horo-engine/issues/57).

## Rejected Alternatives

- Provider-owned simulation or blackboard writes would split the gameplay
  authority and allow results to bypass owner validation.
- Implicit fallback to another installed provider would change the authored
  decision and disclosure contract based on the host's inventory.
- Unrestricted state capture would exceed declared capabilities and prevent
  independent qualification of remote data access.
