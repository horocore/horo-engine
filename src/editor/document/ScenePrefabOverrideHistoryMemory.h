#pragma once

#include "Horo/Prefab/PrefabValueOverrideTypes.h"

#include <cstddef>
#include <optional>
#include <type_traits>

namespace Horo::Editor::SceneDocumentDetail {
    /** @brief Account for owned payload strings and vector storage retained by semantic history. */
    [[nodiscard]] inline std::size_t EstimateOverrideMemoryBytes(const std::optional<Prefab::PrefabValueOverrideSet> &set) noexcept {
        if (!set)
            return 0;
        const auto recordBytes = [](const Prefab::PrefabValueOverrideRecord &record) {
            std::size_t bytes = std::holds_alternative<std::string>(record.value) ? std::get<std::string>(record.value).size() : 0;
            if (const auto *property = std::get_if<Prefab::PrefabPropertyAddress>(&record.target))
                bytes += property->ComponentType().Value().size();
            return bytes;
        };
        std::size_t bytes = set->records.capacity() * sizeof(Prefab::PrefabValueOverrideRecord) +
                            set->conflicts.capacity() * sizeof(Prefab::PrefabValueOverrideConflict) +
                            set->orphans.capacity() * sizeof(Prefab::PrefabValueOverrideOrphan);
        for (const auto &record : set->records)
            bytes += recordBytes(record);
        bytes += set->authoredAgainst.projectVersion.prerelease.size();
        for (const auto &conflict : set->conflicts)
            bytes += recordBytes(conflict.original) + conflict.authoredAgainst.projectVersion.prerelease.size() +
                     conflict.observedSource.projectVersion.prerelease.size();
        for (const auto &orphan : set->orphans)
            bytes += recordBytes(orphan.original) + orphan.authoredAgainst.projectVersion.prerelease.size();
        const auto componentBytes = [](const Prefab::PrefabComponentOverrideRecord &record) {
            return std::visit([]<typename Value>(const Value &value) -> std::size_t {
                if constexpr (std::is_same_v<Value, Prefab::PrefabAddComponentOverride>)
                    return value.payload.component.typeId.Value().size() + value.payload.component.payload.capacity();
                else
                    return value.target.type.Value().size();
            }, record);
        };
        bytes += set->componentRecords.capacity() * sizeof(Prefab::PrefabComponentOverrideRecord) +
                 set->componentConflicts.capacity() * sizeof(Prefab::PrefabComponentOverrideConflict) +
                 set->componentOrphans.capacity() * sizeof(Prefab::PrefabComponentOverrideOrphan);
        for (const auto &record : set->componentRecords)
            bytes += componentBytes(record);
        for (const auto &conflict : set->componentConflicts)
            bytes += componentBytes(conflict.original) + conflict.authoredAgainst.projectVersion.prerelease.size() +
                     conflict.observedSource.projectVersion.prerelease.size();
        for (const auto &orphan : set->componentOrphans)
            bytes += componentBytes(orphan.original) + orphan.authoredAgainst.projectVersion.prerelease.size();
        return bytes;
    }

}  // namespace Horo::Editor::SceneDocumentDetail
