#include "DecisionCookState.h"

#include <algorithm>

namespace Horo::Application::DecisionCookDetail {
    namespace {
        class Strategy final : public Assets::ICookerStrategy {
        public:
            explicit Strategy(std::shared_ptr<const State> state) : state_(std::move(state)) {}

            Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return {"horo.ai.decision-plan.v1", state_->settings, 1};
            }

            Result<std::optional<std::vector<Assets::AssetId>>> InputDependencies(const Assets::CookSourceView &source,
                                                                                  const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<std::optional<std::vector<Assets::AssetId>>>::Failure(MakeError(DecisionCookErrors::Cancelled));
                const auto *prepared = Find(source);
                if (!prepared)
                    return Result<std::optional<std::vector<Assets::AssetId>>>::Failure(MakeError(DecisionCookErrors::Stale));
                return Result<std::optional<std::vector<Assets::AssetId>>>::Success(prepared->dependencies);
            }

            Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<Assets::CookOutputSink>::Failure(MakeError(DecisionCookErrors::Cancelled));
                const auto *prepared = Find(source);
                if (!prepared)
                    return Result<Assets::CookOutputSink>::Failure(MakeError(DecisionCookErrors::Stale));
                return Result<Assets::CookOutputSink>::Success({prepared->payload, prepared->dependencies, {}});
            }

            Result<void> ValidateCookedPayload(const Assets::CookSourceView &source, std::span<const std::uint8_t> payload) const override {
                const auto *prepared = Find(source);
                if (!prepared)
                    return Result<void>::Failure(MakeError(DecisionCookErrors::Stale));
                if (!std::ranges::equal(payload, prepared->payload))
                    return Result<void>::Failure(MakeError(DecisionCookErrors::Invalid));
                return Result<void>::Success();
            }

        private:
            const Prepared *Find(const Assets::CookSourceView &source) const {
                if (!state_->environment.nodes->CanUse() || source.target != state_->target)
                    return nullptr;
                const auto found = std::ranges::lower_bound(state_->prepared, source.id, {}, &Prepared::asset);
                if (found == state_->prepared.end() || found->asset != source.id || found->type != source.type ||
                    found->sourceDigest != source.sourceDigest)
                    return nullptr;
                return &*found;
            }

            const std::shared_ptr<const State> state_;
        };
    }  // namespace

    Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>> Catalog(std::shared_ptr<const State> state,
                                                                         std::span<const Assets::CookerContribution> additional) {
        Assets::CookerCatalog catalog;
        for (const auto &entry : additional) {
            if (auto registered = catalog.Register(entry); registered.HasError())
                return Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>::Failure(registered.ErrorValue());
        }
        const auto strategy = std::make_shared<Strategy>(state);
        for (const auto type : {DecisionBehaviorTreeSourceType, DecisionStateMachineSourceType, DecisionUtilitySourceType}) {
            auto parsed = Assets::AssetTypeId::Parse(type);
            if (parsed.HasError())
                return Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>::Failure(parsed.ErrorValue());
            if (auto registered = catalog.Register({"horo.builtin." + std::string{type}, parsed.Value(), {state->target}, strategy});
                registered.HasError())
                return Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>::Failure(registered.ErrorValue());
        }
        return catalog.Publish();
    }
}  // namespace Horo::Application::DecisionCookDetail
