#include "Horo/Network/RpcDescriptorRegistry.h"
#include "NetworkTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <type_traits>

namespace Horo::Network {
    namespace {
        constexpr RpcDescriptorLimits Limits{};

        [[nodiscard]] RpcId RpcIdentity(const std::uint64_t value) {
            return RpcId::Create(value).Value();
        }

        [[nodiscard]] RpcParameterId ParameterIdentity(const std::uint32_t value) {
            return RpcParameterId::Create(value).Value();
        }

        [[nodiscard]] ReplicationValueTypeId ValueType(const std::uint32_t value = 1) {
            return ReplicationValueTypeId::Create(value).Value();
        }

        [[nodiscard]] ReplicationCodecId Codec(const std::uint32_t value = 1) {
            return ReplicationCodecId::Create(value).Value();
        }

        [[nodiscard]] RpcParameterDescriptor Parameter(const std::uint32_t id, const ReplicationSchemaVersion introduced = {1, 0}) {
            return {
                .id = ParameterIdentity(id),
                .valueType = ValueType(),
                .codec = Codec(),
                .introducedVersion = introduced,
                .requirement = RpcParameterRequirement::Required,
                .limits = {.maximumEncodedBytes = 64, .maximumElementCount = 1},
            };
        }

        [[nodiscard]] RpcDescriptor Rpc(const std::uint64_t id, std::vector<RpcParameterDescriptor> parameters) {
            return {
                .id = RpcIdentity(id),
                .version = {1, 0},
                .compatibility = {.minimum = {1, 0}, .maximum = {1, 0}},
                .owner = ModuleId{"game.rpc"},
                .direction = RpcDirection::ClientToAuthority,
                .delivery = RpcDelivery::ReliableOrdered,
                .target = RpcTarget::Authority,
                .permission = RpcCallerPermission::ObjectOwner,
                .rateLimit = {.maximumCallsPerSecond = 30, .maximumBurst = 5},
                .maximumPayloadBytes = 256,
                .parameters = std::move(parameters),
            };
        }

        [[nodiscard]] ReplicationSerializerDescriptor Serializer() {
            return {
                .valueType = ValueType(),
                .codec = Codec(),
                .owner = ModuleId{"game.rpc"},
                .valueKind = ReplicationValueKind::SignedInteger,
                .maximumEncodedBytes = 64,
                .maximumElementCount = 1,
            };
        }

        template <typename T>
        concept HasExecutableCallback = requires(T value) { value.callback; };
    }  // namespace

    static_assert(!HasExecutableCallback<RpcDescriptor>);
    static_assert(std::is_same_v<decltype(RpcDescriptor::parameters), std::vector<RpcParameterDescriptor>>);

    TEST_CASE("RPC snapshot accepts typed declarations and canonicalizes identity order", "[unit][network][rpc]") {
        const std::array descriptors{Rpc(20, {Parameter(2), Parameter(1)}), Rpc(10, {Parameter(1)})};
        const std::array serializers{Serializer()};
        const auto snapshot = BuildRpcDescriptorSnapshot(descriptors, serializers, Limits).Value();

        REQUIRE(snapshot->Descriptors().size() == 2);
        REQUIRE(snapshot->Descriptors()[0].id == RpcIdentity(10));
        REQUIRE(snapshot->Descriptors()[1].parameters[0].id == ParameterIdentity(1));
        REQUIRE(snapshot->Descriptors()[1].parameters[1].id == ParameterIdentity(2));
        REQUIRE(snapshot->Find(RpcIdentity(20)).Value() == &snapshot->Descriptors()[1]);
        TestSupport::RequireError(snapshot->Find(RpcIdentity(999)), NetworkErrors::RpcUnknown);

        const std::array reordered{Rpc(10, {Parameter(1)}), Rpc(20, {Parameter(1), Parameter(2)})};
        REQUIRE(snapshot->Fingerprint() == BuildRpcDescriptorSnapshot(reordered, serializers, Limits).Value()->Fingerprint());
    }

