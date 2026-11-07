#pragma once
/** @file FoliagePaletteModel.h @brief Private owner-lane palette over actual captured foliage type assets. */
#include "Horo/Editor/TerrainAuthoringDocument.h"
#include "Horo/Terrain/FoliageAuthoringSource.h"

#include <span>
#include <string>

namespace Horo::Editor {
    /** @brief Host-captured source binding and canonically validated definition; neither owns a runtime instance. */
    struct FoliagePaletteItem final {
        Terrain::FoliageAuthoringDefinitionReference source;
        Terrain::FoliageTypeDefinition definition;
        std::string label;
    };

    /** @brief Transient owner-thread palette. Source assets remain the sole definition authority. */
    class FoliagePaletteModel final {
    public:
        /** @brief Atomically replace a bounded complete captured catalogue; a missing active type becomes unselected.
         * @param items Actual host-decoded source assets, with captured source digests and validated dependencies.
         * @return Success or typed invalid/closed/limit failure; prior catalogue survives rejection. */
        [[nodiscard]] Result<void> Replace(std::vector<FoliagePaletteItem> items);
        /** @brief Explicitly select one retained type. @param type Exact stable identity. @return Success or typed unavailable. */
        [[nodiscard]] Result<void> Choose(Terrain::FoliageTypeId type);

        /** @brief Borrow immutable palette rows until the next owner command. @return Captured rows without filesystem work. */
        [[nodiscard]] std::span<const FoliagePaletteItem> Items() const noexcept {
            return items_;
        }

        /** @brief Borrow the explicit active type. @return Null after removal, close or before selection. */
        [[nodiscard]] const FoliagePaletteItem *Active() const noexcept;
        /** @brief Revoke palette selection and discard transient captures. */
        void Close() noexcept;

    private:
        std::vector<FoliagePaletteItem> items_;
        std::optional<Terrain::FoliageTypeId> selected_;
        bool closed_{};
    };
}  // namespace Horo::Editor
