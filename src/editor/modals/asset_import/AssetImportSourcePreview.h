#pragma once

#include "Horo/Foundation/JobSystem.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace Horo::Assets {
    struct AssetImportItem;
    struct AssetImporterContribution;
}  // namespace Horo::Assets

namespace Horo::Editor {
    class IEditorGuiRenderer;

    /** @brief UI-owned texture projection of a source file prepared by its registered importer. */
    class AssetImportSourcePreview final {
    public:
        AssetImportSourcePreview(JobSystem &jobs, IEditorGuiRenderer &renderer) noexcept;
        ~AssetImportSourcePreview();

        void Update(const Assets::AssetImportItem *item, const Assets::AssetImporterContribution *contribution);
        [[nodiscard]] std::uintptr_t TextureId() const noexcept;

    private:
        struct ResultState;
        struct PreviewRequest;
        void Clear() noexcept;
        void StartPreview(const Assets::AssetImportItem &item, const Assets::AssetImporterContribution &contribution,
                          const std::string &path);
        void UploadFinished();
        [[nodiscard]] static Result<void> GeneratePreview(const std::shared_ptr<ResultState> &state, const PreviewRequest &request,
                                                          const CancellationToken &cancellation);

        JobSystem &jobs_;
        IEditorGuiRenderer &renderer_;
        std::string requestedPath_;
        std::string requestedContribution_;
        std::shared_ptr<ResultState> result_;
        std::optional<JobHandle> job_;
        std::uintptr_t textureId_{};
    };
}  // namespace Horo::Editor
