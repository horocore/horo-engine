#include "ReplicationStateCodecTestSupport.h"

#include <functional>

namespace Horo::Network {
    using namespace StateCodecTestSupport;

    namespace {
        class CallbackCodec final : public IReplicationFieldSerializer {
        public:
            const ReplicationSerializerDescriptor &Descriptor() const noexcept override {
                return scalar->Descriptor();
            }

            Result<std::vector<std::byte>> Encode(const ReplicationRuntimeValue &value) const override {
                if (onEncode)
                    onEncode();
                if (rejectEncode)
                    return Result<std::vector<std::byte>>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
                return scalar->Encode(value);
            }

            Result<ReplicationRuntimeValue> Decode(const std::span<const std::byte> bytes) const override {
                if (onDecode)
                    onDecode();
                if (rejectDecode)
                    return Result<ReplicationRuntimeValue>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
                if (noncanonical)
                    return Result<ReplicationRuntimeValue>::Success(99.0);
                return scalar->Decode(bytes);
            }

            Result<bool> CanonicallyEqual(const ReplicationRuntimeValue &left, const ReplicationRuntimeValue &right) const override {
                if (rejectCompare)
                    return Result<bool>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
                return scalar->CanonicallyEqual(left, right);
            }

            std::shared_ptr<const CanonicalScalarReplicationSerializer> scalar{
                CanonicalScalarReplicationSerializer::Create(
                    {ValueType(1), Codec(1), {.value = "game.replication"}, ReplicationValueKind::FloatingPoint, {}, 8, 1})
                    .Value()};
            std::function<void()> onEncode, onDecode;
            bool rejectEncode{}, rejectDecode{}, rejectCompare{}, noncanonical{};
        };

        struct FaultFixture final {
            ReplicationWorldLifecycle lifecycle{Lifecycle()};
            std::shared_ptr<Owner> owner{std::make_shared<Owner>()};
            std::shared_ptr<CallbackCodec> serializer{std::make_shared<CallbackCodec>()};
            std::shared_ptr<const ReplicationSerializerRegistry> registry;
            std::unique_ptr<ReplicationStateCapture> capture;
            std::unique_ptr<ReplicationStateCodec> codec;

            FaultFixture() {
                const std::array schemas{Schema()};
                const auto descriptors = BuildReplicationDescriptorSnapshot(schemas, Limits).Value();
                const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{serializer};
                registry = std::make_shared<const ReplicationSerializerRegistry>(
                    ReplicationSerializerRegistry::Create(descriptors, codecs).Value());
                REQUIRE(lifecycle.RegisterObject(World().scene, World().session, Object()).HasValue());
                const std::array targets{ReplicationCaptureTarget{Object(), owner}};
                capture = ReplicationStateCapture::Prepare(Read(lifecycle), registry, targets).Value();
                REQUIRE(capture->CaptureAtCommit(Read(lifecycle), 1).HasValue());
                codec = MakeCodec(registry);
            }

            ReplicationCapturedStatePin Pin() {
                return capture->Latest(Object().object).Value();
            }
        };
    }  // namespace

    TEST_CASE("Foreign codec decode faults and canonical mismatches publish no state", "[network][state-codec]") {
        FaultFixture fixture;
        const auto wire = fixture.codec->Encode(fixture.Pin()).Value();
        SECTION("typed rejection") {
            fixture.serializer->rejectDecode = true;
        }
        SECTION("foreign throw") {
            fixture.serializer->onDecode = [] {
                throw ForeignCallbackFault{};
            };
        }
        SECTION("allocation failure") {
            fixture.serializer->onDecode = [] {
                throw std::bad_alloc{};
            };
        }
        SECTION("noncanonical value") {
            fixture.serializer->noncanonical = true;
        }
        SECTION("reencode rejection") {
            fixture.serializer->rejectEncode = true;
        }
        REQUIRE(fixture.codec->Decode(wire, Object()).HasError());
        REQUIRE(fixture.Pin()->IsCurrent());
    }

    TEST_CASE("Decode fences cancellation and shutdown after both decoder and canonical encoder", "[network][state-codec]") {
        FaultFixture fixture;
        const auto wire = fixture.codec->Encode(fixture.Pin()).Value();
        CancellationSource cancellation;
        SECTION("decoder cancels") {
            fixture.serializer->onDecode = [&] {
                cancellation.RequestCancellation();
            };
        }
        SECTION("encoder cancels") {
            fixture.serializer->onEncode = [&] {
                cancellation.RequestCancellation();
            };
        }
        SECTION("decoder closes") {
            fixture.serializer->onDecode = [&] {
                fixture.codec->Shutdown();
            };
        }
        SECTION("encoder closes") {
            fixture.serializer->onEncode = [&] {
                fixture.codec->Shutdown();
            };
        }
        REQUIRE(fixture.codec->Decode(wire, Object(), nullptr, cancellation.Token()).HasError());
    }

    TEST_CASE("Encode contains module failure and post-encode retirement without modifying source", "[network][state-codec]") {
        FaultFixture fixture;
        CancellationSource cancellation;
        SECTION("typed rejection") {
            fixture.serializer->rejectEncode = true;
        }
        SECTION("foreign throw") {
            fixture.serializer->onEncode = [] {
                throw ForeignCallbackFault{};
            };
        }
        SECTION("allocation failure") {
            fixture.serializer->onEncode = [] {
                throw std::bad_alloc{};
            };
        }
        SECTION("cancel") {
            fixture.serializer->onEncode = [&] {
                cancellation.RequestCancellation();
            };
        }
        SECTION("capture retire") {
            fixture.serializer->onEncode = [&] {
                fixture.capture->Shutdown();
            };
        }
        const auto pin = fixture.Pin();
        REQUIRE(fixture.codec->Encode(pin, {}, cancellation.Token()).HasError());
        REQUIRE(std::get<double>(pin->Fields()[0].value) == 0.0);
    }

    TEST_CASE("Typed canonical comparison failure cannot silently choose a full or partial delta", "[network][state-codec]") {
        FaultFixture fixture;
        const auto first = fixture.Pin();
        fixture.owner->Commit(2, 2.0);
        REQUIRE(fixture.capture->CaptureAtCommit(Read(fixture.lifecycle, 2), 2).HasValue());
        fixture.serializer->rejectCompare = true;
        REQUIRE(fixture.codec->Encode(fixture.Pin(), Ack(first, *fixture.codec)).HasError());
    }
}  // namespace Horo::Network
