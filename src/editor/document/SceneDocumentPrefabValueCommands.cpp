#include "Horo/Prefab/PrefabErrors.h"
#include "editor/document/PrefabOverrideScope.h"
#include "editor/document/SceneDocumentInternal.h"

#include <new>

namespace Horo::Editor {
    using namespace SceneDocumentDetail;
    static_assert(std::is_nothrow_move_constructible_v<HistoryRecord>);
    static_assert(std::is_nothrow_swappable_v<std::optional<Prefab::PrefabValueOverrideSet>>);

    /** @copydoc MatchesPrefabOverrideScope(const ScenePrefabRevertTarget&,const Prefab::PrefabValueOverrideRecord&) */
    bool MatchesPrefabOverrideScope(const ScenePrefabRevertTarget &selection, const Prefab::PrefabValueOverrideRecord &record) {
        if (const auto *value = std::get_if<Prefab::PrefabValueOverrideTarget>(&selection))
            return record.target == *value;
        const auto *property = std::get_if<Prefab::PrefabPropertyAddress>(&record.target);
        if (const auto *component = std::get_if<Prefab::PrefabComponentOverrideTarget>(&selection))
            return property && property->Object() == component->object && property->ComponentType() == component->type &&
                   property->ComponentInstance() == component->instance;
        if (const auto *object = std::get_if<Prefab::PrefabObjectAddress>(&selection))
            return *object == (property ? property->Object() : std::get<Prefab::PrefabObjectAddress>(record.target));
        return std::holds_alternative<AllScenePrefabOverrides>(selection);
    }

    /** @copydoc MatchesPrefabOverrideScope(const ScenePrefabRevertTarget&,const Prefab::PrefabComponentOverrideRecord&) */
    bool MatchesPrefabOverrideScope(const ScenePrefabRevertTarget &selection, const Prefab::PrefabComponentOverrideRecord &record) {
        const auto *add = std::get_if<Prefab::PrefabAddComponentOverride>(&record);
        const auto *remove = std::get_if<Prefab::PrefabRemoveComponentOverride>(&record);
        const auto &object = add ? add->object : remove->target.object;
        if (const auto *component = std::get_if<Prefab::PrefabComponentOverrideTarget>(&selection))
            return object == component->object && component->type == (add ? add->payload.component.typeId : remove->target.type) &&
                   component->instance == (add ? add->payload.instance : remove->target.instance);
        if (const auto *target = std::get_if<Prefab::PrefabObjectAddress>(&selection))
            return object == *target;
        return std::holds_alternative<AllScenePrefabOverrides>(selection);
    }

    /** @copydoc ErasePrefabOverrideScope */
    void ErasePrefabOverrideScope(Prefab::PrefabValueOverrideSet &set, const ScenePrefabRevertTarget &selection) {
        const auto erase = [&selection](auto &records) {
            std::erase_if(records, [&selection](const auto &record) {
                if constexpr (requires { record.original; })
                    return MatchesPrefabOverrideScope(selection, record.original);
                else
                    return MatchesPrefabOverrideScope(selection, record);
            });
        };
        erase(set.records);
        erase(set.conflicts);
        erase(set.orphans);
        erase(set.componentRecords);
        erase(set.componentConflicts);
        erase(set.componentOrphans);
    }

    namespace {
        /** @brief Stages explicit removal and verifies that the remaining persisted envelope is structurally admissible. */
        Result<Prefab::PrefabValueOverrideResolution> PrepareInstanceRevert(const ScenePrefabInstance &instance,
                                                                            const RevertScenePrefabOverridesCommand &command,
                                                                            const Prefab::EffectivePrefabCandidate &source,
                                                                            const Prefab::PrefabValueSchemaSnapshot *schemas,
                                                                            const Prefab::PrefabLimitProfile &limits,
                                                                            const CancellationToken &cancellation) {
            using Return = Result<Prefab::PrefabValueOverrideResolution>;
            if (command.target.valueless_by_exception())
                return Return::Failure(MakeError(Prefab::PrefabErrors::AddressInvalid));
            if (const auto *component = std::get_if<Prefab::PrefabComponentOverrideTarget>(&command.target);
                component && (!component->instance.IsValid() || component->type.Value().empty()))
                return Return::Failure(MakeError(Prefab::PrefabErrors::AddressInvalid));
            Prefab::PrefabValueOverrideResolution prepared;
            if (instance.valueOverrides) {
                prepared.rebased = *instance.valueOverrides;
                ErasePrefabOverrideScope(prepared.rebased, command.target);
                if (!prepared.rebased.IsEmpty() && prepared.rebased != *instance.valueOverrides)
                    if (auto valid = Prefab::ResolvePrefabValueOverrides(source, prepared.rebased, schemas, limits, cancellation);
                        valid.HasError())
                        return Return::Failure(std::move(valid).ErrorValue());
            }
            return Return::Success(std::move(prepared));
        }

