#include "AllocationProbe.h"
#include "ReplicationCaptureTestSupport.h"

#include <limits>

namespace Horo::Network {
    using namespace CaptureTestSupport;

    namespace {
        class ContainerCodec final : public IReplicationFieldSerializer {
        public:
            explicit ContainerCodec(const ReplicationValueKind kind)
                : descriptor{ValueType(kind == ReplicationValueKind::Utf8Text ? 1 : 2),
                             Codec(kind == ReplicationValueKind::Utf8Text ? 1 : 2),
                             {.value = "game.replication"},
                             kind,
                             {},
                             64,
                             64} {}

            const ReplicationSerializerDescriptor &Descriptor() const noexcept override {
                return descriptor;
            }

            Result<std::vector<std::byte>> Encode(const ReplicationRuntimeValue &) const override {
                ++encodes;
                return Result<std::vector<std::byte>>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
            }

            Result<ReplicationRuntimeValue> Decode(std::span<const std::byte>) const override {
                return Result<ReplicationRuntimeValue>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
            }

            Result<bool> CanonicallyEqual(const ReplicationRuntimeValue &left, const ReplicationRuntimeValue &right) const override {
                // This test codec admits ASCII, an explicit valid subset of UTF-8, without allocating canonical bytes.
                if (descriptor.valueKind == ReplicationValueKind::Utf8Text) {
                    for (const unsigned char value : std::get<std::string>(left))
                        if (value > 127)
                            return Result<bool>::Failure(MakeError(ReplicationCaptureErrors::Invalid));
                }
                return Result<bool>::Success(left == right);
            }

            ReplicationSerializerDescriptor descriptor;
            mutable std::size_t encodes{};
        };

        class ContainerOwner final : public ICommittedReplicationSource {
        public:
            Result<ReplicationCommittedRead> BeginRead(const NetworkObjectMappingEntry &, const std::uint64_t tick) const override {
                reading = true;
                return Result<ReplicationCommittedRead>::Success({tick, tick, tick});
            }

            Result<void> Capture(const ReplicationCommittedRead &, ReplicationCaptureWriter &writer) const override {
                const auto textResult = writer.Write(FieldIdValue(1), std::string_view{text.data(), elements});
                if (textResult.HasError())
                    return textResult;
                return writer.Write(FieldIdValue(2), std::span{bytes}.first(byteElements));
            }

            bool IsCurrent(const ReplicationCommittedRead &) const noexcept override {
                return reading;
            }

            void EndRead(const ReplicationCommittedRead &) const noexcept override {
                reading = false;
            }

            std::array<char, 65> text{};
            std::array<std::byte, 65> bytes{};
            std::size_t elements{64};
            std::size_t byteElements{64};
            mutable bool reading{};
        };

        struct ContainerFixture {
            ReplicationWorldLifecycle lifecycle{Lifecycle()};
            std::shared_ptr<ContainerCodec> textCodec{std::make_shared<ContainerCodec>(ReplicationValueKind::Utf8Text)};
            std::shared_ptr<ContainerCodec> bytesCodec{std::make_shared<ContainerCodec>(ReplicationValueKind::ByteSequence)};
            std::shared_ptr<ContainerOwner> owner{std::make_shared<ContainerOwner>()};
            std::unique_ptr<ReplicationStateCapture> capture;

            ContainerFixture() {
                REQUIRE(lifecycle.RegisterObject(World().scene, World().session, Object()).HasValue());
                auto text = Field(1);
                text.limits = {64, 64};
                auto bytes = Field(2);
                bytes.valueType = ValueType(2);
                bytes.codec = Codec(2);
                bytes.limits = {64, 64};
                const std::array schemas{Schema(10, {text, bytes})};
                const auto descriptors = BuildReplicationDescriptorSnapshot(schemas, Limits).Value();
                const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 2> codecs{textCodec, bytesCodec};
                auto registry = std::make_shared<const ReplicationSerializerRegistry>(
                    ReplicationSerializerRegistry::Create(descriptors, codecs).Value());
                const std::array targets{ReplicationCaptureTarget{Object(), owner}};
                capture = std::move(ReplicationStateCapture::Prepare(Read(lifecycle), registry, targets)).Value();
            }
        };

        class ScalarOwner final : public ICommittedReplicationSource {
        public:
            Result<ReplicationCommittedRead> BeginRead(const NetworkObjectMappingEntry &, const std::uint64_t tick) const override {
                reading = true;
                return Result<ReplicationCommittedRead>::Success({tick, tick, tick});
            }

            Result<void> Capture(const ReplicationCommittedRead &, ReplicationCaptureWriter &writer) const override {
                if (auto written = writer.Write(FieldIdValue(1), true); written.HasError())
                    return written;
                if (auto written = writer.Write(FieldIdValue(2), std::numeric_limits<std::int64_t>::min()); written.HasError())
                    return written;
                if (auto written = writer.Write(FieldIdValue(3), std::numeric_limits<std::uint64_t>::max()); written.HasError())
                    return written;
                return writer.Write(FieldIdValue(4), floating);
            }

            bool IsCurrent(const ReplicationCommittedRead &) const noexcept override {
                return reading;
            }

            void EndRead(const ReplicationCommittedRead &) const noexcept override {
                reading = false;
            }

            double floating{3.0};
            mutable bool reading{};
        };

