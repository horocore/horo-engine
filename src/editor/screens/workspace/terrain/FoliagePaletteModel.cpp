#include "FoliagePaletteModel.h"

#include <algorithm>
#include <set>

namespace Horo::Editor {
    namespace {
        /** @brief Validate an exact asset/type/revision pair without manufacturing a definition identity. */
        bool Matches(const FoliagePaletteItem &item) {
            const auto &data = item.definition.Data();
            if (!item.source.asset.IsValid() || item.source.type != data.type || item.source.revision != data.revision)
                return false;
            return !item.label.empty() && item.label.size() <= 256;
        }

        /** @brief Bound and reject ambiguous assets/types before palette replacement. */
        Result<void> Validate(std::span<const FoliagePaletteItem> items) {
            if (items.size() > 256)
                return Result<void>::Failure(MakeError(TerrainEditErrors::LimitExceeded));
            std::set<Terrain::FoliageTypeId> types;
            std::set<Assets::AssetId> assets;
            for (const auto &item : items) {
                if (!Matches(item) || !types.insert(item.source.type).second || !assets.insert(item.source.asset).second)
                    return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc FoliagePaletteModel::Replace */
    Result<void> FoliagePaletteModel::Replace(std::vector<FoliagePaletteItem> items) {
        if (closed_)
            return Result<void>::Failure(MakeError(TerrainEditErrors::Closed));
        if (auto valid = Validate(items); valid.HasError())
            return valid;
        std::ranges::sort(items, [](const auto &a, const auto &b) {
            return a.source.type < b.source.type;
        });
        items_ = std::move(items);
        if (!Active())
            selected_.reset();
        return Result<void>::Success();
    }

    /** @copydoc FoliagePaletteModel::Choose */
    Result<void> FoliagePaletteModel::Choose(Terrain::FoliageTypeId type) {
        if (closed_)
            return Result<void>::Failure(MakeError(TerrainEditErrors::Closed));
        const auto found = std::ranges::find_if(items_, [type](const auto &item) {
            return item.source.type == type;
        });
        if (found == items_.end())
            return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
        selected_ = type;
        return Result<void>::Success();
    }

    /** @copydoc FoliagePaletteModel::Active */
    const FoliagePaletteItem *FoliagePaletteModel::Active() const noexcept {
        if (!selected_)
            return nullptr;
        const auto found = std::ranges::find_if(items_, [this](const auto &item) {
            return item.source.type == *selected_;
        });
        return found == items_.end() ? nullptr : &*found;
    }

    /** @copydoc FoliagePaletteModel::Close */
    void FoliagePaletteModel::Close() noexcept {
        closed_ = true;
        selected_.reset();
        items_.clear();
    }
}  // namespace Horo::Editor
