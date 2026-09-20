#pragma once

#include "../ExtensionAuthorityIdentityValidation.h"
#include "Horo/Extensions/BackendOperationRegistry.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/MathUtils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <string_view>

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

    [[nodiscard]] inline bool IsProgressRegression(const BackendOperationProgress current, const BackendOperationProgress next) noexcept {
        return Horo::Foundation::Math::IsProgressRegression(current.completedUnits, current.totalUnits, next.completedUnits,
                                                            next.totalUnits);
    }

    [[nodiscard]] inline Error CancellationError() {
        return MakeError(ExtensionErrors::BackendOperationCancelled);
    }

    [[nodiscard]] inline Error AbandonmentError() {
        return MakeError(ExtensionErrors::BackendOperationAbandoned);
    }
}  // namespace Horo::Extensions::BackendOperationValidation
