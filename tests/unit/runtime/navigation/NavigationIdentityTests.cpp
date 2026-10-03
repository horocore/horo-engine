#include "Horo/Navigation/NavigationIdentity.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <set>
#include <string_view>
#include <type_traits>

namespace Horo::Navigation {
    namespace {
        template <typename Identity> Identity MakeIdentity(const std::uint64_t value) {
            const auto result = Identity::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        void ExpectInvalidHandle(const Result<void> &result) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().domain.Value() == NavigationErrors::InvalidHandle.domain.Value());
            REQUIRE(result.ErrorValue().code.Value() == NavigationErrors::InvalidHandle.code.Value());
        }

        template <typename Handle> void CheckWorldOwnedHandle() {
            const auto world = MakeIdentity<NavigationWorldId>(7);
            const Handle handle{world, {3, 2}};
            REQUIRE(handle.IsValid());
            REQUIRE(ValidateNavigationHandleOwner(handle, world).HasValue());
            ExpectInvalidHandle(ValidateNavigationHandleOwner(Handle{}, world));
            ExpectInvalidHandle(ValidateNavigationHandleOwner(handle, {}));
            ExpectInvalidHandle(ValidateNavigationHandleOwner(handle, MakeIdentity<NavigationWorldId>(8)));
        }

        template <typename Handle> void CheckTopologyOwnedHandle() {
            const auto world = MakeIdentity<NavigationWorldId>(7);
            const auto topology = MakeIdentity<NavigationGeneration>(11);
            const Handle handle{world, topology, {3, 2}};
            REQUIRE(handle.IsValid());
            REQUIRE(ValidateNavigationTopologyHandle(handle, world, topology).HasValue());
            ExpectInvalidHandle(ValidateNavigationTopologyHandle(Handle{}, world, topology));
            ExpectInvalidHandle(ValidateNavigationTopologyHandle(handle, {}, topology));
            ExpectInvalidHandle(ValidateNavigationTopologyHandle(handle, world, {}));
            ExpectInvalidHandle(ValidateNavigationTopologyHandle(handle, MakeIdentity<NavigationWorldId>(8), topology));
            ExpectInvalidHandle(ValidateNavigationTopologyHandle(handle, world, MakeIdentity<NavigationGeneration>(12)));
        }

        TEST_CASE("Navigation identities reserve zero and retain exact values", "[unit][navigation][identity]") {
            REQUIRE(NavigationWorldId::Create(0).HasError());
            REQUIRE(NavigationGeneration::Create(0).HasError());
            REQUIRE(NavigationSnapshotToken::Create(0).HasError());
            REQUIRE(SurfaceId::Create(0).HasError());

            const auto world = MakeIdentity<NavigationWorldId>(1);
            const auto generation = MakeIdentity<NavigationGeneration>(2);
            const auto snapshot = MakeIdentity<NavigationSnapshotToken>(3);
            const auto surface = MakeIdentity<SurfaceId>(4);
            REQUIRE(world.Value() == 1);
            REQUIRE(generation.Value() == 2);
            REQUIRE(snapshot.Value() == 3);
            REQUIRE(surface.Value() == 4);
            REQUIRE(MakeIdentity<NavigationWorldId>(std::numeric_limits<std::uint64_t>::max()).IsValid());
            static_assert(!std::is_convertible_v<std::uint64_t, NavigationWorldId>);
            static_assert(!std::is_convertible_v<std::uint64_t, NavigationGeneration>);
            static_assert(!std::is_convertible_v<std::uint64_t, NavigationSnapshotToken>);
            static_assert(!std::is_convertible_v<std::uint64_t, SurfaceId>);
            static_assert(!std::is_same_v<NavigationSnapshotToken, NavigationGeneration>);
        }

