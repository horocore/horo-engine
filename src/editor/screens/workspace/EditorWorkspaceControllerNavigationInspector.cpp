#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <array>
#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Map only closed typed alternatives to the canonical built-in codec. */
        Runtime::BuiltinSceneComponentKind Kind(const NavigationInspectorValue &value) {
            constexpr std::array kinds{Runtime::BuiltinSceneComponentKind::NavigationSurface,
                                       Runtime::BuiltinSceneComponentKind::NavigationRegion,
                                       Runtime::BuiltinSceneComponentKind::NavigationModifier,
                                       Runtime::BuiltinSceneComponentKind::NavigationLink,
                                       Runtime::BuiltinSceneComponentKind::NavigationAgent};
            return kinds[value.index()];
        }

        /** @brief Put exactly one typed value into the codec's detached input and advance its authored generation. */
        Result<SceneSource::SceneObjectComponentSet> Components(const NavigationInspectorValue &value) {
            SceneSource::SceneObjectComponentSet components;
            const auto valid = std::visit([&](auto component) -> Result<void> {
                using T = decltype(component);
                if constexpr (!std::is_same_v<T, Runtime::NavigationAgentComponent>) {
                    if (component.generation == UINT64_MAX)
                        return Result<void>::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Capacity)));
                    ++component.generation;
                }
                if constexpr (std::is_same_v<T, Runtime::NavigationSurfaceComponent>)
                    components.navigationSurface = std::move(component);
                else if constexpr (std::is_same_v<T, Runtime::NavigationRegionComponent>)
                    components.navigationRegion = std::move(component);
                else if constexpr (std::is_same_v<T, Runtime::NavigationModifierComponent>)
                    components.navigationModifier = std::move(component);
                else if constexpr (std::is_same_v<T, Runtime::NavigationLinkComponent>)
                    components.navigationLink = std::move(component);
                else
                    components.navigationAgent = std::move(component);
                return Result<void>::Success();
            }, value);
            if (valid.HasError())
                return Result<SceneSource::SceneObjectComponentSet>::Failure(valid.ErrorValue());
            return Result<SceneSource::SceneObjectComponentSet>::Success(std::move(components));
        }

        /** @brief Preserve source-owned identity, schema and generation before serializing a Prefab draft. */
        bool MatchesSource(const NavigationInspectorValue &value, const Runtime::RuntimeComponentSet &source) {
            return std::visit([&](const auto &component) {
                using T = std::decay_t<decltype(component)>;
                const auto *before = [&]() -> const T * {
                    if constexpr (std::is_same_v<T, Runtime::NavigationSurfaceComponent>)
                        return source.navigationSurface ? &*source.navigationSurface : nullptr;
                    else if constexpr (std::is_same_v<T, Runtime::NavigationRegionComponent>)
                        return source.navigationRegion ? &*source.navigationRegion : nullptr;
                    else if constexpr (std::is_same_v<T, Runtime::NavigationModifierComponent>)
                        return source.navigationModifier ? &*source.navigationModifier : nullptr;
                    else if constexpr (std::is_same_v<T, Runtime::NavigationLinkComponent>)
                        return source.navigationLink ? &*source.navigationLink : nullptr;
                    else
                        return source.navigationAgent ? &*source.navigationAgent : nullptr;
                }();
                if (!before || before->schemaVersion != component.schemaVersion)
                    return false;
                if constexpr (!std::is_same_v<T, Runtime::NavigationAgentComponent>)
                    return before->id == component.id && before->generation == component.generation;
                return true;
            }, value);
        }

        /** @brief Descriptor publications are identity/revision fenced, including replacement at an equal revision. */
        bool SameDefinitions(std::span<const NavigationDefinitionSnapshot> a, std::span<const NavigationDefinitionSnapshot> b) {
            return std::ranges::equal(a, b, [](const auto &left, const auto &right) {
                return left.asset == right.asset && left.session == right.session && left.revision == right.revision &&
                       left.definition == right.definition;
            });
        }

        /** @brief Decode source-owned typed components only once while the exact immutable projection is pinned. */
        Result<Runtime::RuntimeComponentSet> ProjectComponents(const Prefab::ResolvedPrefabObject &object) {
            Runtime::RuntimeComponentSet baseline;
            std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
            for (const auto &raw : object.object.components) {
                auto projected = SceneSource::ProjectPrefabBuiltinComponent(raw, baseline, primitive);
                if (projected.HasError())
                    return Result<Runtime::RuntimeComponentSet>::Failure(projected.ErrorValue());
            }
            return Result<Runtime::RuntimeComponentSet>::Success(std::move(baseline));
        }

        /** @brief Fresh occurrence allocation also reserves all retained source removals and instance additions. */
        std::uint64_t LastOccurrence(const Prefab::ResolvedPrefabObject &object, const ScenePrefabInstance &instance) {
            std::uint64_t next{};
            for (const auto &raw : object.object.components)
                next = std::max(next, raw.instance.Value());
            if (instance.valueOverrides)
                for (const auto &record : instance.valueOverrides->componentRecords) {
                    std::visit([&](const auto &intent) {
                        using T = std::decay_t<decltype(intent)>;
                        if constexpr (std::is_same_v<T, Prefab::PrefabAddComponentOverride>) {
                            if (intent.object == object.key.object)
                                next = std::max(next, intent.payload.instance.Value());
                        } else if (intent.target.object == object.key.object)
                            next = std::max(next, intent.target.instance.Value());
                    }, record);
                }
            return next;
        }

        /** @brief Encode a single exact occurrence replacement without selecting a source or mutating an instance. */
        Result<Prefab::PrefabReplaceComponentEdit> PrepareReplacement(const NavigationInspectorEdit &edit,
                                                                      const Prefab::ResolvedPrefabObject &object, std::uint64_t &next) {
            using Return = Result<Prefab::PrefabReplaceComponentEdit>;
            const auto kind = Kind(edit.value);
            const auto before = std::ranges::find_if(object.object.components, [kind](const auto &raw) {
                return Runtime::FindBuiltinSceneComponent(raw.component.typeId.Value()) == kind;
            });
            if (before == object.object.components.end() || next == UINT64_MAX)
                return Return::Failure(MakeError(Prefab::PrefabErrors::OverrideOrphan));
            auto components = Components(edit.value);
            if (components.HasError())
                return Return::Failure(components.ErrorValue());
            auto raw = SceneSource::EncodePrefabNavigationComponent(kind, Prefab::PrefabComponentInstanceId::Create(++next).Value(),
                                                                    components.Value());
            if (raw.HasError())
                return Return::Failure(raw.ErrorValue());
            return Return::Success({{object.key.object, before->component.typeId, before->instance}, std::move(raw).Value()});
        }

        /** @brief Capture one component gesture against immutable source-owned identities and exact schema/opening fences. */
        Result<EditScenePrefabComponentsCommand> PreparePrefabCommand(const NavigationInspectorRequest &request,
                                                                      const SceneSource::ScenePrefabInstanceProjection &instance,
                                                                      const Prefab::PrefabValueSchemaSnapshot &schemas,
                                                                      std::uint64_t epoch) {
            using Return = Result<EditScenePrefabComponentsCommand>;
            const auto objects = instance.expanded->Objects();
            const auto object = std::ranges::find(objects, *request.prefab->member, &Prefab::ResolvedPrefabObject::key);
            if (object == objects.end())
                return Return::Failure(MakeError(Prefab::PrefabErrors::OverrideOrphan));
            auto baseline = ProjectComponents(*object);
            if (baseline.HasError())
                return Return::Failure(baseline.ErrorValue());
            EditScenePrefabComponentsCommand command{request.prefab->instance,
                                                     {request.context.revision},
                                                     instance.expanded->Revision(),
                                                     schemas.Revision(),
                                                     {},
                                                     epoch};
            auto next = LastOccurrence(*object, instance.authored);
            std::array<bool, 5> edited{};
            for (const auto &edit : request.edits) {
                if (edit.object.value != 0 || edited[edit.value.index()] || !MatchesSource(edit.value, baseline.Value()))
                    return Return::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Stale)));
                edited[edit.value.index()] = true;
                auto replacement = PrepareReplacement(edit, *object, next);
                if (replacement.HasError())
                    return Return::Failure(replacement.ErrorValue());
                command.edits.emplace_back(std::move(replacement).Value());
            }
            return Return::Success(std::move(command));
        }

        /** @brief Resolve every detached edit before allowing any containing Scene/history publication. */
        Result<Prefab::PrefabValueOverrideSet> PrepareOverrides(const Prefab::EffectivePrefabCandidate &source,
                                                                std::optional<Prefab::PrefabValueOverrideSet> current,
                                                                const EditScenePrefabComponentsCommand &command,
                                                                const Prefab::PrefabValueSchemaSnapshot &schemas,
                                                                const Prefab::PrefabLimitProfile &limits,
                                                                const CancellationToken &cancellation) {
            using Return = Result<Prefab::PrefabValueOverrideSet>;
            for (const auto &edit : command.edits) {
                auto prepared = Prefab::PreparePrefabComponentEdit(source, current, edit, &schemas, limits, cancellation);
                if (prepared.HasError())
                    return Return::Failure(prepared.ErrorValue());
                if (!prepared.Value().effective || prepared.Value().rebased.HasUnresolved())
                    return Return::Failure(MakeError(Prefab::PrefabErrors::OverrideSourceStale));
                current = std::move(prepared).Value().rebased;
            }
            return Return::Success(std::move(*current));
        }

        /** @brief Explicit immutable validation inputs, borrowed only through this synchronous source-lease operation. */
        struct PrefabValidationContext final {
            const Prefab::PrefabSourceResolverSnapshot &sources;
            const Prefab::PrefabLimitProfile &limits;
            const Prefab::PrefabValueSchemaSnapshot &schemas;
            std::span<const NavigationDefinitionSnapshot> definitions;
            Runtime::SceneDefinitionId scene;
            const std::optional<SceneSource::TriggerVolumeConversionContext> &trigger;
        };

        /** @brief Validate canonical full Scene conversion and descriptor references while retaining all unrelated authored fields. */
        Result<void> ValidatePrefabCandidate(SceneDocumentSnapshot candidate, const EditScenePrefabComponentsCommand &command,
                                             Prefab::PrefabValueOverrideSet overrides, const PrefabValidationContext &context) {
            const auto placement = std::ranges::find(candidate.prefabInstances, command.instance, &ScenePrefabInstance::instanceId);
            if (placement == candidate.prefabInstances.end())
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::OverrideOrphan));
            placement->valueOverrides = std::move(overrides);
            auto runtime =
                ConvertSceneDocumentToRuntime(candidate, context.scene, context.sources, context.limits, &context.schemas, context.trigger);
            if (runtime.HasError())
                return Result<void>::Failure(runtime.ErrorValue());
            std::vector<SceneSource::SceneObjectSnapshot> graph;
            for (const auto &entity : runtime.Value().Entities()) {
                SceneSource::SceneObjectSnapshot projected{.id = {entity.object.value}, .name = "Navigation validation"};
                projected.components.navigationSurface = entity.components.navigationSurface;
                projected.components.navigationRegion = entity.components.navigationRegion;
                projected.components.navigationModifier = entity.components.navigationModifier;
                projected.components.navigationLink = entity.components.navigationLink;
                projected.components.navigationAgent = entity.components.navigationAgent;
                graph.push_back(std::move(projected));
            }
            return ValidateNavigationInspectorDescriptors(graph, context.definitions);
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::RefreshNavigationInspectorDefinitions */
    void EditorWorkspaceController::RefreshNavigationInspectorDefinitions() {
        m_viewModel.navigationDefinitions.clear();
        if (!m_navigationSettings &&
            std::ranges::none_of(m_document.Objects(),
                                 [](const auto &object) {
            const auto &c = object.components;
            return c.navigationSurface || c.navigationRegion || c.navigationModifier || c.navigationLink || c.navigationAgent;
        }) &&
            m_document.PrefabInstances().empty())
            return;
        auto settings = NavigationSettings();
        if (settings.HasError())
            return;
        auto definitions = settings.Value()->Definitions();
        if (definitions.HasValue())
            m_viewModel.navigationDefinitions.assign(definitions.Value().begin(), definitions.Value().end());
    }

    /** @copydoc EditorWorkspaceController::EnsureNavigationPrefabSchemas */
    Result<void> EditorWorkspaceController::EnsureNavigationPrefabSchemas(const Prefab::PrefabLimitProfile &limits) {
        if (m_navigationPrefabSchemas)
            return Result<void>::Success();
        std::vector<Prefab::PrefabComponentOverridePolicy> components;
        for (const auto &builtin : Runtime::BuiltinSceneComponentSchemas()) {
            auto type = Gameplay::ComponentTypeId::Parse(builtin.typeId);
            if (type.HasError())
                return Result<void>::Failure(type.ErrorValue());
            const bool navigation = builtin.kind >= Runtime::BuiltinSceneComponentKind::NavigationSurface &&
                                    builtin.kind <= Runtime::BuiltinSceneComponentKind::NavigationAgent;
            Gameplay::ComponentDescriptor descriptor{.typeId = type.Value(),
                                                     .displayName = std::string(builtin.typeId),
                                                     .category = "Navigation"};
            const auto maximum = builtin.kind == Runtime::BuiltinSceneComponentKind::Collider ||
                                         builtin.kind == Runtime::BuiltinSceneComponentKind::PhysicsConstraint
                                     ? static_cast<std::uint32_t>(limits.Policy().maximumComponentsPerObject)
                                     : 1U;
            components.push_back({std::move(descriptor), maximum, navigation, navigation, {}, {}, {}});
        }
        auto schemas = Prefab::PrefabValueSchemaSnapshot::Create({}, limits, std::move(components));
        if (schemas.HasError())
            return Result<void>::Failure(schemas.ErrorValue());
        m_navigationPrefabSchemas.emplace(std::move(schemas).Value());
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ApplyNavigationInspectorCommand */
    void EditorWorkspaceController::ApplyNavigationInspectorCommand(const EditorWorkspaceViewCommandData &command) {
        if (!command.navigationPayload || std::this_thread::get_id() != m_physicsOwnerThread)
            return;
        const auto &request = *command.navigationPayload;
        const auto reject = [this](SceneTransactionFailure failure) {
            m_viewModel.navigationEditError = MakeError(SceneTransactionError(failure)).message;
        };
        if (m_viewModel.sceneExternalConflict || m_playSession.IsActive() || !request.context.canEdit ||
            request.context.cancellation.IsCancellationRequested()) {
            reject(SceneTransactionFailure::Unauthorized);
            return;
        }
        RefreshPhysicsInspectorPort();
        RefreshNavigationInspectorDefinitions();
        if (!m_physicsTransactions || request.context.session != m_viewModel.physicsContext.session ||
            request.context.revision != m_document.Revision().value || request.context.state != m_document.State().value ||
            !SameDefinitions(request.definitions, m_viewModel.navigationDefinitions)) {
            reject(SceneTransactionFailure::Stale);
            return;
        }
        if (request.prefab) {
            ApplyPrefabNavigationInspector(request);
            return;
        }
        ApplySceneNavigationInspector(request);
    }

    /** @copydoc EditorWorkspaceController::ApplySceneNavigationInspector */
    void EditorWorkspaceController::ApplySceneNavigationInspector(const NavigationInspectorRequest &request) {
        const auto reject = [this](SceneTransactionFailure failure) {
            m_viewModel.navigationEditError = MakeError(SceneTransactionError(failure)).message;
        };
        const auto &selection = m_selection.Current();
        if (request.selectionRevision != selection.revision.value || request.objects != selection.objects ||
            request.prefab != m_viewModel.prefabSelection || std::ranges::any_of(request.edits, [&](const auto &edit) {
            return std::ranges::find(selection.objects, edit.object) == selection.objects.end();
        })) {
            reject(SceneTransactionFailure::Unauthorized);
            return;
        }
        auto snapshot = m_physicsTransactions->Snapshot();
        if (snapshot.HasError()) {
            m_viewModel.navigationEditError = snapshot.ErrorValue().message;
            return;
        }
        auto prepared = PrepareNavigationInspectorEdits(snapshot.Value(), request.edits, m_viewModel.navigationDefinitions);
        if (prepared.HasError()) {
            m_viewModel.navigationEditError = prepared.ErrorValue().message;
            return;
        }
        SceneCommandResult event;
        event.kind = DocumentChangeKind::ComponentChanged;
        for (const auto &replacement : prepared.Value())
            event.affectedObjects.push_back(replacement.object);
        if (!event.affectedObjects.empty())
            event.object = event.affectedObjects.front();
        auto committed = m_physicsTransactions->Execute(request.context, prepared.Value());
        if (committed.HasError()) {
            m_viewModel.navigationEditError = committed.ErrorValue().message;
            return;
        }
        event.revision = {committed.Value().revision};
        event.state = {committed.Value().state};
        event.committed = committed.Value().committed;
        m_viewModel.navigationEditError.clear();
        HandleDocumentCommandResult(Result<SceneCommandResult>::Success(std::move(event)), "Update navigation components");
    }

    /** @copydoc EditorWorkspaceController::ApplyPrefabNavigationInspector */
    void EditorWorkspaceController::ApplyPrefabNavigationInspector(const NavigationInspectorRequest &request) {
        if (request.prefab != m_viewModel.prefabSelection || !request.prefab->member || !m_prefabSources || !m_prefabSourceCapture ||
            !request.publication || request.publication != m_viewModel.prefabHierarchy || !request.publication->sources ||
            m_viewModel.prefabHierarchyUnavailable || request.publication->publication != m_prefabPublication ||
            m_prefabObservedDocumentEpoch != m_document.ReplacementEpoch() || request.edits.empty() || request.edits.size() > 5 ||
            m_assetRegistry.Revision() != m_prefabSources->resolver.RegistryRevision()) {
            m_viewModel.navigationEditError = MakeError(SceneTransactionError(SceneTransactionFailure::Stale)).message;
            return;
        }
        auto committed = CommitPrefabNavigationInspector(request);
        if (committed.HasError()) {
            m_viewModel.navigationEditError = committed.ErrorValue().message;
            return;
        }
        m_viewModel.navigationEditError.clear();
        HandleDocumentCommandResult(std::move(committed), "Update Prefab navigation components");
    }

    /** @copydoc EditorWorkspaceController::CommitPrefabNavigationInspector */
    Result<SceneCommandResult> EditorWorkspaceController::CommitPrefabNavigationInspector(const NavigationInspectorRequest &request) {
        using Return = Result<SceneCommandResult>;
        auto limits = Prefab::PrefabLimitProfile::Create({});
        if (limits.HasError())
            return Return::Failure(limits.ErrorValue());
        if (auto schemas = EnsureNavigationPrefabSchemas(limits.Value()); schemas.HasError())
            return Return::Failure(schemas.ErrorValue());
        const auto &instances = request.publication->sources->instances;
        const auto instance = std::ranges::find_if(instances, [&](const auto &entry) {
            return entry.authored.instanceId == request.prefab->instance;
        });
        if (instance == instances.end() || !instance->IsSynchronized())
            return Return::Failure(MakeError(Prefab::PrefabErrors::OverrideSourceStale));
        std::vector<Assets::AssetId> roots;
        for (const auto &placement : m_document.PrefabInstances())
            roots.push_back(placement.sourcePrefab.Asset());
        auto captured = m_prefabSourceCapture(m_assetRegistry, roots, limits.Value(), request.context.cancellation);
        if (captured.HasError())
            return Return::Failure(captured.ErrorValue());
        // Current bytes and the mutation lease stay pinned through the final owner publication.
        auto fresh = std::move(captured).Value();
        auto command = PreparePrefabCommand(request, *instance, *m_navigationPrefabSchemas, m_document.ReplacementEpoch());
        if (command.HasError())
            return Return::Failure(command.ErrorValue());
        auto source = fresh.sources.resolver.Resolve(instance->authored.sourcePrefab.Asset(), request.prefab->instance, limits.Value());
        if (source.HasError())
            return Return::Failure(source.ErrorValue());
        if (source.Value().Revision() != command.Value().expectedSource)
            return Return::Failure(MakeError(Prefab::PrefabErrors::OverrideSourceStale));
        auto overrides = PrepareOverrides(source.Value(), instance->authored.valueOverrides, command.Value(), *m_navigationPrefabSchemas,
                                          limits.Value(), request.context.cancellation);
        if (overrides.HasError())
            return Return::Failure(overrides.ErrorValue());
        const PrefabValidationContext context{fresh.sources.resolver,     limits.Value(),
                                              *m_navigationPrefabSchemas, m_viewModel.navigationDefinitions,
                                              m_previewSceneId,           m_triggerBinding};
        if (auto valid = ValidatePrefabCandidate(m_document.Snapshot(), command.Value(), std::move(overrides).Value(), context);
            valid.HasError())
            return Return::Failure(valid.ErrorValue());
        if (auto current = fresh.sources.inputs.VerifyUnchanged(request.context.cancellation); current.HasError())
            return Return::Failure(current.ErrorValue());
        SceneDocumentPrefabValueCommands executor{m_document, m_history};
        auto committed = executor.Execute(command.Value(), fresh.sources.resolver, &*m_navigationPrefabSchemas, limits.Value(),
                                          request.context.cancellation);
        if (committed.HasValue())
            m_prefabSources.emplace(std::move(fresh.sources));
        return committed;
    }
}  // namespace Horo::Editor
