#pragma once

#include "PhysicsStateEventFields.h"

#include <limits>

namespace Horo::Physics::StateDetail {
    /** @brief Complete fixed compatibility tuple; no implementation-specific
     * structure bytes are persisted. */
    inline void CompatibilityFields(Wire &wire, PhysicsStateCompatibility &value) noexcept {
        wire.Integer(value.formatVersion);
        wire.Integer(value.solverSchemaVersion);
        wire.Integer(value.settingsSchemaVersion);
        Digest(wire, value.build);
        Digest(wire, value.platform);
        Digest(wire, value.solver);
        Digest(wire, value.settings.digest);
        Digest(wire, value.content);
        Digest(wire, value.topology);
        Digest(wire, value.filters);
        Digest(wire, value.surfaces);
        auto architecture = static_cast<std::uint8_t>(value.floatingPoint.architecture);
        wire.Integer(architecture);
        value.floatingPoint.architecture = static_cast<PhysicsStateFpArchitecture>(architecture);
        wire.Integer(value.floatingPoint.value);
    }

    /** @brief Required historical world continuation values precede bounded typed
     * pair/event rows. */
    struct SnapshotHeader final {
        static constexpr std::uint32_t Tag = 0x31575048;
        static constexpr std::size_t Bytes = 350;
        static constexpr std::size_t PairBytes = 519;
        static constexpr std::size_t EventBytes = 532;
        std::uint32_t tag{Tag}, version{PhysicsStateSnapshotVersion};
        std::uint64_t totalBytes{}, solverBytes{}, constructionBytes{};
        PhysicsStateCompatibility compatibility;
        std::uint32_t worldVersion{1};
        std::uint64_t completedTick{};
        PhysicsContinuousCollisionPolicy continuousCollision;
        std::uint32_t appliedCommands{};
        std::uint64_t droppedEventCount{};
        std::uint32_t pairCount{}, eventCount{};
    };

    /** @brief Fixed header has an exact closed field manifest, including explicit
     * enum representation. */
    inline void SnapshotHeaderFields(Wire &wire, SnapshotHeader &value) noexcept {
        wire.Integer(value.tag);
        wire.Integer(value.version);
        wire.Integer(value.totalBytes);
        wire.Integer(value.solverBytes);
        wire.Integer(value.constructionBytes);
        CompatibilityFields(wire, value.compatibility);
        wire.Integer(value.worldVersion);
        wire.Integer(value.completedTick);
        auto mode = static_cast<std::uint8_t>(value.continuousCollision.defaultMode);
        wire.Integer(mode);
        value.continuousCollision.defaultMode = static_cast<PhysicsDefaultMotionQuality>(mode);
        wire.Float(value.continuousCollision.thresholdFraction);
        wire.Float(value.continuousCollision.penetrationFraction);
        wire.Integer(value.appliedCommands);
        wire.Integer(value.droppedEventCount);
        wire.Integer(value.pairCount);
        wire.Integer(value.eventCount);
    }

    /** @brief Checks every wire count by division before multiplication, on both
     * 32- and 64-bit size_t. */
    inline bool SnapshotRequiredWireSize(const SnapshotHeader &header, std::size_t maximumBytes, std::size_t &size) noexcept {
        if (maximumBytes < SnapshotHeader::Bytes || header.solverBytes > maximumBytes - SnapshotHeader::Bytes)
            return false;
        size = SnapshotHeader::Bytes + static_cast<std::size_t>(header.solverBytes);
        if (header.constructionBytes < 56 || header.constructionBytes > maximumBytes - size)
            return false;
        size += static_cast<std::size_t>(header.constructionBytes);
        if (header.pairCount > (maximumBytes - size) / SnapshotHeader::PairBytes)
            return false;
        size += static_cast<std::size_t>(header.pairCount) * SnapshotHeader::PairBytes;
        if (header.eventCount > (maximumBytes - size) / SnapshotHeader::EventBytes)
            return false;
        size += static_cast<std::size_t>(header.eventCount) * SnapshotHeader::EventBytes;
        return true;
    }

    /** @brief Serialized declared size must exactly match the complete closed field
     * manifest. */
    inline bool SnapshotWireSize(const SnapshotHeader &header, std::size_t maximumBytes, std::size_t &size) noexcept {
        return SnapshotRequiredWireSize(header, maximumBytes, size) && header.totalBytes == size;
    }
}  // namespace Horo::Physics::StateDetail