        /** @brief Capture the complete semantic before/after value state and its bounded ownership before publication. */
        SceneCommandDelta CaptureValueDelta(const ScenePrefabInstance &instance, const std::optional<Prefab::PrefabValueOverrideSet> &after,
                                            const std::uint64_t epoch) {
            PrefabInstanceValuesDelta values{instance.instanceId, instance.valueOverrides, after};
            values.ownedBytes = EstimateOverrideMemoryBytes(values.before) + EstimateOverrideMemoryBytes(values.after);
            values.replacementEpoch = epoch;
            return SceneCommandDelta{std::move(values)};
        }

        /** @brief Prepare a detached instance edit only after the exact schema and complete source publication match. */
        template <class Command>
        Result<Prefab::PrefabValueOverrideResolution> PrepareInstanceValues(const ScenePrefabInstance &instance, const Command &command,
                                                                            const Prefab::PrefabSourceResolverSnapshot &sources,
                                                                            const Prefab::PrefabValueSchemaSnapshot *schemas,
                                                                            const Prefab::PrefabLimitProfile &limits,
                                                                            const CancellationToken &cancellation) {
            if (command.expectedSchema != (schemas ? schemas->Revision() : Sha256Digest{}))
                return Result<Prefab::PrefabValueOverrideResolution>::Failure(
                    MakeError(Prefab::PrefabErrors::OverrideSourceStale, "Captured property schema changed."));
            auto source = sources.Resolve(instance.sourcePrefab.Asset(), command.instance, limits, cancellation);
            if (source.HasError())
                return Result<Prefab::PrefabValueOverrideResolution>::Failure(source.ErrorValue());
            if (source.Value().Revision() != command.expectedSource)
                return Result<Prefab::PrefabValueOverrideResolution>::Failure(
                    MakeError(Prefab::PrefabErrors::OverrideSourceStale, "Prefab source changed before override preparation."));
            if constexpr (std::is_same_v<Command, RevertScenePrefabOverridesCommand>)
                return PrepareInstanceRevert(instance, command, source.Value(), schemas, limits, cancellation);
            else if constexpr (std::is_same_v<Command, EditScenePrefabComponentsCommand>) {
                if (command.edits.empty() || command.edits.size() > 1024)
                    return Result<Prefab::PrefabValueOverrideResolution>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
                auto current = instance.valueOverrides;
                std::optional<Prefab::PrefabValueOverrideResolution> result;
                for (const auto &edit : command.edits) {
                    auto prepared = Prefab::PreparePrefabComponentEdit(source.Value(), current, edit, schemas, limits, cancellation);
                    if (prepared.HasError())
                        return prepared;
                    if (!prepared.Value().effective || prepared.Value().rebased.HasUnresolved())
                        return Result<Prefab::PrefabValueOverrideResolution>::Failure(MakeError(Prefab::PrefabErrors::OverrideSourceStale));
                    result.emplace(std::move(prepared).Value());
                    current = result->rebased;
                }
                return Result<Prefab::PrefabValueOverrideResolution>::Success(std::move(*result));
            } else if constexpr (std::is_same_v<Command, EditScenePrefabValueCommand>)
                return Prefab::PreparePrefabValueEdit(source.Value(), instance.valueOverrides, command.edit, schemas, limits, cancellation);
            else
                return Prefab::PreparePrefabComponentEdit(source.Value(), instance.valueOverrides, command.edit, schemas, limits,
                                                          cancellation);
        }
    }  // namespace

