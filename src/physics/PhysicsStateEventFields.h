#pragma once

#include "Horo/Physics/PhysicsStateSnapshot.h"
#include "PhysicsStateWireInternal.h"

namespace Horo::Physics::StateDetail {
    /** @brief Stable 128-bit identifiers are explicit bytes, never C++ object
     * padding or UUID text allocation. */
    template <class T> void ProjectId(Wire &wire, T &value) noexcept {
        auto bytes = value.Bytes();
        for (auto &byte : bytes)
            wire.Integer(byte);
        value = T::FromBytes(bytes);
    }

    /** @brief SHA-256 words retain their canonical 32-byte digest order. */
    inline void Digest(Wire &wire, Sha256Digest &value) noexcept {
        for (auto &byte : value.bytes)
            wire.Integer(byte);
    }

    /** @brief Optional required fields have one explicit presence bit and a
     * complete fixed zero representation when absent. */
    template <class T, class Fields> void Optional(Wire &wire, std::optional<T> &value, Fields fields) noexcept {
        bool present = value.has_value();
        wire.Boolean(present);
        T copied = value.value_or(T{});
        fields(wire, copied);
        value = present ? std::optional<T>{copied} : std::nullopt;
    }

    /** @brief Complete persistent material evidence, independent of native material
     * pointers. */
    inline void Material(Wire &wire, PhysicsEventMaterial &value) noexcept {
        ProjectId(wire, value.asset);
        wire.Integer(value.assetGeneration);
        auto slot = value.slot.Value();
        wire.Integer(slot);
        value.slot = PhysicsMaterialSlotId::FromValue(slot);
    }

    /** @brief Catalogue identity and revision stay historical evidence, never new
     * live lookup authority. */
    inline void Surface(Wire &wire, PhysicsStateSurfaceEvidence &value) noexcept {
        ProjectId(wire, value.surface);
        wire.Integer(value.catalogRevision);
        Digest(wire, value.catalogFingerprint);
        wire.Integer(value.collider);
    }

    /** @brief Event endpoint contains stable checkpoint IDs, not raw
     * world/body/shape handle generations. */
    inline void EventEndpoint(Wire &wire, PhysicsStateEventEndpoint &value) noexcept {
        wire.Integer(value.body);
        wire.Integer(value.shape);
        Optional(wire, value.subshape, [](Wire &target, PhysicsShapeSubresourceId &id) {
            auto number = id.Value();
            target.Integer(number);
            id = PhysicsShapeSubresourceId::FromValue(number);
        });
        ProjectId(wire, value.layer);
        ProjectId(wire, value.profile);
        wire.Integer(value.collider);
    }

    /** @brief Copied gameplay contact evidence remains distinct from complete
     * native solver warm-start impulses. */
    inline void EventPoint(Wire &wire, PhysicsContactPoint &value) noexcept {
        std::array one{value.positionOnFirst.x, value.positionOnFirst.y, value.positionOnFirst.z};
        std::array two{value.positionOnSecond.x, value.positionOnSecond.y, value.positionOnSecond.z};
        std::array normal{value.normal.x, value.normal.y, value.normal.z};
        wire.Floats(one);
        wire.Floats(two);
        wire.Floats(normal);
        value.positionOnFirst = {one[0], one[1], one[2]};
        value.positionOnSecond = {two[0], two[1], two[2]};
        value.normal = {normal[0], normal[1], normal[2]};
        wire.Float(value.penetrationDepthMeters);
        Optional(wire, value.normalImpulseEstimateNewtonSeconds, [](Wire &target, float &impulse) {
            target.Float(impulse);
        });
    }

    /** @brief Fixed four-point contact summary includes explicit meaningful count
     * and omitted evidence count. */
    inline void EventSummary(Wire &wire, PhysicsContactSummary &value) noexcept {
        wire.Integer(value.pointCount);
        wire.Integer(value.omittedPointCount);
        for (auto &point : value.points)
            EventPoint(wire, point);
    }

    /** @brief Complete Horo previous-pair values preserve enter/persist/exit
     * ownership across rewind. */
    inline void EventPair(Wire &wire, PhysicsStateEventPair &value) noexcept {
        EventEndpoint(wire, value.first);
        EventEndpoint(wire, value.second);
        Optional(wire, value.firstMaterial, Material);
        Optional(wire, value.secondMaterial, Material);
        Optional(wire, value.firstSurface, Surface);
        Optional(wire, value.secondSurface, Surface);
        EventSummary(wire, value.contact);
        wire.Boolean(value.sensor);
    }

    /** @brief Preserves actual canonical lifecycle emission order, including
     * same-pair exit-before-enter transitions. */
    inline void PublishedEvent(Wire &wire, PhysicsStatePublishedEvent &value) noexcept {
        wire.Integer(value.tick);
        wire.Integer(value.ordinal);
        auto kind = static_cast<std::uint8_t>(value.kind);
        wire.Integer(kind);
        value.kind = static_cast<PhysicsEventKind>(kind);
        EventPair(wire, value.pair);
    }
}  // namespace Horo::Physics::StateDetail
