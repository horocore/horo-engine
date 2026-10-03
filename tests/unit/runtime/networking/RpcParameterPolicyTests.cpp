#include "RpcAuthorizationPolicyTestSupport.h"

#include <limits>

namespace Horo::Network {
    using RpcAuthorizationPolicyTestSupport::CallerPolicy;
    using RpcAuthorizationPolicyTestSupport::Install;
    using RpcDispatchTestSupport::Fixture;

    namespace {
        /** @brief Expected terminal category for the owned numeric policy fixture. */
        enum class NumericOutcome {
            InvalidPolicy,
            InvalidValue,
            Allowed
        };

        class NumericHandler final : public IRpcGameplayHandler {
        public:
            std::size_t calls{};
            ReplicationRuntimeValue last;

            Result<void> Execute(const RpcGameplayContext &, const std::span<const ReplicationRuntimeValue> parameters) override {
                ++calls;
                last = parameters.front();
                return Result<void>::Success();
            }
        };

        /** @brief Owns the exact numeric codec and handler for parameter-policy assertions. */
        struct NumericHarness final {
            std::shared_ptr<RpcGameplayDispatch> dispatch;
            std::shared_ptr<const CanonicalScalarReplicationSerializer> serializer;
            std::shared_ptr<NumericHandler> handler{std::make_shared<NumericHandler>()};
            std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs;

            NumericHarness(Fixture &fixture, const ReplicationSerializerDescriptor &metadata)
                : serializer(CanonicalScalarReplicationSerializer::Create(metadata).Value()), codecs{serializer} {
                fixture.descriptor.parameters.front().valueType = metadata.valueType;
                fixture.descriptor.parameters.front().codec = metadata.codec;
                const std::array declarations{fixture.descriptor};
                const std::array descriptions{metadata};
                const auto descriptors = BuildRpcDescriptorSnapshot(declarations, descriptions).Value();
                dispatch = RpcGameplayDispatch::Create(descriptors, fixture.world).Value();
                REQUIRE(
                    dispatch->RegisterPeer(fixture.session, fixture.connection, fixture.generation, fixture.peer, RpcRemoteRole::Client, 22)
                        .HasValue());
                REQUIRE(dispatch->RegisterObject(fixture.object, fixture.role).HasValue());
            }

            Result<void> Receive(const Fixture &fixture, const ReplicationRuntimeValue &value) const {
                auto message = fixture.Message(1);
                const auto bytes = serializer->Encode(value).Value();
                REQUIRE(bytes.size() == 8);
                message.payload.resize(58);
                message.payload.insert(message.payload.end(), bytes.begin(), bytes.end());
                return dispatch->HandleAdmitted(fixture.Context(), message);
            }

            /** @brief Checks registration, receipt and real handler execution as one typed policy outcome. */
            void Verify(const Fixture &fixture, const RpcGameplayPolicy &policy, const ReplicationRuntimeValue &value,
                        const NumericOutcome expected) const {
                const auto installed = dispatch->RegisterHandler(fixture.rpc, handler, codecs, fixture.moduleLease, policy);
                if (expected == NumericOutcome::InvalidPolicy) {
                    TestSupport::RequireError(installed, NetworkErrors::RpcParameterInvalid);
                    return;
                }
                REQUIRE(installed.HasValue());
                const auto received = Receive(fixture, value);
                if (expected == NumericOutcome::InvalidValue) {
                    TestSupport::RequireError(received, NetworkErrors::RpcParameterInvalid);
                    REQUIRE(handler->calls == 0);
                    return;
                }
                REQUIRE(received.HasValue());
                REQUIRE(dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
                REQUIRE(handler->last == value);
            }
        };
    }  // namespace

