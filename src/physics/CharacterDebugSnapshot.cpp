#include "CharacterWorldInternal.h"

namespace Horo::Character {
    namespace {
        /** @brief Rejects incomplete consumer fences before any registry access. */
        [[nodiscard]] bool IsCaptureRequestValid(const CharacterDebugCaptureRequest &request) noexcept {
            return request.controller.IsValid() && request.physicsWorld.IsValid() && request.collisionFilterGeneration != 0 &&
                   request.originGeneration != 0 && request.maximumProbes <= MaximumCharacterDebugProbes &&
                   request.maximumContacts <= MaximumCharacterContacts;
        }

        /** @brief Distinguishes unsupported producer data from bounded observation omission. */
        [[nodiscard]] CharacterDebugProbeAvailability ProbeAvailability(const Detail::CharacterDebugTraceMetadata &metadata,
                                                                        const std::uint32_t storageCapacity,
                                                                        const std::uint32_t retained) noexcept {
            using enum CharacterDebugProbeAvailability;
            if (!metadata.providerSupported)
                return UnsupportedProvider;
            if (storageCapacity == 0)
                return StorageUnavailable;
            return metadata.observed > retained ? CapacityLimited : Complete;
        }

        /** @brief Checks exact consumer fences without adopting another generation. */
        [[nodiscard]] bool MatchesCaptureWorld(const CharacterDebugCaptureRequest &request,
                                               const CharacterWorldDescriptor &world) noexcept {
            return request.controller.sceneGeneration == world.sceneGeneration && request.controller.world == world.identity &&
                   request.physicsWorld == world.physicsWorld && request.collisionFilterGeneration == world.collisionFilterGeneration &&
                   request.originGeneration == world.originGeneration;
        }

        /** @brief Rejects forbidden reads before touching owner-thread registry or publication storage. */
        [[nodiscard]] CharacterDebugCaptureStatus CaptureAdmission(const auto &impl, const CharacterDebugCaptureRequest &request) noexcept {
            using enum CharacterDebugCaptureStatus;
            if (impl.ownerThread != std::this_thread::get_id())
                return WrongThread;
            if (impl.state.load() == CharacterWorldState::Destroyed)
                return Retired;
            if (impl.ticking.load() || impl.placementActive)
                return Busy;
            if (!IsCaptureRequestValid(request))
                return InvalidRequest;
            if (!MatchesCaptureWorld(request, impl.descriptor))
                return ForeignGeneration;
            return Captured;
        }
    }  // namespace

    /** @copydoc CharacterWorld::CaptureDebugSnapshot */
    CharacterDebugCapture CharacterWorld::CaptureDebugSnapshot(const CharacterDebugCaptureRequest &request) const noexcept {
        if (const auto admission = CaptureAdmission(*impl_, request); admission != CharacterDebugCaptureStatus::Captured)
            return {admission, std::nullopt};
        const auto &world = impl_->descriptor;
        const auto *record = impl_->controllers.TryResolve(request.controller);
        if (record == nullptr)
            return {CharacterDebugCaptureStatus::StaleController, std::nullopt};
        if (!record->spawned)
            return {CharacterDebugCaptureStatus::NoPublication, std::nullopt};
        const auto &metadata = impl_->debug.Metadata(request.controller.slot.index);
        const auto sourceTick = record->publication.sourceTick;
        const auto observationTick = std::max(sourceTick, impl_->published.completedTick);
        const auto age = observationTick - sourceTick;
        if (age > request.maximumAgeTicks)
            return {CharacterDebugCaptureStatus::AgeExceeded, std::nullopt};

        CharacterDebugSnapshot snapshot;
        snapshot.identity_ = {request.controller,
                              world.physicsWorld,
                              world.collisionFilterGeneration,
                              world.originGeneration,
                              metadata.physicsSnapshotRevision,
                              observationTick,
                              sourceTick,
                              age,
                              impl_->published.publicationRevision,
                              record->stateRevision,
                              record->publication.publicationRevision};
        snapshot.capsule_ = record->capsule;
        snapshot.probeRetentionCapacity_ = impl_->debug.Capacity();
        snapshot.transform_ = record->publication;
        snapshot.locomotion_ = record->locomotion;
        if (snapshot.locomotion_) {
            auto &movement = snapshot.locomotion_->movement;
            const auto retained = std::min(movement.contactCount, request.maximumContacts);
            snapshot.omittedContacts_ = movement.contactCount - retained;
            movement.contactCount = retained;
            movement.truncated = movement.truncated || snapshot.omittedContacts_ != 0;
            std::fill(movement.contacts.begin() + retained, movement.contacts.end(), CharacterSurfaceContact{});
        }
        const auto probes = impl_->debug.Probes(request.controller.slot.index);
        snapshot.probeCount_ = std::min(static_cast<std::uint32_t>(probes.size()), request.maximumProbes);
        std::copy_n(probes.begin(), snapshot.probeCount_, snapshot.probes_.begin());
        snapshot.omittedProbes_ = metadata.observed - snapshot.probeCount_;
        snapshot.probeAvailability_ = ProbeAvailability(metadata, impl_->debug.Capacity(), snapshot.probeCount_);
        const auto status = snapshot.omittedProbes_ != 0 || snapshot.omittedContacts_ != 0 ? CharacterDebugCaptureStatus::CapacityLimited
                                                                                           : CharacterDebugCaptureStatus::Captured;
        return {status, std::move(snapshot)};
    }
}  // namespace Horo::Character
