#include "editor/screens/workspace/prefab/PrefabHierarchyProjection.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <format>
#include <new>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        /** @brief Formats a bounded read-only value once during projection preparation, never during Draw. */
        std::string ValueLabel(const Prefab::PrefabOverrideValue &value) {
            return std::visit([](const auto &item) -> std::string {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    if (item.size() <= 128)
                        return item;
                    std::size_t end = 128;
                    while (end > 0 && (static_cast<unsigned char>(item[end]) & 0xc0U) == 0x80U)
                        --end;
                    return item.substr(0, end) + "…";
                } else if constexpr (std::is_same_v<T, bool>)
                    return item ? "true" : "false";
                else if constexpr (std::is_arithmetic_v<T>)
                    return std::format("{}", item);
                else if constexpr (std::is_same_v<T, Math::Vec2>)
                    return std::format("({}, {})", item.x, item.y);
                else if constexpr (std::is_same_v<T, Math::Vec3>)
                    return std::format("({}, {}, {})", item.x, item.y, item.z);
                else if constexpr (std::is_same_v<T, Math::Quaternion>)
                    return std::format("({}, {}, {}, {})", item.x, item.y, item.z, item.w);
                else
                    return std::format("T({}, {}, {}) R({}, {}, {}, {}) S({}, {}, {})", item.translation.x, item.translation.y,
                                       item.translation.z, item.rotation.x, item.rotation.y, item.rotation.z, item.rotation.w, item.scale.x,
                                       item.scale.y, item.scale.z);
            }, value);
        }

        /** @brief Uses stable addresses rather than mutable names for the diff target. */
        std::string TargetLabel(const Prefab::PrefabValueOverrideTarget &target) {
            const auto &object = std::holds_alternative<Prefab::PrefabObjectAddress>(target)
                                     ? std::get<Prefab::PrefabObjectAddress>(target)
                                     : std::get<Prefab::PrefabPropertyAddress>(target).Object();
            std::string scope;
            for (const auto placement : object.NestedInstanceScope())
                scope += std::format("{}/", placement.value);
            if (const auto *property = std::get_if<Prefab::PrefabPropertyAddress>(&target))
                return std::format("{}{} / {} / {} / {}", scope, object.SourceObject().value, property->ComponentType().Value(),
                                   property->ComponentInstance().Value(), property->Property().Value());
            return std::format("{}{} / TRS", scope, object.SourceObject().value);
        }

        /** @brief Selects a closed presentation key from the authoritative value origin. */
        const char *OriginKey(const Prefab::PrefabValueOrigin origin) {
            switch (origin) {
                case Prefab::PrefabValueOrigin::Source:
                    return "workspace.prefab.diff.origin.source";
                case Prefab::PrefabValueOrigin::Variant:
                    return "workspace.prefab.diff.origin.variant";
                case Prefab::PrefabValueOrigin::Nested:
                    return "workspace.prefab.diff.origin.nested";
                case Prefab::PrefabValueOrigin::Instance:
                    return "workspace.prefab.diff.origin.instance";
                case Prefab::PrefabValueOrigin::Inherited:
                    return "workspace.prefab.diff.unavailable";
            }
            return "workspace.prefab.diff.unavailable";
        }

        /** @brief Copies bounded immutable values for the selected occurrence; panels never read documents or interpret payloads. */
        void CaptureValues(PrefabHierarchyEntry &row, const SceneSource::ScenePrefabInstanceProjection &placement) {
            row.valueInspectionFailure = placement.valueInspectionFailure;
            if (!placement.valueSnapshot)
                return;
            const auto &snapshot = *placement.valueSnapshot;
            row.valueDiffIncomplete = !snapshot.Coverage().Complete();
            const auto matches = [&](const Prefab::PrefabValueOverrideTarget &target) {
                const auto &object = std::holds_alternative<Prefab::PrefabObjectAddress>(target)
                                         ? std::get<Prefab::PrefabObjectAddress>(target)
                                         : std::get<Prefab::PrefabPropertyAddress>(target).Object();
                return !row.target.member || row.target.member->object == object;
            };
            for (const auto &value : snapshot.Values())
                if (matches(value.target)) {
                    row.valueDiff.push_back({TargetLabel(value.target), ValueLabel(value.sourceValue), ValueLabel(value.effectiveValue),
                                             value.schemaDefault ? std::optional{ValueLabel(*value.schemaDefault)} : std::nullopt,
                                             OriginKey(value.origin), nullptr, value.sourceValue != value.effectiveValue});
                    if (value.disposition == Prefab::PrefabValueDisposition::RemovedByInstance) {
                        row.valueDiff.back().stateKey = "workspace.prefab.diff.component_removed";
                        row.valueDiff.back().effective.clear();
                        row.valueDiff.back().effectivePresent = false;
                        row.valueDiff.back().changed = true;
                    }
                }
            for (const auto &conflict : snapshot.Conflicts())
                if (matches(conflict.original.target))
                    row.valueDiff.push_back({TargetLabel(conflict.original.target),
                                             {},
                                             ValueLabel(conflict.original.value),
                                             {},
                                             "workspace.prefab.diff.origin.instance",
                                             "workspace.prefab.diff.conflict",
                                             true,
                                             false,
                                             true});
            for (const auto &orphan : snapshot.Orphans())
                if (matches(orphan.original.target))
                    row.valueDiff.push_back({TargetLabel(orphan.original.target),
                                             {},
                                             ValueLabel(orphan.original.value),
                                             {},
                                             "workspace.prefab.diff.origin.instance",
                                             "workspace.prefab.diff.orphan",
                                             true,
                                             false,
                                             true});
            for (const auto &evidence : snapshot.Components())
                std::visit([&](const auto &item) {
                    using T = std::decay_t<decltype(item)>;
                    const auto &record = [&]() -> const Prefab::PrefabComponentOverrideRecord & {
                        if constexpr (std::is_same_v<T, Prefab::PrefabComponentOverrideRecord>)
                            return item;
                        else
                            return item.original;
                    }();
                    const auto *add = std::get_if<Prefab::PrefabAddComponentOverride>(&record);
                    const auto &object = add ? add->object : std::get<Prefab::PrefabRemoveComponentOverride>(record).target.object;
                    if (row.target.member && row.target.member->object != object)
                        return;
                    const char *state = add ? "workspace.prefab.diff.component_added" : "workspace.prefab.diff.component_removed";
                    if constexpr (std::is_same_v<T, Prefab::PrefabComponentOverrideConflict>)
                        state = "workspace.prefab.diff.conflict";
                    if constexpr (std::is_same_v<T, Prefab::PrefabComponentOverrideOrphan>)
                        state = "workspace.prefab.diff.orphan";
                    const auto &type =
                        add ? add->payload.component.typeId : std::get<Prefab::PrefabRemoveComponentOverride>(record).target.type;
                    const auto instance =
                        add ? add->payload.instance : std::get<Prefab::PrefabRemoveComponentOverride>(record).target.instance;
                    row.valueDiff.push_back({std::format("{} / {}", type.Value(), instance.Value()),
                                             {},
                                             {},
                                             {},
                                             "workspace.prefab.diff.origin.instance",
                                             state,
                                             true,
                                             false,
                                             false});
                }, evidence);
            for (const auto &opaque : snapshot.Opaque())
                if (!row.target.member || row.target.member == opaque.object)
                    row.valueDiff.push_back(
                        {opaque.type.Value(), {}, {}, {}, nullptr, "workspace.prefab.diff.unavailable", false, false, false});
        }

        /** @brief Appends one placement and its exact source members to an already bounded detached candidate. */
        void AppendPlacement(std::vector<PrefabHierarchyEntry> &rows, const SceneSource::ScenePrefabInstanceProjection &placement,
                             const Prefab::PrefabSceneIdentityMap *identities, const std::optional<Error> &failure) {
            const PrefabHierarchyTarget root{placement.authored.instanceId, std::nullopt};
            const bool overridden = placement.authored.valueOverrides.has_value();
            rows.emplace_back(root, std::nullopt, placement.authored.sourcePrefab.Asset(),
                              placement.expanded && !placement.expanded->Objects().empty()
                                  ? placement.expanded->Objects().front().object.name
                                  : placement.authored.sourcePrefab.Asset().ToString(),
                              placement.authored.sourcePrefab.Asset().ToString(), overridden, failure,
                              identities && placement.expanded && !placement.expanded->Objects().empty()
                                  ? identities->Find(placement.expanded->Objects().front().key)
                                  : std::nullopt);
            CaptureValues(rows.back(), placement);
            if (!placement.expanded)
                return;
            for (const auto &object : placement.expanded->Objects()) {
                const PrefabHierarchyTarget target{placement.authored.instanceId, object.key};
                const PrefabHierarchyTarget parent =
                    object.parent ? PrefabHierarchyTarget{placement.authored.instanceId, *object.parent} : root;
                rows.emplace_back(target, parent, object.sourcePrefab, object.object.name, object.sourcePrefab.ToString(), overridden,
                                  failure, identities ? identities->Find(object.key) : std::nullopt);
                CaptureValues(rows.back(), placement);
            }
        }

        /** @brief Reuses the canonical scene remapper across placements before attaching optional viewport identities. */
        void PopulateRows(PrefabHierarchyProjection &prepared, const std::span<const SceneSource::SceneObjectSnapshot> authored,
                          const Prefab::PrefabLimitProfile &limits, const std::size_t count) {
            std::vector<Prefab::PrefabSceneObjectId> occupied;
            occupied.reserve(authored.size() + count);
            for (const auto &object : authored)
                occupied.push_back({object.id.value});
            for (const auto &placement : prepared.sources->instances) {
                if (!placement.expanded) {
                    AppendPlacement(prepared.entries, placement, nullptr, placement.failure);
                    continue;
                }
                auto identities = Prefab::RemapPrefabCandidateToScene(*placement.expanded, occupied, {}, limits);
                if (identities.HasError()) {
                    AppendPlacement(prepared.entries, placement, nullptr, identities.ErrorValue());
                    continue;
                }
                AppendPlacement(prepared.entries, placement, &identities.Value(), placement.failure);
                for (const auto &mapping : identities.Value().Mappings())
                    occupied.push_back(mapping.scene);
            }
        }
    }  // namespace

    /** @copydoc PrefabHierarchyProjection::Find */
    const PrefabHierarchyEntry *PrefabHierarchyProjection::Find(const PrefabHierarchyTarget &target) const noexcept {
        const auto found = std::ranges::find(entries, target, &PrefabHierarchyEntry::target);
        return found == entries.end() ? nullptr : std::to_address(found);
    }

    /** @copydoc BuildPrefabHierarchyProjection */
    Result<PrefabHierarchyProjection> BuildPrefabHierarchyProjection(const Assets::AssetId session, const std::uint64_t publication,
                                                                     const std::uint64_t revision,
                                                                     std::shared_ptr<const SceneSource::ScenePrefabProjection> sources,
                                                                     const std::size_t maximumRows,
                                                                     const std::span<const SceneSource::SceneObjectSnapshot> authored,
                                                                     const Prefab::PrefabLimitProfile &limits) {
        if (!session.IsValid() || publication == 0 || !sources || maximumRows == 0)
            return Result<PrefabHierarchyProjection>::Failure(MakeError(Prefab::PrefabErrors::DocumentInvalid));
        std::size_t count{};
        for (const auto &placement : sources->instances) {
            const std::size_t members = placement.expanded ? placement.expanded->Objects().size() : 0;
            if (count >= maximumRows || members > maximumRows - count - 1)
                return Result<PrefabHierarchyProjection>::Failure(MakeError(Prefab::PrefabErrors::ObjectCountExceeded));
            count += members + 1;
        }
        try {
            PrefabHierarchyProjection prepared{session, publication, revision, std::move(sources), {}};
            prepared.entries.reserve(count);
            PopulateRows(prepared, authored, limits, count);
            return Result<PrefabHierarchyProjection>::Success(std::move(prepared));
        } catch (const std::bad_alloc &) {
            return Result<PrefabHierarchyProjection>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }
}  // namespace Horo::Editor
