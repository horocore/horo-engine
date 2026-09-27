#include "Horo/Release/ReleaseBuildProvenance.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <ranges>
#include <vector>

using namespace Horo;
using namespace Horo::Release;

namespace {
    ReleaseBuildProvenanceData Fixture() {
        ReleaseBuildProvenanceData data;
        data.sourceEpochSeconds = 1'700'000'000U;
        data.features = {"renderer", "audio"};
        data.environment = {{"SDK_VERSION", {}}};
        data.files = {{"bin/game", 4U, {}}, {"assets/archive.bin", 6U, {}}};
        return data;
    }
}  // namespace

TEST_CASE("Unsigned provenance canonicalizes independent of inventory order", "[release][provenance]") {
    auto first = ReleaseBuildProvenance::Create(Fixture());
    REQUIRE(first.HasValue());
    auto secondData = Fixture();
    std::ranges::reverse(secondData.files);
    std::ranges::reverse(secondData.features);
    auto second = ReleaseBuildProvenance::Create(std::move(secondData));
    REQUIRE(second.HasValue());
    CHECK(first.Value().Digest() == second.Value().Digest());
    auto parsed = ReleaseBuildProvenance::ParseCanonical(first.Value().CanonicalJson());
    REQUIRE(parsed.HasValue());
    CHECK(parsed.Value().Compare(first.Value()).empty());
}

TEST_CASE("Unsigned provenance reports precise input and file variance", "[release][provenance]") {
    auto first = ReleaseBuildProvenance::Create(Fixture());
    REQUIRE(first.HasValue());
    auto changed = Fixture();
    changed.sourceEpochSeconds += 1U;
    changed.environment.front().digest.bytes[0] = 1U;
    changed.files.front().size += 1U;
    changed.files.push_back({"new.txt", 0U, {}});
    auto second = ReleaseBuildProvenance::Create(std::move(changed));
    REQUIRE(second.HasValue());
    const auto variance = first.Value().Compare(second.Value());
    CHECK(variance == std::vector<ReleaseBuildVariance>{{"normalization.sourceEpochSeconds"},
                                                        {"environment.SDK_VERSION"},
                                                        {"files.bin/game"},
                                                        {"files.new.txt"}});
}

TEST_CASE("Unsigned provenance rejects unsafe paths and noncanonical bytes", "[release][provenance]") {
    auto data = Fixture();
    data.files.front().path = "../secret";
    CHECK(ReleaseBuildProvenance::Create(std::move(data)).HasError());
    data = Fixture();
    data.files.push_back({"BIN/GAME", 4U, {}});
    CHECK(ReleaseBuildProvenance::Create(std::move(data)).HasError());
    auto valid = ReleaseBuildProvenance::Create(Fixture());
    REQUIRE(valid.HasValue());
    auto noncanonical = valid.Value().CanonicalJson();
    noncanonical.insert(1U, " ");
    CHECK(ReleaseBuildProvenance::ParseCanonical(noncanonical).HasError());
    auto secret = Fixture();
    secret.environment.front().name = "PATH=/home/user";
    CHECK(ReleaseBuildProvenance::Create(std::move(secret)).HasError());
}
