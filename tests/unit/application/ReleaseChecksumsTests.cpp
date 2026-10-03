#include "Horo/Release/ReleaseChecksums.h"

#include <catch2/catch_test_macros.hpp>
#include <span>
#include <string>
#include <vector>

using namespace Horo;
using namespace Horo::Release;

namespace {
    Sha256Digest Digest(const std::string_view bytes) {
        return ComputeSha256(std::as_bytes(std::span{bytes.data(), bytes.size()}));
    }
}  // namespace

TEST_CASE("Release checksums sort final artifact hashes without metadata self-reference", "[release][checksums]") {
    const std::vector<ReleaseArtifactRecord> artifacts{{"notices/license.txt", ReleaseArtifactRole::Notice, 7U, Digest("license")},
                                                       {"bin/game", ReleaseArtifactRole::Binary, 4U, Digest("game")}};
    const auto result = BuildReleaseChecksums(artifacts);
    REQUIRE(result.HasValue());
    const auto expected = FormatSha256(artifacts[1].digest).substr(7U) + "  bin/game\n" + FormatSha256(artifacts[0].digest).substr(7U) +
                          "  notices/license.txt\n";
    CHECK(result.Value() == expected);
    CHECK(result.Value().find("manifest.json") == std::string::npos);
    CHECK(result.Value().find("checksums.txt") == std::string::npos);
}

TEST_CASE("Release checksums reject duplicate, unsafe, and self-referential paths", "[release][checksums]") {
    const auto digest = Digest("game");
    const ReleaseArtifactRecord good{"bin/game", ReleaseArtifactRole::Binary, 4U, digest};
    CHECK(BuildReleaseChecksums(std::span<const ReleaseArtifactRecord>{}).HasError());
    CHECK(
        BuildReleaseChecksums(std::vector<ReleaseArtifactRecord>{good, {"BIN/GAME", ReleaseArtifactRole::Binary, 4U, digest}}).HasError());
    CHECK(BuildReleaseChecksums(std::vector<ReleaseArtifactRecord>{{"../secret", ReleaseArtifactRole::Binary, 4U, digest}}).HasError());
    CHECK(BuildReleaseChecksums(std::vector<ReleaseArtifactRecord>{{"checksums.txt", ReleaseArtifactRole::Notice, 4U, digest}}).HasError());
    CHECK(BuildReleaseChecksums(std::vector<ReleaseArtifactRecord>{{"manifest.json", ReleaseArtifactRole::Notice, 4U, digest}}).HasError());
}
