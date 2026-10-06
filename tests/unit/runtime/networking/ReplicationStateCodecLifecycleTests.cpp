#include "ReplicationStateCodecTestSupport.h"

namespace Horo::Network {
    using namespace StateCodecTestSupport;

    TEST_CASE("State codec cancellation and shutdown retain complete prior decoded state", "[network][state-codec]") {
        CodecFixture fixture;
        const auto wire = fixture.Delta();
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        REQUIRE(fixture.codec->Encode(fixture.capture.Pin(), {}, cancellation.Token()).HasError());
        REQUIRE(fixture.codec->Decode(wire, Object(), &fixture.decoded, cancellation.Token()).HasError());
        fixture.codec->Shutdown();
        fixture.codec->Shutdown();
        REQUIRE_FALSE(fixture.decoded.IsCurrent());
        REQUIRE(fixture.codec->Encode(fixture.capture.Pin()).HasError());
        REQUIRE(fixture.codec->Decode(fixture.full, Object()).HasError());
        REQUIRE(std::get<double>(fixture.decoded.Fields()[0].value) == 1.0);
        REQUIRE(fixture.capture.Pin()->IsCurrent());
    }

    TEST_CASE("World retirement and capture shutdown reject old source and acknowledgement generations", "[network][state-codec]") {
        CodecFixture fixture;
        SECTION("capture closes") {
            fixture.capture.capture->Shutdown();
        }
        SECTION("object retires") {
            REQUIRE(fixture.capture.lifecycle.RetireObject(World().scene, World().session, Object().object).HasValue());
        }
        SECTION("world closes") {
            fixture.capture.lifecycle.BeginShutdown();
        }
        REQUIRE(fixture.codec->Encode(fixture.first).HasError());
        REQUIRE_FALSE(fixture.first->IsCurrent());
        REQUIRE(std::get<double>(fixture.first->Fields()[0].value) == 1.0);
    }

    TEST_CASE("Cancellation and generation revocation inside canonical comparison reject partial output", "[network][state-codec]") {
        CodecFixture fixture;
        fixture.capture.Capture(2, 2.0);
        CancellationSource cancellation;
        SECTION("cancel") {
            fixture.capture.codec->context = &cancellation;
            fixture.capture.codec->onCompare = [](void *context) {
                static_cast<CancellationSource *>(context)->RequestCancellation();
            };
        }
        SECTION("close capture") {
            fixture.capture.codec->context = fixture.capture.capture.get();
            fixture.capture.codec->onCompare = [](void *context) {
                static_cast<ReplicationStateCapture *>(context)->Shutdown();
            };
        }
        SECTION("close codec") {
            fixture.capture.codec->context = fixture.codec.get();
            fixture.capture.codec->onCompare = [](void *context) {
                static_cast<ReplicationStateCodec *>(context)->Shutdown();
            };
        }
        SECTION("foreign exception") {
            fixture.capture.codec->onCompare = [](void *) {
                throw ForeignCallbackFault{};
            };
        }
        REQUIRE(fixture.codec->Encode(fixture.capture.Pin(), Ack(fixture.first, *fixture.codec), cancellation.Token()).HasError());
        REQUIRE(std::get<double>(fixture.decoded.Fields()[0].value) == 1.0);
    }

    TEST_CASE("State codec rejects recursive admission and foreign-thread calls", "[network][state-codec]") {
        CodecFixture fixture;
        fixture.capture.Capture(2, 2.0);

        struct Reentry final {
            CodecFixture *fixture;
            bool rejected{};
        } reentry{&fixture};

        fixture.capture.codec->context = &reentry;
        fixture.capture.codec->onCompare = [](void *context) {
            auto &state = *static_cast<Reentry *>(context);
            state.rejected = state.fixture->codec->Decode(state.fixture->full, Object()).HasError();
        };
        REQUIRE(fixture.codec->Encode(fixture.capture.Pin(), Ack(fixture.first, *fixture.codec)).HasValue());
        REQUIRE(reentry.rejected);
        bool rejected{};
        std::thread worker{[&] {
            rejected = fixture.codec->Decode(fixture.full, Object()).HasError();
        }};
        worker.join();
        REQUIRE(rejected);
    }

    TEST_CASE("Decoded baselines revoke when their sole codec owner is destroyed", "[network][state-codec]") {
        CodecFixture fixture;
        fixture.codec.reset();
        REQUIRE_FALSE(fixture.decoded.IsCurrent());
        REQUIRE(std::get<double>(fixture.decoded.Fields()[0].value) == 1.0);
    }
}  // namespace Horo::Network
