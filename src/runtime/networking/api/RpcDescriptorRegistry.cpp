#include "Horo/Network/RpcDescriptorRegistry.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <new>
#include <string_view>
#include <utility>

namespace Horo::Network {
    namespace {
        template <std::unsigned_integral T> void AppendInteger(std::vector<std::byte> &bytes, const T value) {
            for (std::size_t byte = sizeof(T); byte > 0; --byte) {
                const auto shift = static_cast<unsigned>((byte - 1) * 8);
                bytes.push_back(static_cast<std::byte>(value >> shift));
            }
        }

        void AppendSize(std::vector<std::byte> &bytes, const std::size_t value) {
            AppendInteger(bytes, static_cast<std::uint64_t>(value));
        }

        void AppendText(std::vector<std::byte> &bytes, const std::string_view value) {
            AppendSize(bytes, value.size());
            bytes.insert(bytes.end(), reinterpret_cast<const std::byte *>(value.data()),
                         reinterpret_cast<const std::byte *>(value.data() + value.size()));
        }

        void AppendData(std::vector<std::byte> &bytes, const std::span<const std::byte> value) {
            AppendSize(bytes, value.size());
            bytes.insert(bytes.end(), value.begin(), value.end());
        }

        void AppendVersion(std::vector<std::byte> &bytes, const ReplicationSchemaVersion version) {
            AppendInteger(bytes, version.major);
            AppendInteger(bytes, version.minor);
        }

        [[nodiscard]] Sha256Digest Fingerprint(const std::span<const RpcDescriptor> descriptors) {
            std::vector<std::byte> bytes;
            AppendText(bytes, "horo.network.rpc-descriptor-set.v1");
            AppendSize(bytes, descriptors.size());
            for (const RpcDescriptor &descriptor : descriptors) {
                AppendInteger(bytes, descriptor.id.Value());
                AppendVersion(bytes, descriptor.version);
                AppendVersion(bytes, descriptor.compatibility.minimum);
                AppendVersion(bytes, descriptor.compatibility.maximum);
                AppendText(bytes, descriptor.owner.value);
                AppendInteger(bytes, static_cast<std::uint8_t>(descriptor.direction));
                AppendInteger(bytes, static_cast<std::uint8_t>(descriptor.delivery));
                AppendInteger(bytes, static_cast<std::uint8_t>(descriptor.target));
                AppendInteger(bytes, static_cast<std::uint8_t>(descriptor.permission));
                AppendInteger(bytes, static_cast<std::uint8_t>(descriptor.customPermission.has_value()));
                if (descriptor.customPermission.has_value())
                    AppendInteger(bytes, descriptor.customPermission->Value());
                AppendInteger(bytes, descriptor.rateLimit.maximumCallsPerSecond);
                AppendInteger(bytes, descriptor.rateLimit.maximumBurst);
                AppendSize(bytes, descriptor.maximumPayloadBytes);
                AppendSize(bytes, descriptor.parameters.size());
                for (const RpcParameterDescriptor &parameter : descriptor.parameters) {
                    AppendInteger(bytes, parameter.id.Value());
                    AppendInteger(bytes, parameter.valueType.Value());
                    AppendInteger(bytes, parameter.codec.Value());
                    AppendVersion(bytes, parameter.introducedVersion);
                    AppendInteger(bytes, static_cast<std::uint8_t>(parameter.requirement));
                    AppendInteger(bytes, parameter.limits.maximumEncodedBytes);
                    AppendInteger(bytes, parameter.limits.maximumElementCount);
                    AppendInteger(bytes, static_cast<std::uint8_t>(parameter.canonicalDefault.has_value()));
                    if (parameter.canonicalDefault.has_value())
                        AppendData(bytes, parameter.canonicalDefault->canonicalBytes);
                }
                AppendSize(bytes, descriptor.tombstonedParameters.size());
                for (const RpcParameterId id : descriptor.tombstonedParameters)
                    AppendInteger(bytes, id.Value());
            }
            return ComputeSha256(bytes);
        }

