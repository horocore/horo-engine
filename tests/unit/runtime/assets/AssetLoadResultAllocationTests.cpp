#include "AllocationProbe.h"
#include "Horo/Assets/AssetProvider.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace {
    using Horo::Assets::AssetLoadResult;
    using Horo::Tests::AllocationProbe::ScopedFailure;
    using Horo::Tests::AllocationProbe::ScopedMeasurement;

    /** @brief Creates the deterministic nonzero asset identity used by envelope transfer regressions. */
    Horo::Assets::AssetId Identity() {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = 7;
        return Horo::Assets::AssetId::FromBytes(bytes);
    }

    /** @brief Ensures checked MSVC actually exercises the destination allocation failure sweep. */
    void RequireCheckedDestinationAllocation(std::size_t requests) {
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
        REQUIRE(requests > 0);
#else
        static_cast<void>(requests);
#endif
    }
}  // namespace

TEST_CASE("Asset load envelope default storage propagates every allocation failure", "[assets][allocation][load]") {
    static_assert(!std::is_nothrow_default_constructible_v<AssetLoadResult>);
    std::optional<AssetLoadResult> measured;
    std::size_t requests{};
    {
        ScopedMeasurement measurement;
        measured.emplace();
        requests = measurement.Snapshot().requests;
    }
    REQUIRE(measured->bytes.empty());
    RequireCheckedDestinationAllocation(requests);
    for (std::size_t index = 0; index < requests; ++index) {
        INFO(index);
        std::optional<AssetLoadResult> destination;
        bool failed{};
        {
            ScopedFailure failure(index);
            try {
                destination.emplace();
            } catch (const std::bad_alloc &) {
                failed = true;
            }
        }
        REQUIRE(failed);
        CHECK_FALSE(destination.has_value());
    }
}

TEST_CASE("Asset load envelope move preserves its source until the destination exists", "[assets][allocation][load]") {
    static_assert(!std::is_nothrow_move_constructible_v<AssetLoadResult>);
    const std::vector<std::uint8_t> expected(128, 0xa5);
    AssetLoadResult source{Identity(), {17}, expected};
    const auto *originalStorage = source.bytes.data();
    std::optional<AssetLoadResult> measured;
    std::size_t requests{};
    {
        ScopedMeasurement measurement;
        measured.emplace(std::move(source));
        requests = measurement.Snapshot().requests;
    }
    REQUIRE(measured->id == Identity());
    CHECK(measured->sourceRegistryRevision.value == 17);
    CHECK(measured->bytes == expected);
    CHECK(measured->bytes.data() == originalStorage);
    CHECK(source.bytes.empty());
    RequireCheckedDestinationAllocation(requests);
    for (std::size_t index = 0; index < requests; ++index) {
        INFO(index);
        AssetLoadResult original{Identity(), {17}, expected};
        const auto *storage = original.bytes.data();
        std::optional<AssetLoadResult> destination;
        bool failed{};
        {
            ScopedFailure failure(index);
            try {
                destination.emplace(std::move(original));
            } catch (const std::bad_alloc &) {
                failed = true;
            }
        }
        REQUIRE(failed);
        CHECK_FALSE(destination.has_value());
        CHECK(original.id == Identity());
        CHECK(original.sourceRegistryRevision.value == 17);
        CHECK(original.bytes == expected);
        CHECK(original.bytes.data() == storage);
    }
}

TEST_CASE("Result transport preserves an asset envelope through destination allocation failure", "[assets][allocation][load]") {
    const std::vector<std::uint8_t> expected(128, 0x7b);
    auto source = Horo::Result<AssetLoadResult>::Success(AssetLoadResult{Identity(), {19}, expected});
    const auto *storage = source.Value().bytes.data();
    std::optional<Horo::Result<AssetLoadResult>> measured;
    std::size_t requests{};
    {
        ScopedMeasurement measurement;
        measured.emplace(std::move(source));
        requests = measurement.Snapshot().requests;
    }
    REQUIRE(measured->HasValue());
    CHECK(measured->Value().bytes == expected);
    CHECK(measured->Value().bytes.data() == storage);
    RequireCheckedDestinationAllocation(requests);
    for (std::size_t index = 0; index < requests; ++index) {
        INFO(index);
        auto original = Horo::Result<AssetLoadResult>::Success(AssetLoadResult{Identity(), {19}, expected});
        const auto *originalStorage = original.Value().bytes.data();
        std::optional<Horo::Result<AssetLoadResult>> destination;
        bool failed{};
        {
            ScopedFailure failure(index);
            try {
                destination.emplace(std::move(original));
            } catch (const std::bad_alloc &) {
                failed = true;
            }
        }
        REQUIRE(failed);
        CHECK_FALSE(destination.has_value());
        REQUIRE(original.HasValue());
        CHECK(original.Value().id == Identity());
        CHECK(original.Value().sourceRegistryRevision.value == 19);
        CHECK(original.Value().bytes == expected);
        CHECK(original.Value().bytes.data() == originalStorage);
    }
}

TEST_CASE("Asset load move assignment transfers existing storage with allocations denied", "[assets][allocation][load]") {
    static_assert(std::is_nothrow_move_assignable_v<AssetLoadResult>);
    const std::vector<std::uint8_t> expected(128, 0xc3);
    AssetLoadResult source{Identity(), {23}, expected};
    AssetLoadResult destination{Identity(), {2}, std::vector<std::uint8_t>(8, 1)};
    const auto *storage = source.bytes.data();
    {
        ScopedFailure failure(0);
        destination = std::move(source);
    }
    CHECK(destination.sourceRegistryRevision.value == 23);
    CHECK(destination.bytes == expected);
    CHECK(destination.bytes.data() == storage);
    CHECK(source.bytes.empty());
}
