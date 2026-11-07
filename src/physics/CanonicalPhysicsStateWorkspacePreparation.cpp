#include "CanonicalPhysicsStateAccounting.h"
#include "CanonicalPhysicsStateBackendInternal.h"
#include "CanonicalPhysicsStateCloneInternal.h"
#include "CanonicalPhysicsStateColdCost.h"
#include "CanonicalPhysicsStateConstructionExport.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Reports actual cold reservation components using bounded stack formatting. */
        Error ReservationBudgetError(std::size_t maximum, std::size_t native, std::size_t source, std::size_t candidate) {
            std::array<char, 192> message{};
            auto *next = message.data();
            const auto *end = message.data() + message.size();
            constexpr std::array<std::string_view, 4> labels{"Physics state retention ceiling=", " native requests=", " source owner=",
                                                             " candidate owner="};
            const std::array values{maximum, native, source, candidate};
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (labels[index].size() > static_cast<std::size_t>(end - next))
                    return MakeError(PhysicsErrors::CapacityExceeded);
                next = std::copy(labels[index].begin(), labels[index].end(), next);
                const auto converted = std::to_chars(next, message.data() + message.size(), values[index]);
                if (converted.ec != std::errc{})
                    return MakeError(PhysicsErrors::CapacityExceeded);
                next = converted.ptr;
            }
            return MakeError(PhysicsErrors::CapacityExceeded, {message.data(), static_cast<std::size_t>(next - message.data())});
        }

        /** @brief Actual cumulative native requests plus both Horo owner capacities are conservative retained reservations, not RSS. */
        Result<std::size_t> CandidateReservation(const CanonicalWorld &source, const CanonicalWorld &candidate,
                                                 const CanonicalPhysicsStateColdCost &cost, std::size_t maximum) {
            using Outcome = Result<std::size_t>;
            if (cost.overflow || cost.requestedBytes > maximum)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            auto oldOwner = CanonicalPhysicsStateOwnerPayloadBytes(source);
            if (oldOwner.HasError())
                return oldOwner;
            auto newOwner = CanonicalPhysicsStateOwnerPayloadBytes(candidate);
            if (newOwner.HasError())
                return newOwner;
            if (oldOwner.Value() > maximum - cost.requestedBytes)
                return Outcome::Failure(ReservationBudgetError(maximum, cost.requestedBytes, oldOwner.Value(), newOwner.Value()));
            const auto subtotal = cost.requestedBytes + oldOwner.Value();
            if (newOwner.Value() > maximum - subtotal)
                return Outcome::Failure(ReservationBudgetError(maximum, cost.requestedBytes, oldOwner.Value(), newOwner.Value()));
            return Outcome::Success(subtotal + newOwner.Value());
        }

        bool Joined(const CanonicalWorld &world, const PhysicsWorldSettings &settings) noexcept {
            return world.native.system && world.settingsIdentity == settings.Identity() && world.pendingBodyBatch.expired() &&
                   !world.contactRoute.load();
        }
    }  // namespace

    /** @copydoc CanonicalPhysicsStateWorkspace::Impl::PrepareCandidate */
    Result<void> CanonicalPhysicsStateWorkspace::Impl::PrepareCandidate(const CanonicalWorld &world, std::size_t maximumBytes) {
        CanonicalPhysicsStateColdCost cost;
        auto cloned = [&] {
            CanonicalPhysicsStateColdCostScope observation{cost};
            if (!observation.Admitted())
                return Result<CanonicalWorldHandle>::Failure(MakeError(PhysicsErrors::InvalidState));
            return CloneCanonicalPhysicsStateTopology(owner, settings);
        }();
        if (cloned.HasError())
            return Result<void>::Failure(cloned.ErrorValue());
        candidate.reset(static_cast<CanonicalWorld *>(cloned.Value().value));
        auto reservation = CandidateReservation(world, *candidate, cost, maximumBytes);
        if (reservation.HasError())
            return Result<void>::Failure(reservation.ErrorValue());
        auto sourceMap = PrepareCanonicalPhysicsStateTopologyMap(world, bindings.View(), shapes);
        if (sourceMap.HasError())
            return Result<void>::Failure(sourceMap.ErrorValue());
        auto clonedMap = PrepareCanonicalPhysicsStateTopologyMap(*candidate, bindings.View(), shapes);
        if (clonedMap.HasError())
            return Result<void>::Failure(clonedMap.ErrorValue());
        ownerMap = std::move(sourceMap).Value();
        candidateMap = std::move(clonedMap).Value();
        auto originalTopology = CanonicalPhysicsStateTopologyFingerprint(world, bindings.View(), ownerMap);
        if (originalTopology.HasError())
            return Result<void>::Failure(originalTopology.ErrorValue());
        auto replayedTopology = CanonicalPhysicsStateTopologyFingerprint(*candidate, bindings.View(), candidateMap);
        if (replayedTopology.HasError())
            return Result<void>::Failure(replayedTopology.ErrorValue());
        if (originalTopology.Value() != replayedTopology.Value())
            return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported,
                                                   "The retained native candidate does not reproduce exact immutable construction state."));
        candidateReservation = reservation.Value();
        return Result<void>::Success();
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::Impl::PreparePayloads */
    Result<void> CanonicalPhysicsStateWorkspace::Impl::PreparePayloads(const CanonicalWorld &world,
                                                                       const PhysicsStateCaptureConfiguration &configuration,
                                                                       const CanonicalPhysicsStateBounds &bounds) {
        if (auto inventory = PrepareInventories(world, configuration); inventory.HasError())
            return inventory;
        if (auto buffers = PrepareBuffers(bounds, configuration.maximumRetainedBytes); buffers.HasError())
            return buffers;
        if (auto native = PrepareCandidate(world, configuration.maximumRetainedBytes); native.HasError())
            return native;
        auto manifest = PrepareCanonicalConstructionManifest(world, bindings.View(), ownerMap, configuration.maximumRetainedBytes,
                                                             configuration.owners);
        if (manifest.HasError())
            return Result<void>::Failure(manifest.ErrorValue());
        construction = std::move(manifest).Value();
        return Result<void>::Success();
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::Prepare */
    Result<std::unique_ptr<CanonicalPhysicsStateWorkspace>> CanonicalPhysicsStateWorkspace::Prepare(
        CanonicalWorldHandle handle, const PhysicsWorldSettings &settings, const PhysicsStateCaptureConfiguration &configuration) noexcept {
        using Outcome = Result<std::unique_ptr<CanonicalPhysicsStateWorkspace>>;
        const auto *world = static_cast<const CanonicalWorld *>(handle.value);
        if (!world || !Joined(*world, settings))
            return Outcome::Failure(MakeError(PhysicsErrors::InvalidState));
        auto bounds = PlanCanonicalPhysicsStateBounds(settings, configuration.maximumRetainedBytes);
        if (bounds.HasError())
            return Outcome::Failure(bounds.ErrorValue());
        auto value = std::make_unique<Impl>(settings);
        value->owner = handle;
        if (auto prepared = value->PreparePayloads(*world, configuration, bounds.Value()); prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
        auto workspace = std::unique_ptr<CanonicalPhysicsStateWorkspace>{new CanonicalPhysicsStateWorkspace{std::move(value)}};
        const auto resourceBytes = CanonicalPhysicsStateResourceBytes(workspace->Resources());
        if (resourceBytes > configuration.maximumRetainedBytes ||
            workspace->RetainedPayloadBytes() > configuration.maximumRetainedBytes - resourceBytes)
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        return Outcome::Success(std::move(workspace));
    }
}  // namespace Horo::Physics::Detail
