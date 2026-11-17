#include "AllocationProbe.h"
#include "PhysicsStateConstructionManifestCodec.h"
#include "PhysicsStateEnvelope.h"
#include "PhysicsStateRestartInput.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Physics::StateDetail {
    namespace {
        template <class T> T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().message : std::string{"success"}));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Format-only values provide no native/build/resource authority; public
         * owner admission is tested separately. */
        PhysicsStateCompatibility Compatibility() noexcept {
            PhysicsStateCompatibility value;
            value.build.bytes[0] = 1;
            value.platform.bytes[0] = 2;
            value.solver.bytes[0] = 3;
            value.settings.digest.bytes[0] = 4;
            value.content.bytes[0] = 5;
            value.topology.bytes[0] = 6;
            value.filters.bytes[0] = 7;
            value.surfaces.bytes[0] = 8;
            value.floatingPoint = {PhysicsStateFpArchitecture::ArmFpcr, 0};
            return value;
        }

        struct Fixture final {
            static constexpr PhysicsStateLimits Limits{2, 1, 1, 1, 1, 8192};
            std::array<PhysicsStateBody, 1> bodies{{{.id = 1}}};
            std::array<std::uint8_t, 2> scratch{};
            std::array<std::byte, 512> solverBytes{};
            std::array<std::byte, 128> constructionBytes{};
            std::array<std::byte, 4096> bytes{};
            std::array<PhysicsStateEventPair, 1> pairs{};
            std::array<PhysicsStatePublishedEvent, 4> events{};
            std::array<std::uint8_t, 4> eventOrdinals{};
            std::unique_ptr<SolverStorage> decoded{Take(PrepareSolverStorage(Limits))};
            std::size_t solverSize{}, constructionSize{}, size{};
            PhysicsStateWorldView world{.completedTick = 17};

            Fixture() {
                PhysicsStateSolverView solver{1.0F / 60.0F, {0, -9.81F, 0}, bodies};
                solverSize = Take(EncodeSolverState(solver, Limits, scratch, solverBytes));
                PrepareConstruction();
                Encode();
            }

            void PrepareConstruction() {
                const auto fp = Compatibility().floatingPoint;
                std::array<PhysicsStateShapeRecipe, 1> nodes{{{fp, PhysicsShapeDescriptor{PhysicsBoxShape{}}}}};
                std::array<PhysicsStateShapeRecipeRoot, 1> roots{{{1, 0}}};
                std::array<std::uint32_t, 2> depths{};
                const PhysicsStateConstructionManifest manifest{.shapes = {nodes, {}, roots}};
                const PhysicsStateConstructionManifestLimits limits{.shapes = {1, 0, 1, 1, 4096},
                                                                    .maximumRetainedBytes = 4096,
                                                                    .maximumEncodedBytes = constructionBytes.size()};
                constructionSize = Take(EncodeConstructionManifest(manifest, limits, fp, depths, constructionBytes));
            }

            void Encode() {
                size = Take(EncodeSnapshotEnvelope(Compatibility(), std::span{solverBytes}.first(solverSize), world, bytes, eventOrdinals,
                                                   std::span{constructionBytes}.first(constructionSize)));
            }

            Result<ParsedSnapshotEnvelope> Decode(std::size_t length) {
                return DecodeSnapshotEnvelope(std::span{bytes}.first(length), *decoded, pairs, events, eventOrdinals);
            }
        };
    }  // namespace

    TEST_CASE("Physics complete state envelope has one closed explicit wire manifest", "[physics][state][codec]") {
        Fixture fixture;
        REQUIRE(fixture.size == SnapshotHeader::Bytes + fixture.solverSize + fixture.constructionSize);
        SnapshotHeader header;
        std::array<std::byte, SnapshotHeader::Bytes> prefix{};
        Wire writer{std::span<std::byte>{prefix}};
        SnapshotHeaderFields(writer, header);
        REQUIRE(writer.Size() == prefix.size());
        auto parsed = Take(fixture.Decode(fixture.size));
        CHECK(parsed.compatibility == Compatibility());
        CHECK(parsed.world.completedTick == fixture.world.completedTick);
        CHECK(parsed.solver.bodies.front().id == 1);
        CHECK(std::ranges::equal(parsed.construction, std::span{fixture.constructionBytes}.first(fixture.constructionSize)));
    }

    TEST_CASE("Physics envelope rejects every truncation and unknown required "
              "world version",
              "[physics][state][malformed]") {
        Fixture fixture;
        for (std::size_t length = 0; length < fixture.size; ++length)
            REQUIRE(fixture.Decode(length).HasError());
        REQUIRE(fixture.Decode(fixture.size + 1).HasError());
        constexpr std::size_t WorldVersionOffset = 32 + 3 * 4 + 8 * 32 + 1 + 8;
        fixture.bytes[WorldVersionOffset] = std::byte{2};
        REQUIRE(fixture.Decode(fixture.size).HasError());
    }

    TEST_CASE("Physics envelope rejects hidden values under absent optional event fields", "[physics][state][malformed]") {
        Fixture fixture;
        fixture.world.previousPairs = fixture.pairs;
        fixture.Encode();
        REQUIRE(fixture.Decode(fixture.size).HasValue());
        const auto absentSubshapeValue =
            SnapshotHeader::Bytes + fixture.solverSize + fixture.constructionSize + 2 * sizeof(PhysicsStateObjectId) + 1;
        fixture.bytes[absentSubshapeValue] = std::byte{1};
        CHECK(fixture.Decode(fixture.size).HasError());
    }

    TEST_CASE("Physics envelope insufficient output preserves the complete previous wire", "[physics][state][bounds]") {
        Fixture fixture;
        const auto before = fixture.bytes;
        CHECK(EncodeSnapshotEnvelope(Compatibility(), std::span{fixture.solverBytes}.first(fixture.solverSize), fixture.world,
                                     std::span{fixture.bytes}.first(fixture.size - 1), fixture.eventOrdinals,
                                     std::span{fixture.constructionBytes}.first(fixture.constructionSize))
                  .HasError());
        CHECK(fixture.bytes == before);
    }

    TEST_CASE("Physics envelope canonical comparison and reused decode allocate no heap", "[physics][state][allocation]") {
        Fixture fixture;
        bool succeeded{};
        Tests::AllocationProbe::Measurement measured;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            succeeded = fixture.Decode(fixture.size).HasValue();
            measured = measurement.Snapshot();
        }
        REQUIRE(succeeded);
        CHECK(measured.requests == 0);
    }

    namespace {
        void PermuteEvents(Fixture &fixture) {
            constexpr std::array<std::uint32_t, 3> ordinals{2, 0, 1};
            for (std::size_t index = 0; index < ordinals.size(); ++index) {
                auto &event = fixture.events[index];
                event.tick = fixture.world.completedTick;
                event.ordinal = ordinals[index];
                event.pair.first.body = 2 * index + 1;
                event.pair.first.shape = 1;
                event.pair.second.body = 2 * index + 2;
                event.pair.second.shape = 1;
            }
            fixture.world.publishedEvents = std::span<const PhysicsStatePublishedEvent>{fixture.events}.first(ordinals.size());
            fixture.Encode();
        }

        void WriteOrdinal(Fixture &fixture, std::size_t row, std::uint32_t ordinal) noexcept {
            const auto offset = SnapshotHeader::Bytes + fixture.solverSize + fixture.constructionSize + row * SnapshotHeader::EventBytes +
                                sizeof(std::uint64_t);
            for (std::size_t index = 0; index < sizeof(ordinal); ++index)
                fixture.bytes[offset + index] = static_cast<std::byte>((ordinal >> (8 * index)) & 255U);
        }
    }  // namespace

    TEST_CASE("Physics wire rows preserve a dense semantic event permutation instead of replacing it by row order",
              "[physics][state][codec][events]") {
        Fixture fixture;
        PermuteEvents(fixture);
        auto decoded = Take(fixture.Decode(fixture.size));
        REQUIRE(decoded.world.publishedEvents.size() == 3);
        CHECK(decoded.world.publishedEvents[0].ordinal == 2);
        CHECK(decoded.world.publishedEvents[1].ordinal == 0);
        CHECK(decoded.world.publishedEvents[2].ordinal == 1);
    }

    TEST_CASE("Physics wire rejects duplicate with gap and out-of-range semantic ordinals", "[physics][state][malformed][events]") {
        Fixture fixture;
        PermuteEvents(fixture);
        const auto original = fixture.bytes;
        for (std::uint32_t bad : {0U, 3U, std::numeric_limits<std::uint32_t>::max()}) {
            fixture.bytes = original;
            WriteOrdinal(fixture, 2, bad);
            REQUIRE(fixture.Decode(fixture.size).HasError());
        }
    }

    TEST_CASE("Physics ordinal workspace admission preserves previous output and successful permutation decode allocates no heap",
              "[physics][state][allocation][events]") {
        Fixture fixture;
        PermuteEvents(fixture);
        const auto before = fixture.bytes;
        REQUIRE(EncodeSnapshotEnvelope(Compatibility(), std::span{fixture.solverBytes}.first(fixture.solverSize), fixture.world,
                                       fixture.bytes, std::span{fixture.eventOrdinals}.first(2),
                                       std::span{fixture.constructionBytes}.first(fixture.constructionSize))
                    .HasError());
        CHECK(fixture.bytes == before);
        bool succeeded{};
        Tests::AllocationProbe::Measurement measured;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            succeeded = fixture.Decode(fixture.size).HasValue();
            measured = measurement.Snapshot();
        }
        REQUIRE(succeeded);
        CHECK(measured.requests == 0);
    }

    TEST_CASE("Cold snapshot section inspection admits no authority and bounds every required span before receiving allocation",
              "[physics][state][codec][bounds]") {
        Fixture fixture;
        const auto input = std::span<const std::byte>{fixture.bytes}.first(fixture.size);
        const auto before = Tests::AllocationProbe::Count();
        auto sections = InspectSnapshotEnvelope(input);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(sections.HasValue());
        CHECK(before == after);
        CHECK(sections.Value().solver.size() == fixture.solverSize);
        CHECK(sections.Value().construction.size() == fixture.constructionSize);
        CHECK(sections.Value().world.empty());
        CHECK(std::ranges::equal(sections.Value().construction, std::span{fixture.constructionBytes}.first(fixture.constructionSize)));
        for (std::size_t n = 0; n < input.size(); ++n)
            REQUIRE(InspectSnapshotEnvelope(input.first(n)).HasError());
    }

    namespace {
        RestartInputLimits RestartLimits() noexcept {
            return {Fixture::Limits, {.shapes = {2, 1, 2, 2, 4096}, .maximumRetainedBytes = 65536, .maximumEncodedBytes = 4096}, 1, 4, 8192,
                    1024 * 1024};
        }
    }  // namespace

    TEST_CASE("Cold restart input owns the SAME complete canonical payload before native reconstruction",
              "[physics][state][restart][codec]") {
        Fixture fixture;
        const auto expected = fixture.bytes;
        auto input =
            Take(RestartInput::Prepare(std::span{fixture.bytes}.first(fixture.size), RestartLimits(), Compatibility().floatingPoint));
        fixture.bytes.fill(std::byte{0});
        CHECK(std::ranges::equal(input->Bytes(), std::span{expected}.first(fixture.size)));
        CHECK(input->Envelope().solver.bodies.front().id == 1);
        CHECK(input->Envelope().world.completedTick == 17);
        CHECK(input->Construction().shapes.roots.front().shape == 1);
        CHECK(input->RetainedPayloadBytes() <= RestartLimits().maximumRetainedBytes);
    }

    TEST_CASE("Cold restart input rejects profile storage and malformed input without changing old owned bytes",
              "[physics][state][restart][malformed]") {
        Fixture fixture;
        const auto bytes = std::span{fixture.bytes}.first(fixture.size);
        auto previous = Take(RestartInput::Prepare(bytes, RestartLimits(), Compatibility().floatingPoint));
        const auto expected = fixture.bytes;
        auto controls = Compatibility().floatingPoint;
        controls.value ^= 1;
        CHECK(RestartInput::Prepare(bytes, RestartLimits(), controls).HasError());
        auto limits = RestartLimits();
        limits.maximumRetainedBytes = 1;
        CHECK(RestartInput::Prepare(bytes, limits, Compatibility().floatingPoint).HasError());
        limits = RestartLimits();
        limits.maximumWireBytes = fixture.size - 1;
        CHECK(RestartInput::Prepare(bytes, limits, Compatibility().floatingPoint).HasError());
        CHECK(RestartInput::Prepare(bytes.first(bytes.size() - 1), RestartLimits(), Compatibility().floatingPoint).HasError());
        CHECK(std::ranges::equal(previous->Bytes(), std::span{expected}.first(fixture.size)));
    }

    TEST_CASE("Published cold restart values expose no growing frame storage", "[physics][state][restart][allocation]") {
        Fixture fixture;
        auto input =
            Take(RestartInput::Prepare(std::span{fixture.bytes}.first(fixture.size), RestartLimits(), Compatibility().floatingPoint));
        std::uint64_t body{};
        std::size_t bytes{};
        Tests::AllocationProbe::Measurement measured;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            body = input->Envelope().solver.bodies.front().id;
            bytes = input->RetainedPayloadBytes();
            measured = measurement.Snapshot();
        }
        CHECK(body == 1);
        CHECK(bytes != 0);
        CHECK(measured.requests == 0);
    }
}  // namespace Horo::Physics::StateDetail
