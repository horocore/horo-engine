#include "Horo/Release/GitHubReleasePublication.h"
#include "ReleaseTestFixtures.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>

using namespace Horo;
using namespace Horo::Release;
using namespace ReleaseTestFixtures;

namespace {
    class TemporaryCandidate final {
    public:
        TemporaryCandidate()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-github-release-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
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

    ReleaseExecutionPlan PublicationPlan() {
        auto request = Request();
        request.profile = Profile(ReleaseSigningPolicy::Disabled, true);
        request.publicationDestination = ReleaseDestinationId{"github-releases"};
        auto outcome = PreflightRelease(request, Facts(request));
        REQUIRE(outcome.plan.has_value());
        return std::move(*outcome.plan);
    }

    ReleaseArtifactManifest Manifest(const ReleaseExecutionPlan &plan) {
        ReleaseArtifactManifestData data;
        data.candidate = ReleaseCandidateId{42U};
        data.product = plan.Request().profile.Product();
        data.version = plan.Request().version.productVersion;
        data.sourceRevision = plan.Request().version.sourceRevision;
        data.platform = plan.Request().profile.Platform();
        data.architecture = plan.Request().architecture;
        data.configuration = plan.Request().configuration;
        data.build = {"build_42"};
        data.toolchainId = plan.Request().toolchainId;
        data.frozen = plan.Identities();
        data.artifacts = {{"bin/editor", ReleaseArtifactRole::Binary, 6U, Digest("editor")}};
        auto manifest = ReleaseArtifactManifest::Create(std::move(data));
        REQUIRE(manifest.HasValue());
        return std::move(manifest).Value();
    }

    class ArchiveProbe final : public IReleaseCandidateSmokeProbe {
    public:
        [[nodiscard]] ReleaseCandidateSmokeKind Kind() const noexcept override {
            return ReleaseCandidateSmokeKind::ArchiveReadable;
        }

        [[nodiscard]] Result<void> Check(const std::filesystem::path &, const ReleaseArtifactManifest &) override {
            return Result<void>::Success();
        }
    };

    class FakeGitHubClient final : public IGitHubReleaseClient {
    public:
        [[nodiscard]] Result<GitHubReleaseIdentity> FindExisting(const std::string_view repository, const std::string_view tag) override {
            ++lookups;
            if (missing)
                return Result<GitHubReleaseIdentity>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            return Result<GitHubReleaseIdentity>::Success({std::string{repository}, wrongTag ? "v0.4.3" : std::string{tag}, releaseId});
        }

        [[nodiscard]] Result<void> UploadExact(const GitHubReleaseIdentity &release, const std::string_view name,
                                               const std::filesystem::path &file, const ReleaseArtifactRecord &evidence) override {
            if (release.releaseId != releaseId || !std::filesystem::is_regular_file(file))
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            auto [entry, inserted] =
                assets.try_emplace(std::string{name}, GitHubReleaseAssetEvidence{std::string{name}, evidence.size, evidence.digest});
            if (!inserted && (entry->second.size != evidence.size || entry->second.digest != evidence.digest))
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputCollision));
            ++uploads;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<GitHubReleaseAssetEvidence> ReadAsset(const GitHubReleaseIdentity &release,
                                                                   const std::string_view name) override {
            if (release.releaseId != releaseId || !assets.contains(std::string{name}))
                return Result<GitHubReleaseAssetEvidence>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            return Result<GitHubReleaseAssetEvidence>::Success(assets.at(std::string{name}));
        }

        [[nodiscard]] Result<void> Commit(const GitHubReleaseIdentity &release, const ReleaseChannel &, const Sha256Digest &) override {
            if (release.releaseId != releaseId)
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            ++commits;
            return Result<void>::Success();
        }

        std::map<std::string, GitHubReleaseAssetEvidence> assets;
        std::uint64_t releaseId{72U};
        int lookups{};
        int uploads{};
        int commits{};
        bool missing{};
        bool wrongTag{};
    };
}  // namespace

TEST_CASE("GitHub adapter publishes only declared bytes to the existing tagged release", "[release][github]") {
    TemporaryCandidate directory;
    const auto plan = PublicationPlan();
    auto manifest = Manifest(plan);
    WriteFile(directory.root / "bin/editor", "editor");
    WriteFile(directory.root / "manifest.json", manifest.CanonicalJson());
    ArchiveProbe probe;
    IReleaseCandidateSmokeProbe *probes[]{&probe};
    const ReleaseCandidateSmokeKind required[]{ReleaseCandidateSmokeKind::ArchiveReadable};
    auto verified = VerifyReleaseCandidate(directory.root, manifest, required, nullptr, probes);
    REQUIRE(verified.HasValue());
    ReleasePublicationRequest request{plan, verified.Value(), manifest, {ReleaseChannelKind::Stable, {}}};
    FakeGitHubClient client;
    GitHubReleasePublicationAdapter adapter{"horocore/horo-engine", client};

    auto published = PublishVerifiedReleaseCandidate(request, adapter);
    REQUIRE(published.HasValue());
    CHECK(published.Value().remoteIdentity == "72");
    CHECK(client.assets.size() == 2U);
    CHECK(client.assets.contains("bin%2Feditor"));
    CHECK(client.assets.contains("manifest.json"));
    CHECK(client.commits == 1);
    CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasValue());
    CHECK(client.commits == 2);
}

TEST_CASE("GitHub adapter rejects missing, changed, or corrupted remote release before channel commit", "[release][github]") {
    TemporaryCandidate directory;
    const auto plan = PublicationPlan();
    auto manifest = Manifest(plan);
    WriteFile(directory.root / "bin/editor", "editor");
    WriteFile(directory.root / "manifest.json", manifest.CanonicalJson());
    ArchiveProbe probe;
    IReleaseCandidateSmokeProbe *probes[]{&probe};
    const ReleaseCandidateSmokeKind required[]{ReleaseCandidateSmokeKind::ArchiveReadable};
    auto verified = VerifyReleaseCandidate(directory.root, manifest, required, nullptr, probes);
    REQUIRE(verified.HasValue());
    ReleasePublicationRequest request{plan, verified.Value(), manifest, {ReleaseChannelKind::Stable, {}}};
    FakeGitHubClient client;
    GitHubReleasePublicationAdapter adapter{"horocore/horo-engine", client};

    client.missing = true;
    CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasError());
    CHECK(client.uploads == 0);
    client.missing = false;
    client.wrongTag = true;
    CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasError());
    CHECK(client.uploads == 0);
    client.wrongTag = false;
    auto receipt = adapter.Upload(request);
    REQUIRE(receipt.HasValue());
    client.assets.at("bin%2Feditor").digest = Digest("tampered");
    CHECK(adapter.VerifyRemote(request, receipt.Value()).HasError());
    CHECK(client.commits == 0);
    client.assets.at("bin%2Feditor").digest = Digest("editor");
    ++client.releaseId;
    CHECK(adapter.VerifyRemote(request, receipt.Value()).HasError());
    CHECK(adapter.CommitChannel(request, receipt.Value()).HasError());
    CHECK(client.commits == 0);
}
