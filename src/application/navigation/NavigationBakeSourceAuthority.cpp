#include "Horo/Application/NavigationBakeSourceAuthority.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <tuple>

namespace Horo::Application {
    namespace NavigationBakeDetail {
        /** @brief Protected authoritative evidence; an adoption lease excludes host source transactions. */
        struct SourceState {
            std::mutex mutex;
            Navigation::NavigationBakeInputRevisions revisions;
            std::vector<Navigation::NavigationSourceObservation> sources;
        };
    }  // namespace NavigationBakeDetail

    /** @copydoc NavigationBakeSourceLease::NavigationBakeSourceLease */
    NavigationBakeSourceLease::NavigationBakeSourceLease(std::shared_ptr<NavigationBakeDetail::SourceState> state,
                                                         std::unique_lock<std::mutex> lock) noexcept
        : state_(std::move(state)), lock_(std::move(lock)) {}

    /** @copydoc NavigationBakeSourceAuthority::NavigationBakeSourceAuthority */
    NavigationBakeSourceAuthority::NavigationBakeSourceAuthority() : state_(std::make_shared<NavigationBakeDetail::SourceState>()) {}

    /** @copydoc NavigationBakeSourceAuthority::UpdateCurrent */
    Result<void> NavigationBakeSourceAuthority::UpdateCurrent(const Navigation::NavigationBakeInputRevisions &revisions,
                                                              std::vector<Navigation::NavigationSourceObservation> sources) {
        using namespace Navigation;
        if (!revisions.requestGeneration.IsValid() || !revisions.definition.IsValid() || !revisions.scene.IsValid() ||
            !revisions.areaRegistry.IsValid() || !revisions.projectProfile.IsValid() || !revisions.coordinates.IsValid() ||
            !revisions.geometry.IsValid())
            return Result<void>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        if (sources.size() > NavigationSourceGeometryLimits::MaximumContributions)
            return Result<void>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        if (std::ranges::any_of(sources, [](const auto &source) {
            return !source.producer.IsValid() || !source.contribution.IsValid() || !source.revision.IsValid() ||
                   source.kind >= NavigationSourceProducerKind::Count || source.contentDigest == Sha256Digest{};
        }))
            return Result<void>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        std::ranges::sort(sources, [](const auto &a, const auto &b) {
            return std::tie(a.producer, a.contribution) < std::tie(b.producer, b.contribution);
        });
        if (std::ranges::adjacent_find(sources, [](const auto &a, const auto &b) {
            return a.producer == b.producer && a.contribution == b.contribution;
        }) != sources.end())
            return Result<void>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        std::unique_lock lock(state_->mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<void>::Failure(MakeError(NavigationErrors::BakeJobAdmissionRejected));
        state_->revisions = revisions;
        state_->sources = std::move(sources);
        return Result<void>::Success();
    }

    /** @copydoc NavigationBakeSourceAuthority::TryAcquirePublication */
    Result<NavigationBakeSourceLease> NavigationBakeSourceAuthority::TryAcquirePublication(
        const Navigation::NavigationBakeInputSnapshot &input, const CancellationToken &cancellation) const {
        std::unique_lock lock(state_->mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return Result<NavigationBakeSourceLease>::Failure(MakeError(Navigation::NavigationErrors::BakeJobAdmissionRejected));
        if (cancellation.IsCancellationRequested())
            return Result<NavigationBakeSourceLease>::Failure(MakeError(Navigation::NavigationErrors::BakeInputCancelled));
        if (auto fresh = input.ValidatePublication(input.Revisions().requestGeneration, state_->revisions, state_->sources);
            fresh.HasError())
            return Result<NavigationBakeSourceLease>::Failure(fresh.ErrorValue());
        return Result<NavigationBakeSourceLease>::Success(NavigationBakeSourceLease(state_, std::move(lock)));
    }
}  // namespace Horo::Application
