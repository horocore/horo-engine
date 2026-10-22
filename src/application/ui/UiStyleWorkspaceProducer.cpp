#include "Horo/Application/UiStyleWorkspace.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <limits>
#include <new>

namespace Horo::Application {
    namespace {
        /** @brief Terminal snapshot acquisition synchronizes the worker envelope before owner reads. */
        bool Terminal(const JobState state) noexcept {
            return state == JobState::Succeeded || state == JobState::Failed || state == JobState::Cancelled;
        }
    }  // namespace

    /** @brief Worker-only output; owner reads only after terminal JobHandle snapshot synchronization. No GUI/document borrow exists. */
    struct UiStyleWorkspaceProducer::Work final {
        UiStyleWorkspaceConfig config;
        Assets::AssetRegistrySnapshot assets;
        Assets::AssetId workspace;
        std::uint64_t request{};
        Runtime::Ui::RuntimeStyleAssetId theme;
        Runtime::Ui::RuntimeStyleGeneration generation;
        std::optional<UiStyleWorkspaceCandidate> candidate;

        /** @brief Retain every input through joined nested cook/load work and preserve typed failures. */
        Result<void> Execute(JobSystem &jobs, const CancellationToken &cancellation) {
            try {
                auto result = PrepareUiStyleWorkspace(jobs, config, assets, workspace, request, theme, generation, cancellation);
                if (result.HasError())
                    return Result<void>::Failure(result.ErrorValue());
                candidate.emplace(std::move(result).Value());
                return Result<void>::Success();
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::CapacityExceeded));
            } catch (const std::exception &) {
                return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::StyleInvalid));
            }
        }
    };

    /** @copydoc UiStyleWorkspaceProducer::UiStyleWorkspaceProducer */
    UiStyleWorkspaceProducer::UiStyleWorkspaceProducer(JobSystem &jobs, UiStyleWorkspaceConfig config, const Assets::AssetId workspace)
        : jobs_(jobs), config_(std::move(config)), workspace_(workspace), owner_(std::this_thread::get_id()) {}

    /** @copydoc UiStyleWorkspaceProducer::~UiStyleWorkspaceProducer */
    UiStyleWorkspaceProducer::~UiStyleWorkspaceProducer() {
        if (work_) {
            static_cast<void>(handle_.RequestCancel());
            static_cast<void>(handle_.Wait());
        }
    }

    /** @copydoc UiStyleWorkspaceProducer::Submit */
    Result<std::uint64_t> UiStyleWorkspaceProducer::Submit(Assets::AssetRegistrySnapshot assets,
                                                           const Runtime::Ui::RuntimeStyleAssetId theme) {
        using R = Result<std::uint64_t>;
        if (std::this_thread::get_id() != owner_ || closed_ || work_ || !workspace_.IsValid())
            return R::Failure(MakeError(Runtime::Ui::UiErrors::StyleLifecycleUnavailable));
        if (issued_ == std::numeric_limits<std::uint64_t>::max())
            return R::Failure(MakeError(Runtime::Ui::UiErrors::GenerationExhausted));
        const auto *record = assets.Find(Assets::AssetId::FromBytes(theme.Bytes()));
        if (!theme.IsValid() || !record || record->type.Value() != Runtime::Ui::UiStyleAssetTypeName)
            return R::Failure(MakeError(Runtime::Ui::UiErrors::StyleReferenceInvalid));
        try {
            const auto request = ++issued_;
            auto work = std::make_shared<Work>(Work{config_,
                                                    std::move(assets),
                                                    workspace_,
                                                    request,
                                                    theme,
                                                    Runtime::Ui::RuntimeStyleGeneration::Create(request).Value(),
                                                    {}});
            const JobProducerScope admission{JobProducerRole::ExternalUnknown};
            auto submitted = jobs_.SubmitResult({}, [work, jobs = &jobs_](const CancellationToken &cancellation) {
                return work->Execute(*jobs, cancellation);
            });
            if (submitted.HasError())
                return R::Failure(submitted.ErrorValue());
            handle_ = std::move(submitted).Value();
            work_ = std::move(work);
            return R::Success(request);
        } catch (const std::bad_alloc &) {
            return R::Failure(MakeError(Runtime::Ui::UiErrors::CapacityExceeded));
        }
    }

    /** @copydoc UiStyleWorkspaceProducer::TakeCompleted */
    Result<std::optional<UiStyleWorkspaceCandidate>> UiStyleWorkspaceProducer::TakeCompleted(const Assets::AssetRegistryRevision assets) {
        using R = Result<std::optional<UiStyleWorkspaceCandidate>>;
        if (std::this_thread::get_id() != owner_ || closed_)
            return R::Failure(MakeError(Runtime::Ui::UiErrors::StyleLifecycleUnavailable));
        if (!work_)
            return R::Success(std::nullopt);
        const auto snapshot = handle_.Snapshot();
        if (!snapshot || !Terminal(snapshot->state))
            return R::Success(std::nullopt);
        auto work = std::move(work_);
        handle_ = {};
        if (snapshot->error)
            return R::Failure(*snapshot->error);
        if (!work->candidate || work->workspace != workspace_ || work->request != issued_ || work->assets.Revision() != assets)
            return R::Failure(MakeError(Runtime::Ui::UiErrors::AssetRegistryStale));
        return R::Success(std::move(work->candidate));
    }

    /** @copydoc UiStyleWorkspaceProducer::Join */
    Result<void> UiStyleWorkspaceProducer::Join(const JoinOptions &options) const {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::StyleLifecycleUnavailable));
        return work_ ? handle_.Wait(options) : Result<void>::Success();
    }

    /** @copydoc UiStyleWorkspaceProducer::Shutdown */
    Result<void> UiStyleWorkspaceProducer::Shutdown(const JoinOptions &options) {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::StyleLifecycleUnavailable));
        closed_ = true;
        if (!work_)
            return Result<void>::Success();
        static_cast<void>(handle_.RequestCancel());
        auto joined = handle_.Wait(options);
        const auto snapshot = handle_.Snapshot();
        if (!snapshot || !Terminal(snapshot->state))
            return joined;
        work_.reset();
        handle_ = {};
        return Result<void>::Success();
    }
}  // namespace Horo::Application