        [[nodiscard]] bool ValidSerializerMetadata(const ReplicationSerializerDescriptor &serializer) noexcept {
            using enum ReplicationQuantizationMode;
            const bool validQuantization = serializer.quantization.mode == Exact
                                               ? serializer.quantization.step == 0.0
                                               : serializer.quantization.mode == NearestStep &&
                                                     serializer.valueKind == ReplicationValueKind::FloatingPoint &&
                                                     std::isfinite(serializer.quantization.step) && serializer.quantization.step > 0.0;
            return serializer.valueType.IsValid() && serializer.codec.IsValid() && serializer.valueKind < ReplicationValueKind::Count &&
                   validQuantization && serializer.maximumEncodedBytes > 0 && serializer.maximumElementCount > 0;
        }

        [[nodiscard]] bool Supports(const RpcParameterDescriptor &parameter, const ModuleId &owner,
                                    const std::span<const ReplicationSerializerDescriptor> serializers) {
            return std::ranges::any_of(serializers, [&](const ReplicationSerializerDescriptor &serializer) {
                return ValidSerializerMetadata(serializer) && serializer.owner == owner && serializer.valueType == parameter.valueType &&
                       serializer.codec == parameter.codec && serializer.maximumEncodedBytes >= parameter.limits.maximumEncodedBytes &&
                       serializer.maximumElementCount >= parameter.limits.maximumElementCount;
            });
        }

        [[nodiscard]] const RpcDescriptor *FindDescriptor(const std::span<const RpcDescriptor> descriptors, const RpcId id) noexcept {
            const auto found = std::ranges::lower_bound(descriptors, id, {}, &RpcDescriptor::id);
            return found != descriptors.end() && found->id == id ? std::to_address(found) : nullptr;
        }

        [[nodiscard]] const RpcParameterDescriptor *FindParameter(const RpcDescriptor &descriptor, const RpcParameterId id) noexcept {
            const auto found = std::ranges::lower_bound(descriptor.parameters, id, {}, &RpcParameterDescriptor::id);
            return found != descriptor.parameters.end() && found->id == id ? std::to_address(found) : nullptr;
        }