    /** @copydoc SceneDocumentPrefabValueCommands::SceneDocumentPrefabValueCommands */
    SceneDocumentPrefabValueCommands::SceneDocumentPrefabValueCommands(SceneDocument &document, EditorHistory &history) noexcept
        : m_document(document), m_history(history) {}

    /** @copydoc SceneDocumentPrefabValueCommands::ExecuteEdit */
    template <class Command>
    Result<SceneCommandResult> SceneDocumentPrefabValueCommands::ExecuteEdit(const Command &command,
                                                                             const Prefab::PrefabSourceResolverSnapshot &sources,
                                                                             const Prefab::PrefabValueSchemaSnapshot *schemas,
                                                                             const Prefab::PrefabLimitProfile &limits,
                                                                             const CancellationToken &cancellation) {
        try {
            if (command.expectedReplacementEpoch != m_document.m_replacementEpoch || command.expectedRevision != m_document.m_revision)
                return Result<SceneCommandResult>::Failure(
                    MakeError(Prefab::PrefabErrors::OverrideSourceStale, "Scene revision changed before override preparation."));
            auto instance = FindEditablePrefabInstance(m_document.m_objects, m_document.m_prefabInstances, command.instance);
            if (instance.HasError())
                return Result<SceneCommandResult>::Failure(instance.ErrorValue());
            auto prepared = PrepareInstanceValues(*instance.Value(), command, sources, schemas, limits, cancellation);
            if (prepared.HasError())
                return Result<SceneCommandResult>::Failure(prepared.ErrorValue());
            auto resolution = std::move(prepared).Value();
            auto &set = resolution.rebased;
            std::optional<Prefab::PrefabValueOverrideSet> after;
            if (!set.IsEmpty())
                after = std::move(set);
            if (after == instance.Value()->valueOverrides)
                return Result<SceneCommandResult>::Success(PrefabNoOpResult(m_document.m_revision, m_document.m_state, command.instance,
                                                                            DocumentChangeKind::PrefabInstanceValuesChanged));
            auto delta = CaptureValueDelta(*instance.Value(), after, m_document.m_replacementEpoch);
            if (auto valid = ValidateHistoryDelta(delta, 1); valid.HasError())
                return Result<SceneCommandResult>::Failure(valid.ErrorValue());
            const auto bytes = EstimateMemoryBytes(delta, 1);
            std::vector affected{command.instance};
            SceneCommandResult result{{},
                                      {m_document.m_revision.value + 1},
                                      {m_document.m_nextStateId},
                                      DocumentChangeKind::PrefabInstanceValuesChanged,
                                      {},
                                      true,
                                      command.instance,
                                      affected};
            HistoryRecord entry{m_document.m_state, result.state, std::move(delta), {}, bytes, std::move(affected)};
            m_history.m_impl->undo.reserve(m_history.m_impl->undo.size() + 1);
            if (cancellation.IsCancellationRequested())
                return Result<SceneCommandResult>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
            // All allocation, validation and cancellation work precedes this owner-thread publication.
            instance.Value()->valueOverrides.swap(after);
            m_document.m_revision = result.revision;
            m_document.m_state = result.state;
            ++m_document.m_nextStateId;
            PushHistory(*m_history.m_impl, std::move(entry));
            return Result<SceneCommandResult>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<SceneCommandResult>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc SceneDocumentPrefabValueCommands::Execute(const EditScenePrefabValueCommand&,const
     * Prefab::PrefabSourceResolverSnapshot&,const Prefab::PrefabValueSchemaSnapshot*,const Prefab::PrefabLimitProfile&,const
     * CancellationToken&) */
    Result<SceneCommandResult> SceneDocumentPrefabValueCommands::Execute(const EditScenePrefabValueCommand &command,
                                                                         const Prefab::PrefabSourceResolverSnapshot &sources,
                                                                         const Prefab::PrefabValueSchemaSnapshot *schemas,
                                                                         const Prefab::PrefabLimitProfile &limits,
                                                                         const CancellationToken &cancellation) {
        return ExecuteEdit(command, sources, schemas, limits, cancellation);
    }

    /** @copydoc SceneDocumentPrefabValueCommands::Execute(const EditScenePrefabComponentCommand&,const
     * Prefab::PrefabSourceResolverSnapshot&,const Prefab::PrefabValueSchemaSnapshot*,const Prefab::PrefabLimitProfile&,const
     * CancellationToken&) */
    Result<SceneCommandResult> SceneDocumentPrefabValueCommands::Execute(const EditScenePrefabComponentCommand &command,
                                                                         const Prefab::PrefabSourceResolverSnapshot &sources,
                                                                         const Prefab::PrefabValueSchemaSnapshot *schemas,
                                                                         const Prefab::PrefabLimitProfile &limits,
                                                                         const CancellationToken &cancellation) {
        return ExecuteEdit(command, sources, schemas, limits, cancellation);
    }

    /** @copydoc SceneDocumentPrefabValueCommands::Execute(const EditScenePrefabComponentsCommand&,const
     * Prefab::PrefabSourceResolverSnapshot&,const Prefab::PrefabValueSchemaSnapshot*,const Prefab::PrefabLimitProfile&,const
     * CancellationToken&) */
    Result<SceneCommandResult> SceneDocumentPrefabValueCommands::Execute(const EditScenePrefabComponentsCommand &command,
                                                                         const Prefab::PrefabSourceResolverSnapshot &sources,
                                                                         const Prefab::PrefabValueSchemaSnapshot *schemas,
                                                                         const Prefab::PrefabLimitProfile &limits,
                                                                         const CancellationToken &cancellation) {
        return ExecuteEdit(command, sources, schemas, limits, cancellation);
    }

    /** @copydoc SceneDocumentPrefabValueCommands::Execute(const RevertScenePrefabOverridesCommand&,const
     * Prefab::PrefabSourceResolverSnapshot&,const Prefab::PrefabValueSchemaSnapshot*,const Prefab::PrefabLimitProfile&,const
     * CancellationToken&) */
    Result<SceneCommandResult> SceneDocumentPrefabValueCommands::Execute(const RevertScenePrefabOverridesCommand &command,
                                                                         const Prefab::PrefabSourceResolverSnapshot &sources,
                                                                         const Prefab::PrefabValueSchemaSnapshot *schemas,
                                                                         const Prefab::PrefabLimitProfile &limits,
                                                                         const CancellationToken &cancellation) {
        return ExecuteEdit(command, sources, schemas, limits, cancellation);
    }

    /** @copydoc SceneDocumentPrefabValueCommands::Replay */
    Result<SceneCommandResult> SceneDocumentPrefabValueCommands::Replay(const bool undo) {
        try {
            auto &from = undo ? m_history.m_impl->undo : m_history.m_impl->redo;
            auto &to = undo ? m_history.m_impl->redo : m_history.m_impl->undo;
            const auto &entry = from.back();
            const auto &delta = std::get<PrefabInstanceValuesDelta>(entry.delta);
            if (delta.replacementEpoch != m_document.m_replacementEpoch)
                return Result<SceneCommandResult>::Failure(
                    MakeError(Prefab::PrefabErrors::OverrideSourceStale, "Prefab value history belongs to a replaced scene opening."));
            const auto instance = FindPrefabInstance(m_document.m_prefabInstances, delta.instance);
            if (instance == m_document.m_prefabInstances.end())
                return Result<SceneCommandResult>::Failure(MakeError(Prefab::PrefabErrors::OverrideOrphan));
            auto prepared = undo ? delta.before : delta.after;
            auto result = HistoryCommandResult(m_document, entry, undo ? DocumentChangeKind::Undone : DocumentChangeKind::Redone);
            result.revision = {m_document.m_revision.value + 1};
            result.state = undo ? entry.beforeState : entry.afterState;
            to.reserve(to.size() + 1);
            // Retained history remains authoritative until the prepared owned state is ready.
            instance->valueOverrides.swap(prepared);
            m_document.m_revision = result.revision;
            m_document.m_state = result.state;
            to.push_back(std::move(from.back()));
            from.pop_back();
            return Result<SceneCommandResult>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<SceneCommandResult>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }
}  // namespace Horo::Editor
