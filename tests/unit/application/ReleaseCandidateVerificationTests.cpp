#include "Horo/Release/ReleaseCandidateVerification.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>

using namespace Horo;
using namespace Horo::Release;

namespace {
    class TemporaryCandidate final {
    public:
        TemporaryCandidate()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-candidate-verification-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(root / "bin");
        }

        ~TemporaryCandidate() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path root;
    };

    void WriteFile(const std::filesystem::path &path, const std::string_view bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    ReleaseArtifactManifest Manifest(const bool signedCandidate) {
        ReleaseArtifactManifestData data;
        data.candidate = ReleaseCandidateId{42U};
        data.product = {DistributionProductKind::GameRuntime, {}};
        data.version = GameProductVersion{{1U, 2U, 3U, {}, {}}};
        data.sourceRevision = {"abc123"};
        data.platform = DistributionPlatform::Linux;
        data.architecture = DistributionArchitecture::X64;
        data.configuration = ReleaseBuildConfiguration::Shipping;
        data.build = {"build_42"};
        data.toolchainId = "clang_20";
        data.artifacts = {{"bin/game", ReleaseArtifactRole::Binary, 4U, ComputeSha256(std::as_bytes(std::span{"game", 4U}))}};
        if (signedCandidate)
            data.signing = {"test-signature", "test-publisher", "key-1", data.artifacts.front().digest};
        auto manifest = ReleaseArtifactManifest::Create(std::move(data));
        REQUIRE(manifest.HasValue());
        return std::move(manifest).Value();
    }

    class FakeSignatureVerifier final : public IReleaseCandidateSignatureVerifier {
    public:
        [[nodiscard]] Result<void> Verify(const std::filesystem::path &, const ReleaseArtifactManifest &) override {
            ++calls;
            return Result<void>::Success();
        }

        int calls{};
    };

    class FakeSmokeProbe final : public IReleaseCandidateSmokeProbe {
    public:
        explicit FakeSmokeProbe(const ReleaseCandidateSmokeKind kind) : kind_(kind) {}

        [[nodiscard]] ReleaseCandidateSmokeKind Kind() const noexcept override {
            return kind_;
        }

        [[nodiscard]] Result<void> Check(const std::filesystem::path &root, const ReleaseArtifactManifest &) override {
            ++calls;
            if (mutate)
                WriteFile(root / "bin/game", "evil");
            return Result<void>::Success();
        }

        int calls{};
        bool mutate{};

    private:
        ReleaseCandidateSmokeKind kind_;
    };
}  // namespace

TEST_CASE("Final candidate verification gates smoke probes on exact bytes and platform signature", "[release][verification]") {
    TemporaryCandidate candidate;
    auto manifest = Manifest(true);
    WriteFile(candidate.root / "bin/game", "game");
    WriteFile(candidate.root / "manifest.json", manifest.CanonicalJson());
    FakeSignatureVerifier signature;
    FakeSmokeProbe archive{ReleaseCandidateSmokeKind::ArchiveReadable};
    FakeSmokeProbe launch{ReleaseCandidateSmokeKind::InstallAndLaunch};
    IReleaseCandidateSmokeProbe *probes[]{&archive, &launch};
    const ReleaseCandidateSmokeKind required[]{ReleaseCandidateSmokeKind::ArchiveReadable, ReleaseCandidateSmokeKind::InstallAndLaunch};

    CHECK(VerifyReleaseCandidate(candidate.root, manifest, required, nullptr, probes).HasError());
    CHECK(archive.calls == 0);
    auto verified = VerifyReleaseCandidate(candidate.root, manifest, required, &signature, probes);
    REQUIRE(verified.HasValue());
    CHECK(verified.Value().Candidate() == manifest.Data().candidate);
    CHECK(verified.Value().ManifestDigest() == manifest.Digest());
    CHECK(verified.Value().Root() == candidate.root);
    CHECK(signature.calls == 1);
    CHECK(archive.calls == 1);
    CHECK(launch.calls == 1);

    WriteFile(candidate.root / "bin/game", "evil");
    CHECK(VerifyReleaseCandidate(candidate.root, manifest, required, &signature, probes).HasError());
    CHECK(signature.calls == 1);
    CHECK(archive.calls == 1);
}

TEST_CASE("Final candidate verification rejects missing or duplicate required checks and smoke mutations", "[release][verification]") {
    TemporaryCandidate candidate;
    auto manifest = Manifest(false);
    WriteFile(candidate.root / "bin/game", "game");
    WriteFile(candidate.root / "manifest.json", manifest.CanonicalJson());
    FakeSmokeProbe archive{ReleaseCandidateSmokeKind::ArchiveReadable};
    IReleaseCandidateSmokeProbe *probes[]{&archive};
    const ReleaseCandidateSmokeKind duplicate[]{ReleaseCandidateSmokeKind::ArchiveReadable, ReleaseCandidateSmokeKind::ArchiveReadable};
    const ReleaseCandidateSmokeKind missing[]{ReleaseCandidateSmokeKind::InstallAndLaunch};
    const ReleaseCandidateSmokeKind required[]{ReleaseCandidateSmokeKind::ArchiveReadable};

    CHECK(VerifyReleaseCandidate(candidate.root, manifest, duplicate, nullptr, probes).HasError());
    CHECK(VerifyReleaseCandidate(candidate.root, manifest, missing, nullptr, probes).HasError());
    CHECK(archive.calls == 0);

    archive.mutate = true;
    CHECK(VerifyReleaseCandidate(candidate.root, manifest, required, nullptr, probes).HasError());
    CHECK(archive.calls == 1);
}