        std::shared_ptr<const ReplicationSerializerRegistry> ScalarRegistry() {
            using enum ReplicationValueKind;
            const std::array kinds{Boolean, SignedInteger, UnsignedInteger, FloatingPoint};
            std::vector<ReplicationFieldDescriptor> fields;
            std::vector<std::shared_ptr<const IReplicationFieldSerializer>> codecs;
            for (std::size_t index{}; index < kinds.size(); ++index) {
                const auto id = static_cast<std::uint32_t>(index + 1);
                auto field = Field(id);
                field.valueType = ValueType(id);
                field.codec = Codec(id);
                fields.push_back(field);
                const ReplicationSerializerDescriptor
                    descriptor{ValueType(id), Codec(id), {.value = "game.replication"}, kinds[index], {}, 8, 1};
                codecs.push_back(CanonicalScalarReplicationSerializer::Create(descriptor).Value());
            }
            const std::array schemas{Schema(10, std::move(fields))};
            const auto snapshot = BuildReplicationDescriptorSnapshot(schemas, Limits).Value();
            return std::make_shared<const ReplicationSerializerRegistry>(ReplicationSerializerRegistry::Create(snapshot, codecs).Value());
        }
    }  // namespace

    TEST_CASE("Prepared containers rotate copied immutable storage without allocating or encoding", "[network][capture][allocation]") {
        ContainerFixture fixture;
        REQUIRE(fixture.capture->CaptureAtCommit(Read(fixture.lifecycle), 1).Value().published == 1);
        const auto first = fixture.capture->Latest(Object().object).Value();
        const auto allocations = Tests::AllocationProbe::Count();
        const auto frees = Tests::AllocationProbe::FreeCount();
        bool passed = true;
        for (std::uint64_t tick = 2; tick <= 64; ++tick) {
            fixture.owner->text.fill(static_cast<char>(tick));
            fixture.owner->bytes.fill(static_cast<std::byte>(tick));
            const auto read =
                fixture.lifecycle.AcquireCaptureRead({World().scene, World().session, Runtime::RuntimePhase::NetworkFlush, tick, {}});
            if (read.HasError()) {
                passed = false;
                break;
            }
            const auto report = fixture.capture->CaptureAtCommit(read.Value(), tick);
            if (report.HasError() || report.Value().published != 1) {
                passed = false;
                break;
            }
        }
        const auto after = Tests::AllocationProbe::Count();
        const auto reclaimed = Tests::AllocationProbe::FreeCount();
        REQUIRE(passed);
        REQUIRE(after == allocations);
        REQUIRE(reclaimed == frees);
        REQUIRE(std::get<std::string>(first->Fields()[0].value) == std::string(64, '\0'));
        REQUIRE(std::get<std::vector<std::byte>>(first->Fields()[1].value) == std::vector<std::byte>(64));
        REQUIRE(fixture.textCodec->encodes == 0);
        REQUIRE(fixture.bytesCodec->encodes == 0);
    }

    TEST_CASE("Oversized and invalid container values never replace a complete prior pin", "[network][capture]") {
        ContainerFixture fixture;
        REQUIRE(fixture.capture->CaptureAtCommit(Read(fixture.lifecycle), 1).Value().published == 1);
        const auto prior = fixture.capture->Latest(Object().object).Value();
        fixture.owner->elements = 65;
        REQUIRE(fixture.capture->CaptureAtCommit(Read(fixture.lifecycle, 2), 2).Value().failed == 1);
        REQUIRE(fixture.capture->Latest(Object().object).Value() == prior);
        fixture.owner->elements = 64;
        fixture.owner->byteElements = 65;
        REQUIRE(fixture.capture->CaptureAtCommit(Read(fixture.lifecycle, 3), 3).Value().failed == 1);
        REQUIRE(fixture.capture->Latest(Object().object).Value() == prior);
        fixture.owner->byteElements = 64;
        fixture.owner->text[0] = static_cast<char>(0xFF);
        REQUIRE(fixture.capture->CaptureAtCommit(Read(fixture.lifecycle, 4), 4).Value().failed == 1);
        REQUIRE(fixture.capture->Latest(Object().object).Value() == prior);
        REQUIRE_FALSE(fixture.owner->reading);
    }

    TEST_CASE("All scalar writer representations publish exact typed copies and reject nonfinite partial candidates",
              "[network][capture]") {
        auto lifecycle = Lifecycle();
        REQUIRE(lifecycle.RegisterObject(World().scene, World().session, Object()).HasValue());
        auto owner = std::make_shared<ScalarOwner>();
        const std::array targets{ReplicationCaptureTarget{Object(), owner}};
        auto capture = std::move(ReplicationStateCapture::Prepare(Read(lifecycle), ScalarRegistry(), targets)).Value();
        REQUIRE(capture->CaptureAtCommit(Read(lifecycle), 1).Value().published == 1);
        const auto prior = capture->Latest(Object().object).Value();
        REQUIRE(std::get<bool>(prior->Fields()[0].value));
        REQUIRE(std::get<std::int64_t>(prior->Fields()[1].value) == std::numeric_limits<std::int64_t>::min());
        REQUIRE(std::get<std::uint64_t>(prior->Fields()[2].value) == std::numeric_limits<std::uint64_t>::max());
        REQUIRE(std::get<double>(prior->Fields()[3].value) == 3.0);
        owner->floating = std::numeric_limits<double>::quiet_NaN();
        REQUIRE(capture->CaptureAtCommit(Read(lifecycle, 2), 2).Value().failed == 1);
        REQUIRE(capture->Latest(Object().object).Value() == prior);
        REQUIRE_FALSE(owner->reading);
        owner->floating = 4.0;
        REQUIRE(capture->CaptureAtCommit(Read(lifecycle, 3), 3).Value().published == 1);
        REQUIRE(std::get<double>(prior->Fields()[3].value) == 3.0);
    }
}  // namespace Horo::Network
