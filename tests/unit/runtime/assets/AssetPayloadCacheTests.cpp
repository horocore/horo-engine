#include "Horo/Assets/AssetPayloadCache.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Assets {
    TEST_CASE("Immutable payload accounting follows the allocation across eviction shutdown and worker release",
              "[unit][assets][payload_cache]") {
        auto cache = std::move(AssetPayloadCache::Create(1, 4)).Value();
        constexpr std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        auto pin = std::move(cache->Admit(bytes)).Value();
        REQUIRE(pin.Bytes().size() == 4);
        REQUIRE(pin.Digest() == ComputeSha256(bytes));
        auto repeated = std::move(cache->Admit(bytes)).Value();
        REQUIRE(pin.SharesAllocationWith(repeated));
        REQUIRE(cache->Snapshot().retainedPayloadBytes == 4);
        cache->Evict(pin.Digest());
        REQUIRE(cache->Snapshot().residentPayloadBytes == 0);
        REQUIRE(cache->Snapshot().retainedPayloadBytes == 4);
        auto revived = std::move(cache->Admit(bytes)).Value();
        REQUIRE(revived.SharesAllocationWith(pin));
        const std::array other{std::byte{9}};
        REQUIRE(cache->Admit(other).HasError());
        cache->Shutdown();
        REQUIRE(cache->Admit(bytes).HasError());
        REQUIRE(cache->Snapshot().retainedPayloadBytes == 4);
        repeated = {};
        revived = {};
        bool workerVerified = false;
        std::thread worker{[lease = std::move(pin), &workerVerified] {
            workerVerified = lease.Bytes().front() == std::byte{1};
        }};
        worker.join();
        REQUIRE(workerVerified);
        REQUIRE(cache->Snapshot().retainedPayloadBytes == 0);
    }

    TEST_CASE("Immutable payload pins survive cache destruction and expired entries free bounded capacity",
              "[unit][assets][payload_cache]") {
        REQUIRE(AssetPayloadCache::Create(0, 1).HasError());
        REQUIRE(AssetPayloadCache::Create(1, 0).HasError());
        REQUIRE(AssetPayloadCache::Create(65'537, 4).HasError());
        auto cache = std::move(AssetPayloadCache::Create(1, 4)).Value();
        REQUIRE(cache->Admit({}).HasError());
        constexpr std::array first{std::byte{7}};
        constexpr std::array second{std::byte{8}, std::byte{9}};
        auto pin = std::move(cache->Admit(first)).Value();
        REQUIRE(!pin.SharesAllocationWith({}));
        const auto digest = pin.Digest();
        pin = {};
        REQUIRE(cache->Admit(second).HasError());
        cache->Evict(digest);
        pin = std::move(cache->Admit(second)).Value();
        REQUIRE(cache->Snapshot().residentEntries == 1);
        REQUIRE(cache->Snapshot().bookkeepingBytes > 0);
        cache.reset();
        REQUIRE(std::ranges::equal(pin.Bytes(), second));
        REQUIRE(AssetPayloadLease{}.Bytes().empty());
        REQUIRE(AssetPayloadLease{}.Digest() == Sha256Digest{});
    }
}  // namespace Horo::Assets
