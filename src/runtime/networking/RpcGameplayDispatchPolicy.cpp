#include "Horo/Network/NetworkErrors.h"
#include "RpcGameplayDispatchState.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <type_traits>

namespace Horo::Network {
    namespace {
        /** @brief Checks ordered inclusive endpoints without converting exact integer values to floating point. */
        bool ValidScalarRange(const RpcParameterConstraint &constraint) {
            return std::visit([&constraint]<typename T>(const T minimum) {
                const auto maximum = std::get<T>(constraint.maximum);
                if constexpr (std::is_same_v<T, double>) {
                    if (!std::isfinite(minimum) || !std::isfinite(maximum))
                        return false;
                }
                return minimum <= maximum;
            }, constraint.minimum);
        }
    }  // namespace

    /** @copydoc RpcGameplayDispatch::ChargeWork */
    Result<void> RpcGameplayDispatch::ChargeWork(const Peer &peer, const std::size_t bytes, const std::uint64_t nowTick) {
        if (nowTick < lastAdmissionTick_)
            return Result<void>::Failure(MakeError(NetworkErrors::MessageDeliveryInvalid));
        lastAdmissionTick_ = nowTick;
        auto scope = std::ranges::find_if(work_, [&peer](const WorkScope &entry) {
            return entry.connection == peer.connection && entry.generation == peer.generation && entry.peer == peer.identity;
        });
        if (scope == work_.end()) {
            if (work_.size() == limits_.maximumCallerScopes + 1)
                return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
            scope = work_.emplace(work_.end(), peer.connection, peer.generation, peer.identity, nowTick, 0, 0);
        }
        auto &global = work_.front();
        for (WorkScope *entry : {std::to_address(scope), &global}) {
            if (nowTick - entry->windowStart >= limits_.ticksPerSecond) {
                entry->windowStart = nowTick;
                entry->bytes = 0;
                entry->attempts = 0;
            }
        }
        if (scope->attempts >= limits_.maximumAttemptsPerSecond || global.attempts >= limits_.maximumGlobalAttemptsPerSecond ||
            bytes > limits_.maximumBytesPerSecond - scope->bytes || bytes > limits_.maximumGlobalBytesPerSecond - global.bytes)
            return Result<void>::Failure(MakeError(NetworkErrors::RpcRateLimited));
        ++scope->attempts;
        ++global.attempts;
        scope->bytes += bytes;
        global.bytes += bytes;
        return Result<void>::Success();
    }

    /** @copydoc RpcGameplayDispatch::ChargeRate */
    Result<void> RpcGameplayDispatch::ChargeRate(const Peer &peer, const RpcDescriptor &descriptor, const std::uint64_t nowTick) {
        auto scope = std::ranges::find_if(rates_, [&peer, &descriptor](const RateScope &entry) {
            return entry.connection == peer.connection && entry.generation == peer.generation && entry.peer == peer.identity &&
                   entry.id == descriptor.id;
        });
        const std::uint64_t capacity = static_cast<std::uint64_t>(descriptor.rateLimit.maximumBurst) * limits_.ticksPerSecond;
        if (scope == rates_.end()) {
            if (rates_.size() == limits_.maximumRateScopes)
                return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
            scope =
                rates_.emplace(rates_.end(), peer.connection, peer.generation, peer.identity, descriptor.id, nowTick, nowTick, capacity, 0);
        }
        if (nowTick < scope->refillTick)
            return Result<void>::Failure(MakeError(NetworkErrors::MessageDeliveryInvalid));
        const auto elapsed = std::min(nowTick - scope->refillTick, limits_.ticksPerSecond);
        const auto refill = elapsed * descriptor.rateLimit.maximumCallsPerSecond;
        scope->credit += std::min(refill, capacity - scope->credit);
        scope->refillTick = nowTick;
        if (nowTick - scope->windowStart >= limits_.ticksPerSecond) {
            scope->windowStart = nowTick;
            scope->calls = 0;
        }
        if (scope->credit < limits_.ticksPerSecond || scope->calls >= descriptor.rateLimit.maximumCallsPerSecond)
            return Result<void>::Failure(MakeError(NetworkErrors::RpcRateLimited));
        scope->credit -= limits_.ticksPerSecond;
        ++scope->calls;
        return Result<void>::Success();
    }

