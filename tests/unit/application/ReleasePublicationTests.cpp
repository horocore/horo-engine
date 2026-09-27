#include "Horo/Release/ReleasePublication.h"
#include "ReleaseTestFixtures.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>

using namespace Horo;
using namespace Horo::Release;
using namespace ReleaseTestFixtures;

namespace {
    class TemporaryCandidate final {
    public:
        TemporaryCandidate()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-publication-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
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

    class RecordingAdapter final : public IReleasePublicationAdapter {
    public:
        [[nodiscard]] ReleaseDestinationId Destination() const override {
            return {"github-releases"};
        }

        [[nodiscard]] Result<ReleasePublicationReceipt> Upload(const ReleasePublicationRequest &request) override {
            ++uploads;
            return Result<ReleasePublicationReceipt>::Success({Destination(), request.verified.Candidate(),
                                                               request.verified.ManifestDigest(), request.manifest.Artifacts().size(),
                                                               omitRemoteIdentity ? "" : "release-42"});
        }

        [[nodiscard]] Result<void> VerifyRemote(const ReleasePublicationRequest &request, const ReleasePublicationReceipt &) override {
            ++verifications;
            if (mutateLocal)
                WriteFile(request.verified.Root() / "bin/editor", "change");
            if (rejectRemote)
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CommitChannel(const ReleasePublicationRequest &, const ReleasePublicationReceipt &) override {
            ++commits;
            return Result<void>::Success();
        }

        int uploads{};
        int verifications{};
        int commits{};
        bool mutateLocal{};
        bool rejectRemote{};
        bool omitRemoteIdentity{};
    };
}  // namespace

TEST_CASE("Publication commits a channel only after exact local and remote candidate verification", "[release][publication]") {
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
    RecordingAdapter adapter;

    auto published = PublishVerifiedReleaseCandidate(request, adapter);
    REQUIRE(published.HasValue());
    CHECK(adapter.uploads == 1);
    CHECK(adapter.verifications == 1);
    CHECK(adapter.commits == 1);

    adapter.rejectRemote = true;
    CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasError());
    CHECK(adapter.commits == 1);

    adapter.rejectRemote = false;
    adapter.omitRemoteIdentity = true;
    CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasError());
    CHECK(adapter.verifications == 2);
    CHECK(adapter.commits == 1);
    adapter.omitRemoteIdentity = false;

    adapter.mutateLocal = true;
    CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasError());
    CHECK(adapter.commits == 1);
}

TEST_CASE("Publication rejects changed candidates and unauthorized channel identities before upload", "[release][publication]") {
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
    ReleasePublicationRequest request{plan, verified.Value(), manifest, {ReleaseChannelKind::ProjectDefined, "Invalid/Name"}};
    RecordingAdapter adapter;

    CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasError());
    CHECK(adapter.uploads == 0);

    request.channel = {ReleaseChannelKind::Preview, {}};
    WriteFile(directory.root / "bin/editor", "change");
    CHECK(PublishVerifiedReleaseCandidate(request, adapter).HasError());
    CHECK(adapter.uploads == 0);
}
