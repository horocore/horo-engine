#include "AnimationAssetCookState.h"
#include "AnimationAssetSourceInternal.h"

#include <algorithm>

namespace Horo::Animation {
    namespace {
        bool ValidCookLimits(const AnimationAssetCookLimits &limits) noexcept {
            return limits.maximumAssets > 0 && limits.maximumAssets <= 16384 && limits.maximumPreparedBytes > 0 &&
                   limits.maximumPreparedBytes <= 1024ULL * 1024ULL * 1024ULL && limits.maximumReferences > 0 &&
                   limits.maximumReferences <= 1048576;
        }
    }  // namespace

    class PreparedAnimationCook::State::Strategy final : public Assets::ICookerStrategy {
    public:
        Strategy(std::shared_ptr<const State> state, Assets::AssetTypeId type) : state_(std::move(state)), type_(std::move(type)) {}

        Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
            constexpr std::string_view semantic = "horo.animation.canonical-prepared.v1/HANIM001/skeleton1.0/clip1.0/graph1.4";
            return {"horo.animation.canonical-prepared.v1", ComputeSha256(std::as_bytes(std::span{semantic})), 1};
        }

        Result<std::optional<std::vector<Assets::AssetId>>> InputDependencies(const Assets::CookSourceView &source,
                                                                              const CancellationToken &cancellation) const override {
            using Return = Result<std::optional<std::vector<Assets::AssetId>>>;
            if (cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(AnimationAssetErrors::Cancelled));
            const auto *entry = state_->Find(source.id);
            if (!entry || entry->type != type_ || !state_->Matches(source, *entry))
                return Return::Failure(MakeError(AnimationAssetErrors::Stale));
            std::vector<Assets::AssetId> ids;
            for (const auto &role : entry->payload.dependencies)
                ids.push_back(role.id);
            return Return::Success(std::move(ids));
        }

        Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source, const CancellationToken &cancellation) const override {
            using Return = Result<Assets::CookOutputSink>;
            const auto dependencies = InputDependencies(source, cancellation);
            if (dependencies.HasError())
                return Return::Failure(dependencies.ErrorValue());
            const auto *entry = state_->Find(source.id);
            return Return::Success({entry->payload.bytes, *dependencies.Value(), {}});
        }

        Result<void> ValidateCookedPayload(const Assets::CookSourceView &source, std::span<const std::uint8_t> payload) const override {
            const auto *entry = state_->Find(source.id);
            if (!entry || entry->type != type_ || !state_->Matches(source, *entry))
                return Result<void>::Failure(MakeError(AnimationAssetErrors::Stale));
            if (!std::ranges::equal(payload, entry->payload.bytes))
                return Result<void>::Failure(MakeError(AnimationAssetErrors::Stale));
            return Result<void>::Success();
        }

    private:
        std::shared_ptr<const State> state_;
        Assets::AssetTypeId type_;
    };

    bool PreparedAnimationCook::State::Matches(const Assets::CookSourceView &source, const Entry &entry) const noexcept {
        const auto *captured = capture.Find(source.id);
        if (!captured)
            return false;
        return source.id == entry.id && source.type == entry.type && source.target == target &&
               source.sourceDigest == captured->sourceDigest && std::ranges::equal(source.bytes, captured->bytes);
    }

    Result<PreparedAnimationCook> PreparedAnimationCook::Create(const Assets::AssetCookInputSnapshot &capture, AssetCookTargetId target,
                                                                AnimationAssetCookLimits limits, const CancellationToken &cancellation) {
        using Return = Result<PreparedAnimationCook>;
        if (!target.IsValid() || !ValidCookLimits(limits) || capture.Sources().size() > 16384)
            return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
        try {
            AssetSourceInternal::Check({limits.codec, cancellation, true}, limits.source);
            auto state = std::make_shared<State>(capture, std::move(target), std::move(limits));
            auto identity = Assets::AssetCookKeyProjection::Create(capture, {}, state->limits.maximumAssets);
            if (identity.HasError())
                return Return::Failure(std::move(identity).ErrorValue());
            state->captureIdentity = std::move(identity).Value();
            if (auto built = state->Build(cancellation); built.HasError())
                return Return::Failure(std::move(built).ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(AnimationAssetErrors::Cancelled));
            return Return::Success(PreparedAnimationCook(std::move(state)));
        } catch (const AssetSourceInternal::Rejection &error) {
            return Return::Failure(MakeError(*error.code));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
        }
    }

    std::vector<Assets::CookerContribution> PreparedAnimationCook::Contributions() const {
        std::vector<Assets::CookerContribution> contributions;
        for (const auto kind : {"skeleton", "clip", "graph"}) {
            const std::string typeName = std::string("core.animation.") + kind;
            const auto type = Assets::AssetTypeId::Parse(typeName).Value();
            if (!std::ranges::any_of(state_->entries, [&](const auto &entry) {
                return entry.type == type;
            }))
                continue;
            contributions.push_back(
                {std::string("horo.animation.cooked.") + kind, type, {state_->target}, std::make_shared<State::Strategy>(state_, type)});
        }
        return contributions;
    }

    bool PreparedAnimationCook::UsesCapture(const Assets::AssetCookInputSnapshot &capture) const {
        return state_->captureIdentity->Matches(capture);
    }

    Result<void> PreparedAnimationCook::ValidateEmitted(std::span<const Assets::AssetCookCandidateArtifactView> artifacts,
                                                        const CancellationToken &cancellation) const {
        try {
            return state_->ValidateEmitted(artifacts, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Capacity));
        }
    }

    Result<std::vector<Assets::AssetDependency>> PreparedAnimationCook::ExpandDependencies(
        std::span<const Assets::AssetDependency> roots, std::span<const Assets::AssetCookCandidateArtifactView> artifacts,
        std::size_t maximumAssets, const CancellationToken &cancellation) const {
        using Return = Result<std::vector<Assets::AssetDependency>>;
        try {
            return state_->Expand(roots, artifacts, maximumAssets, cancellation);
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
        }
    }
}  // namespace Horo::Animation
