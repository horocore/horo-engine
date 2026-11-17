#include "Horo/PlatformServices/PlatformCloudObjects.h"
#include "PlatformServicesTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <utility>

namespace Horo::PlatformServices {
    using TestSupport::RequireError;

    TEST_CASE("Cloud staging admits bounded complete bytes and never publishes partial transfer", "[platform-services][cloud][transfer]") {
        const std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}};
        RequireError(CloudBlobStagingBuffer::Begin(1, 0), CloudObjectErrors::InvalidLimits);
        RequireError(CloudBlobStagingBuffer::Begin(1, MaximumCloudObjectPayloadBytes + 1), CloudObjectErrors::InvalidLimits);
        RequireError(CloudBlobStagingBuffer::Begin(std::numeric_limits<std::uint64_t>::max(), 3), CloudObjectErrors::PayloadTooLarge);
        auto admitted = CloudBlobStagingBuffer::Begin(3, 3, ComputeSha256(bytes));
        REQUIRE(admitted.HasValue());
        auto staging = std::move(admitted).Value();
        REQUIRE(staging.Append(std::span{bytes}.first(1)).HasValue());
        SECTION("complete chunks publish once") {
            REQUIRE(staging.Append(std::span{bytes}.subspan(1)).HasValue());
            const auto complete = staging.Finish();
            REQUIRE(complete.HasValue());
            CHECK(std::ranges::equal(complete.Value().Bytes(), bytes));
            RequireError(staging.Finish(), CloudObjectErrors::InvalidRequest);
            RequireError(staging.Append({}), CloudObjectErrors::InvalidRequest);
        }
        SECTION("short input cannot escape") {
            RequireError(staging.Finish(), CloudObjectErrors::PartialTransfer);
            RequireError(staging.Append(std::span{bytes}.subspan(1)), CloudObjectErrors::InvalidRequest);
        }
        SECTION("excess bytes permanently close staging") {
            RequireError(staging.Append(bytes), CloudObjectErrors::PayloadTooLarge);
            RequireError(staging.Finish(), CloudObjectErrors::InvalidRequest);
        }
        SECTION("digest failure does not publish bytes") {
            const std::array corrupt{std::byte{5}, std::byte{6}};
            REQUIRE(staging.Append(corrupt).HasValue());
            RequireError(staging.Finish(), CloudObjectErrors::IntegrityMismatch);
        }
        auto empty = CloudBlobStagingBuffer::Begin(0, 1);
        REQUIRE(empty.HasValue());
        auto moved = std::move(empty).Value();
        RequireError(std::move(empty).Value().Finish(), CloudObjectErrors::InvalidRequest);
        REQUIRE(moved.Finish().HasValue());
    }

}  // namespace Horo::PlatformServices
