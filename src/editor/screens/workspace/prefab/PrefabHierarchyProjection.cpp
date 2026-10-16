#include "editor/screens/workspace/prefab/PrefabHierarchyProjection.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <new>

namespace Horo::Editor {
    namespace {
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
            if (!placement.expanded)
                return;
            for (const auto &object : placement.expanded->Objects()) {
                const PrefabHierarchyTarget target{placement.authored.instanceId, object.key};
                const PrefabHierarchyTarget parent =
                    object.parent ? PrefabHierarchyTarget{placement.authored.instanceId, *object.parent} : root;
                rows.emplace_back(target, parent, object.sourcePrefab, object.object.name, object.sourcePrefab.ToString(), overridden,
                                  failure, identities ? identities->Find(object.key) : std::nullopt);
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
