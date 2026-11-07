#include "DecisionCookState.h"

#include <new>

namespace Horo::Application {
    namespace DecisionCookErrors {
        const ErrorCodeDescriptor Invalid{
            .domain = ErrorDomainId{"horo.ai.cook"},
            .code = ErrorCode{"ai.cook.invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Decision cook inputs, bounds, source family or publication authority are incomplete.",
            .remediationHint = "Supply real pinned graph files and the host schema/package/read fence before cooking.",
            .userActionable = true,
        };
        const ErrorCodeDescriptor Stale{
            .domain = ErrorDomainId{"horo.ai.cook"},
            .code = ErrorCode{"ai.cook.stale"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "A captured decision source or executable provider generation is no longer current.",
            .remediationHint = "Capture current immutable files, schemas and admitted provider generations, then retry.",
            .userActionable = true,
        };
        const ErrorCodeDescriptor Cancelled{
            .domain = ErrorDomainId{"horo.ai.cook"},
            .code = ErrorCode{"ai.cook.cancelled"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Decision cooking was cancelled before publication.",
            .remediationHint = "Retry with a live operation when the host is ready.",
            .userActionable = true,
        };
    }  // namespace DecisionCookErrors

    namespace {
        Result<void> ValidateRequest(const DecisionAssetCookRequest &request) {
            if (!request.assets.pinnedInputs || !request.assets.validateHostInputs || request.assets.keyProjection ||
                request.assets.dependentPhase)
                return Result<void>::Failure(MakeError(DecisionCookErrors::Invalid));
            if (!request.assets.target.IsValid() || request.maximumPreparedBytes == 0 ||
                request.maximumPreparedBytes > 64ULL * 1024ULL * 1024ULL || request.additionalCookers.size() > 128)
                return Result<void>::Failure(MakeError(DecisionCookErrors::Invalid));
            return request.assets.validateHostInputs();
        }

        Result<std::shared_ptr<const DecisionCookDetail::State>> Prepare(const DecisionAssetCookRequest &request,
                                                                         const CancellationToken &cancellation) {
            if (auto valid = ValidateRequest(request); valid.HasError())
                return Result<std::shared_ptr<const DecisionCookDetail::State>>::Failure(valid.ErrorValue());
            auto state = std::make_shared<DecisionCookDetail::State>();
            state->files = request.assets.pinnedInputs;
            state->environment = request.environment;
            state->limits = request.limits;
            state->target = request.assets.target;
            state->maximumPreparedBytes = request.maximumPreparedBytes;
            if (auto capture = DecisionCookDetail::Capture(*state, request.assets.registry, cancellation); capture.HasError())
                return Result<std::shared_ptr<const DecisionCookDetail::State>>::Failure(capture.ErrorValue());
            if (auto prepared = DecisionCookDetail::PrepareRoots(*state, cancellation); prepared.HasError())
                return Result<std::shared_ptr<const DecisionCookDetail::State>>::Failure(prepared.ErrorValue());
            return Result<std::shared_ptr<const DecisionCookDetail::State>>::Success(std::move(state));
        }
    }  // namespace

    Result<Assets::AssetCookReport> DecisionAssetCookHost::Cook(JobSystem &jobs, const DecisionAssetCookRequest &request,
                                                                const CancellationToken &cancellation) {
        try {
            auto prepared = Prepare(request, cancellation);
            if (prepared.HasError())
                return Result<Assets::AssetCookReport>::Failure(prepared.ErrorValue());
            auto catalog = DecisionCookDetail::Catalog(prepared.Value(), request.additionalCookers);
            if (catalog.HasError())
                return Result<Assets::AssetCookReport>::Failure(catalog.ErrorValue());
            auto assets = request.assets;
            const auto state = std::move(prepared).Value();
            const auto original = request.assets.validateHostInputs;
            assets.validateHostInputs = [state, original, cancellation]() -> Result<void> {
                if (auto current = original(); current.HasError())
                    return current;
                return DecisionCookDetail::Verify(*state, cancellation);
            };
            return Assets::AssetCookService{jobs, std::move(catalog).Value()}.Cook(assets, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<Assets::AssetCookReport>::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
        } catch (...) {
            return Result<Assets::AssetCookReport>::Failure(MakeError(DecisionCookErrors::Invalid));
        }
    }
}  // namespace Horo::Application
