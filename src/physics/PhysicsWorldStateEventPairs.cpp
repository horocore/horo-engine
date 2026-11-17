#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateSurfaceBinding.h"
#include "PhysicsWorldStateEvents.h"

#include <algorithm>
#include <tuple>

namespace Horo::Physics::Detail {
    namespace {
        void ReverseContact(PhysicsContactSummary &contact) noexcept {
            for (auto &point : std::span{contact.points}.first(contact.pointCount)) {
                std::swap(point.positionOnFirst, point.positionOnSecond);
                point.normal = -point.normal;
            }
            std::ranges::sort(std::span{contact.points}.first(contact.pointCount), [](const auto &one, const auto &two) {
                return std::tie(one.positionOnFirst, one.positionOnSecond, one.normal, one.penetrationDepthMeters) <
                       std::tie(two.positionOnFirst, two.positionOnSecond, two.normal, two.penetrationDepthMeters);
            });
        }

        template <class T> void Reverse(T &value) noexcept {
            std::swap(value.first, value.second);
            std::swap(value.firstMaterial, value.secondMaterial);
            std::swap(value.firstSurface, value.secondSurface);
            ReverseContact(value.contact);
        }

        Result<PhysicsStateEventEndpoint> CaptureEndpoint(const CanonicalPhysicsStateWorkspace &workspace,
                                                          const PhysicsEventEndpoint &source,
                                                          const std::optional<PhysicsEventMaterial> &material,
                                                          const std::optional<PhysicsSurfaceEvidence> &surface) {
            using Outcome = Result<PhysicsStateEventEndpoint>;
            PhysicsStateEventEndpoint endpoint{workspace.BodyId(source.body),
                                               workspace.ShapeId(source.shape),
                                               source.subshape,
                                               source.layer,
                                               source.profile,
                                               source.collider};
            auto resolved = workspace.ResolveEventEndpoint(endpoint, material, StateDetail::CaptureStateSurface(surface));
            if (resolved.HasError())
                return Outcome::Failure(resolved.ErrorValue());
            if (resolved.Value() != source)
                return Outcome::Failure(MakeError(PhysicsErrors::CapabilityStale));
            return Outcome::Success(endpoint);
        }
    }  // namespace

    /** @copydoc StatePairLess */
    bool StatePairLess(const PhysicsStateEventPair &one, const PhysicsStateEventPair &two) noexcept {
        return std::tie(one.first, one.second) < std::tie(two.first, two.second);
    }

    /** @copydoc CapturePhysicsStatePair */
    Result<PhysicsStateEventPair> CapturePhysicsStatePair(const CanonicalPhysicsStateWorkspace &workspace,
                                                          const PhysicsEventProjectionPairState &source) {
        using Outcome = Result<PhysicsStateEventPair>;
        if (source.contact.pointCount > MaximumPhysicsContactPoints)
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        auto first = CaptureEndpoint(workspace, source.pair.first, source.firstMaterial, source.firstSurface);
        if (first.HasError())
            return Outcome::Failure(first.ErrorValue());
        auto second = CaptureEndpoint(workspace, source.pair.second, source.secondMaterial, source.secondSurface);
        if (second.HasError())
            return Outcome::Failure(second.ErrorValue());
        PhysicsStateEventPair value{first.Value(),
                                    second.Value(),
                                    source.firstMaterial,
                                    source.secondMaterial,
                                    StateDetail::CaptureStateSurface(source.firstSurface),
                                    StateDetail::CaptureStateSurface(source.secondSurface),
                                    source.contact,
                                    source.sensor};
        if (value.second < value.first)
            Reverse(value);
        if (!(value.first < value.second))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        return Outcome::Success(value);
    }

    /** @copydoc RestorePhysicsStatePair */
    Result<PhysicsEventProjectionPairState> RestorePhysicsStatePair(const CanonicalPhysicsStateWorkspace &workspace,
                                                                    const PhysicsStateEventPair &source) {
        using Outcome = Result<PhysicsEventProjectionPairState>;
        if (!(source.first < source.second) || source.contact.pointCount > MaximumPhysicsContactPoints)
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        auto first = workspace.ResolveEventEndpoint(source.first, source.firstMaterial, source.firstSurface);
        if (first.HasError())
            return Outcome::Failure(first.ErrorValue());
        auto second = workspace.ResolveEventEndpoint(source.second, source.secondMaterial, source.secondSurface);
        if (second.HasError())
            return Outcome::Failure(second.ErrorValue());
        auto firstSurface = workspace.ResolveStateSurface(source.firstSurface);
        if (firstSurface.HasError())
            return Outcome::Failure(firstSurface.ErrorValue());
        auto secondSurface = workspace.ResolveStateSurface(source.secondSurface);
        if (secondSurface.HasError())
            return Outcome::Failure(secondSurface.ErrorValue());
        // Use a wire-shaped local only to share the exact orientation operation; native handles remain native-only.
        PhysicsEventProjectionPairState value{{first.Value(), second.Value()},
                                              source.firstMaterial,
                                              source.secondMaterial,
                                              firstSurface.Value(),
                                              secondSurface.Value(),
                                              source.contact,
                                              source.sensor};
        if (value.pair.second < value.pair.first) {
            std::swap(value.pair.first, value.pair.second);
            std::swap(value.firstMaterial, value.secondMaterial);
            std::swap(value.firstSurface, value.secondSurface);
            ReverseContact(value.contact);
        }
        return Outcome::Success(value);
    }
}  // namespace Horo::Physics::Detail
