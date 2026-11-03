#pragma once

#include "CanonicalPhysicsStateStream.h"
#include "Horo/Physics/PhysicsStateSnapshot.h"

#include <Jolt/Geometry/Sphere.h>
#include <Jolt/Math/Vector.h>
#include <Jolt/Physics/Constraints/MotorSettings.h>

namespace Horo::Physics::Detail {
    /** @brief Translates typed scalar values through the private pinned native
     * recorder grammar.
     * @details This class never consumes public wire bytes. Read is used ONLY for
     * the actual native SaveState bridge; Write is used ONLY after complete public
     * typed validation. Vec3's fourth SIMD lane is excluded from the public schema
     * and reconstructed by the native Vec3 constructor.
     */
    class CanonicalPhysicsStateFields final {
    public:
        CanonicalPhysicsStateFields(CanonicalPhysicsStateStream &stream, bool reading) noexcept : stream_(stream), reading_(reading) {}

        /** @brief Private native scalar representation is translated only inside the
         * adapter bridge. */
        template <class T> void Value(T &value) {
            if (reading_)
                stream_.Read(value);
            else
                stream_.Write(value);
        }

        /** @brief Handles a SIMD-backed native vector without exposing its redundant
         * lane or padding. */
        void Vector(std::array<float, 3> &value) {
            JPH::Vec3 native{value[0], value[1], value[2]};
            Value(native);
            if (reading_)
                value = {native.GetX(), native.GetY(), native.GetZ()};
        }

        /** @brief Native Float3 has a different recorder width from SIMD Vec3. */
        void Float3(std::array<float, 3> &value) {
            JPH::Float3 native{value[0], value[1], value[2]};
            Value(native);
            if (reading_)
                value = {native.x, native.y, native.z};
        }

        /** @brief Required unit quaternion preserves all four actual native
         * components. */
        void Rotation(std::array<float, 4> &value) {
            JPH::Quat native{value[0], value[1], value[2], value[3]};
            Value(native);
            if (reading_)
                value = {native.GetX(), native.GetY(), native.GetZ(), native.GetW()};
        }

        /** @brief Constraint Vector<2> contains exactly two required lambda channels.
         */
        void Lambda2(std::array<float, 2> &value) {
            JPH::Vector<2> native;
            native[0] = value[0];
            native[1] = value[1];
            Value(native);
            if (reading_)
                value = {native[0], native[1]};
        }

        /** @brief Sleep-test sphere representation is translated to explicit
         * center/radius fields. */
        void Sphere(PhysicsStateSleepSphere &value) {
            JPH::Sphere native{JPH::Vec3{value.center[0], value.center[1], value.center[2]}, value.radius};
            Value(native);
            if (reading_) {
                const auto center = native.GetCenter();
                value = {{center.GetX(), center.GetY(), center.GetZ()}, native.GetRadius()};
            }
        }

        /** @brief Native enum width is confined to the bridge; public wire width is
         * one checked byte. */
        void Motor(std::uint8_t &value) {
            auto native = static_cast<JPH::EMotorState>(value);
            Value(native);
            if (reading_)
                value = static_cast<std::uint8_t>(native);
        }

        /** @brief Exposes exact bridge status to the aggregate capture, never native
         * success from byte count alone. */
        bool Failed() const noexcept {
            return stream_.IsFailed();
        }

    private:
        CanonicalPhysicsStateStream &stream_;
        bool reading_{};
    };

    /** @brief Actual MotionProperties.cpp63 grammar, including hidden sleep and
     * accumulated force state. */
    inline void CanonicalMotionFields(CanonicalPhysicsStateFields &fields, PhysicsStateMotion &motion) {
        fields.Vector(motion.linearVelocity);
        fields.Vector(motion.angularVelocity);
        fields.Vector(motion.force);
        fields.Vector(motion.torque);
        for (auto &sphere : motion.sleepSpheres)
            fields.Sphere(sphere);
        fields.Value(motion.sleepTimer);
        fields.Value(motion.allowSleeping);
    }

    /** @brief Actual Body.cpp296 grammar; presence comes from retained native
     * motion storage, not motion-type guessing. */
    inline void CanonicalBodyFields(CanonicalPhysicsStateFields &fields, PhysicsStateBody &body) {
        fields.Vector(body.centerOfMass);
        fields.Rotation(body.rotation);
        if (body.hasMotion)
            CanonicalMotionFields(fields, body.motion);
    }

    /** @brief Translates the exact retained supported joint family, including its
     * hidden warm-start state. */
    void CanonicalJointFields(CanonicalPhysicsStateFields &fields, PhysicsStateConstraint &joint);
}  // namespace Horo::Physics::Detail
