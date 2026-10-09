#pragma once

#include "Horo/Physics/CharacterControllerContracts.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace Horo::Character::Detail {
    /** @brief Tick-local blocking constraints, independent from the truncated presentation contact prefix. */
    struct SweepPlanes final {
        std::array<Math::Vec3, MaximumCharacterSweepHits> normals{};
        std::uint32_t count{};

        /** @brief Retains exact distinct normals; an unrepresentable constraint must stop unswept travel. */
        [[nodiscard]] bool Add(const Math::Vec3 normal) noexcept {
            if (const auto active = std::span{normals}.first(count); std::ranges::find(active, normal) != active.end())
                return true;
            if (count == normals.size())
                return false;
            normals[count++] = normal;
            return true;
        }

        /** @brief Checks every retained plane; tolerance bounds only floating-point projection roundoff. */
        [[nodiscard]] bool Allows(const Math::Vec3 candidate) const noexcept {
            const float tolerance = 1.0e-6F * Math::Length(candidate);
            return Math::IsFinite(candidate) && std::ranges::all_of(std::span{normals}.first(count), [&](const Math::Vec3 normal) {
                return Math::Dot(candidate, normal) >= -tolerance;
            });
        }

        /** @brief Keeps the closest feasible non-amplifying candidate; canonical enumeration breaks exact ties. */
        void Consider(const Math::Vec3 intent, const Math::Vec3 candidate, Math::Vec3 &best, float &cost) const noexcept {
            const float distance = Math::LengthSquared(candidate - intent);
            if (distance < cost && Math::LengthSquared(candidate) <= Math::LengthSquared(intent) && Allows(candidate)) {
                best = candidate;
                cost = distance;
            }
        }

        /** @brief Projects onto the feasible cone using faces and two-plane creases; zero is the conservative closed-corner fallback.
         * At most 32 faces and 496 pairs are examined with at most 32 checks each. No iteration, allocation or query is hidden here.
         */
        [[nodiscard]] Math::Vec3 Clip(const Math::Vec3 intent) const noexcept {
            if (Allows(intent))
                return intent;
            Math::Vec3 best{};
            float cost = Math::LengthSquared(intent);
            for (std::uint32_t first{}; first < count; ++first) {
                const auto normal = normals[first];
                Consider(intent, intent - normal * (Math::Dot(intent, normal) / Math::LengthSquared(normal)), best, cost);
                for (std::uint32_t second = first + 1; second < count; ++second) {
                    const auto crease = Math::Cross(normal, normals[second]);
                    const float squared = Math::LengthSquared(crease);
                    if (squared > 1.0e-12F)
                        Consider(intent, crease * (Math::Dot(intent, crease) / squared), best, cost);
                }
            }
            return best;
        }

        /** @brief Constrains post-slope travel without allowing cone projection to manufacture forbidden ascent. */
        [[nodiscard]] Math::Vec3 ClipTravel(Math::Vec3 remaining, const Math::Vec3 up, const float maximumAscent,
                                            const bool steepConstraint) const noexcept {
            if (const float ascent = Math::Dot(remaining, up); steepConstraint && ascent > maximumAscent)
                remaining -= up * (ascent - maximumAscent);
            remaining = Clip(remaining);
            if (const float ascent = Math::Dot(remaining, up); steepConstraint && ascent > maximumAscent) {
                remaining -= up * (ascent - maximumAscent);
                if (!Allows(remaining))
                    remaining = {};
            }
            return remaining;
        }
    };
}  // namespace Horo::Character::Detail
