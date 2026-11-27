#pragma once

#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateEnvelope.h"

#include <algorithm>
#include <limits>
#include <tuple>

namespace Horo::Physics::StateDetail::EnvelopeDetail {
    /** @brief Rejects representation aliasing before writes; subtraction avoids
     * pointer-range overflow. */
    template <class T, class U> bool Aliases(std::span<T> one, std::span<U> two) noexcept {
        if (one.empty() || two.empty())
            return false;
        const auto a = reinterpret_cast<std::uintptr_t>(one.data());
        const auto b = reinterpret_cast<std::uintptr_t>(two.data());
        return a <= b ? b - a < one.size_bytes() : a - b < two.size_bytes();
    }

    inline bool OrdinalScratchDisjoint(std::span<const std::byte> output, std::span<const std::byte> solver,
                                       const PhysicsStateWorldView &world, std::span<std::uint8_t> ordinals) noexcept {
        return !Aliases(ordinals, output) && !Aliases(ordinals, solver) && !Aliases(ordinals, world.previousPairs) &&
               !Aliases(ordinals, world.publishedEvents);
    }

    inline bool EncodeInputsDisjoint(std::span<std::byte> output, std::span<const std::byte> solver, const PhysicsStateWorldView &world,
                                     std::span<std::uint8_t> ordinals, std::span<const std::byte> construction) noexcept {
        return OrdinalScratchDisjoint(output, solver, world, ordinals) && !Aliases(ordinals, construction) && !Aliases(output, solver) &&
               !Aliases(output, world.previousPairs) && !Aliases(output, world.publishedEvents) && !Aliases(output, construction);
    }

    /** @brief Stable event ordinal must be the exact retained emission sequence;
     * event ownership is checked by the actual owner. */
    inline bool ValidEvents(const PhysicsStateWorldView &world, std::span<std::uint8_t> ordinals) noexcept {
        std::ranges::fill(ordinals.first(world.publishedEvents.size()), std::uint8_t{});
        const PhysicsStatePublishedEvent *previous{};
        for (const auto &event : world.publishedEvents) {
            if (event.tick != world.completedTick || event.ordinal >= world.publishedEvents.size() ||
                event.kind > PhysicsEventKind::TriggerExit)
                return false;
            if (ordinals[event.ordinal] != 0)
                return false;
            ordinals[event.ordinal] = 1;
            if (previous && !(std::tie(previous->pair.first, previous->pair.second, previous->ordinal) <
                              std::tie(event.pair.first, event.pair.second, event.ordinal)))
                return false;
            previous = &event;
        }
        return true;
    }

    /** @brief Primitive closed world admission does not grant topology, material or
     * resource authority. */
    inline Result<void> ValidateWorld(const PhysicsStateWorldView &world, std::span<std::uint8_t> ordinals) {
        if (ordinals.size() < world.publishedEvents.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (world.completedTick == 0 || world.previousPairs.size() > std::numeric_limits<std::uint32_t>::max() ||
            world.publishedEvents.size() > std::numeric_limits<std::uint32_t>::max() || !ValidEvents(world, ordinals))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        return ValidatePhysicsContinuousCollisionPolicy(world.continuousCollision);
    }

    /** @brief All fixed rows use the same production field codec for writing,
     * reading and canonical comparison. */
    template <class Pair, class Event> void Rows(Wire &wire, std::span<Pair> pairs, std::span<Event> events) noexcept {
        for (const auto &source : pairs) {
            auto pair = source;
            EventPair(wire, pair);
        }
        for (const auto &source : events) {
            auto event = source;
            PublishedEvent(wire, event);
        }
    }

    /** @brief Exact private header counts are admitted before any scalar serializer
     * is entered. */
    inline SnapshotHeader Header(const PhysicsStateCompatibility &compatibility, std::size_t solverBytes,
                                 const PhysicsStateWorldView &world) noexcept {
        SnapshotHeader header;
        header.compatibility = compatibility;
        header.solverBytes = solverBytes;
        header.completedTick = world.completedTick;
        header.continuousCollision = world.continuousCollision;
        header.appliedCommands = world.appliedCommands;
        header.droppedEventCount = world.droppedEventCount;
        header.pairCount = static_cast<std::uint32_t>(world.previousPairs.size());
        header.eventCount = static_cast<std::uint32_t>(world.publishedEvents.size());
        return header;
    }

    /** @brief Computes closed bounded size without trusting a serialized total. */
    inline bool EncodeSize(SnapshotHeader &header, std::size_t capacity, std::size_t &size) noexcept {
        if (!SnapshotRequiredWireSize(header, capacity, size))
            return false;
        header.totalBytes = size;
        return true;
    }
}  // namespace Horo::Physics::StateDetail::EnvelopeDetail
