#include "AnimationPoseComposition.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Shares the bounded double quaternion norm across validation and normalization. */
        double RotationNormSquared(const Math::Quaternion value) noexcept {
            const std::array<double, 4> components{value.x, value.y, value.z, value.w};
            return std::inner_product(components.begin(), components.end(), components.begin(), 0.0);
        }

        /** @brief Normalizes finite nondegenerate rotations with a double squared-length intermediate. */
        Math::Quaternion NormalizeRotation(const Math::Quaternion value) noexcept {
            const auto inverseLength = 1.0 / std::sqrt(RotationNormSquared(value));
            return {static_cast<float>(value.x * inverseLength), static_cast<float>(value.y * inverseLength),
                    static_cast<float>(value.z * inverseLength), static_cast<float>(value.w * inverseLength)};
        }

        /** @brief Computes convex combinations without overflowing finite opposite extrema. */
        Math::Vec3 BlendVector(const Math::Vec3 first, const Math::Vec3 second, const float weight) noexcept {
            const auto component = [weight](const float a, const float b) {
                return static_cast<float>((1.0 - weight) * static_cast<double>(a) + weight * static_cast<double>(b));
            };
            return {component(first.x, second.x), component(first.y, second.y), component(first.z, second.z)};
        }

        /** @brief Uses double intermediates for reference-relative translation and scale arithmetic. */
        Math::Vec3 AdditiveVector(const Math::Vec3 base, const Math::Vec3 sample, const Math::Vec3 reference, const float weight,
                                  const bool scale) noexcept {
            const auto component = [weight, scale](const float a, const float b, const float r) {
                const auto value = scale ? static_cast<double>(a) * (1.0 + weight * (static_cast<double>(b) / r - 1.0))
                                         : static_cast<double>(a) + weight * (static_cast<double>(b) - r);
                return std::abs(value) > std::numeric_limits<float>::max() ? std::numeric_limits<float>::quiet_NaN()
                                                                           : static_cast<float>(value);
            };
            return {component(base.x, sample.x, reference.x), component(base.y, sample.y, reference.y),
                    component(base.z, sample.z, reference.z)};
        }

        /** @brief Applies the explicitly selected rotation-delta space after normalizing operands. */
        Math::Transform AdditiveTransform(const Math::Transform &base, const Math::Transform &sample, const Math::Transform &reference,
                                          const float weight, const GraphBlendMode mode) noexcept {
            const auto sampledRotation = NormalizeRotation(sample.rotation);
            const auto inverseReference = NormalizeRotation(reference.rotation).Inverse();
            const auto delta =
                mode == GraphBlendMode::AdditiveLocal ? inverseReference * sampledRotation : sampledRotation * inverseReference;
            const auto weightedDelta = Math::Slerp(Math::Quaternion::Identity(), delta, weight);
            const auto baseRotation = NormalizeRotation(base.rotation);
            return {AdditiveVector(base.translation, sample.translation, reference.translation, weight, false),
                    (mode == GraphBlendMode::AdditiveLocal ? baseRotation * weightedDelta : weightedDelta * baseRotation).Normalized(),
                    AdditiveVector(base.scale, sample.scale, reference.scale, weight, true)};
        }

        /** @brief Accepts an omitted optional view or one complete skeleton view. */
        bool OptionalViewSize(const std::size_t size, const std::size_t joints) noexcept {
            return size == 0 || size == joints;
        }

        /** @brief Validates finite weight and exact base/overlay/optional dense view sizes. */
        bool DenseViews(const PoseCompositionInput &input, const std::size_t joints) noexcept {
            return std::isfinite(input.weight) && input.base.size() == joints && input.overlay.size() == joints &&
                   OptionalViewSize(input.mask.size(), joints) && OptionalViewSize(input.coverage.size(), joints);
        }

        /** @brief Validates all dense view sizes before candidate access. */
        GraphInstanceFailure ValidateViews(const PoseCompositionInput &input, const std::size_t joints) noexcept {
            using enum GraphInstanceFailure;
            if (joints == 0 || joints > SkeletonAssetHardLimits::Joints)
                return LimitExceeded;
            if (input.mode >= GraphBlendMode::Count)
                return TypeMismatch;
            if (!DenseViews(input, joints))
                return Malformed;
            const auto referenceSize = input.mode == GraphBlendMode::Normalized ? 0 : joints;
            return input.reference.size() == referenceSize ? None : Malformed;
        }

        /** @brief Checks one already size-admitted mask and contribution marker. */
        bool JointMask(const PoseCompositionInput &input, const std::size_t joint, const float mask) noexcept {
            return std::isfinite(mask) && mask >= 0.0F && mask <= 1.0F && (input.coverage.empty() || input.coverage[joint] <= 1);
        }

        /** @brief Reads optional authored coverage only after dense view size admission. */
        bool AbsentOverlay(const PoseCompositionInput &input, const std::size_t joint) noexcept {
            return !input.coverage.empty() && input.coverage[joint] == 0;
        }

        /** @brief Interpolates one normalized pose pair with the existing endpoint semantics. */
        Math::Transform NormalizedTransform(const Math::Transform &base, const Math::Transform &overlay, const float weight) noexcept {
            if (weight == 1.0F)
                return overlay;
            return {BlendVector(base.translation, overlay.translation, weight),
                    Math::Slerp(NormalizeRotation(base.rotation), NormalizeRotation(overlay.rotation), weight),
                    BlendVector(base.scale, overlay.scale, weight)};
        }

        /** @brief Composes one candidate joint while retaining exact zero-weight/absent base values. */
        GraphInstanceFailure ComposeJoint(const PoseCompositionInput &input, const std::size_t joint, const float layerWeight,
                                          Math::Transform &output) noexcept {
            using enum GraphInstanceFailure;
            const auto mask = input.mask.empty() ? 1.0F : input.mask[joint];
            if (!JointMask(input, joint, mask) || !ValidPoseTransform(input.base[joint]))
                return Malformed;
            const auto weight = layerWeight * mask;
            if (weight == 0.0F || AbsentOverlay(input, joint)) {
                output = input.base[joint];
                return None;
            }
            if (!ValidPoseTransform(input.overlay[joint]))
                return Malformed;
            if (input.mode == GraphBlendMode::Normalized) {
                output = NormalizedTransform(input.base[joint], input.overlay[joint], weight);
            } else {
                if (!ValidPoseTransform(input.reference[joint]))
                    return Malformed;
                output = AdditiveTransform(input.base[joint], input.overlay[joint], input.reference[joint], weight, input.mode);
            }
            return ValidPoseTransform(output) ? None : Malformed;
        }
    }  // namespace

    /** @copydoc ValidPoseTransform */
    bool ValidPoseTransform(const Math::Transform &transform) noexcept {
        if (!Math::IsFinite(transform.translation) || !Math::IsFinite(transform.scale) || !Math::IsFinite(transform.rotation))
            return false;
        return RotationNormSquared(transform.rotation) > static_cast<double>(Math::DefaultEpsilon) * Math::DefaultEpsilon &&
               std::abs(transform.scale.x) > 1.0e-8F && std::abs(transform.scale.y) > 1.0e-8F && std::abs(transform.scale.z) > 1.0e-8F;
    }

    /** @copydoc ComposePose */
    GraphInstanceFailure ComposePose(const PoseCompositionInput &input, const std::span<Math::Transform> output) noexcept {
        using enum GraphInstanceFailure;
        if (const auto failure = ValidateViews(input, output.size()); failure != None)
            return failure;
        const auto layerWeight = std::clamp(input.weight, 0.0F, 1.0F);
        for (std::size_t joint = 0; joint < output.size(); ++joint)
            if (const auto failure = ComposeJoint(input, joint, layerWeight, output[joint]); failure != None)
                return failure;
        return None;
    }
}  // namespace Horo::Animation::Detail