    TEST_CASE("RPC registration rejects duplicate identity unsupported parameters and unsafe permissions", "[unit][network][rpc]") {
        const std::array serializers{Serializer()};
        const std::array duplicates{Rpc(10, {Parameter(1)}), Rpc(10, {Parameter(2)})};
        TestSupport::RequireError(BuildRpcDescriptorSnapshot(duplicates, serializers, Limits), NetworkErrors::RpcDescriptorConflict);

        auto duplicateParameter = Rpc(10, {Parameter(1), Parameter(1)});
        const std::array duplicateParameterSet{duplicateParameter};
        TestSupport::RequireError(BuildRpcDescriptorSnapshot(duplicateParameterSet, serializers, Limits),
                                  NetworkErrors::RpcDescriptorConflict);

        auto unsupported = Rpc(10, {Parameter(1)});
        unsupported.parameters.front().codec = Codec(9);
        const std::array unsupportedSet{unsupported};
        TestSupport::RequireError(BuildRpcDescriptorSnapshot(unsupportedSet, serializers, Limits), NetworkErrors::RpcParameterUnsupported);

        auto malformedSerializer = Serializer();
        malformedSerializer.valueKind = ReplicationValueKind::Count;
        const std::array malformedSerializers{malformedSerializer};
        const std::array supportedSet{Rpc(10, {Parameter(1)})};
        TestSupport::RequireError(BuildRpcDescriptorSnapshot(supportedSet, malformedSerializers, Limits),
                                  NetworkErrors::RpcParameterUnsupported);

        auto unsafe = Rpc(10, {Parameter(1)});
        unsafe.permission = RpcCallerPermission::AuthorityOnly;
        const std::array unsafeSet{unsafe};
        TestSupport::RequireError(BuildRpcDescriptorSnapshot(unsafeSet, serializers, Limits), NetworkErrors::RpcDescriptorInvalid);
    }

    TEST_CASE("RPC descriptor validation fails closed for malformed routing defaults rates and bounds", "[unit][network][rpc]") {
        auto outbound = Rpc(10, {Parameter(1)});
        outbound.direction = RpcDirection::AuthorityToClient;
        outbound.target = RpcTarget::AllClients;
        outbound.permission = RpcCallerPermission::AuthorityOnly;
        REQUIRE(ValidateRpcDescriptor(outbound, Limits).HasValue());

        auto malformed = outbound;
        malformed.target = RpcTarget::Authority;
        TestSupport::RequireError(ValidateRpcDescriptor(malformed, Limits), NetworkErrors::RpcDescriptorInvalid);

        malformed = Rpc(10, {Parameter(1)});
        malformed.rateLimit.maximumBurst = 31;
        TestSupport::RequireError(ValidateRpcDescriptor(malformed, Limits), NetworkErrors::RpcDescriptorInvalid);

        malformed = Rpc(10, {Parameter(1)});
        malformed.parameters.front().requirement = RpcParameterRequirement::Optional;
        TestSupport::RequireError(ValidateRpcDescriptor(malformed, Limits), NetworkErrors::RpcDescriptorInvalid);

        malformed.parameters.front().canonicalDefault = RpcParameterDefault{{std::byte{0x01}}};
        REQUIRE(ValidateRpcDescriptor(malformed, Limits).HasValue());

        RpcDescriptorLimits narrow = Limits;
        narrow.maximumPayloadBytes = 128;
        TestSupport::RequireError(ValidateRpcDescriptor(malformed, narrow), NetworkErrors::RpcCapacityExceeded);
    }

