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
        const auto reject = [this](const Error &error) {
            m_viewModel.navigationEditError = error.message;
        };
        if (request.prefab != m_viewModel.prefabSelection || !request.prefab->member || !m_prefabSources || !request.publication ||
            request.publication != m_viewModel.prefabHierarchy || m_viewModel.prefabHierarchyUnavailable ||
            request.publication->publication != m_prefabPublication || m_prefabObservedDocumentEpoch != m_document.ReplacementEpoch() ||
            request.edits.empty() || request.edits.size() > 5 ||
            m_assetRegistry.Revision() != m_prefabSources->resolver.RegistryRevision()) {
            reject(MakeError(SceneTransactionError(SceneTransactionFailure::Stale)));
            return;
        }
        auto limits = Prefab::PrefabLimitProfile::Create({});
        if (limits.HasError()) {
            reject(limits.ErrorValue());
            return;
        }
        if (auto schemas = EnsureNavigationPrefabSchemas(limits.Value()); schemas.HasError()) {
            reject(schemas.ErrorValue());
            return;
        }
        const auto &instances = request.publication->sources->instances;
        const auto instance = std::ranges::find_if(instances, [&](const auto &entry) {
            return entry.authored.instanceId == request.prefab->instance;
        });
        if (instance == instances.end() || !instance->IsSynchronized()) {
            reject(MakeError(Prefab::PrefabErrors::OverrideSourceStale));
            return;
        }
        if (!m_prefabSourceCapture) {
            reject(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
            return;
        }
        std::vector<Assets::AssetId> roots;
        for (const auto &placement : m_document.PrefabInstances())
            roots.push_back(placement.sourcePrefab.Asset());
        auto captured = m_prefabSourceCapture(m_assetRegistry, roots, limits.Value(), request.context.cancellation);
        if (captured.HasError()) {
            reject(captured.ErrorValue());
            return;
        }
        // The current source bytes and project mutation lease remain pinned through final owner publication.
        auto fresh = std::move(captured).Value();
        const auto objects = instance->expanded->Objects();
        const auto object = std::ranges::find(objects, *request.prefab->member, &Prefab::ResolvedPrefabObject::key);
        if (object == objects.end()) {
            reject(MakeError(Prefab::PrefabErrors::OverrideOrphan));
            return;
        }
        EditScenePrefabComponentsCommand command{request.prefab->instance,
                                                 m_document.Revision(),
                                                 instance->expanded->Revision(),
                                                 m_navigationPrefabSchemas->Revision(),
                                                 {},
                                                 m_document.ReplacementEpoch()};
        Runtime::RuntimeComponentSet baseline;
        std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
        for (const auto &raw : object->object.components) {
            auto projected = SceneSource::ProjectPrefabBuiltinComponent(raw, baseline, primitive);
            if (projected.HasError()) {
                reject(projected.ErrorValue());
                return;
            }
        }
        std::uint64_t next{};
        for (const auto &raw : object->object.components)
            next = std::max(next, raw.instance.Value());
        if (instance->authored.valueOverrides)
            for (const auto &record : instance->authored.valueOverrides->componentRecords) {
                std::visit([&](const auto &intent) {
                    using T = std::decay_t<decltype(intent)>;
                    if constexpr (std::is_same_v<T, Prefab::PrefabAddComponentOverride>) {
                        if (intent.object == request.prefab->member->object)
                            next = std::max(next, intent.payload.instance.Value());
                    } else if (intent.target.object == request.prefab->member->object)
                        next = std::max(next, intent.target.instance.Value());
                }, record);
            }
        std::array<bool, 5> edited{};
        for (const auto &edit : request.edits) {
            if (edit.object.value != 0 || edited[edit.value.index()] || !MatchesSource(edit.value, baseline)) {
                reject(MakeError(SceneTransactionError(SceneTransactionFailure::Stale)));
                return;
            }
            edited[edit.value.index()] = true;
            const auto kind = Kind(edit.value);
            const auto before = std::ranges::find_if(object->object.components, [kind](const auto &raw) {
                return Runtime::FindBuiltinSceneComponent(raw.component.typeId.Value()) == kind;
            });
            if (before == object->object.components.end() || next == UINT64_MAX) {
                reject(MakeError(Prefab::PrefabErrors::OverrideOrphan));
                return;
            }
            auto components = Components(edit.value);
            if (components.HasError()) {
                reject(components.ErrorValue());
                return;
            }
            auto raw = SceneSource::EncodePrefabNavigationComponent(kind, Prefab::PrefabComponentInstanceId::Create(++next).Value(),
                                                                    components.Value());
            if (raw.HasError()) {
                reject(raw.ErrorValue());
                return;
            }
            command.edits.push_back(
                Prefab::PrefabReplaceComponentEdit{{request.prefab->member->object, before->component.typeId, before->instance},
                                                   std::move(raw).Value()});
        }
        auto source = fresh.sources.resolver.Resolve(instance->authored.sourcePrefab.Asset(), command.instance, limits.Value());
        if (source.HasError()) {
            reject(source.ErrorValue());
            return;
        }
        if (source.Value().Revision() != command.expectedSource) {
            reject(MakeError(Prefab::PrefabErrors::OverrideSourceStale));
            return;
        }
        auto current = instance->authored.valueOverrides;
        for (const auto &edit : command.edits) {
            auto prepared = Prefab::PreparePrefabComponentEdit(source.Value(), current, edit, &*m_navigationPrefabSchemas, limits.Value());
            if (prepared.HasError()) {
                reject(prepared.ErrorValue());
                return;
            }
            if (!prepared.Value().effective || prepared.Value().rebased.HasUnresolved()) {
                reject(MakeError(Prefab::PrefabErrors::OverrideSourceStale));
                return;
            }
            current = std::move(prepared).Value().rebased;
        }
        auto candidate = m_document.Snapshot();
        const auto placement = std::ranges::find(candidate.prefabInstances, command.instance, &ScenePrefabInstance::instanceId);
        if (placement == candidate.prefabInstances.end()) {
            reject(MakeError(Prefab::PrefabErrors::OverrideOrphan));
            return;
        }
        placement->valueOverrides = std::move(current);
        auto valid = ConvertSceneDocumentToRuntime(candidate, m_previewSceneId, fresh.sources.resolver, limits.Value(),
                                                   &*m_navigationPrefabSchemas, m_triggerBinding);
        if (valid.HasError()) {
            reject(valid.ErrorValue());
            return;
        }
        std::vector<SceneSource::SceneObjectSnapshot> graph;
        for (const auto &entity : valid.Value().Entities()) {
            SceneSource::SceneObjectSnapshot projected{.id = {entity.object.value}, .name = "Navigation validation"};
            projected.components.navigationSurface = entity.components.navigationSurface;
            projected.components.navigationRegion = entity.components.navigationRegion;
            projected.components.navigationModifier = entity.components.navigationModifier;
            projected.components.navigationLink = entity.components.navigationLink;
            projected.components.navigationAgent = entity.components.navigationAgent;
            graph.push_back(std::move(projected));
        }
        if (auto descriptors = ValidateNavigationInspectorDescriptors(graph, m_viewModel.navigationDefinitions); descriptors.HasError()) {
            reject(descriptors.ErrorValue());
            return;
        }
        SceneDocumentPrefabValueCommands executor{m_document, m_history};
        auto committed =
            executor.Execute(command, fresh.sources.resolver, &*m_navigationPrefabSchemas, limits.Value(), request.context.cancellation);
        if (committed.HasError()) {
            reject(committed.ErrorValue());
            return;
        }
        m_prefabSources.emplace(std::move(fresh.sources));
        m_viewModel.navigationEditError.clear();
        HandleDocumentCommandResult(std::move(committed), "Update Prefab navigation components");
    }
}  // namespace Horo::Editor
