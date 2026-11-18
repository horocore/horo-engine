#include "Horo/Release/GitHubReleasePublication.h"
#include "HostReleaseManifestSignatureVerifier.h"
#include "HostSignatureDocument.h"
#include "ReleaseTestFixtures.h"
#include "support/OpenXRProductSigningTestSupport.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <utility>

using namespace Horo;
using namespace Horo::Release;
using namespace ReleaseTestFixtures;

namespace {
    constexpr std::string_view SourceCommit = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

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
        request.version.sourceRevision.value = SourceCommit;
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
        explicit FakeGitHubClient(const bool mismatchedBody = false) : wrongBody(mismatchedBody) {}

        [[nodiscard]] Result<GitHubReleaseIdentity> FindExisting(const std::string_view repository, const std::string_view tag) override {
            ++lookups;
            if (missing)
                return Result<GitHubReleaseIdentity>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            return Result<GitHubReleaseIdentity>::Success(
                {std::string{repository}, wrongTag ? "v0.4.3" : std::string{tag}, releaseId,
                 wrongSource ? "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" : std::string{SourceCommit},
                 wrongBody ? "Unreviewed notes" : nlohmann::json::parse(Notes()).at("markdown").get<std::string>()});
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
            auto observed = assets.at(std::string{name});
            if (name == ReleaseManifestSignatureFilename && !changeLocalOnSignatureRead.empty())
                WriteFile(changeLocalOnSignatureRead, "changed after upload");
            if (name == ReleaseManifestSignatureFilename && corruptRemoteSignature)
                observed.digest = Digest("remote corruption");
            return Result<GitHubReleaseAssetEvidence>::Success(std::move(observed));
        }

        [[nodiscard]] Result<void> Commit(const GitHubReleaseIdentity &release, const ReleaseChannel &, const Sha256Digest &) override {
            if (release.releaseId != releaseId)
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            ++commits;
            return Result<void>::Success();
        }

        Result<void> CommitVerified(const GitHubReleaseIdentity &release, const ReleaseChannel &channel,
                                    const GitHubReleaseFinalMetadata &expected) override {
            if (!expected.manifestSignature || legacyCommit)
                return IGitHubReleaseClient::CommitVerified(release, channel, expected);
            const auto metadata = ReadAsset(release, ReleaseManifestFilename);
            const auto envelope = ReadAsset(release, ReleaseManifestSignatureFilename);
            if (metadata.HasError() || envelope.HasError() || metadata.Value().size != expected.manifestSize ||
                metadata.Value().digest != expected.manifestDigest || envelope.Value().size != expected.manifestSignature->size ||
                envelope.Value().digest != expected.manifestSignature->digest)
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            return Commit(release, channel, expected.manifestDigest);
        }

        std::map<std::string, GitHubReleaseAssetEvidence> assets;
        std::filesystem::path changeLocalOnSignatureRead;
        std::uint64_t releaseId{72U};
        int lookups{};
        int uploads{};
        int commits{};
        bool missing{};
        bool wrongTag{};
        bool wrongSource{};
        bool wrongBody{};
        bool corruptRemoteSignature{};
        bool legacyCommit{};
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
    FakeGitHubClient missingClient;
    missingClient.missing = true;
    GitHubReleasePublicationAdapter missingAdapter{"horocore/horo-engine", missingClient};
    CHECK(PublishVerifiedReleaseCandidate(request, missingAdapter).HasError());
    CHECK(missingClient.uploads == 0);

    FakeGitHubClient wrongTagClient;
    wrongTagClient.wrongTag = true;
    GitHubReleasePublicationAdapter wrongTagAdapter{"horocore/horo-engine", wrongTagClient};
    CHECK(PublishVerifiedReleaseCandidate(request, wrongTagAdapter).HasError());
    CHECK(wrongTagClient.uploads == 0);

    FakeGitHubClient wrongSourceClient;
    wrongSourceClient.wrongSource = true;
    GitHubReleasePublicationAdapter wrongSourceAdapter{"horocore/horo-engine", wrongSourceClient};
    CHECK(PublishVerifiedReleaseCandidate(request, wrongSourceAdapter).HasError());
    CHECK(wrongSourceClient.uploads == 0);

    FakeGitHubClient wrongBodyClient{true};
    GitHubReleasePublicationAdapter wrongBodyAdapter{"horocore/horo-engine", wrongBodyClient};
    CHECK(PublishVerifiedReleaseCandidate(request, wrongBodyAdapter).HasError());
    CHECK(wrongBodyClient.uploads == 0);

