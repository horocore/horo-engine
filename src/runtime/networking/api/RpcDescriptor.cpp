#include "Horo/Network/RpcDescriptor.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <new>
#include <string_view>

namespace Horo::Network {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Invalid() {
            return Result<T>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
        }

        [[nodiscard]] constexpr bool IsModuleWordCharacter(const unsigned char character) noexcept {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        }

        [[nodiscard]] constexpr bool IsModuleSeparator(const unsigned char character) noexcept {
            return character == '.' || character == '-' || character == '_';
        }

        [[nodiscard]] bool IsCanonicalModuleIdentity(const std::string_view value) noexcept {
            if (value.empty())
                return false;
            bool previousSeparator = true;
            for (const char raw : value) {
                const auto character = static_cast<unsigned char>(raw);
                if (IsModuleSeparator(character)) {
                    if (previousSeparator)
                        return false;
                    previousSeparator = true;
                    continue;
                }
                if (!IsModuleWordCharacter(character))
                    return false;
                previousSeparator = false;
            }
            return !previousSeparator;
        }

        [[nodiscard]] bool ValidLimits(const RpcDescriptorLimits &limits) noexcept {
            return limits.maximumRpcs > 0 && limits.maximumParametersPerRpc > 0 && limits.maximumOwnerIdentityBytes > 0 &&
                   limits.maximumDefaultBytesPerParameter > 0 && limits.maximumTotalDefaultBytes > 0 && limits.maximumPayloadBytes > 0;
        }

        [[nodiscard]] bool ValidRoute(const RpcDescriptor &descriptor) noexcept {
            if (descriptor.direction == RpcDirection::ClientToAuthority)
                return descriptor.target == RpcTarget::Authority && descriptor.permission != RpcCallerPermission::AuthorityOnly;
            if (descriptor.direction == RpcDirection::AuthorityToClient)
                return descriptor.target != RpcTarget::Authority && descriptor.permission == RpcCallerPermission::AuthorityOnly;
            return false;
        }

