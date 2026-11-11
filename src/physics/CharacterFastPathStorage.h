#pragma once

/** @file CharacterFastPathStorage.h
 * @brief Target-private fixed-capacity Character command, query and output storage.
 */

#include "CharacterEventProjectionInternal.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Physics/CharacterControllerContracts.h"
#include "Horo/Physics/CharacterWorldSettings.h"
#include "Horo/Physics/PhysicsQuery.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace Horo::Character::Detail {
    /** @brief Owned intent plus a producer-lifetime fence sampled once at command closure. */
    struct CharacterQueuedMovement final {
        CharacterMovementRequest request;
        CancellationToken revocation;
    };
    /** @brief Result of one owner-thread append into a bounded Character collection. */
    enum class CharacterFastPathAppendStatus : std::uint8_t {
        Appended,
        ReplacedWorst,
        RejectedFull,
        RejectedInvalid,
    };

    using CharacterFastPathEventKind = CharacterMovementEventKind;
    using CharacterFastPathEvent = CharacterMovementEvent;

    /** @brief Target-private staged impulse evidence; Physics owns the eventual mutation. */
    struct CharacterFastPathImpulse final {
        Physics::BodyHandle body;
        std::uint64_t sequence{};
        Math::Vec3 point{};
        Math::Vec3 impulse{};
    };

    /** @brief Allocation-free snapshot of bounded Character fast-path occupancy and pressure. */
    struct CharacterFastPathSnapshot final {
        std::uint32_t retainedContacts{};
        std::uint32_t queuedHits{};
        std::uint32_t queuedEvents{};
        std::uint32_t stagedImpulses{};
        std::uint64_t scratchBytesUsed{};
        std::uint64_t contactOverflowCount{};
        std::uint64_t hitOverflowCount{};
        std::uint64_t eventOverflowCount{};
        std::uint64_t impulseOverflowCount{};
        std::uint64_t scratchOverflowCount{};
        std::uint64_t invalidInputCount{};
    };

    struct CharacterFastPathCapacities final {
        std::size_t scratch{};
        std::size_t contacts{};
        std::size_t hits{};
        std::size_t events{};
        std::size_t impulses{};
    };

    /** @brief Atomic telemetry only; owner-thread vectors remain externally synchronized. */
    struct CharacterFastPathCounters final {
        std::atomic<std::uint32_t> contactCount{};
        std::atomic<std::uint32_t> hitCount{};
        std::atomic<std::uint32_t> eventCount{};
        std::atomic<std::uint32_t> impulseCount{};
        std::atomic<std::uint64_t> scratchBytesUsed{};
        std::atomic<std::uint64_t> contactOverflowCount{};
        std::atomic<std::uint64_t> hitOverflowCount{};
        std::atomic<std::uint64_t> eventOverflowCount{};
        std::atomic<std::uint64_t> impulseOverflowCount{};
        std::atomic<std::uint64_t> scratchOverflowCount{};
        std::atomic<std::uint64_t> invalidInputCount{};
    };

    [[nodiscard]] inline bool CharacterFastPathIsSupported(const Physics::PhysicsQueryResponse response) noexcept {
        return response == Physics::PhysicsQueryResponse::Overlap || response == Physics::PhysicsQueryResponse::Block;
    }

    [[nodiscard]] inline bool CharacterFastPathMaterialLess(const Physics::PhysicsQueryMaterial &left,
                                                            const Physics::PhysicsQueryMaterial &right) noexcept {
        if (const auto order = left.asset <=> right.asset; order != 0)
            return order < 0;
        if (left.assetGeneration != right.assetGeneration)
            return left.assetGeneration < right.assetGeneration;
        return left.slot < right.slot;
    }

    /** @brief Orders optional copied physical evidence with absence before an exact asset generation and slot. */
    [[nodiscard]] inline bool CharacterFastPathOptionalMaterialLess(const std::optional<Physics::PhysicsQueryMaterial> &left,
                                                                    const std::optional<Physics::PhysicsQueryMaterial> &right) noexcept {
        if (left.has_value() && right.has_value())
            return CharacterFastPathMaterialLess(*left, *right);
        return !left.has_value() && right.has_value();
    }

    [[nodiscard]] inline bool CharacterFastPathMaterialIsValid(const Physics::PhysicsQueryMaterial &material) noexcept {
        return material.asset.IsValid() && material.assetGeneration != 0 && material.slot.IsValid();
    }

    /** @brief Checks the retained contact's finite geometry independently of Physics provenance. */
    [[nodiscard]] inline bool CharacterFastPathIsValidContactGeometry(const CharacterSurfaceContact &contact,
                                                                      const bool normalIsUnit) noexcept {
        if (!Math::IsFinite(contact.point) || !normalIsUnit)
            return false;
        return std::isfinite(contact.penetrationDepthMeters) && contact.penetrationDepthMeters >= 0.0F;
    }

    /** @brief Checks the bounded contact's optional Physics child and explicit material provenance. */
    [[nodiscard]] inline bool CharacterFastPathIsValidContactProvenance(const CharacterSurfaceContact &contact) noexcept {
        return (!contact.subshape.has_value() || contact.subshape->IsValid()) &&
               (contact.materialSource == CharacterMaterialSource::Query ||
                contact.materialSource == CharacterMaterialSource::DescriptorFallback);
    }

    /** @brief Require finite optional impact and complete independent semantic provenance before sorting. */
    [[nodiscard]] inline bool CharacterFastPathContactPayloadIsValid(const CharacterSurfaceContact &contact) noexcept {
        if (contact.impactVelocityMetersPerSecond && !Math::IsFinite(*contact.impactVelocityMetersPerSecond))
            return false;
        if (!contact.surface)
            return contact.surfaceSource == CharacterSemanticSurfaceSource::None;
        if (contact.surfaceSource != CharacterSemanticSurfaceSource::Query &&
            contact.surfaceSource != CharacterSemanticSurfaceSource::DescriptorDefault)
            return false;
        const auto &surface = *contact.surface;
        return surface.surface.IsValid() && surface.catalogRevision != 0 && surface.mappingGeneration != 0 &&
               surface.catalogFingerprint != Sha256Digest{};
    }

    /** @brief Complete copied payload tie-break after contact identity/geometry has compared equal. */
    [[nodiscard]] inline bool CharacterFastPathContactPayloadLess(const CharacterSurfaceContact &left,
                                                                  const CharacterSurfaceContact &right) noexcept {
        return std::tie(left.materialSource, left.material.asset, left.material.assetGeneration, left.material.slot, left.surface,
                        left.surfaceSource, left.impactVelocityMetersPerSecond) <
               std::tie(right.materialSource, right.material.asset, right.material.assetGeneration, right.material.slot, right.surface,
                        right.surfaceSource, right.impactVelocityMetersPerSecond);
    }

    /**
     * @brief Owns every fixed-capacity Character hot-path collection for one prepared world.
     *
     * All backing storage is reserved during world preparation. Append operations never grow a
     * vector: once a collection is full they retain the canonical best `capacity` records and
     * count the rejected/reduced evidence. The owning world mutates contacts, hits, events,
     * impulses and scratch only on its owner thread; the command queue is protected by the
     * CharacterWorld command lock. The atomic counters support read-only telemetry snapshots but
     * do not make record mutation concurrent-safe, so callers crossing the owner boundary must
     * provide external synchronization.
     */
    class CharacterFastPathStorage final {
    public:
        /** @brief Reserves all command, movement-result, contact, hit, event, impulse and byte-scratch storage. */
        explicit CharacterFastPathStorage(const CharacterWorldSettings &settings)
            : capacities_{.scratch = static_cast<std::size_t>(settings.Values().work.scratchBytes),
                          .contacts = settings.Values().capacities.maximumRetainedContacts,
                          .hits = settings.Values().capacities.maximumQueuedQueries,
                          .events = settings.Values().capacities.maximumQueuedEvents,
                          .impulses = settings.Values().capacities.maximumStagedImpulses} {
            const CharacterWorldCapacities &capacities = settings.Values().capacities;
            const CharacterWorldWorkBudgets &work = settings.Values().work;
            commands_.reserve(capacities.maximumQueuedCommands);
            commandScratch_.reserve(work.maximumCommandsPerTick);
            movementResults_.reserve(work.maximumCommandsPerTick);
            contacts_.reserve(capacities_.contacts);
            hits_.reserve(capacities_.hits);
            events_.reserve(capacities_.events);
            publishedEvents_.reserve(capacities_.events);
            terminalStates_.reserve(capacities.maximumControllers);
            publishedTerminalStates_.reserve(capacities.maximumControllers);
            impulses_.reserve(capacities_.impulses);
            scratch_.resize(capacities_.scratch);
        }

        CharacterFastPathStorage(const CharacterFastPathStorage &) = delete;
        CharacterFastPathStorage &operator=(const CharacterFastPathStorage &) = delete;

        CharacterFastPathStorage(CharacterFastPathStorage &&) = delete;
        CharacterFastPathStorage &operator=(CharacterFastPathStorage &&) = delete;

        /** @brief Returns the prepared command queue storage owned by the Character world. */
        [[nodiscard]] std::vector<CharacterQueuedMovement> &Commands() noexcept {
            return commands_;
        }

        /** @brief Returns the prepared command queue storage through a read-only view. */
        [[nodiscard]] const std::vector<CharacterQueuedMovement> &Commands() const noexcept {
            return commands_;
        }

        /** @brief Returns the prepared per-tick command ordering storage. */
        [[nodiscard]] std::vector<CharacterMovementRequest> &CommandScratch() noexcept {
            return commandScratch_;
        }

        /** @brief Returns the prepared per-tick command ordering storage read-only. */
        [[nodiscard]] const std::vector<CharacterMovementRequest> &CommandScratch() const noexcept {
            return commandScratch_;
        }

        /** @brief Returns the prepared per-tick movement-result candidates. */
        [[nodiscard]] std::vector<CharacterMovementResult> &MovementResults() noexcept {
            return movementResults_;
        }

        /** @brief Returns the prepared movement-result candidates read-only. */
        [[nodiscard]] const std::vector<CharacterMovementResult> &MovementResults() const noexcept {
            return movementResults_;
        }

        /** @brief Returns the total command capacity reserved during preparation. */
        [[nodiscard]] std::size_t CommandCapacity() const noexcept {
            return commands_.capacity();
        }

        /** @brief Returns the command scratch capacity reserved during preparation. */
        [[nodiscard]] std::size_t CommandScratchCapacity() const noexcept {
            return commandScratch_.capacity();
        }

        /** @brief Returns retained contact evidence in the last canonicalized order. */
        [[nodiscard]] std::span<const CharacterSurfaceContact> Contacts() const noexcept {
            return contacts_;
        }

        /** @brief Returns retained query-hit evidence in the last canonicalized order. */
        [[nodiscard]] std::span<const Physics::PhysicsQueryHit> Hits() const noexcept {
            return hits_;
        }

        /** @brief Returns retained physical facts in the last canonicalized order. */
        [[nodiscard]] std::span<const CharacterFastPathEvent> Events() const noexcept {
            return events_;
        }

        /**
         * @brief Retains one validated contact or the canonical better record when full.
         * @param contact Contact evidence copied into world-owned storage.
         * @return Append/replacement/full/invalid disposition without allocation.
         */
        [[nodiscard]] CharacterFastPathAppendStatus TryAppendContact(const CharacterSurfaceContact &contact) noexcept {
            return AppendReduced(contacts_, capacities_.contacts, contact, ContactLess, counters_.contactCount,
                                 counters_.contactOverflowCount, IsValidContact(contact));
        }

        /**
         * @brief Retains one validated query hit or the canonical closer record when full.
         * @param hit Query evidence copied into world-owned storage.
         * @return Append/replacement/full/invalid disposition without allocation.
         */
        [[nodiscard]] CharacterFastPathAppendStatus TryAppendHit(const Physics::PhysicsQueryHit &hit) noexcept {
            return AppendReduced(hits_, capacities_.hits, hit, Physics::PhysicsQueryHitLess, counters_.hitCount, counters_.hitOverflowCount,
                                 IsValidHit(hit));
        }

        /**
         * @brief Retains one physical fact in canonical event order.
         * @param event Event evidence copied into world-owned storage.
         * @return Append/replacement/full/invalid disposition without allocation.
         */
        [[nodiscard]] CharacterFastPathAppendStatus TryAppendEvent(const CharacterFastPathEvent &event) noexcept {
            const auto status = AppendReduced(events_, capacities_.events, event, CharacterMovementEventRetainBefore, counters_.eventCount,
                                              counters_.eventOverflowCount, IsValidEvent(event));
            if (status != CharacterFastPathAppendStatus::RejectedInvalid)
                Increment(stagedEmittedEvents_);
            if (status == CharacterFastPathAppendStatus::RejectedFull || status == CharacterFastPathAppendStatus::ReplacedWorst)
                Increment(stagedDroppedEvents_);
            return status;
        }

        /** @brief Reuse prepared buffers for an attempted stream publication; no failed attempt alters the committed journal. */
        void BeginEventFrame() noexcept {
            events_.assign(publishedEvents_.begin(), publishedEvents_.end());
            terminalStates_.clear();
            stagedEmittedEvents_ = 0;
            stagedDroppedEvents_ = 0;
            counters_.eventCount.store(static_cast<std::uint32_t>(events_.size()));
        }

        /** @brief Controller capacity admission guarantees this immutable terminal projection can always be retained. */
        void StageTerminalState(const CharacterEventTerminalState &state) noexcept {
            terminalStates_.push_back(state);
        }

        /** @brief Publish only after the entire controller transaction succeeds, using allocation-free prepared swaps. */
        void PublishEventFrame(CharacterEventCapture frame) noexcept {
            events_.swap(publishedEvents_);
            terminalStates_.swap(publishedTerminalStates_);
            cumulativeDroppedEvents_ +=
                std::min(stagedDroppedEvents_, std::numeric_limits<std::uint64_t>::max() - cumulativeDroppedEvents_);
            frame.emittedEvents = stagedEmittedEvents_;
            frame.droppedEvents = stagedDroppedEvents_;
            frame.cumulativeDroppedEvents = cumulativeDroppedEvents_;
            frame.availableTerminalStates = static_cast<std::uint32_t>(publishedTerminalStates_.size());
            publishedEventFrame_ = frame;
        }

        /** @brief Copy only unseen retained facts, preserving a complete terminal projection or rejecting before output mutation. */
        CharacterEventCapture CaptureEvents(std::uint64_t after, std::span<CharacterMovementEvent> output,
                                            std::span<CharacterEventTerminalState> terminal) const noexcept {
            auto frame = publishedEventFrame_;
            if (frame.publicationRevision == 0)
                return frame;
            if (after > frame.publicationRevision) {
                frame.status = CharacterEventCaptureStatus::Stale;
                return frame;
            }
            if (after == frame.publicationRevision) {
                frame.status = CharacterEventCaptureStatus::NoNewPublication;
                return frame;
            }
            if (terminal.size() < publishedTerminalStates_.size()) {
                frame.status = CharacterEventCaptureStatus::CapacityLimited;
                return frame;
            }
            std::ranges::copy(publishedTerminalStates_, terminal.begin());
            frame.copiedTerminalStates = frame.availableTerminalStates;
            CopyEvents(after, output, frame);
            return frame;
        }

        /**
         * @brief Retains one staged Physics impulse in canonical target order.
         * @param impulse Typed impulse evidence copied into world-owned storage.
         * @return Append/replacement/full/invalid disposition without allocation.
         */
        [[nodiscard]] CharacterFastPathAppendStatus TryAppendImpulse(const CharacterFastPathImpulse &impulse) noexcept {
            return AppendReduced(impulses_, capacities_.impulses, impulse, ImpulseLess, counters_.impulseCount,
                                 counters_.impulseOverflowCount, IsValidImpulse(impulse));
        }

        /**
         * @brief Reserves one aligned byte range from the prepared per-tick scratch arena.
         * @param byteCount Positive number of bytes required by one operation.
         * @param alignment Power-of-two alignment supported by the prepared allocation.
         * @return Borrowed bytes valid until the next reset, or empty on invalid/exhausted input.
         */
        [[nodiscard]] std::optional<std::span<std::byte>> TryAllocateScratch(const std::size_t byteCount,
                                                                             const std::size_t alignment) noexcept {
            if (byteCount == 0 || alignment == 0 || !std::has_single_bit(alignment)) {
                SaturatingIncrement(counters_.invalidInputCount);
                return std::nullopt;
            }
            if (scratchOffset_ > capacities_.scratch) {
                SaturatingIncrement(counters_.scratchOverflowCount);
                return std::nullopt;
            }

            void *candidate = scratch_.data() + scratchOffset_;
            if (auto available = capacities_.scratch - scratchOffset_; std::align(alignment, byteCount, candidate, available) == nullptr) {
                SaturatingIncrement(counters_.scratchOverflowCount);
                return std::nullopt;
            }

            auto *const aligned = static_cast<std::byte *>(candidate);
            const auto start = static_cast<std::size_t>(aligned - scratch_.data());
            scratchOffset_ = start + byteCount;
            counters_.scratchBytesUsed.store(scratchOffset_);
            return std::span<std::byte>{aligned, byteCount};
        }

        /** @brief Sorts every collected domain by its stable Horo tie-break without allocating. */
        void Canonicalize() noexcept {
            std::ranges::sort(contacts_, ContactLess);
            std::ranges::sort(hits_, Physics::PhysicsQueryHitLess);
            std::ranges::sort(events_, CharacterMovementEventLess);
            std::ranges::sort(impulses_, ImpulseLess);
        }

        /** @brief Clears per-tick command and result scratch, contacts, hits, impulses and byte scratch. */
        void ResetTransient() noexcept {
            commandScratch_.clear();
            movementResults_.clear();
            contacts_.clear();
            hits_.clear();
            impulses_.clear();
            scratchOffset_ = 0;
            counters_.contactCount.store(0);
            counters_.hitCount.store(0);
            counters_.impulseCount.store(0);
            counters_.scratchBytesUsed.store(0);
        }

        /** @brief Clears every retained record while preserving all prepared backing capacity. */
        void ResetAll() noexcept {
            commands_.clear();
            events_.clear();
            publishedEvents_.clear();
            terminalStates_.clear();
            publishedTerminalStates_.clear();
            publishedEventFrame_ = {};
            ResetTransient();
            counters_.eventCount.store(0);
        }

        /** @brief Returns an allocation-free occupancy and overflow snapshot. */
        [[nodiscard]] CharacterFastPathSnapshot Snapshot() const noexcept {
            return {.retainedContacts = counters_.contactCount.load(),
                    .queuedHits = counters_.hitCount.load(),
                    .queuedEvents = counters_.eventCount.load(),
                    .stagedImpulses = counters_.impulseCount.load(),
                    .scratchBytesUsed = counters_.scratchBytesUsed.load(),
                    .contactOverflowCount = counters_.contactOverflowCount.load(),
                    .hitOverflowCount = counters_.hitOverflowCount.load(),
                    .eventOverflowCount = counters_.eventOverflowCount.load(),
                    .impulseOverflowCount = counters_.impulseOverflowCount.load(),
                    .scratchOverflowCount = counters_.scratchOverflowCount.load(),
                    .invalidInputCount = counters_.invalidInputCount.load()};
        }

    private:
        template <typename T, typename Less>
        [[nodiscard]] CharacterFastPathAppendStatus AppendReduced(std::vector<T> &records, const std::size_t capacity, const T &value,
                                                                  Less less, std::atomic<std::uint32_t> &count,
                                                                  std::atomic<std::uint64_t> &overflow, const bool valid) noexcept {
            using enum CharacterFastPathAppendStatus;
            if (!valid) {
                SaturatingIncrement(counters_.invalidInputCount);
                return RejectedInvalid;
            }
            if (records.size() < capacity) {
                records.push_back(value);
                count.store(static_cast<std::uint32_t>(records.size()));
                return Appended;
            }

            SaturatingIncrement(overflow);
            if (records.empty())
                return RejectedFull;

            if (auto worst = std::ranges::max_element(records, less); worst != records.end() && less(value, *worst)) {
                *worst = value;
                return ReplacedWorst;
            }
            return RejectedFull;
        }

        static void Increment(std::uint64_t &value) noexcept {
            if (value != std::numeric_limits<std::uint64_t>::max())
                ++value;
        }

        void CopyEvents(std::uint64_t after, std::span<CharacterMovementEvent> output, CharacterEventCapture &frame) const noexcept {
            for (const auto &event : publishedEvents_) {
                if (event.streamPublicationRevision <= after)
                    continue;
                if (frame.copiedEvents < output.size())
                    output[frame.copiedEvents++] = event;
                ++frame.availableEvents;
            }
            frame.readDroppedEvents = frame.availableEvents - frame.copiedEvents;
            frame.status = frame.readDroppedEvents ? CharacterEventCaptureStatus::CapacityLimited : CharacterEventCaptureStatus::Captured;
        }

        static void SaturatingIncrement(std::atomic<std::uint64_t> &counter) noexcept {
            std::uint64_t current = counter.load();
            while (current != std::numeric_limits<std::uint64_t>::max() && !counter.compare_exchange_weak(current, current + 1U)) {
                // A failed compare-exchange refreshes current; the empty body intentionally retries the saturating increment.
            }
        }

        [[nodiscard]] static bool IsValidContact(const CharacterSurfaceContact &contact) noexcept {
            return contact.shape.IsValid() && (!contact.body.has_value() || contact.body->IsValid()) &&
                   (!contact.body.has_value() || contact.body->world == contact.shape.world) &&
                   CharacterFastPathIsValidContactGeometry(contact, IsUnit(contact.normal)) &&
                   CharacterFastPathMaterialIsValid(contact.material) && CharacterFastPathIsValidContactProvenance(contact) &&
                   CharacterFastPathContactPayloadIsValid(contact);
        }

        [[nodiscard]] static bool IsValidHit(const Physics::PhysicsQueryHit &hit) noexcept {
            return hit.body.IsValid() && hit.shape.IsValid() && hit.body.world == hit.shape.world && hit.channel.IsValid() &&
                   hit.profile.IsValid() && hit.layer.IsValid() && hit.filterSchemaGeneration != 0 &&
                   CharacterFastPathIsSupported(hit.response) && Math::IsFinite(hit.position) &&
                   (!hit.normal.has_value() || IsUnit(*hit.normal)) && (!hit.subshape.has_value() || hit.subshape->IsValid()) &&
                   (!hit.material.has_value() || CharacterFastPathMaterialIsValid(*hit.material)) && std::isfinite(hit.distanceMeters) &&
                   hit.distanceMeters >= 0.0F;
        }

        /** @brief Correlation fields all identify one actual committed controller/world publication. */
        [[nodiscard]] static bool IsValidEventCorrelation(const CharacterFastPathEvent &event) noexcept {
            return event.controller.IsValid() && event.tick != 0 && event.ordinal != 0 && event.sequence != 0 && event.stateRevision != 0 &&
                   event.publicationRevision != 0 && event.streamPublicationRevision != 0;
        }

        [[nodiscard]] static bool IsValidEvent(const CharacterFastPathEvent &event) noexcept {
            return IsValidEventCorrelation(event) && event.kind <= CharacterFastPathEventKind::SlideEnd &&
                   Math::IsFinite(event.achievedVelocityMetersPerSecond) &&
                   (!event.impactVelocity || Math::IsFinite(*event.impactVelocity)) &&
                   (!event.contact.has_value() || IsValidContact(*event.contact));
        }

        [[nodiscard]] static bool IsValidImpulse(const CharacterFastPathImpulse &impulse) noexcept {
            return impulse.body.IsValid() && impulse.sequence != 0 && Math::IsFinite(impulse.point) && Math::IsFinite(impulse.impulse);
        }

        [[nodiscard]] static bool IsUnit(const Math::Vec3 value) noexcept {
            if (!Math::IsFinite(value))
                return false;
            const double squaredNorm =
                static_cast<double>(value.x) * value.x + static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z;
            return std::abs(squaredNorm - 1.0) <= Physics::PhysicsQueryUnitVectorSquaredNormTolerance;
        }

        [[nodiscard]] static bool ContactLess(const CharacterSurfaceContact &left, const CharacterSurfaceContact &right) noexcept {
            if (left.penetrationDepthMeters != right.penetrationDepthMeters)
                return left.penetrationDepthMeters > right.penetrationDepthMeters;
            if (const auto order = left.body <=> right.body; order != 0)
                return order < 0;
            if (const auto order = left.shape <=> right.shape; order != 0)
                return order < 0;
            if (left.point != right.point)
                return left.point < right.point;
            if (left.normal != right.normal)
                return left.normal < right.normal;
            if (const auto order = left.subshape <=> right.subshape; order != 0)
                return order < 0;
            return CharacterFastPathContactPayloadLess(left, right);
        }

        [[nodiscard]] static bool ImpulseLess(const CharacterFastPathImpulse &left, const CharacterFastPathImpulse &right) noexcept {
            if (const auto order = left.body <=> right.body; order != 0)
                return order < 0;
            if (left.sequence != right.sequence)
                return left.sequence < right.sequence;
            if (left.point != right.point)
                return left.point < right.point;
            return left.impulse < right.impulse;
        }

        std::vector<CharacterQueuedMovement> commands_;
        std::vector<CharacterMovementRequest> commandScratch_;
        std::vector<CharacterMovementResult> movementResults_;
        std::vector<CharacterSurfaceContact> contacts_;
        std::vector<Physics::PhysicsQueryHit> hits_;
        std::vector<CharacterFastPathEvent> events_;
        std::vector<CharacterFastPathEvent> publishedEvents_;
        std::vector<CharacterEventTerminalState> terminalStates_;
        std::vector<CharacterEventTerminalState> publishedTerminalStates_;
        CharacterEventCapture publishedEventFrame_;
        std::uint64_t stagedEmittedEvents_{};
        std::uint64_t stagedDroppedEvents_{};
        std::uint64_t cumulativeDroppedEvents_{};
        std::vector<CharacterFastPathImpulse> impulses_;
        std::vector<std::byte> scratch_;
        CharacterFastPathCapacities capacities_{};
        std::size_t scratchOffset_{};
        CharacterFastPathCounters counters_{};
    };
}  // namespace Horo::Character::Detail
