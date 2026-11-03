#include "Horo/Runtime/Save/SaveCanonicalCodec.h"
#include "PhysicsStateWorldTestSupport.h"

#include <algorithm>
#include <optional>
#include <thread>

namespace Horo::Physics::StateTest {
    namespace {
        Runtime::CanonicalEncodedValue SaveValue(std::span<const std::byte> state) {
            Runtime::CanonicalValueWriter writer;
            Check(writer.WriteBytes(state));
            return Take(std::move(writer).Finalize());
        }

        std::vector<std::byte> LoadValue(const Runtime::CanonicalEncodedValue &value) {
            auto reader = Take(Runtime::CanonicalValueReader::Create(value.Bytes()));
            auto bytes = Take(reader.ReadBytes(64U * 1024U * 1024U));
            Check(reader.RequireFinished());
            return bytes;
        }
    }  // namespace

    TEST_CASE("Real Save canonical byte value carries the exact Physics contract back through native atomic restore",
              "[physics][native][state][save]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto snapshot = fixture.Capture();
        const auto saved = SaveValue(snapshot->Bytes());
        auto bytes = LoadValue(saved);
        REQUIRE(std::ranges::equal(bytes, snapshot->Bytes()));
        fixture.Advance(2);
        auto decoded = Take(fixture.world->DecodeStateSnapshot(bytes));
        auto candidate = Take(fixture.world->PrepareStateRestore(decoded));
        Check(candidate.ValidatePublication());
        candidate.Publish();
        CHECK(fixture.world->PublishedTick().completedTick == snapshot->World().completedTick);
        const auto recaptured = fixture.Capture();
        CHECK(std::ranges::equal(recaptured->Bytes(), snapshot->Bytes()));
    }

    TEST_CASE("Save byte framing cannot authorize malformed Physics solver state or replace the last-good world",
              "[physics][native][state][save][malformed]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto original = fixture.Capture();
        std::vector<std::byte> malformed(original->Bytes().begin(), original->Bytes().end());
        malformed[4] ^= std::byte{1};
        auto genericSave = SaveValue(malformed);
        auto recovered = LoadValue(genericSave);
        const auto before = fixture.world->PublishedTick();
        Test::RequireError(fixture.world->DecodeStateSnapshot(recovered), PhysicsErrors::DescriptorInvalid);
        CHECK(SamePublication(fixture.world->PublishedTick(), before));
        const auto stillGood = fixture.Capture();
        CHECK(std::ranges::equal(stillGood->Bytes(), original->Bytes()));
    }

    TEST_CASE("Save worker reads an immutable Physics lease without recapturing or relabeling a newer owner tick",
              "[physics][native][state][save][worker]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto original = fixture.Capture();
        std::optional<Runtime::CanonicalEncodedValue> saved;
        bool success{};
        std::jthread worker([lease = original, &saved, &success] {
            Runtime::CanonicalValueWriter writer;
            auto written = writer.WriteBytes(lease->Bytes());
            if (written.HasError())
                return;
            auto sealed = std::move(writer).Finalize();
            if (sealed.HasError())
                return;
            saved.emplace(std::move(sealed).Value());
            success = true;
        });
        fixture.Advance();
        worker.join();
        REQUIRE(success);
        auto restored = Take(fixture.world->DecodeStateSnapshot(LoadValue(*saved)));
        CHECK(restored->World().completedTick == original->World().completedTick);
        CHECK(restored->World().completedTick != fixture.world->PublishedTick().completedTick);
        CHECK(std::ranges::equal(restored->Bytes(), original->Bytes()));
    }
}  // namespace Horo::Physics::StateTest
