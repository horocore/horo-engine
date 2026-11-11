#include "Horo/Vfx/VfxSpawnRequest.h"

#include <cmath>

namespace Horo::Vfx {
    namespace {
        /** @brief Checks normalized finite direction evidence without normalizing malformed input. */
        [[nodiscard]] bool Unit(const Math::Vec3 value) noexcept {
            const double length =
                static_cast<double>(value.x) * value.x + static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z;
            return Math::IsFinite(value) && std::abs(length - 1.0) <= 0.0001;
        }

        /** @brief Checks exact shape expected by the canonical quaternion rotation operation. */
        [[nodiscard]] bool Unit(const Math::Quaternion value) noexcept {
            const double length = static_cast<double>(value.x) * value.x + static_cast<double>(value.y) * value.y +
                                  static_cast<double>(value.z) * value.z + static_cast<double>(value.w) * value.w;
            return Math::IsFinite(value) && std::abs(length - 1.0) <= 0.0001;
        }

        /** @brief Keeps every effective scale component finite and positive before extraction arithmetic. */
        [[nodiscard]] bool ValidScale(const VfxSpawnRequest &request) noexcept {
            const auto scale = request.transform.scale;
            if (!Math::IsFinite(scale) || !std::isfinite(request.scale) || request.scale <= 0.0F)
                return false;
            const auto effective = scale * request.scale;
            return Math::IsFinite(effective) && effective.x > 0.0F && effective.y > 0.0F && effective.z > 0.0F;
        }

        /** @brief Validates schema presence, bounded canonical channel order and finite scalar values. */
        [[nodiscard]] bool ValidParameters(const VfxParameterBlock &parameters) noexcept {
            if (parameters.count > VfxSpawnHardLimits::Parameters || (parameters.schema == 0) != (parameters.count == 0))
                return false;
            std::uint16_t previous{};
            bool first = true;
            for (const auto &value : parameters.Values()) {
                if (!std::isfinite(value.value) || (!first && value.channel <= previous))
                    return false;
                first = false;
                previous = value.channel;
            }
            return true;
        }

        /** @brief Checks required ordering and ownership dimensions; these values are not capabilities. */
        [[nodiscard]] bool ValidIdentity(const VfxSpawnRequest &request) noexcept {
            return request.effectAsset.IsValid() && request.scene.IsValid() && request.owner.IsValid() && request.ownerGeneration != 0 &&
                   request.tick != 0 && request.occurrence.producer != 0 && request.occurrence.sequence != 0;
        }

        /** @brief Checks fixed mapping correlation dimensions separately from source identity. */
        [[nodiscard]] bool ValidRouting(const VfxSpawnRequest &request) noexcept {
            return request.bindingGeneration != 0 && request.destination != 0 && request.layerOrdinal < VfxSpawnHardLimits::Layers &&
                   request.requirement < VfxRequirementClass::Count;
        }
    }  // namespace

    /** @copydoc VfxParameterBlock::Values */
    std::span<const VfxParameterValue> VfxParameterBlock::Values() const noexcept {
        return count <= values.size() ? std::span<const VfxParameterValue>{values.data(), count} : std::span<const VfxParameterValue>{};
    }

    /** @copydoc ValidateVfxSpawnRequest */
    VfxSpawnStatus ValidateVfxSpawnRequest(const VfxSpawnRequest &request) noexcept {
        using enum Horo::Vfx::VfxSpawnStatus;
        if (!ValidIdentity(request) || !ValidRouting(request))
            return InvalidRequest;
        if (!Unit(request.transform.orientation) || !ValidScale(request))
            return InvalidTransform;
        if (request.impactNormal && !Unit(*request.impactNormal))
            return InvalidImpactNormal;
        return ValidParameters(request.parameters) ? Accepted : InvalidParameters;
    }

    /** @copydoc MakeVfxSpawnOutcome */
    VfxSpawnOutcome MakeVfxSpawnOutcome(const VfxSpawnRequest &request, const VfxSpawnStatus status,
                                        const EffectSystemId instance) noexcept {
        return {status,  request.occurrence, request.bindingGeneration, request.destination, request.layerOrdinal, request.effectAsset,
                instance};
    }
}  // namespace Horo::Vfx
