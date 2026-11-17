#include "Horo/Prefab/PreparedPrefabSpawn.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Prefab {
    namespace {
        /** @brief Samples explicit root intent before canonical redundant-record elimination, preserving its runtime placement precedence.
         */
        bool RootIntent(const CookedPrefabData &source, const PrefabValueOverrideSet &set) {
            const auto &root = source.entities.front().provenance.sourceObject;
            const auto addressesRoot = [&root](const PrefabValueOverrideRecord &record) {
                const auto *target = std::get_if<PrefabObjectAddress>(&record.target);
                return target && *target == root;
            };
            return std::ranges::any_of(set.records, addressesRoot) || std::ranges::any_of(set.conflicts, [&](const auto &value) {
                return addressesRoot(value.original);
            }) || std::ranges::any_of(set.orphans, [&](const auto &value) {
                return addressesRoot(value.original);
            });
        }

        /** @brief Matches an exact stable member occurrence instead of relying on array position after topology edits. */
        bool SameMember(const CookedPrefabMember &a, const CookedPrefabMember &b) {
            if (a.index() != b.index())
                return false;
            if (const auto *raw = std::get_if<RawComponentPayload>(&a)) {
                const auto &other = std::get<RawComponentPayload>(b);
                return raw->instance == other.instance && raw->component.typeId == other.component.typeId;
            }
            const auto &first = std::get<Gameplay::BehaviorComponent>(a);
            const auto &second = std::get<Gameplay::BehaviorComponent>(b);
            return first.instanceId == second.instanceId && first.typeId == second.typeId;
        }

        /** @brief Keeps a removed referenced member as a typed incompatibility; it can never retarget a neighboring slot. */
        Result<void> Remap(CookedPrefabMemberSlot &slot, const CookedPrefabData &source, const CookedPrefabData &output) {
            if (slot.entity.value >= source.entities.size() || slot.member >= source.entities[slot.entity.value].members.size())
                return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
            const auto &before = source.entities[slot.entity.value].members[slot.member];
            const auto &after = output.entities[slot.entity.value].members;
            const auto found = std::ranges::find_if(after, [&before](const auto &member) {
                return SameMember(before, member);
            });
            if (found == after.end())
                return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid,
                                                       "A saved prefab delta removes a required reference or initialization member."));
            slot.member = static_cast<std::uint32_t>(found - after.begin());
            return Result<void>::Success();
        }

        /** @brief Rebinds every exact member reference/initialization through stable identities after component topology changes. */
        Result<void> RemapInterfaces(const CookedPrefabData &source, CookedPrefabData &output) {
            for (auto &reference : output.references) {
                if (auto valid = Remap(reference.owner, source, output); valid.HasError())
                    return valid;
                if (auto *target = std::get_if<CookedPrefabMemberSlot>(&reference.target))
                    if (auto valid = Remap(*target, source, output); valid.HasError())
                        return valid;
            }
            for (auto &initialization : output.initialization)
                if (auto valid = Remap(initialization.owner, source, output); valid.HasError())
                    return valid;
            return Result<void>::Success();
        }

        /** @brief Replaces only the complete canonical core's descriptor values while preserving original creation provenance. */
        Result<CookedPrefabData> Descriptors(const CookedPrefabData &source, const std::vector<ResolvedPrefabObject> &objects) {
            if (objects.size() != source.entities.size())
                return Result<CookedPrefabData>::Failure(MakeError(PrefabErrors::IdentityCollision));
            auto output = source;
            for (std::size_t index = 0; index < objects.size(); ++index) {
                const auto &object = objects[index];
                auto &entity = output.entities[index];
                if (object.key.object != entity.provenance.sourceObject || object.sourcePrefab != entity.provenance.sourceAsset)
                    return Result<CookedPrefabData>::Failure(MakeError(PrefabErrors::IdentityCollision));
                entity.localTransform = object.effectiveLocalTransform;
                entity.members.clear();
                for (const auto &component : object.object.components)
                    entity.members.emplace_back(component);
                for (const auto &behavior : object.object.behaviors)
                    entity.members.emplace_back(behavior);
            }
            if (auto valid = RemapInterfaces(source, output); valid.HasError())
                return Result<CookedPrefabData>::Failure(valid.ErrorValue());
            return Result<CookedPrefabData>::Success(std::move(output));
        }

        /** @brief Converts only an actually resolved whole delta into private projected descriptor data. */
        Result<std::shared_ptr<const CookedPrefabData>> PrepareData(const PrefabTemplateLease &source,
                                                                    const PrefabValueOverrideSet &overrides,
                                                                    const PrefabValueSchemaSnapshot *schemas,
                                                                    const PrefabLimitProfile &limits,
                                                                    const CancellationToken &cancellation) {
            using Outcome = Result<std::shared_ptr<const CookedPrefabData>>;
            auto resolved = ResolveCookedPrefabOverrides(*source.Template(), overrides, schemas, limits, cancellation);
            if (resolved.HasError())
                return Outcome::Failure(resolved.ErrorValue());
            if (!resolved.Value().effective)
                return Outcome::Failure(
                    MakeError(PrefabErrors::OverrideConflict,
                              "Saved prefab override intent requires explicit compatibility resolution before loading."));
            auto descriptors = Descriptors(source.Template()->Data(), *resolved.Value().effective);
            if (descriptors.HasError())
                return Outcome::Failure(descriptors.ErrorValue());
            return Outcome::Success(std::make_shared<const CookedPrefabData>(std::move(descriptors).Value()));
        }
    }  // namespace

    /** @copydoc PreparedPrefabSpawn::PreparedPrefabSpawn */
    PreparedPrefabSpawn::PreparedPrefabSpawn(PrefabTemplateLease source, std::shared_ptr<const CookedPrefabData> data,
                                             bool overridesRoot) noexcept
        : source_(std::move(source)), data_(std::move(data)), overridesRoot_(overridesRoot) {}

    /** @copydoc PreparedPrefabSpawn::Prepare */
    Result<PreparedPrefabSpawn> PreparedPrefabSpawn::Prepare(PrefabTemplateLease source,
                                                             const std::optional<PrefabValueOverrideSet> &overrides,
                                                             const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits,
                                                             const CancellationToken &cancellation) {
        if (!source.Template())
            return Result<PreparedPrefabSpawn>::Failure(MakeError(PrefabErrors::AssetNotLoaded));
        if (cancellation.IsCancellationRequested())
            return Result<PreparedPrefabSpawn>::Failure(MakeError(PrefabErrors::Cancelled));
        if (!overrides)
            return Result<PreparedPrefabSpawn>::Success(PreparedPrefabSpawn{std::move(source)});
        try {
            auto data = PrepareData(source, *overrides, schemas, limits, cancellation);
            if (data.HasError())
                return Result<PreparedPrefabSpawn>::Failure(data.ErrorValue());
            const bool root = RootIntent(source.Template()->Data(), *overrides);
            return Result<PreparedPrefabSpawn>::Success(PreparedPrefabSpawn{std::move(source), std::move(data).Value(), root});
        } catch (const std::bad_alloc &) {
            return Result<PreparedPrefabSpawn>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PreparedPrefabSpawn::Source */
    const PrefabTemplateLease &PreparedPrefabSpawn::Source() const noexcept {
        return source_;
    }

    /** @copydoc PreparedPrefabSpawn::Data */
    const CookedPrefabData &PreparedPrefabSpawn::Data() const noexcept {
        return data_ ? *data_ : source_.Template()->Data();
    }

    /** @copydoc PreparedPrefabSpawn::OverridesRootTransform */
    bool PreparedPrefabSpawn::OverridesRootTransform() const noexcept {
        return overridesRoot_;
    }
}  // namespace Horo::Prefab
