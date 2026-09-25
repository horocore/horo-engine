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

    struct AssetImportSourcePreview::PreviewRequest {
        Assets::AssetImporterContribution descriptor;
        TransparentStringMap<std::string> settings;
        std::string path;
        std::string extension;
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

    /** @brief Builds one preview without retaining references to modal-owned state. */
    Result<void> AssetImportSourcePreview::GeneratePreview(const std::shared_ptr<ResultState> &state, const PreviewRequest &request,
                                                           const CancellationToken &cancellation) {
        struct CompletionGuard {
            ResultState &state;

            ~CompletionGuard() {
                state.finished.store(true, std::memory_order_release);
            }
        } guard{*state};

        if (cancellation.IsCancellationRequested())
            return Result<void>::Success();
        std::error_code error;
        const auto bytes = std::filesystem::file_size(request.path, error);
        if (error || bytes > MaximumPreviewSourceBytes)
            return Result<void>::Success();
        auto source = Assets::ReadAssetImportSource(request.path);
        auto resolved = Assets::ResolveImportSettings(request.descriptor, request.settings);
        if (source.HasError() || resolved.HasError() || cancellation.IsCancellationRequested())
            return Result<void>::Success();
        auto prepared = request.descriptor.strategy->Import(Assets::AssetImportInput{.sourceBytes = source.Value(),
                                                                                     .sourceExtension = request.extension,
                                                                                     .settings = std::move(resolved).Value()},
                                                            cancellation);
        if (prepared.HasValue() && !cancellation.IsCancellationRequested()) {
            auto preview = request.descriptor.previewProvider->GeneratePreview(
                Assets::AssetPreviewInput{
                    .editorPayload = prepared.Value().editorPayload,
                    .absoluteAssetPath = request.path,
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
        return Result<void>::Success();
    }

    /** @brief Replaces an obsolete request and schedules one owned background preview. */
    void AssetImportSourcePreview::StartPreview(const Assets::AssetImportItem &item, const Assets::AssetImporterContribution &contribution,
                                                const std::string &path) {
        Clear();
        requestedPath_ = path;
        requestedContribution_ = contribution.contributionId;
        result_ = std::make_shared<ResultState>();
        const auto state = result_;
        PreviewRequest request{.descriptor = contribution, .settings = item.settings, .path = path, .extension = item.sourceExtension};
        auto submitted = jobs_.SubmitResult({}, [state, request = std::move(request)](const CancellationToken &cancellation) {
            return GeneratePreview(state, request, cancellation);
        });
        if (submitted.HasValue())
            job_ = std::move(submitted).Value();
        else
            result_->finished.store(true, std::memory_order_release);
    }

    /** @brief Uploads only a completed image belonging to the currently selected request. */
    void AssetImportSourcePreview::UploadFinished() {
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

    void AssetImportSourcePreview::Update(const Assets::AssetImportItem *item, const Assets::AssetImporterContribution *contribution) {
        if (item == nullptr || contribution == nullptr || !contribution->strategy || !contribution->previewProvider ||
            !item->absoluteSourcePath.is_absolute()) {
            if (!requestedPath_.empty())
                Clear();
            return;
        }
        const std::string path = item->absoluteSourcePath.string();
        if (path != requestedPath_ || contribution->contributionId != requestedContribution_)
            StartPreview(*item, *contribution, path);
        UploadFinished();
    }

    std::uintptr_t AssetImportSourcePreview::TextureId() const noexcept {
        return textureId_;
    }
}  // namespace Horo::Editor