    /** @copydoc RpcGameplayDispatch::ValidatePolicy */
    Result<void> RpcGameplayDispatch::ValidatePolicy(const Binding &binding) {
        const auto &policy = *binding.policy;
        const auto &descriptor = *binding.descriptor;
        if ((policy.objectSchema && (!policy.objectSchema->IsValid() || policy.objectSchemaVersion.major == 0)) ||
            (!policy.objectSchema && policy.objectSchemaVersion != ReplicationSchemaVersion{}))
            return Result<void>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
        if ((policy.permission.has_value() != static_cast<bool>(policy.caller)) ||
            (policy.permission &&
             (descriptor.permission != RpcCallerPermission::Custom || policy.permission != descriptor.customPermission)))
            return Result<void>::Failure(MakeError(NetworkErrors::RpcPermissionUnsupported));
        for (std::size_t index = 0; index < policy.parameters.size(); ++index) {
            const auto &constraint = policy.parameters[index];
            const auto parameter = std::ranges::find(descriptor.parameters, constraint.parameter, &RpcParameterDescriptor::id);
            if (parameter == descriptor.parameters.end() || constraint.minimum.index() != constraint.maximum.index())
                return Result<void>::Failure(MakeError(NetworkErrors::RpcParameterUnsupported));
            if (const auto offset = static_cast<std::size_t>(parameter - descriptor.parameters.begin());
                static_cast<std::size_t>(binding.metadata[offset].valueKind) != constraint.minimum.index() + 1)
                return Result<void>::Failure(MakeError(NetworkErrors::RpcParameterUnsupported));
            if (!ValidScalarRange(constraint) || std::ranges::any_of(policy.parameters.begin(), policy.parameters.begin() + index,
                                                                     [&constraint](const RpcParameterConstraint &other) {
                return other.parameter == constraint.parameter;
            }))
                return Result<void>::Failure(MakeError(NetworkErrors::RpcParameterInvalid));
        }
        return Result<void>::Success();
    }

    /** @copydoc RpcGameplayDispatch::AuthorizePolicy */
    Result<void> RpcGameplayDispatch::AuthorizePolicy(const Binding &binding, const Peer &peer, const Object &object,
                                                      const std::span<const ReplicationRuntimeValue> values) const {
        const auto role = object.role->Snapshot();
        if (role.HasError())
            return Result<void>::Failure(role.ErrorValue());
        if (binding.policy->objectSchema &&
            (role.Value().schema != *binding.policy->objectSchema || role.Value().schemaVersion != binding.policy->objectSchemaVersion))
            return Result<void>::Failure(MakeError(NetworkErrors::RpcPermissionDenied));
        for (const auto &value : values) {
            if (const auto floating = std::get_if<double>(&value); floating && !std::isfinite(*floating))
                return Result<void>::Failure(MakeError(NetworkErrors::RpcParameterInvalid));
        }
        for (const auto &constraint : binding.policy->parameters) {
            const auto &parameters = binding.descriptor->parameters;
            const auto parameter = std::ranges::find(parameters, constraint.parameter, &RpcParameterDescriptor::id);
            const auto &value = values[static_cast<std::size_t>(parameter - parameters.begin())];
            const bool inside = std::visit([&value, &constraint]<typename T>(const T minimum) {
                const auto number = std::get_if<T>(&value);
                return number && *number >= minimum && *number <= std::get<T>(constraint.maximum);
            }, constraint.minimum);
            if (!inside)
                return Result<void>::Failure(MakeError(NetworkErrors::RpcParameterInvalid));
        }
        if (binding.descriptor->permission != RpcCallerPermission::Custom)
            return Result<void>::Success();
        if (!binding.policy->caller)
            return Result<void>::Failure(MakeError(NetworkErrors::RpcPermissionUnsupported));
        try {
            const auto allowed =
                binding.policy->caller->Authorize({peer.identity, peer.role, peer.connection, peer.generation, role.Value()});
            if (allowed.HasError())
                return Result<void>::Failure(WrapError(NetworkErrors::RpcPermissionDenied, allowed.ErrorValue()));
            if (allowed.Value())
                return Result<void>::Success();
        } catch (...) {  // NOSONAR: Required module exception containment boundary.
            // Host-approved policies are external module code, including non-standard exception sources.
            return Result<void>::Failure(MakeError(NetworkErrors::RpcPermissionDenied));
        }
        return Result<void>::Failure(MakeError(NetworkErrors::RpcPermissionDenied));
    }
}  // namespace Horo::Network