        [[nodiscard]] Result<void> ValidateSerializers(const RpcDescriptor &descriptor,
                                                       const std::span<const ReplicationSerializerDescriptor> serializers) {
            for (const RpcParameterDescriptor &parameter : descriptor.parameters) {
                if (!Supports(parameter, descriptor.owner, serializers))
                    return Result<void>::Failure(MakeError(NetworkErrors::RpcParameterUnsupported));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<RpcDescriptor>> BuildCandidate(const std::span<const RpcDescriptor> input,
                                                                        const std::span<const ReplicationSerializerDescriptor> serializers,
                                                                        const RpcDescriptorLimits &limits) {
            if (input.size() > limits.maximumRpcs)
                return Result<std::vector<RpcDescriptor>>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
            std::vector<RpcDescriptor> descriptors{input.begin(), input.end()};
            std::size_t defaultBytes{};
            for (RpcDescriptor &descriptor : descriptors) {
                std::ranges::sort(descriptor.parameters, {}, &RpcParameterDescriptor::id);
                std::ranges::sort(descriptor.tombstonedParameters);
                if (const Result<void> valid = ValidateRpcDescriptor(descriptor, limits); valid.HasError())
                    return Result<std::vector<RpcDescriptor>>::Failure(valid.ErrorValue());
                if (const Result<void> supported = ValidateSerializers(descriptor, serializers); supported.HasError())
                    return Result<std::vector<RpcDescriptor>>::Failure(supported.ErrorValue());
                for (const RpcParameterDescriptor &parameter : descriptor.parameters) {
                    if (!parameter.canonicalDefault.has_value())
                        continue;
                    const std::size_t count = parameter.canonicalDefault->canonicalBytes.size();
                    if (count > limits.maximumTotalDefaultBytes - defaultBytes)
                        return Result<std::vector<RpcDescriptor>>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
                    defaultBytes += count;
                }
            }
            std::ranges::sort(descriptors, {}, &RpcDescriptor::id);
            if (std::ranges::adjacent_find(descriptors, {}, &RpcDescriptor::id) != descriptors.end())
                return Result<std::vector<RpcDescriptor>>::Failure(MakeError(NetworkErrors::RpcDescriptorConflict));
            return Result<std::vector<RpcDescriptor>>::Success(std::move(descriptors));
        }

        [[nodiscard]] bool RetainsParameters(const RpcDescriptor &previous, const RpcDescriptor &candidate) {
            return std::ranges::all_of(previous.parameters, [&](const RpcParameterDescriptor &parameter) {
                if (const RpcParameterDescriptor *replacement = FindParameter(candidate, parameter.id); replacement != nullptr)
                    return *replacement == parameter;
                return parameter.requirement == RpcParameterRequirement::Optional &&
                       std::ranges::binary_search(candidate.tombstonedParameters, parameter.id);
            });
        }

        [[nodiscard]] bool CompatibleAdditions(const RpcDescriptor &previous, const RpcDescriptor &candidate) {
            return std::ranges::all_of(candidate.parameters, [&](const RpcParameterDescriptor &parameter) {
                if (FindParameter(previous, parameter.id) != nullptr)
                    return true;
                return parameter.introducedVersion > previous.version && parameter.requirement == RpcParameterRequirement::Optional &&
                       parameter.canonicalDefault.has_value();
            });
        }

        [[nodiscard]] bool RetainsTombstones(const RpcDescriptor &previous, const RpcDescriptor &candidate) {
            return std::ranges::all_of(previous.tombstonedParameters, [&](const RpcParameterId id) {
                return std::ranges::binary_search(candidate.tombstonedParameters, id);
            });
        }

        [[nodiscard]] bool PreservesParameterIdentities(const RpcDescriptor &previous, const RpcDescriptor &candidate) {
            return std::ranges::all_of(previous.parameters, [&](const RpcParameterDescriptor &parameter) {
                if (const RpcParameterDescriptor *retained = FindParameter(candidate, parameter.id); retained != nullptr)
                    return retained->valueType == parameter.valueType && retained->codec == parameter.codec;
                return std::ranges::binary_search(candidate.tombstonedParameters, parameter.id);
            });
        }

        [[nodiscard]] bool RetainsRoutingSemantics(const RpcDescriptor &previous, const RpcDescriptor &candidate) noexcept {
            return candidate.direction == previous.direction && candidate.delivery == previous.delivery &&
                   candidate.target == previous.target && candidate.permission == previous.permission &&
                   candidate.customPermission == previous.customPermission;
        }

        [[nodiscard]] bool RetainsInvocationBounds(const RpcDescriptor &previous, const RpcDescriptor &candidate) noexcept {
            return candidate.rateLimit == previous.rateLimit && candidate.maximumPayloadBytes == previous.maximumPayloadBytes;
        }

        [[nodiscard]] bool CompatibleReplacement(const RpcDescriptor &previous, const RpcDescriptor &candidate) {
            if (candidate.owner != previous.owner || candidate.version < previous.version)
                return false;
            if (candidate.version == previous.version)
                return candidate == previous;
            if (!RetainsTombstones(previous, candidate) || !PreservesParameterIdentities(previous, candidate))
                return false;
            if (!candidate.compatibility.Contains(previous.version))
                return candidate.version.major > previous.version.major;
            return RetainsRoutingSemantics(previous, candidate) && RetainsInvocationBounds(previous, candidate) &&
                   RetainsParameters(previous, candidate) && CompatibleAdditions(previous, candidate);
        }

        [[nodiscard]] Result<void> ValidateReplacement(const RpcDescriptorSnapshotPtr &previous,
                                                       const std::span<const RpcDescriptor> candidate) {
            if (previous == nullptr)
                return Result<void>::Failure(MakeError(NetworkErrors::RpcDescriptorInvalid));
            for (const RpcDescriptor &prior : previous->Descriptors()) {
                const RpcDescriptor *replacement = FindDescriptor(candidate, prior.id);
                if (replacement == nullptr || !CompatibleReplacement(prior, *replacement))
                    return Result<void>::Failure(MakeError(NetworkErrors::RpcDescriptorIncompatible));
            }
            return Result<void>::Success();
        }

    }  // namespace

    RpcDescriptorSnapshot::RpcDescriptorSnapshot(ConstructionKey, std::vector<RpcDescriptor> descriptors, const Sha256Digest &fingerprint)
        : descriptors_(std::move(descriptors)), fingerprint_(fingerprint) {}

    Result<RpcDescriptorSnapshotPtr> RpcDescriptorSnapshot::Build(const std::span<const RpcDescriptor> descriptors,
                                                                  const std::span<const ReplicationSerializerDescriptor> serializers,
                                                                  const RpcDescriptorLimits &limits,
                                                                  const RpcDescriptorSnapshotPtr *previous) {
        try {
            auto candidate = BuildCandidate(descriptors, serializers, limits);
            if (candidate.HasError())
                return Result<RpcDescriptorSnapshotPtr>::Failure(candidate.ErrorValue());
            if (previous != nullptr) {
                if (const Result<void> compatible = ValidateReplacement(*previous, candidate.Value()); compatible.HasError())
                    return Result<RpcDescriptorSnapshotPtr>::Failure(compatible.ErrorValue());
            }
            const Sha256Digest fingerprint = ::Horo::Network::Fingerprint(candidate.Value());
            return Result<RpcDescriptorSnapshotPtr>::Success(
                std::make_shared<const RpcDescriptorSnapshot>(ConstructionKey{}, std::move(candidate).Value(), fingerprint));
        } catch (const std::bad_alloc &) {
            return Result<RpcDescriptorSnapshotPtr>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        }
    }

    /** @copydoc RpcDescriptorSnapshot::Descriptors */
    std::span<const RpcDescriptor> RpcDescriptorSnapshot::Descriptors() const noexcept {
        return descriptors_;
    }

    /** @copydoc RpcDescriptorSnapshot::Find */
    Result<const RpcDescriptor *> RpcDescriptorSnapshot::Find(const RpcId id) const {
        if (!id.IsValid())
            return Result<const RpcDescriptor *>::Failure(MakeError(NetworkErrors::IdentityInvalid));
        const RpcDescriptor *descriptor = FindDescriptor(descriptors_, id);
        if (descriptor == nullptr)
            return Result<const RpcDescriptor *>::Failure(MakeError(NetworkErrors::RpcUnknown));
        return Result<const RpcDescriptor *>::Success(descriptor);
    }

    /** @copydoc RpcDescriptorSnapshot::Fingerprint */
    const Sha256Digest &RpcDescriptorSnapshot::Fingerprint() const noexcept {
        return fingerprint_;
    }

    /** @copydoc BuildRpcDescriptorSnapshot */
    Result<RpcDescriptorSnapshotPtr> BuildRpcDescriptorSnapshot(const std::span<const RpcDescriptor> descriptors,
                                                                const std::span<const ReplicationSerializerDescriptor> serializers,
                                                                const RpcDescriptorLimits &limits) {
        return RpcDescriptorSnapshot::Build(descriptors, serializers, limits, nullptr);
    }

    /** @copydoc BuildRpcDescriptorReplacement */
    Result<RpcDescriptorSnapshotPtr> BuildRpcDescriptorReplacement(const RpcDescriptorSnapshotPtr &previous,
                                                                   const std::span<const RpcDescriptor> descriptors,
                                                                   const std::span<const ReplicationSerializerDescriptor> serializers,
                                                                   const RpcDescriptorLimits &limits) {
        return RpcDescriptorSnapshot::Build(descriptors, serializers, limits, &previous);
    }
}  // namespace Horo::Network
