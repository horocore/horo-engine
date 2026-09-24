#include "AssetImportSourcePreview.h"

#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetImportOperation.h"
#include "editor/renderer/EditorGuiRenderer.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <utility>

namespace Horo::Editor {
    namespace {
        constexpr std::uintmax_t MaximumPreviewSourceBytes = 64U * 1024U * 1024U;
        constexpr std::uint32_t PreviewWidth = 272;
        constexpr std::uint32_t PreviewHeight = 232;
    }  // namespace

    struct AssetImportSourcePreview::ResultState {
        std::mutex mutex;
        std::optional<Assets::AssetPreviewImage> image;
        std::atomic<bool> finished{false};
    };

    AssetImportSourcePreview::AssetImportSourcePreview(JobSystem &jobs, IEditorGuiRenderer &renderer) noexcept
        : jobs_(jobs), renderer_(renderer) {}

    AssetImportSourcePreview::~AssetImportSourcePreview() {
        Clear();
    }

    void AssetImportSourcePreview::Clear() noexcept {
        if (job_)
            (void)job_->RequestCancel();
        job_.reset();
        result_.reset();
        requestedPath_.clear();
        requestedContribution_.clear();
        if (textureId_ != 0) {
            renderer_.DestroyTexture(textureId_);
            textureId_ = 0;
        }
    }

    void AssetImportSourcePreview::Update(const Assets::AssetImportItem *item, const Assets::AssetImporterContribution *contribution) {
        if (item == nullptr || contribution == nullptr || !contribution->strategy || !contribution->previewProvider ||
            !item->absoluteSourcePath.is_absolute()) {
            if (!requestedPath_.empty())
                Clear();
            return;
        }

        const std::string path = item->absoluteSourcePath.string();
        if (path != requestedPath_ || contribution->contributionId != requestedContribution_) {
            Clear();
            requestedPath_ = path;
            requestedContribution_ = contribution->contributionId;
            result_ = std::make_shared<ResultState>();
            const auto state = result_;
            const auto strategy = contribution->strategy;
            const auto provider = contribution->previewProvider;
            const Assets::AssetImporterContribution descriptor = *contribution;
            const auto settings = item->settings;
            const std::string extension = item->sourceExtension;
            auto submitted = jobs_.SubmitResult({},
                                                [state, strategy, provider, descriptor, settings, path,
                                                 extension](const CancellationToken &cancellation) -> Result<void> {
                auto finish = [&state] {
                    state->finished.store(true, std::memory_order_release);
                };
                if (cancellation.IsCancellationRequested()) {
                    finish();
                    return Result<void>::Success();
                }
                std::error_code error;
                const auto bytes = std::filesystem::file_size(path, error);
                if (error || bytes > MaximumPreviewSourceBytes) {
                    finish();
                    return Result<void>::Success();
                }
                auto source = Assets::ReadAssetImportSource(path);
                auto resolved = Assets::ResolveImportSettings(descriptor, settings);
                if (source.HasError() || resolved.HasError() || cancellation.IsCancellationRequested()) {
                    finish();
                    return Result<void>::Success();
                }
                auto prepared = strategy->Import(Assets::AssetImportInput{.sourceBytes = source.Value(),
                                                                          .sourceExtension = extension,
                                                                          .settings = std::move(resolved).Value()},
                                                 cancellation);
                if (prepared.HasValue() && !cancellation.IsCancellationRequested()) {
                    auto preview = provider->GeneratePreview(
                        Assets::AssetPreviewInput{
                            .editorPayload = prepared.Value().editorPayload,
                            .absoluteAssetPath = path,
                            .assetType = prepared.Value().type,
                            .width = PreviewWidth,
                            .height = PreviewHeight,
                        },
                        cancellation);
                    if (preview.HasValue() && preview.Value().IsValid() && !cancellation.IsCancellationRequested()) {
                        const std::scoped_lock lock{state->mutex};
                        state->image = std::move(preview).Value();
                    }
                }
                finish();
                return Result<void>::Success();
            });
            if (submitted.HasValue())
                job_ = std::move(submitted).Value();
            else
                result_->finished.store(true, std::memory_order_release);
        }

        if (textureId_ != 0 || !result_ || !result_->finished.load(std::memory_order_acquire))
            return;
        std::optional<Assets::AssetPreviewImage> image;
        {
            const std::scoped_lock lock{result_->mutex};
            image = std::move(result_->image);
        }
        result_.reset();
        if (!image || !image->IsValid())
            return;
        auto uploaded =
            renderer_.CreateTexture(EditorRgba8ImageView{.width = image->width, .height = image->height, .pixels = image->pixels});
        if (uploaded.HasValue())
            textureId_ = std::move(uploaded).Value();
    }

    std::uintptr_t AssetImportSourcePreview::TextureId() const noexcept {
        return textureId_;
    }
}  // namespace Horo::Editor
