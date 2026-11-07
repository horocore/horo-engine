#include "PhysicsStateEnvelopeInternal.h"

namespace Horo::Physics::StateDetail {
    using namespace EnvelopeDetail;

    namespace {
        /** @brief Required section versions and exact complete length reject before
         * staging mutation. */
        Result<SnapshotHeader> ReadHeader(std::span<const std::byte> input) {
            SnapshotHeader header;
            Wire prefix{input};
            SnapshotHeaderFields(prefix, header);
            std::size_t size{};
            if (prefix.Failed() || header.tag != SnapshotHeader::Tag || header.version != PhysicsStateSnapshotVersion ||
                header.worldVersion != 1 || !SnapshotWireSize(header, input.size(), size) || size != input.size())
                return Result<SnapshotHeader>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            if (auto valid = ValidatePhysicsStateCompatibility(header.compatibility); valid.HasError())
                return Result<SnapshotHeader>::Failure(valid.ErrorValue());
            return Result<SnapshotHeader>::Success(header);
        }

        bool WorldDisjoint(std::span<const std::byte> input, std::span<PhysicsStateEventPair> pairs,
                           std::span<PhysicsStatePublishedEvent> events, std::span<std::uint8_t> ordinals) noexcept {
            return !Aliases(input, pairs) && !Aliases(input, events) && !Aliases(pairs, events) && !Aliases(ordinals, input) &&
                   !Aliases(ordinals, pairs) && !Aliases(ordinals, events);
        }

        /** @brief No envelope row/input may overlap any solver destination, including
         * its validation scratch. */
        bool Disjoint(std::span<const std::byte> input, const SolverStorage &solver, std::span<PhysicsStateEventPair> pairs,
                      std::span<PhysicsStatePublishedEvent> events, std::span<std::uint8_t> ordinals) noexcept {
            const std::array storage{std::as_bytes(std::span{solver.bodies}),           std::as_bytes(std::span{solver.activeBodyOrder}),
                                     std::as_bytes(std::span{solver.constraints}),      std::as_bytes(std::span{solver.pairs}),
                                     std::as_bytes(std::span{solver.manifolds}),        std::as_bytes(std::span{solver.ccdManifolds}),
                                     std::as_bytes(std::span{solver.activeBodyScratch})};
            for (auto bytes : storage)
                if (Aliases(bytes, input) || Aliases(bytes, pairs) || Aliases(bytes, events) || Aliases(bytes, ordinals))
                    return false;
            return WorldDisjoint(input, pairs, events, ordinals);
        }

        /** @brief Canonical comparison rejects ignored absent-optional bytes without
         * another allocation or second wire copy. */
        Result<PhysicsStateWorldView> ReadWorld(std::span<const std::byte> input, const SnapshotHeader &header,
                                                std::span<PhysicsStateEventPair> pairs, std::span<PhysicsStatePublishedEvent> events,
                                                std::span<std::uint8_t> ordinals) {
            auto usedPairs = pairs.first(header.pairCount);
            auto usedEvents = events.first(header.eventCount);
            const auto payload = input.subspan(SnapshotHeader::Bytes + static_cast<std::size_t>(header.solverBytes) +
                                               static_cast<std::size_t>(header.constructionBytes));
            Wire rows{payload};
            for (auto &pair : usedPairs)
                EventPair(rows, pair);
            for (auto &event : usedEvents)
                PublishedEvent(rows, event);
            auto canonical = Wire::Comparison(payload);
            Rows(canonical, std::span<const PhysicsStateEventPair>{usedPairs}, std::span<const PhysicsStatePublishedEvent>{usedEvents});
            PhysicsStateWorldView world{header.completedTick,
                                        header.continuousCollision,
                                        header.appliedCommands,
                                        header.droppedEventCount,
                                        usedPairs,
                                        usedEvents};
            if (!rows.Complete() || !canonical.Complete())
                return Result<PhysicsStateWorldView>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            if (auto valid = ValidateWorld(world, ordinals); valid.HasError())
                return Result<PhysicsStateWorldView>::Failure(valid.ErrorValue());
            return Result<PhysicsStateWorldView>::Success(world);
        }
    }  // namespace

    /** @copydoc InspectSnapshotEnvelope */
    Result<SnapshotEnvelopeSections> InspectSnapshotEnvelope(std::span<const std::byte> input) {
        auto parsed = ReadHeader(input);
        if (parsed.HasError())
            return Result<SnapshotEnvelopeSections>::Failure(parsed.ErrorValue());
        const auto &header = parsed.Value();
        const auto solverBytes = static_cast<std::size_t>(header.solverBytes);
        const auto constructionBytes = static_cast<std::size_t>(header.constructionBytes);
        const auto constructionOffset = SnapshotHeader::Bytes + solverBytes;
        return Result<SnapshotEnvelopeSections>::Success({header, input.subspan(SnapshotHeader::Bytes, solverBytes),
                                                          input.subspan(constructionOffset, constructionBytes),
                                                          input.subspan(constructionOffset + constructionBytes)});
    }

    /** @copydoc DecodeSnapshotEnvelope */
    Result<ParsedSnapshotEnvelope> DecodeSnapshotEnvelope(std::span<const std::byte> input, SolverStorage &solver,
                                                          std::span<PhysicsStateEventPair> pairs,
                                                          std::span<PhysicsStatePublishedEvent> events,
                                                          std::span<std::uint8_t> eventOrdinalScratch) {
        using Outcome = Result<ParsedSnapshotEnvelope>;
        auto parsed = ReadHeader(input);
        if (parsed.HasError())
            return Outcome::Failure(parsed.ErrorValue());
        const auto &header = parsed.Value();
        if (header.pairCount > pairs.size() || header.eventCount > events.size() || header.eventCount > eventOrdinalScratch.size())
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (!Disjoint(input, solver, pairs, events, eventOrdinalScratch))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        auto decoded = DecodeSolverState(input.subspan(SnapshotHeader::Bytes, static_cast<std::size_t>(header.solverBytes)), solver);
        if (decoded.HasError())
            return Outcome::Failure(decoded.ErrorValue());
        auto world = ReadWorld(input, header, pairs, events, eventOrdinalScratch);
        if (world.HasError())
            return Outcome::Failure(world.ErrorValue());
        const auto construction = input.subspan(SnapshotHeader::Bytes + static_cast<std::size_t>(header.solverBytes),
                                                static_cast<std::size_t>(header.constructionBytes));
        return Outcome::Success({header.compatibility, decoded.Value(), world.Value(), construction});
    }
}  // namespace Horo::Physics::StateDetail