    TEST_CASE("RPC policy declaration rejects mismatched identity, malformed and duplicate typed constraints",
              "[unit][network][rpc][policy]") {
        Fixture fixture(false, RpcTarget::Authority, true, true);
        fixture.dispatch->RevokeHandler(fixture.rpc);
        const std::array codecs{fixture.serializer};
        RpcGameplayPolicy policy;
        SECTION("foreign custom identity") {
            policy.permission = RpcPermissionId::Create(2).Value();
            policy.caller = std::make_shared<CallerPolicy>();
        }
        SECTION("missing implementation") {
            policy.permission = RpcPermissionId::Create(1).Value();
        }
        SECTION("foreign parameter") {
            policy.parameters.emplace_back(RpcParameterId::Create(2).Value(), std::int64_t{0}, std::int64_t{10});
        }
        SECTION("inverted range") {
            policy.parameters.emplace_back(RpcParameterId::Create(1).Value(), std::int64_t{10}, std::int64_t{0});
        }
        SECTION("wrong numeric representation") {
            policy.parameters.emplace_back(RpcParameterId::Create(1).Value(), std::uint64_t{0}, std::uint64_t{10});
        }
        SECTION("mixed range representation") {
            policy.parameters.emplace_back(RpcParameterId::Create(1).Value(), std::int64_t{0}, std::uint64_t{10});
        }
        SECTION("duplicate constraint") {
            policy.parameters.emplace_back(RpcParameterId::Create(1).Value(), std::int64_t{0}, std::int64_t{10});
            policy.parameters.push_back(policy.parameters.front());
        }
        SECTION("schema without version") {
            policy.objectSchema = ReplicationSchemaId::Create(3).Value();
        }
        SECTION("version without schema") {
            policy.objectSchemaVersion = {1, 0};
        }
        REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease, policy).HasError());
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasError());
        REQUIRE(fixture.handler->calls == 0);
    }

    TEST_CASE("RPC scalar ranges are inclusive and reject canonical but invalid Gameplay values", "[unit][network][rpc][policy]") {
        Fixture fixture(false, RpcTarget::Authority, true, true);
        RpcGameplayPolicy policy;
        SECTION("exact endpoints") {
            policy.parameters.emplace_back(RpcParameterId::Create(1).Value(), std::int64_t{7}, std::int64_t{7});
        }
        SECTION("below minimum") {
            policy.parameters.emplace_back(RpcParameterId::Create(1).Value(), std::int64_t{8}, std::int64_t{10});
        }
        SECTION("above maximum") {
            policy.parameters.emplace_back(RpcParameterId::Create(1).Value(), std::int64_t{0}, std::int64_t{6});
        }
        Install(fixture, policy);
        const auto received = fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1));
        if (std::get<std::int64_t>(policy.parameters.front().minimum) == 7) {
            REQUIRE(received.HasValue());
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        } else {
            TestSupport::RequireError(received, NetworkErrors::RpcParameterInvalid);
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().consumed == 0);
            REQUIRE(fixture.handler->calls == 0);
        }
    }

    TEST_CASE("RPC numeric constraints preserve unsigned precision and require finite floating endpoints", "[unit][network][rpc][policy]") {
        Fixture fixture(false, RpcTarget::Authority, true, true);
        RpcScalar minimum = 7.0;
        RpcScalar maximum = 7.0;
        ReplicationRuntimeValue value = 7.0;
        auto expected = NumericOutcome::Allowed;
        auto metadata = Fixture::SerializerMetadata();
        metadata.valueType = ReplicationValueTypeId::Create(2).Value();
        metadata.codec = ReplicationCodecId::Create(2).Value();
        metadata.valueKind = ReplicationValueKind::FloatingPoint;
        SECTION("floating endpoint") {
            // The unchanged finite endpoints exercise the successful floating representation.
        }
        SECTION("unsigned endpoint cannot lose precision through double") {
            minimum = std::numeric_limits<std::uint64_t>::max();
            maximum = minimum;
            value = std::numeric_limits<std::uint64_t>::max();
            metadata.valueKind = ReplicationValueKind::UnsignedInteger;
        }
        SECTION("floating outside range") {
            value = 8.0;
            expected = NumericOutcome::InvalidValue;
        }
        SECTION("NaN endpoint") {
            minimum = std::numeric_limits<double>::quiet_NaN();
            expected = NumericOutcome::InvalidPolicy;
        }
        SECTION("infinite maximum") {
            maximum = std::numeric_limits<double>::infinity();
            expected = NumericOutcome::InvalidPolicy;
        }
        SECTION("infinite minimum") {
            minimum = -std::numeric_limits<double>::infinity();
            expected = NumericOutcome::InvalidPolicy;
        }
        NumericHarness numeric(fixture, metadata);
        RpcGameplayPolicy policy;
        policy.parameters.emplace_back(RpcParameterId::Create(1).Value(), minimum, maximum);
        numeric.Verify(fixture, policy, value, expected);
    }

}  // namespace Horo::Network
