# Custom perception sense extension and Gameplay SDK 30

`GameplayDescriptorBundle` gains an explicit custom-sense table and count. This
changes the native SDK layout from 29 to 30; rebuild gameplay modules, generated
bundle sources, SDK consumers and their host together with the matching toolchain.
Old modules fail the existing exact SDK boundary check before the new tail is read.
Never relabel an old binary as SDK 30. Rollback requires the matching SDK 29 host
and modules, not mixed descriptor layouts. The canonical generated migration guide
also records this boundary in `sdk/MigrationGuide.md.in`.

Native modules use `HORO_AI_SENSE(Type, "persistent_decimal_id")` and implement
`IPerceptionSenseProvider` plus `DescribePerceptionSense`. The generator emits the
table in numeric identity order, annotates source locations and rejects duplicate,
reserved, malformed and excessive declarations before replacing the prior bundle.
Modules without senses publish an explicit empty table. Metadata construction
failures produce an invalid contribution rejected before module activation;
the exported noexcept bundle getter must never terminate the process.

`GameModuleHost::AcquirePerceptionSenses` retains the actual module owner and its
admission state. `GameplayPackageModuleHost::AcquirePerceptionSenses` resolves the
existing verified role and returns its locked package version, artifact and
activation generation with that same native lease. It does not introduce another
trust policy. Reload/uninstall revokes calls first and waits for instance-owned
destroy callbacks and metadata leases before unloading native code.

Compose the scene's registry from the exact captured sense/stimulus contracts and
declared listener. Create instances through the generation with explicit host
capabilities and the agent/listener cancellation ancestry. Providers receive only
frozen facts and return candidate indices with binary outcomes. They cannot supply
arbitrary entities, positions, memory keys or blackboard writes. Built-in event
identities remain reserved. Bounds are 64 senses per generation, 128 candidates per
call, 4096 provider work units and one MiB declared native instance storage.

The `AISceneIntegration` owner calls `RunPerceptionSenseAtSensePoll` with the actual
spatial owner, current Scene view and recipient memory. It rejects stale
publications, changed Scene views and dead generations after callbacks, then
atomically stages the complete validated result. Run the existing declared-key
blackboard projection separately at `BlackboardSync`; omission/truncation does not
clear previous sensing. Native admission alone does not wire the product host.

Header ownership remains narrow: provider contracts belong to `HoroAI`, native
annotation helpers to `HoroGameplayApi`, and the Scene bridge to
`HoroAISceneIntegration`. Public-header consumers must rebuild for all affected
owners. No repository-wide include root or private Scene contract is published.

Coverage includes bounded core callbacks, wrong owner/capability, exception and
output isolation, real native image reload/unload, verified package-role
retirement, generated-table rollback and actual Scene publication/despawn memory
transactions. Current native/Scene additions still require the scheduled Windows
build/test pass and final worktree-aware preflight. Passing value-only fixtures is
not real Scene or native-image evidence. The bounded Lua adapter and declared
package-script composition require their own native test pass; product dispatch
remains outstanding and is not hidden by native registration coverage.

Scripts use `LuaPerceptionSenseProgram::Compile` with a mandatory acquired native
perception parent. The source returns a predicate accepting six exact signed
64-bit millimeter coordinates (candidate then listener), returning boolean or nil.
The scalar tier removes string/table/utf8 libraries, protected calls, metatable/raw
access and logging/load/random entry points. It also clears the string metatable,
so literal-string methods cannot recover unmetered native pattern matching.
VM allocation is rejected during sensing; source/VM setup is confined to activation.
One instruction ceiling applies to the entire candidate evaluation. A private
capture path admits this enforced VM tier while public native capture still rejects
raw Script callbacks. Program and native-parent retirement both revoke acquired
instances. `CompilePerceptionSense` on the package host reads only the current
verified role's declared script bytes; a physical file or unrelated node lease
cannot replace that authority.
