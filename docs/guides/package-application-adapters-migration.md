# Shared package application adapters

HORO-127 adds the application-owned `HoroEngine::PackageApplication` target over
existing `PackageRestoreService`, `PackageInstallService` and
`PackageLifecycleService`. There is one operation journal and mutation lane.
Construction checks the exact lifecycle/install authority and canonical restore/install project-root binding.
The additive `ProjectRoot` and `UsesInstallService` inspection methods change no module ABI layout; existing
callers remain valid and C++ hosts rebuild against the updated headers.
The host exclusively routes package mutations through that owner and retains its
borrowed authorities and scheduler until nonblocking Close succeeds. Closing
admission does not grant permission to destroy an authority while its admitted I/O
job is still running. Final-owner abandonment follows the existing explicit fatal
lifetime precondition used by PackageRestoreService; normal hosts cancel and drain.

Restore preserves the existing exact lockfile request, source selection,
publisher and resource policies. Install centrally restores that same request and
then runs the existing durable installer on an explicitly admitted I/O job.
Presentation code does not reproduce those transitions. A postcommit cancellation
cannot rewrite the durable install result. Activation remains on the same owner
lane and waits for a genuine Startup/Quiescent host boundary. Protocol inputs
cannot choose that boundary or approve trust. Disable is an explicit empty
selection, not an uninstall or trust mutation.

`HoroEngine::CliPackageAdapters` and `HoroEngine::McpPackageAdapters` are separate
adapter targets. They depend on the application contract; the application target
has no CLI, MCP or editor dependency. Registrations/descriptors remain inert.
Standalone generated public-header consumers compile all three contracts through
only their assigned target usage requirements. The native extension C ABI,
GameRuntimeContext layout and SDK epoch are unchanged.

Operation identities never wrap or alias a later operation in the same owner.
Adapters retain exact project capability/generation bindings and use immutable
snapshots for progress. A replaced operation ID fails rather than displaying or
cancelling its successor. Errors from restore/install/lifecycle keep their original
identities and causes; protocol rendering redacts private paths and messages.

Current main provides verified archive inventory and locked identity, version,
source, contribution IDs and dependency evidence. It does not implement the
normative semantic package-kind/license/import-copy manifest model. Presentation
must explicitly mark those facts unavailable. Unknown evidence never authorizes
trust, import/copy, uninstall or another mutation requiring that fact. This ticket
does not invent a competing semantic manifest parser or bypass the owning policy.

The non-installed `HoroPackageProjectHost` composition contract stages only
`PackageProjectSession.h`, `PackageProjectHost.h` and `PackageHostPolicy.h`, rather than exporting the shared application source
directory. The root schedules bounded `PreparePackageProject` metadata reading
on its I/O lane, then constructs the owner-lane session from explicit filesystem,
cache root, scheduler, wall clock, execution trust, native gate, platform and
transport policy. The default portable restore mode is offline; no online source
or execution approval is discovered from project data. Aliasing application
capabilities retain the complete session authorities. Roots retain each current
or retiring project session until typed `Close` succeeds; callbacks must drain
before the borrowed services are destroyed. Final-owner lane/busy violations use
the existing PackageApplication lifetime precondition and terminate; ordinary
shutdown must cancel and pump to completion instead.

The editor session retains a generation per modal opening, reconnects to actual
application operations, and rejects stale controls before forwarding commands.
Refresh and drawing do not execute mutations or advance an activation boundary.
Closing presentation releases only its view; operation ownership remains in the
application. Missing package kind/license/import-copy facts are displayed as
unavailable. The view never turns unavailable evidence into execution permission.

The actual `horo-engine package restore|install|activate|disable` dispatcher uses
an invocation-bound factory, with explicit absolute `--project`,
`--package-cache-root` and `--package-temporary-root` paths. Activate additionally
requires repeatable exact `--enable package-id:descriptor` selections. Offline
mode and missing execution approval remain explicit host policy; commands do not
discover a network provider or convert requested enablement into native trust.
`horo-engine mcp serve` accepts the same storage binding when package tools are
requested. Its real stdio root publishes package tools once alongside existing
observability tools, retains exact project/generation admission, and pumps the
application between bounded idle reads. MCP disconnect drains protocol callbacks
before the executable cancels and joins package work.

`HoroEditor` injects the same project host into the real File > Project packages
modal route. The workspace supplies only its actual idle/running play boundary,
checked against the exact project binding. Independent frame pumping continues
after modal closure or workspace departure. Shutdown requests package Close,
closes views, cancels/joins workers while filesystem, clocks and execution trust
remain alive, and completes Close before destroying those services. Constructor
rollback uses the existing `EditorWorkerShutdown` guard with the same order.

The root prepares bounded regular project metadata on its I/O worker and validates
both request-to-lock intent and existing restore compatibility. A host-selected
one-second sample age bounds admission; sample time is recorded before scheduling,
not when completion is observed. Expired evidence permits drainage but no new
mutation or native activation. Changed/malformed metadata or project retirement
revokes the prior generation before another activation. Old modal/MCP capabilities
fail closed and callers can explicitly refresh/reconnect after correction. No
watcher, ambient service lookup, blocking draw-path read, or presentation trust
boolean replaces the canonical authorities.