        [[nodiscard]] Result<void> ValidateIdentityAndVersion(const RpcDescriptor &descriptor) {
            if (!descriptor.id.IsValid() || !descriptor.version.IsValid() || !descriptor.compatibility.minimum.IsValid() ||
                descriptor.compatibility.maximum != descriptor.version ||
                descriptor.compatibility.minimum.major != descriptor.version.major ||
                descriptor.compatibility.minimum > descriptor.compatibility.maximum || !IsCanonicalModuleIdentity(descriptor.owner.value))
                return Invalid<void>();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRoutingAndPermission(const RpcDescriptor &descriptor) {
            if (descriptor.direction >= RpcDirection::Count || descriptor.delivery >= RpcDelivery::Count ||
                descriptor.target >= RpcTarget::Count || descriptor.permission >= RpcCallerPermission::Count || !ValidRoute(descriptor))
                return Invalid<void>();
            if (descriptor.customPermission.has_value() != (descriptor.permission == RpcCallerPermission::Custom) ||
                (descriptor.customPermission.has_value() && !descriptor.customPermission->IsValid()))
                return Invalid<void>();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateDeclaredBounds(const RpcDescriptor &descriptor, const RpcDescriptorLimits &limits) {
            if (descriptor.rateLimit.maximumCallsPerSecond == 0 || descriptor.rateLimit.maximumBurst == 0 ||
                descriptor.rateLimit.maximumBurst > descriptor.rateLimit.maximumCallsPerSecond || descriptor.maximumPayloadBytes == 0)
                return Invalid<void>();
            if (descriptor.owner.value.size() > limits.maximumOwnerIdentityBytes ||
                descriptor.maximumPayloadBytes > limits.maximumPayloadBytes ||
                descriptor.parameters.size() > limits.maximumParametersPerRpc ||
                descriptor.tombstonedParameters.size() > limits.maximumParametersPerRpc ||
                descriptor.parameters.size() > limits.maximumParametersPerRpc - descriptor.tombstonedParameters.size())
                return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateParameterShape(const RpcParameterDescriptor &parameter, const RpcDescriptor &descriptor) {
            if (!parameter.id.IsValid() || !parameter.valueType.IsValid() || !parameter.codec.IsValid() ||
                !parameter.introducedVersion.IsValid() || parameter.introducedVersion > descriptor.version ||
                parameter.requirement >= RpcParameterRequirement::Count || parameter.limits.maximumEncodedBytes == 0 ||
                parameter.limits.maximumElementCount == 0 || parameter.limits.maximumEncodedBytes > descriptor.maximumPayloadBytes)
                return Invalid<void>();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateParameterDefault(const RpcParameterDescriptor &parameter, const RpcDescriptorLimits &limits) {
            if (parameter.canonicalDefault.has_value()) {
                if (parameter.requirement != RpcParameterRequirement::Optional ||
                    parameter.canonicalDefault->canonicalBytes.size() > limits.maximumDefaultBytesPerParameter ||
                    parameter.canonicalDefault->canonicalBytes.size() > parameter.limits.maximumEncodedBytes)
                    return Invalid<void>();
            } else if (parameter.requirement == RpcParameterRequirement::Optional) {
                return Invalid<void>();
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateParameter(const RpcParameterDescriptor &parameter, const RpcDescriptor &descriptor,
                                                     const RpcDescriptorLimits &limits) {
            if (const Result<void> shape = ValidateParameterShape(parameter, descriptor); shape.HasError())
                return shape;
            if (const Result<void> defaultValue = ValidateParameterDefault(parameter, limits); defaultValue.HasError())
                return defaultValue;
            if (parameter.requirement == RpcParameterRequirement::Required &&
                parameter.introducedVersion > descriptor.compatibility.minimum)
                return Result<void>::Failure(MakeError(NetworkErrors::RpcDescriptorIncompatible));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateIdentities(const RpcDescriptor &descriptor) {
            std::vector<RpcParameterId> active;
            active.reserve(descriptor.parameters.size());
            for (const RpcParameterDescriptor &parameter : descriptor.parameters)
                active.push_back(parameter.id);
            std::ranges::sort(active);
            if (std::ranges::adjacent_find(active) != active.end())
                return Result<void>::Failure(MakeError(NetworkErrors::RpcDescriptorConflict));

            std::vector<RpcParameterId> tombstones = descriptor.tombstonedParameters;
            if (std::ranges::any_of(tombstones, [](const RpcParameterId id) {
                return !id.IsValid();
            }))
                return Result<void>::Failure(MakeError(NetworkErrors::RpcDescriptorConflict));
            std::ranges::sort(tombstones);
            if (std::ranges::adjacent_find(tombstones) != tombstones.end() ||
                std::ranges::any_of(tombstones, [&active](const RpcParameterId id) {
                return std::ranges::binary_search(active, id);
            }))
                return Result<void>::Failure(MakeError(NetworkErrors::RpcDescriptorConflict));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateRpcDescriptor */
    Result<void> ValidateRpcDescriptor(const RpcDescriptor &descriptor, const RpcDescriptorLimits &limits) {
        try {
            if (!ValidLimits(limits))
                return Invalid<void>();
            if (const Result<void> identity = ValidateIdentityAndVersion(descriptor); identity.HasError())
                return identity;
            if (const Result<void> routing = ValidateRoutingAndPermission(descriptor); routing.HasError())
                return routing;
            if (const Result<void> bounds = ValidateDeclaredBounds(descriptor, limits); bounds.HasError())
                return bounds;
            for (const RpcParameterDescriptor &parameter : descriptor.parameters) {
                if (const Result<void> valid = ValidateParameter(parameter, descriptor, limits); valid.HasError())
                    return valid;
            }
            return ValidateIdentities(descriptor);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        }
    }
}  // namespace Horo::Network
