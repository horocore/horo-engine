#pragma once

#include "Horo/Network/ReplicationStateCodec.h"
#include "ReplicationCaptureTestSupport.h"

namespace Horo::Network::StateCodecTestSupport {
    using CaptureTestSupport::Codec;
    using CaptureTestSupport::CountingCodec;
    using CaptureTestSupport::Field;
    using CaptureTestSupport::FieldIdValue;
    using CaptureTestSupport::Fixture;
    using CaptureTestSupport::ForeignCallbackFault;
    using CaptureTestSupport::Lifecycle;
    using CaptureTestSupport::Limits;
    using CaptureTestSupport::Object;
    using CaptureTestSupport::Owner;
    using CaptureTestSupport::Read;
    using CaptureTestSupport::Registry;
    using CaptureTestSupport::Schema;
    using CaptureTestSupport::SchemaId;
    using CaptureTestSupport::ValueType;
    using CaptureTestSupport::World;
    inline constexpr std::size_t WireHeaderBytes = 172;

    inline ReplicationRoleBinding Recipient() {
        return {ReplicationRoleRevision::Create(1).Value(),
                World().session,
                Object().object,
                SchemaId(10),
                {1, 0},
                ReplicationExecutionRole::SimulatedClient,
                NetworkPeerId::Create(2).Value(),
                std::nullopt};
    }

    inline std::unique_ptr<ReplicationStateCodec> MakeCodec(const std::shared_ptr<const ReplicationSerializerRegistry> &registry,
                                                            const ReplicationStateCodecLimits &limits = {}) {
        auto result = ReplicationStateCodec::Create(registry, Recipient(), ReplicationRecordKind::Update, 1, {}, limits);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline ReplicationAcknowledgedBaseline Ack(ReplicationCapturedStatePin state, const ReplicationStateCodec &codec) {
        const auto revision = state->PublicationRevision();
        return {std::move(state), revision, Recipient().revision, codec.ProjectionFingerprint(), codec.DescriptorGeneration()};
    }

    inline void Put(std::vector<std::byte> &wire, const std::size_t offset, const std::uint64_t value, const std::size_t width) {
        for (std::size_t index{}; index < width; ++index)
            wire.at(offset + index) = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    }

    struct CodecFixture final {
        Fixture capture;
        std::unique_ptr<ReplicationStateCodec> codec{MakeCodec(capture.registry)};
        ReplicationCapturedStatePin first;
        std::vector<std::byte> full;
        ReplicationDecodedState decoded{Initial()};

        CodecFixture() = default;

        ReplicationDecodedState Initial() {
            capture.Capture(1, 1.0);
            first = capture.Pin();
            full = codec->Encode(first).Value();
            auto state = codec->Decode(full, Object());
            REQUIRE(state.HasValue());
            return std::move(state).Value();
        }

        std::vector<std::byte> Delta() {
            capture.Capture(2, 2.0);
            return codec->Encode(capture.Pin(), Ack(first, *codec)).Value();
        }
    };
}  // namespace Horo::Network::StateCodecTestSupport
