#pragma once

/** @file CharacterFastPathStorage.h
 * @brief Target-private fixed-capacity Character command, query and output storage.
 */

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
#include <utility>
#include <vector>

namespace Horo::Character::Detail {
    /** @brief Result of one owner-thread append into a bounded Character collection. */
    enum class CharacterFastPathAppendStatus : std::uint8_t {
        Appended,
        ReplacedWorst,
        RejectedFull,
        RejectedInvalid,
    };

    /** @brief Physical fact kinds retained by the Character event fast path. */
    enum class CharacterFastPathEventKind : std::uint8_t {
        Landed,
        LeftGround,
        HitWall,
        HitCeiling,
        SurfaceChanged,
        SlideStart,
        SlideEnd,
    };

    /** @brief Target-private bounded physical fact record with a total canonical order. */
    struct CharacterFastPathEvent final {
        CharacterControllerHandle controller;
        std::uint64_t tick{};
        std::uint64_t ordinal{};
        CharacterFastPathEventKind kind{CharacterFastPathEventKind::Landed};
        std::optional<CharacterSurfaceContact> contact;
        Math::Vec3 impactVelocity{};
    };

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
        /** @brief Reserves all command, contact, hit, event, impulse and byte-scratch storage. */
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
            contacts_.reserve(capacities_.contacts);
            hits_.reserve(capacities_.hits);
            events_.reserve(capacities_.events);
            impulses_.reserve(capacities_.impulses);
            scratch_.resize(capacities_.scratch);
        }

        CharacterFastPathStorage(const CharacterFastPathStorage &) = delete;
        CharacterFastPathStorage &operator=(const CharacterFastPathStorage &) = delete;

        CharacterFastPathStorage(CharacterFastPathStorage &&) = delete;
        CharacterFastPathStorage &operator=(CharacterFastPathStorage &&) = delete;

        /** @brief Returns the prepared command queue storage owned by the Character world. */
        [[nodiscard]] std::vector<CharacterMovementRequest> &Commands() noexcept {
            return commands_;
        }

        /** @brief Returns the prepared command queue storage through a read-only view. */
        [[nodiscard]] const std::vector<CharacterMovementRequest> &Commands() const noexcept {
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
            return AppendReduced(hits_, capacities_.hits, hit, HitLess, counters_.hitCount, counters_.hitOverflowCount, IsValidHit(hit));
        }

        /**
         * @brief Retains one physical fact in canonical event order.
         * @param event Event evidence copied into world-owned storage.
         * @return Append/replacement/full/invalid disposition without allocation.
         */
        [[nodiscard]] CharacterFastPathAppendStatus TryAppendEvent(const CharacterFastPathEvent &event) noexcept {
            return AppendReduced(events_, capacities_.events, event, EventLess, counters_.eventCount, counters_.eventOverflowCount,
                                 IsValidEvent(event));
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
            if (byteCount > capacities_.scratch - start) {
                SaturatingIncrement(counters_.scratchOverflowCount);
                return std::nullopt;
            }
            scratchOffset_ = start + byteCount;
            counters_.scratchBytesUsed.store(scratchOffset_);
            return std::span<std::byte>{aligned, byteCount};
        }

        /** @brief Sorts every collected domain by its stable Horo tie-break without allocating. */
        void Canonicalize() noexcept {
            std::ranges::sort(contacts_, ContactLess);
            std::ranges::sort(hits_, HitLess);
            std::ranges::sort(events_, EventLess);
            std::ranges::sort(impulses_, ImpulseLess);
        }

        /** @brief Clears per-tick command scratch, contacts, hits, impulses and byte scratch. */
        void ResetTransient() noexcept {
            commandScratch_.clear();
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

        static void SaturatingIncrement(std::atomic<std::uint64_t> &counter) noexcept {
            std::uint64_t current = counter.load();
            while (current != std::numeric_limits<std::uint64_t>::max() && !counter.compare_exchange_weak(current, current + 1U)) {
                // Retry using the value observed by the failed compare-exchange.
            }
        }

        [[nodiscard]] static bool IsValidContact(const CharacterSurfaceContact &contact) noexcept {
            return contact.shape.IsValid() && (!contact.body.has_value() || contact.body->IsValid()) &&
                   (!contact.body.has_value() || contact.body->world == contact.shape.world) && Math::IsFinite(contact.point) &&
                   IsUnit(contact.normal) && IsValidMaterial(contact.material) && std::isfinite(contact.penetrationDepthMeters) &&
                   contact.penetrationDepthMeters >= 0.0F;
        }

        [[nodiscard]] static bool IsValidHit(const Physics::PhysicsQueryHit &hit) noexcept {
            return hit.body.IsValid() && hit.shape.IsValid() && hit.body.world == hit.shape.world && hit.channel.IsValid() &&
                   hit.profile.IsValid() && hit.layer.IsValid() && hit.filterSchemaGeneration != 0 &&
                   CharacterFastPathIsSupported(hit.response) && Math::IsFinite(hit.position) &&
                   (!hit.normal.has_value() || IsUnit(*hit.normal)) && (!hit.subshape.has_value() || hit.subshape->IsValid()) &&
                   (!hit.material.has_value() || IsValidMaterial(*hit.material)) && std::isfinite(hit.distanceMeters) &&
                   hit.distanceMeters >= 0.0F;
        }

        [[nodiscard]] static bool IsValidEvent(const CharacterFastPathEvent &event) noexcept {
            return event.controller.IsValid() && event.tick != 0 && event.ordinal != 0 &&
                   event.kind <= CharacterFastPathEventKind::SlideEnd && Math::IsFinite(event.impactVelocity) &&
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

        [[nodiscard]] static bool IsValidMaterial(const Physics::PhysicsQueryMaterial &material) noexcept {
            return material.asset.IsValid() && material.assetGeneration != 0 && material.slot.IsValid();
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
            return CharacterFastPathMaterialLess(left.material, right.material);
        }

        [[nodiscard]] static bool HitLess(const Physics::PhysicsQueryHit &left, const Physics::PhysicsQueryHit &right) noexcept {
            return Physics::PhysicsQueryHitLess(left, right);
        }

        [[nodiscard]] static bool EventLess(const CharacterFastPathEvent &left, const CharacterFastPathEvent &right) noexcept {
            if (const auto order = left.controller <=> right.controller; order != 0)
                return order < 0;
            if (left.tick != right.tick)
                return left.tick < right.tick;
            if (left.kind != right.kind)
                return left.kind < right.kind;
            if (left.ordinal != right.ordinal)
                return left.ordinal < right.ordinal;
            if (left.contact.has_value() != right.contact.has_value())
                return !left.contact.has_value();
            if (left.contact.has_value()) {
                if (ContactLess(*left.contact, *right.contact))
                    return true;
                if (ContactLess(*right.contact, *left.contact))
                    return false;
            }
            return left.impactVelocity < right.impactVelocity;
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

        std::vector<CharacterMovementRequest> commands_;
        std::vector<CharacterMovementRequest> commandScratch_;
        std::vector<CharacterSurfaceContact> contacts_;
        std::vector<Physics::PhysicsQueryHit> hits_;
        std::vector<CharacterFastPathEvent> events_;
        std::vector<CharacterFastPathImpulse> impulses_;
        std::vector<std::byte> scratch_;
        CharacterFastPathCapacities capacities_{};
        std::size_t scratchOffset_{};
        CharacterFastPathCounters counters_{};
    };
}  // namespace Horo::Character::Detail
