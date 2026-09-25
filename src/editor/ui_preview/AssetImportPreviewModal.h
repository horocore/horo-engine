#pragma once

/** @file AssetImportPreviewModal.h
 * @brief Preview-only Asset Import scenarios backed by the production modal.
 */

#include "Horo/Editor/AssetImportModal.h"

namespace Horo::Editor {
    enum class AssetImportPreviewScenario {
        Populated,
        Empty,
        Advanced,
        Unsupported
    };

    class AssetImportPreviewModal final : public AssetImportModal {
    public:
        using AssetImportModal::AssetImportModal;

        void SetScenario(AssetImportPreviewScenario scenario) noexcept;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &context) override;

    private:
        AssetImportPreviewScenario scenario_{AssetImportPreviewScenario::Populated};
        void PopulateScenario();
    };
}  // namespace Horo::Editor
