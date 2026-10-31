#include "OpenXRComponentManifest.h"
#include "OpenXRLoaderLease.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Application::Internal;

    static_assert(!std::is_default_constructible_v<OpenXRLoaderLease::ConstructionKey>);
    static_assert(!std::is_aggregate_v<OpenXRLoaderLease::ConstructionKey>);

    nlohmann::json ComponentDocument() {
        const auto digest = FormatSha256(ComputeSha256({}));
        return {{"schemaVersion", 1},
                {"backend", 7},
                {"contractVersion", {1, 1, 0}},
                {"build", "test-build"},
                {"releaseChannel", "development"},
                {"platform", {{"os", "linux"}, {"architecture", "x64"}, {"sdkAbi", "horo-sdk-2"}}},
                {"linkage", "dynamic"},
                {"loaderSource", "bundled_verified"},
                {"loader", {{"path", "runtime/xr/libopenxr_loader.so"}, {"sha256", digest}}},
                {"loaderVersions", {{"minimum", {1, 0, 0}}, {"maximum", {1, 1, 0}}}},
                {"bridges", {{"platform", 1}, {"renderer", 1}}},
                {"dependencies", nlohmann::json::array()},
                {"licenses", digest},
                {"provenance", digest}};
    }

    Result<OpenXRComponentManifest> Parse(const std::string &text) {
        return ParseOpenXRComponentManifest(std::as_bytes(std::span{text}));
    }

    TEST_CASE("OpenXR component metadata retains canonical exact identity without issuing install evidence", "[xr][install]") {
        const auto source = ComponentDocument().dump();
        auto parsed = Parse(source);
        REQUIRE(parsed.HasValue());
        CHECK(parsed.Value().identity.backend.Value() == 7);
        CHECK(parsed.Value().identity.contract == XR::CurrentXRContractVersion);
        CHECK(parsed.Value().platform.operatingSystem == "linux");
        CHECK(parsed.Value().loader.path.Value() == "runtime/xr/libopenxr_loader.so");
        CHECK(parsed.Value().dependencies.empty());
        CHECK(parsed.Value().descriptorDigest == ComputeSha256(std::as_bytes(std::span{source})));
        STATIC_REQUIRE_FALSE(std::is_default_constructible_v<VerifiedOpenXRComponent>);
        STATIC_REQUIRE_FALSE(std::is_aggregate_v<VerifiedOpenXRComponent>);
    }

    TEST_CASE("OpenXR component metadata rejects duplicate unknown oversized and malformed documents", "[xr][install]") {
        auto source = ComponentDocument();
        SECTION("unknown execution policy") {
            source["allowUnsigned"] = true;
            CHECK(Parse(source.dump()).HasError());
        }
        SECTION("duplicate backend") {
            auto text = source.dump();
            text.insert(1, "\"backend\":9,");
            CHECK(Parse(text).HasError());
        }
        SECTION("bounded input") {
            CHECK(Parse(std::string(65537, ' ')).HasError());
            CHECK(Parse("").HasError());
        }
        SECTION("wrong numeric type") {
            source["backend"] = "7";
            CHECK(Parse(source.dump()).HasError());
        }
        SECTION("unapproved loader source") {
            source["loaderSource"] = "platform_provided";
            CHECK(Parse(source.dump()).HasError());
        }
    }

    TEST_CASE("OpenXR component metadata rejects path escape dependency aliasing and invalid native ABI versions", "[xr][install]") {
        auto source = ComponentDocument();
        SECTION("path escape") {
            source["loader"]["path"] = "runtime/xr/../foreign.so";
            CHECK(Parse(source.dump()).HasError());
        }
        SECTION("loader dependency collision") {
            source["dependencies"].push_back(source["loader"]);
            CHECK(Parse(source.dump()).HasError());
        }
        SECTION("dependency capacity") {
            source["dependencies"] = nlohmann::json::array();
            for (unsigned index = 0; index < 17; ++index)
                source["dependencies"].push_back(
                    {{"path", "runtime/xr/lib" + std::to_string(index) + ".so"}, {"sha256", source["loader"]["sha256"]}});
            CHECK(Parse(source.dump()).HasError());
        }
        SECTION("version overflow") {
            source["contractVersion"] = {65536, 1, 0};
            CHECK(Parse(source.dump()).HasError());
        }
        SECTION("reversed loader interval") {
            source["loaderVersions"]["minimum"] = {1, 2, 0};
            CHECK(Parse(source.dump()).HasError());
        }
        SECTION("zero bridge") {
            source["bridges"]["renderer"] = 0;
            CHECK(Parse(source.dump()).HasError());
        }
    }
}  // namespace
