#include "AllocationProbe.h"
#include "Horo/Assets/AssetId.h"

#include <catch2/catch_test_macros.hpp>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace {
    using Horo::Assets::AssetTypeId;
    using Horo::Tests::AllocationProbe::ScopedFailure;
    using Horo::Tests::AllocationProbe::ScopedMeasurement;
}  // namespace

TEST_CASE("Asset type parsing propagates every allocation failure without changing input", "[assets][allocation]") {
    const std::string text = "core." + std::string(80, 'a');
    std::size_t requests{};
    {
        std::optional<Horo::Result<AssetTypeId>> parsed;
        {
            ScopedMeasurement measurement;
            parsed.emplace(AssetTypeId::Parse(text));
            requests = measurement.Snapshot().requests;
        }
        REQUIRE(parsed->HasValue());
        REQUIRE(parsed->Value().Value() == text);
    }
    REQUIRE(requests > 0);
    for (std::size_t index = 0; index < requests; ++index) {
        INFO("allocation index=" << index);
        std::optional<Horo::Result<AssetTypeId>> parsed;
        bool failed{};
        {
            ScopedFailure failure(index);
            try {
                parsed.emplace(AssetTypeId::Parse(text));
            } catch (const std::bad_alloc &) {
                failed = true;
            }
        }
        REQUIRE(failed);
        CHECK_FALSE(parsed.has_value());
        CHECK(text == "core." + std::string(80, 'a'));
    }
}

TEST_CASE("Asset type move preserves its source through every destination allocation failure", "[assets][allocation]") {
    const std::string text = "core." + std::string(80, 'a');
    auto original = AssetTypeId::Parse(text);
    REQUIRE(original.HasValue());
    std::optional<AssetTypeId> moved;
    std::size_t requests{};
    {
        ScopedMeasurement measurement;
        moved.emplace(std::move(original).Value());
        requests = measurement.Snapshot().requests;
    }
    REQUIRE(moved->Value() == text);
    REQUIRE(original.Value().Value().empty());
    for (std::size_t index = 0; index < requests; ++index) {
        INFO("allocation index=" << index);
        auto source = AssetTypeId::Parse(text);
        REQUIRE(source.HasValue());
        std::optional<AssetTypeId> destination;
        bool failed{};
        {
            ScopedFailure failure(index);
            try {
                destination.emplace(std::move(source).Value());
            } catch (const std::bad_alloc &) {
                failed = true;
            }
        }
        REQUIRE(failed);
        CHECK_FALSE(destination.has_value());
        CHECK(source.Value().Value() == text);
    }
}