    FakeGitHubClient client;
    GitHubReleasePublicationAdapter adapter{"horocore/horo-engine", client};
    auto receipt = adapter.Upload(request);
    REQUIRE(receipt.HasValue());
    client.assets.at("bin%2Feditor").digest = Digest("tampered");
    CHECK(adapter.VerifyRemote(request, receipt.Value()).HasError());
    CHECK(client.commits == 0);
    client.assets.at("bin%2Feditor").digest = Digest("editor");
    FakeGitHubClient changedBodyClient{true};
    changedBodyClient.assets = client.assets;
    GitHubReleasePublicationAdapter changedBodyAdapter{"horocore/horo-engine", changedBodyClient};
    CHECK(changedBodyAdapter.VerifyRemote(request, receipt.Value()).HasError());
    CHECK(changedBodyAdapter.CommitChannel(request, receipt.Value()).HasError());
    ++client.releaseId;
    CHECK(adapter.VerifyRemote(request, receipt.Value()).HasError());
    CHECK(adapter.CommitChannel(request, receipt.Value()).HasError());
    CHECK(client.commits == 0);
}

TEST_CASE("GitHub publishes exact verified detached release metadata alongside the manifest", "[release][github][signed-manifest]") {
    TemporaryCandidate directory;
    const auto plan = PublicationPlan();
    const auto manifest = Manifest(plan);
    const auto &canonical = manifest.CanonicalJson();
    const auto signaturePath = directory.root / ReleaseManifestSignatureFilename;
    WriteFile(directory.root / "bin/editor", "editor");
    WriteFile(directory.root / ReleaseManifestFilename, canonical);
    Tests::OpenXRProduct::SigningKey signer;
    auto roots = std::make_shared<Security::TrustedRootStore>();
    REQUIRE(roots->Add(signer.root).HasValue());
    const Security::ArtifactVerifier verifier{Security::CreateMbedTlsSignatureProvider(), roots};
    const Application::Internal::HostReleaseManifestSignatureVerifier host{verifier};
    const auto detached = Application::Internal::EncodeDetachedSignatureDocument(signer.Sign(std::as_bytes(std::span{canonical})));
    WriteFile(signaturePath, detached);

    ArchiveProbe probe;
    IReleaseCandidateSmokeProbe *probes[]{&probe};
    const ReleaseCandidateSmokeKind required[]{ReleaseCandidateSmokeKind::ArchiveReadable};
    const auto verified = VerifyReleaseCandidate(directory.root, manifest, required, nullptr, probes, &host);
    REQUIRE(verified.HasValue());
    REQUIRE(verified.Value().ManifestSignature());
    const ReleasePublicationRequest request{plan, verified.Value(), manifest, {ReleaseChannelKind::Stable, {}}};
    FakeGitHubClient client;
    GitHubReleasePublicationAdapter adapter{"horocore/horo-engine", client};

    SECTION("Complete remote metadata is checked and exact retry remains idempotent") {
        const auto published = PublishVerifiedReleaseCandidate(request, adapter);
        REQUIRE(published.HasValue());
        CHECK(published.Value().manifestSignature == verified.Value().ManifestSignature());
        CHECK(published.Value().artifactCount == 1);
        CHECK(client.assets.size() == 3);
        const auto &envelope = client.assets.at(std::string{ReleaseManifestSignatureFilename});
        CHECK(envelope.size == detached.size());
        CHECK(envelope.digest == Digest(detached));
        CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasValue());
        CHECK(client.assets.size() == 3);
        CHECK(client.commits == 2);
    }
    SECTION("Local changes prevent even the first remote lookup") {
        auto changed = detached;
        changed.front() = '!';
        WriteFile(signaturePath, changed);
        CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasError());
        CHECK(client.lookups == 0);
        CHECK(client.uploads == 0);
    }
    SECTION("Local changes detected after upload block the channel commit") {
        client.changeLocalOnSignatureRead = signaturePath;
        CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasError());
        CHECK(client.uploads == 3);
        CHECK(client.commits == 0);
    }
    SECTION("Remote envelope corruption blocks the channel commit") {
        client.corruptRemoteSignature = true;
        CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasError());
        CHECK(client.uploads == 3);
        CHECK(client.commits == 0);
    }
    SECTION("A legacy manifest-only transport cannot finalize an authenticated detached candidate") {
        client.legacyCommit = true;
        CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasError());
        CHECK(client.commits == 0);
    }
    SECTION("A receipt without the frozen envelope identity cannot verify or finalize") {
        auto receipt = adapter.Upload(request);
        REQUIRE(receipt.HasValue());
        receipt.Value().manifestSignature.reset();
        CHECK(adapter.VerifyRemote(request, receipt.Value()).HasError());
        CHECK(adapter.CommitChannel(request, receipt.Value()).HasError());
        CHECK(client.commits == 0);
    }
}
