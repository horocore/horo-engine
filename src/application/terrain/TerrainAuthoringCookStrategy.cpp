#include "TerrainAuthoringCookInternal.h"

#include <algorithm>

namespace Horo::Application::TerrainCookDetail {
    namespace {
        /** @brief Pure immutable source strategy; actual geometry runs only in ordinary shared AssetCookService CPU jobs. */
        class Strategy final : public Assets::ICookerStrategy {
        public:
            Strategy(std::shared_ptr<const Capture> capture, TerrainAuthoringCookProfile profile, CancellationToken cancellation)
                : capture_(std::move(capture)), profile_(std::move(profile)), cancellation_(std::move(cancellation)),
                  settings_(ProfileDigest(profile_)) {}

            Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return {"horo.terrain.authoring.geometry.v2", settings_, 1};
            }

            Result<std::optional<std::vector<Assets::AssetId>>> InputDependencies(const Assets::CookSourceView &source,
                                                                                  const CancellationToken &cancellation) const override {
                using Return = Result<std::optional<std::vector<Assets::AssetId>>>;
                if (cancellation.IsCancellationRequested())
                    return Return::Failure(MakeError(TerrainAuthoringCookErrors::Cancelled));
                if (!Matches(source))
                    return Return::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
                return Return::Success(capture_->dependencies);
            }

            Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                const CancellationToken &cancellation) const override {
                using Return = Result<Assets::CookOutputSink>;
                if (!Matches(source))
                    return Return::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
                auto bundle = MakeBundle(*capture_, profile_, cancellation);
                if (bundle.HasError())
                    return Return::Failure(std::move(bundle).ErrorValue());
                return Return::Success({std::move(bundle).Value(), {}, {}});
            }

            Result<void> ValidateCookedPayload(const Assets::CookSourceView &source, std::span<const std::uint8_t> payload) const override {
                if (!Matches(source))
                    return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
                // Domain validation recomputes the bounded canonical result. This deliberately spends tooling CPU
                // even on cache hits rather than accepting structurally valid geometry for a different hole mask/profile.
                auto expected = MakeBundle(*capture_, profile_, cancellation_);
                if (expected.HasError())
                    return Result<void>::Failure(std::move(expected).ErrorValue());
                if (!std::ranges::equal(expected.Value(), payload))
                    return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Invalid));
                return Result<void>::Success();
            }

        private:
            bool Matches(const Assets::CookSourceView &source) const noexcept {
                const auto *root = capture_->inputs->Find(source.id);
                return source.id == capture_->source.source.sourceAsset && root && root->sourceDigest == source.sourceDigest &&
                       source.target == profile_.target;
            }

            std::shared_ptr<const Capture> capture_;
            TerrainAuthoringCookProfile profile_;
            CancellationToken cancellation_;
            Sha256Digest settings_;
        };
    }  // namespace

    /** @copydoc MakeCatalog */
    Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>> MakeCatalog(std::shared_ptr<const Capture> capture,
                                                                             const TerrainAuthoringCookRequest &request,
                                                                             const CancellationToken &cancellation) {
        Assets::CookerCatalog catalog;
        auto registered = catalog.Register({"horo.builtin.terrain.authoring",
                                            Assets::AssetTypeId::Parse("core.terrain").Value(),
                                            {request.profile.target},
                                            std::make_shared<Strategy>(std::move(capture), request.profile, cancellation)});
        if (registered.HasError())
            return Result<std::shared_ptr<const Assets::CookerCatalogSnapshot>>::Failure(std::move(registered).ErrorValue());
        return catalog.Publish();
    }
}  // namespace Horo::Application::TerrainCookDetail
