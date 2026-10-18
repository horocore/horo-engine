#include "AssetRegistrySnapshotState.h"

#include <algorithm>
#include <memory>

namespace Horo::Assets {
    /** @copydoc AssetRegistrySnapshot::Revision */
    AssetRegistryRevision AssetRegistrySnapshot::Revision() const noexcept {
        return state_ ? state_->revision : AssetRegistryRevision{};
    }

    /** @copydoc AssetRegistrySnapshot::Records */
    std::span<const AssetRecord> AssetRegistrySnapshot::Records() const noexcept {
        return state_ ? std::span<const AssetRecord>{state_->records} : std::span<const AssetRecord>{};
    }

    /** @copydoc AssetRegistrySnapshot::Find */
    const AssetRecord *AssetRegistrySnapshot::Find(const AssetId id) const noexcept {
        if (!state_)
            return nullptr;
        const auto found = std::ranges::lower_bound(state_->records, id, {}, &AssetRecord::id);
        return found != state_->records.end() && found->id == id ? std::to_address(found) : nullptr;
    }

    /** @copydoc AssetRegistrySnapshot::FindByPath */
    const AssetRecord *AssetRegistrySnapshot::FindByPath(const std::string_view normalizedProjectPath) const noexcept {
        if (!state_)
            return nullptr;
        const auto found = std::ranges::find(state_->records, normalizedProjectPath, [](const AssetRecord &record) -> std::string_view {
            return record.sourcePath.String();
        });
        return found == state_->records.end() ? nullptr : std::to_address(found);
    }

}  // namespace Horo::Assets
