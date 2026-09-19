#pragma once

#include "../ExtensionAuthorityIdentityValidation.h"
#include "Horo/Extensions/BackendOperationRegistry.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <string_view>
#include <tuple>

namespace Horo::Extensions::BackendOperationValidation {
    inline constexpr std::size_t MaximumIdentityBytes = 256;
    inline constexpr std::size_t MaximumOperationTypes = 64;
    inline constexpr std::size_t MaximumDiagnostics = 4096;
    inline constexpr std::size_t MaximumDiagnosticBytes = 1U << 20U;
    inline constexpr std::size_t MaximumResultBytes = 64U << 20U;

    [[nodiscard]] inline bool ValidIdentity(const std::string_view value) noexcept {
        return Detail::IsCanonicalExtensionAuthorityId(value);
    }

    [[nodiscard]] inline bool ValidTypedId(const std::string_view value) noexcept {
        return Detail::IsCanonicalExtensionAuthorityId(value) && value.size() <= MaximumIdentityBytes;
    }

    [[nodiscard]] inline bool ValidProvider(const ApplicationCapabilityProviderIdentity &provider) noexcept {
        return ValidIdentity(provider.moduleId) && ValidIdentity(provider.providerId) && provider.generation != 0;
    }

    [[nodiscard]] inline bool ValidProviderDescriptor(const BackendOperationProviderDescriptor &descriptor) noexcept {
        if (!ValidProvider(descriptor.provider) || descriptor.operationTypes.empty() ||
            descriptor.operationTypes.size() > MaximumOperationTypes || descriptor.maximumOperations == 0 ||
            descriptor.maximumOperations > BackendOperationRegistry::MaximumOperations || descriptor.maximumDiagnostics == 0 ||
            descriptor.maximumDiagnostics > MaximumDiagnostics || descriptor.maximumDiagnosticBytes == 0 ||
            descriptor.maximumDiagnosticBytes > MaximumDiagnosticBytes || descriptor.maximumResultBytes > MaximumResultBytes)
            return false;
        for (const auto &operation : descriptor.operationTypes) {
            if (!ValidTypedId(operation.type.value) || !ValidTypedId(operation.result.value))
                return false;
        }
        for (auto left = descriptor.operationTypes.begin(); left != descriptor.operationTypes.end(); ++left) {
            if (std::ranges::any_of(std::next(left), descriptor.operationTypes.end(), [&left](const auto &right) {
                return right.type == left->type || right.result == left->result;
            }))
                return false;
        }
        return true;
    }

    [[nodiscard]] inline bool ValidOperationDescriptor(const BackendOperationDescriptor &descriptor) noexcept {
        return ValidTypedId(descriptor.initialPhase.value);
    }

    [[nodiscard]] inline bool ValidProgress(const BackendOperationProgress progress) noexcept {
        return progress.totalUnits != 0 && progress.completedUnits <= progress.totalUnits;
    }

    struct WideProduct final {
        std::uint64_t high{};
        std::uint64_t low{};
    };

    [[nodiscard]] constexpr WideProduct MultiplyWide(const std::uint64_t left, const std::uint64_t right) noexcept {
        constexpr std::uint64_t lowerMask = 0xffffffffULL;
        const std::uint64_t leftLow = left & lowerMask;
        const std::uint64_t leftHigh = left >> 32U;
        const std::uint64_t rightLow = right & lowerMask;
        const std::uint64_t rightHigh = right >> 32U;
        const std::uint64_t lowProduct = leftLow * rightLow;
        const std::uint64_t firstCross = leftHigh * rightLow + (lowProduct >> 32U);
        const std::uint64_t secondCross = leftLow * rightHigh + (firstCross & lowerMask);
        return {.high = leftHigh * rightHigh + (firstCross >> 32U) + (secondCross >> 32U),
                .low = (secondCross << 32U) + (lowProduct & lowerMask)};
    }

    [[nodiscard]] inline bool IsProgressRegression(const BackendOperationProgress current, const BackendOperationProgress next) noexcept {
        const WideProduct nextProduct = MultiplyWide(next.completedUnits, current.totalUnits);
        const WideProduct currentProduct = MultiplyWide(current.completedUnits, next.totalUnits);
        return std::tie(nextProduct.high, nextProduct.low) < std::tie(currentProduct.high, currentProduct.low);
    }

    [[nodiscard]] inline Error CancellationError() {
        return MakeError(ExtensionErrors::BackendOperationCancelled);
    }

    [[nodiscard]] inline Error AbandonmentError() {
        return MakeError(ExtensionErrors::BackendOperationAbandoned);
    }
}  // namespace Horo::Extensions::BackendOperationValidation