    TEST_CASE("RPC compatible replacement admits optional additions and preserves prior generation on failure", "[unit][network][rpc]") {
        const std::array serializers{Serializer()};
        const std::array initialDescriptors{Rpc(10, {Parameter(1)})};
        const auto previous = BuildRpcDescriptorSnapshot(initialDescriptors, serializers, Limits).Value();
        const Sha256Digest priorFingerprint = previous->Fingerprint();

        auto replacement = Rpc(10, {Parameter(1)});
        replacement.version = {1, 1};
        replacement.compatibility.maximum = replacement.version;
        auto optional = Parameter(2, {1, 1});
        optional.requirement = RpcParameterRequirement::Optional;
        optional.canonicalDefault = RpcParameterDefault{{std::byte{0x00}}};
        replacement.parameters.push_back(optional);
        const std::array replacementSet{replacement};
        const auto accepted = BuildRpcDescriptorReplacement(previous, replacementSet, serializers, Limits);
        REQUIRE(accepted.HasValue());
        REQUIRE((accepted.Value()->Descriptors().front().version == ReplicationSchemaVersion{1, 1}));

        replacement.delivery = RpcDelivery::Unreliable;
        const std::array incompatibleSet{replacement};
        TestSupport::RequireError(BuildRpcDescriptorReplacement(previous, incompatibleSet, serializers, Limits),
                                  NetworkErrors::RpcDescriptorIncompatible);
        REQUIRE(previous->Fingerprint() == priorFingerprint);
        REQUIRE((previous->Descriptors().front().version == ReplicationSchemaVersion{1, 0}));

        auto narrowedMinor = Rpc(10, {Parameter(1)});
        narrowedMinor.version = {1, 1};
        narrowedMinor.compatibility = {.minimum = {1, 1}, .maximum = {1, 1}};
        const std::array narrowedMinorSet{narrowedMinor};
        TestSupport::RequireError(BuildRpcDescriptorReplacement(previous, narrowedMinorSet, serializers, Limits),
                                  NetworkErrors::RpcDescriptorIncompatible);

        auto priorWithTombstone = Rpc(20, {Parameter(1)});
        priorWithTombstone.tombstonedParameters.push_back(ParameterIdentity(9));
        const std::array priorWithTombstoneSet{priorWithTombstone};
        const auto previousMajor = BuildRpcDescriptorSnapshot(priorWithTombstoneSet, serializers, Limits).Value();
        auto nextMajor = Rpc(20, {Parameter(1)});
        nextMajor.version = {2, 0};
        nextMajor.compatibility = {.minimum = {2, 0}, .maximum = {2, 0}};
        const std::array nextMajorSet{nextMajor};
        TestSupport::RequireError(BuildRpcDescriptorReplacement(previousMajor, nextMajorSet, serializers, Limits),
                                  NetworkErrors::RpcDescriptorIncompatible);

        nextMajor.tombstonedParameters.push_back(ParameterIdentity(9));
        nextMajor.parameters.clear();
        const std::array missingRetirementSet{nextMajor};
        TestSupport::RequireError(BuildRpcDescriptorReplacement(previousMajor, missingRetirementSet, serializers, Limits),
                                  NetworkErrors::RpcDescriptorIncompatible);

        nextMajor.tombstonedParameters.push_back(ParameterIdentity(1));
        const std::array retiredMajorSet{nextMajor};
        REQUIRE(BuildRpcDescriptorReplacement(previousMajor, retiredMajorSet, serializers, Limits).HasValue());
    }

    TEST_CASE("RPC snapshot owns declaration storage and rejects null or incomplete lifecycle replacement", "[unit][network][rpc]") {
        const std::array serializers{Serializer()};
        RpcDescriptorSnapshotPtr snapshot;
        {
            const std::array temporary{Rpc(10, {Parameter(1)})};
            snapshot = BuildRpcDescriptorSnapshot(temporary, serializers, Limits).Value();
        }
        REQUIRE(snapshot->Descriptors().front().parameters.front().id == ParameterIdentity(1));

        const std::array emptyReplacement{Rpc(20, {Parameter(1)})};
        TestSupport::RequireError(BuildRpcDescriptorReplacement(snapshot, emptyReplacement, serializers, Limits),
                                  NetworkErrors::RpcDescriptorIncompatible);
        TestSupport::RequireError(BuildRpcDescriptorReplacement({}, emptyReplacement, serializers, Limits),
                                  NetworkErrors::RpcDescriptorInvalid);
    }
}  // namespace Horo::Network
