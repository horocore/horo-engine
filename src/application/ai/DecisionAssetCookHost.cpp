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
        class LocalPolicyCookStrategy final : public Assets::ICookerStrategy {
        public:
            explicit LocalPolicyCookStrategy(const LocalPolicyAssetCookAdmission &admission) : admission_(admission) {}

            Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return {"horo.ai.local-policy.int16-linear.v1", {}, 1};
            }

            Result<std::optional<std::vector<Assets::AssetId>>> InputDependencies(const Assets::CookSourceView &,
                                                                                  const CancellationToken &cancellation) const override {
                using Dependencies = Result<std::optional<std::vector<Assets::AssetId>>>;
                if (cancellation.IsCancellationRequested())
                    return Dependencies::Failure(MakeError(DecisionCookErrors::Cancelled));
                return Dependencies::Success(std::vector<Assets::AssetId>{});
            }

            Result<void> ValidateCookedPayload(const Assets::CookSourceView &source,
                                               const std::span<const std::uint8_t> payload) const override {
                if (source.target != admission_.packageTarget)
                    return Result<void>::Failure(MakeError(AI::AIErrors::CapabilityUnavailable));
                auto admitted = AI::LocalLearnedPolicy::DecodeCooked(payload, source.sourceDigest, admission_.target, admission_.tasks,
                                                                     admission_.blackboard);
                if (admitted.HasError())
                    return Result<void>::Failure(std::move(admitted).ErrorValue());
                return Result<void>::Success();
            }

            Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                const CancellationToken &cancellation) const override {
                using Output = Result<Assets::CookOutputSink>;
                if (cancellation.IsCancellationRequested())
                    return Output::Failure(MakeError(DecisionCookErrors::Cancelled));
                if (auto valid = ValidateCookedPayload(source, source.bytes); valid.HasError())
                    return Output::Failure(std::move(valid).ErrorValue());
                return Output::Success({{source.bytes.begin(), source.bytes.end()}, {}, {}});
            }

        private:
            const LocalPolicyAssetCookAdmission admission_;
        };

        Result<void> ValidateRequest(const DecisionAssetCookRequest &request) {
            if (!request.assets.pinnedInputs || !request.assets.validateHostInputs || request.assets.keyProjection ||
                request.assets.dependentPhase)
                return Result<void>::Failure(MakeError(DecisionCookErrors::Invalid));
            if (!request.assets.target.IsValid() || request.maximumPreparedBytes == 0 ||
                request.maximumPreparedBytes > 64ULL * 1024ULL * 1024ULL || request.additionalCookers.size() > 128)
                return Result<void>::Failure(MakeError(DecisionCookErrors::Invalid));
            if (request.localPolicy && request.localPolicy->packageTarget != request.assets.target)
                return Result<void>::Failure(MakeError(AI::AIErrors::CapabilityUnavailable));
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
            state->querySchema = request.querySchema;
            state->localPolicy = request.localPolicy;
            if (auto capture = DecisionCookDetail::Capture(*state, request.assets.registry, cancellation); capture.HasError())
                return Result<std::shared_ptr<const DecisionCookDetail::State>>::Failure(capture.ErrorValue());
            if (auto prepared = DecisionCookDetail::PrepareRoots(*state, cancellation); prepared.HasError())
                return Result<std::shared_ptr<const DecisionCookDetail::State>>::Failure(prepared.ErrorValue());
            return Result<std::shared_ptr<const DecisionCookDetail::State>>::Success(std::move(state));
        }
    }  // namespace

    Result<Assets::CookerContribution> MakeLocalPolicyAssetCooker(const LocalPolicyAssetCookAdmission &admission) {
        using Contribution = Result<Assets::CookerContribution>;
        if (!admission.packageTarget.IsValid() || !admission.target.id.IsValid())
            return Contribution::Failure(MakeError(AI::AIErrors::CapabilityUnavailable));
        try {
            auto type = Assets::AssetTypeId::Parse(AI::LocalPolicyAssetType);
            if (type.HasError())
                return Contribution::Failure(std::move(type).ErrorValue());
            return Contribution::Success({"horo.builtin.local-policy",
                                          type.Value(),
                                          {admission.packageTarget},
                                          std::make_shared<LocalPolicyCookStrategy>(admission)});
        } catch (const std::bad_alloc &) {
            return Contribution::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
        }
    }

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
            assets.validateHostInputs = [state, original, cancellation]() {
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