        TEST_CASE("Stable surface identity serializes with canonical byte order", "[unit][navigation][identity]") {
            const auto surface = MakeIdentity<SurfaceId>(0x0102030405060708ULL);
            const SerializedSurfaceId expected{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
            REQUIRE(SerializeSurfaceId(surface) == expected);
            const auto decoded = DeserializeSurfaceId(expected);
            REQUIRE(decoded.HasValue());
            REQUIRE(decoded.Value() == surface);
            REQUIRE(DeserializeSurfaceId({}).HasError());
        }

        TEST_CASE("World-owned navigation handles reject replacement worlds", "[unit][navigation][identity]") {
            CheckWorldOwnedHandle<NavRequestHandle>();
            CheckWorldOwnedHandle<NavigationObstacleHandle>();
            CheckWorldOwnedHandle<CrowdAgentHandle>();
            CheckWorldOwnedHandle<PathId>();
            static_assert(!std::is_same_v<NavRequestHandle, NavigationObstacleHandle>);
            static_assert(!std::is_same_v<NavigationObstacleHandle, CrowdAgentHandle>);
            static_assert(!std::is_same_v<PathId, NavRequestHandle>);
        }

        TEST_CASE("Provider handles reject replacement worlds and topology generations", "[unit][navigation][identity]") {
            CheckTopologyOwnedHandle<NavigationSurfaceHandle>();
            CheckTopologyOwnedHandle<NavigationTileHandle>();
            CheckTopologyOwnedHandle<NavigationPolygonHandle>();
            static_assert(!std::is_same_v<NavigationSurfaceHandle, NavigationTileHandle>);
            static_assert(!std::is_same_v<NavigationTileHandle, NavigationPolygonHandle>);
        }

        TEST_CASE("Navigation handle identity includes every owner and slot generation", "[unit][navigation][identity]") {
            const auto world = MakeIdentity<NavigationWorldId>(20);
            const auto topology = MakeIdentity<NavigationGeneration>(30);
            const NavigationTileHandle original{world, topology, {4, 5}};
            REQUIRE(original != NavigationTileHandle{MakeIdentity<NavigationWorldId>(21), topology, {4, 5}});
            REQUIRE(original != NavigationTileHandle{world, MakeIdentity<NavigationGeneration>(31), {4, 5}});
            REQUIRE(original != NavigationTileHandle{world, topology, {5, 5}});
            REQUIRE(original != NavigationTileHandle{world, topology, {4, 6}});
            REQUIRE_FALSE(NavigationTileHandle{world, topology, {decltype(original.slot)::InvalidIndex, 5}}.IsValid());
            REQUIRE_FALSE(NavigationTileHandle{world, topology, {4, 0}}.IsValid());
            static_assert(std::is_trivially_copyable_v<NavigationTileHandle>);
        }

        TEST_CASE("Navigation errors expose unique definitive descriptors", "[unit][navigation][errors]") {
            const std::array<const ErrorCodeDescriptor *, 3> descriptors{{
                &NavigationErrors::IdentityInvalid,
                &NavigationErrors::InvalidHandle,
                &NavigationErrors::GenerationExhausted,
            }};
            std::set<std::string_view> unique;
            for (const ErrorCodeDescriptor *descriptor : descriptors) {
                REQUIRE(descriptor->domain.Value() == "horo.navigation");
                REQUIRE(unique.insert(descriptor->code.Value()).second);
                REQUIRE_FALSE(descriptor->summary.empty());
                REQUIRE_FALSE(descriptor->remediationHint.empty());
            }
            REQUIRE(NavigationErrors::IdentityInvalid.code.Value() == "navigation.identity.invalid");
            REQUIRE(NavigationErrors::InvalidHandle.code.Value() == "navigation.handle.invalid");
            REQUIRE(NavigationErrors::GenerationExhausted.code.Value() == "navigation.generation.exhausted");
            REQUIRE(NavigationErrors::GenerationExhausted.defaultSeverity == ErrorSeverity::Critical);
        }
    }  // namespace
}  // namespace Horo::Navigation
