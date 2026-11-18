#include "Horo/Release/ReleaseCandidatePublisher.h"
#include "Horo/Release/ReleaseCandidateVerification.h"
#include "HostReleaseManifestSignatureVerifier.h"
#include "HostSignatureDocument.h"
#include "ReleaseTestFixtures.h"
#include "support/OpenXRProductSigningTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

using namespace Horo;
using namespace Horo::Release;

namespace {
    class TemporaryDirectory final {
    public:
        TemporaryDirectory()
            : path(std::filesystem::temp_directory_path() /
                   ("horo-release-stage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    [[nodiscard]] ReleaseExecutionPlan Plan(const std::filesystem::path &outputRoot) {
        auto request = ReleaseTestFixtures::Request();
        request.outputRoot = outputRoot;
        auto facts = ReleaseTestFixtures::Facts(request);
        auto prepared = PreflightRelease(request, facts);
        REQUIRE(prepared.issues.empty());
        REQUIRE(prepared.plan.has_value());
        return std::move(*prepared.plan);
    }

    [[nodiscard]] ReleaseArtifactManifest Manifest(const ReleaseExecutionPlan &plan, const ReleaseCandidateId candidate) {
        ReleaseArtifactManifestData data;
        data.candidate = candidate;
        data.product = plan.Request().profile.Product();
        data.version = plan.Request().version.productVersion;
        data.sourceRevision = plan.Request().version.sourceRevision;
        data.platform = plan.Request().profile.Platform();
        data.architecture = plan.Request().architecture;
        data.configuration = plan.Request().configuration;
        data.build = {"build_42"};
        data.toolchainId = plan.Request().toolchainId;
        data.frozen = plan.Identities();
        data.artifacts = {{"bin/editor", ReleaseArtifactRole::Binary, 6U, ReleaseTestFixtures::Digest("editor")}};
        auto manifest = ReleaseArtifactManifest::Create(std::move(data));
        REQUIRE(manifest.HasValue());
        return std::move(manifest).Value();
    }

    void WriteFile(const std::filesystem::path &path, const std::string_view content) {
        std::ofstream output(path, std::ios::binary);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    std::span<const std::byte> Bytes(std::string_view text) {
        return std::as_bytes(std::span{text});
    }

    /** @brief Normally read-only smoke fixture that can demonstrate same-authority envelope replacement. */
    class ManifestSmokeProbe final : public IReleaseCandidateSmokeProbe {
    public:
        ReleaseCandidateSmokeKind Kind() const noexcept override {
            return ReleaseCandidateSmokeKind::ArchiveReadable;
        }

        Result<void> Check(const std::filesystem::path &root, const ReleaseArtifactManifest &) override {
            ++calls;
            if (!replacement.empty())
                WriteFile(root / ReleaseManifestSignatureFilename, replacement);
            return Result<void>::Success();
        }

        std::string replacement;
        unsigned calls{};
    };
}  // namespace

TEST_CASE("Release candidate publisher promotes only a fully verified private stage", "[release][staging]") {
    TemporaryDirectory output;
    auto plan = Plan(output.path);
    NativeDurableFileSystem files;
    NativeReleaseCandidatePublisher publisher{files};
    auto stage = publisher.Begin(plan, ReleaseCandidateId{42U});
    REQUIRE(stage.HasValue());
    CHECK(std::filesystem::is_directory(stage.Value().StageRoot() / "bin"));
    CHECK_FALSE(std::filesystem::exists(stage.Value().FinalRoot()));
    CHECK(publisher.Begin(plan, ReleaseCandidateId{42U}).HasError());
    auto contender = publisher.Begin(plan, ReleaseCandidateId{43U});
    REQUIRE(contender.HasValue());

    WriteFile(stage.Value().StageRoot() / "bin/editor", "editor");
    auto manifest = Manifest(plan, stage.Value().Candidate());
    REQUIRE(publisher.Promote(plan, stage.Value(), manifest).HasValue());
    CHECK(std::filesystem::is_directory(stage.Value().FinalRoot()));
    CHECK_FALSE(std::filesystem::exists(stage.Value().StageRoot()));
    CHECK(VerifyReleaseArtifactTree(stage.Value().FinalRoot(), manifest).HasValue());
    CHECK(publisher.Promote(plan, stage.Value(), manifest).HasValue());
    CHECK(publisher.Promote(plan, contender.Value(), Manifest(plan, contender.Value().Candidate())).HasError());
    CHECK(std::filesystem::is_directory(contender.Value().StageRoot()));
    CHECK(publisher.Begin(plan, ReleaseCandidateId{44U}).HasError());
}

TEST_CASE("Release candidate publisher refuses a retry when published bytes changed", "[release][staging][recovery]") {
    TemporaryDirectory output;
    auto plan = Plan(output.path);
    NativeDurableFileSystem files;
    NativeReleaseCandidatePublisher publisher{files};
    auto stage = publisher.Begin(plan, ReleaseCandidateId{45U});
    REQUIRE(stage.HasValue());
    WriteFile(stage.Value().StageRoot() / "bin/editor", "editor");
    auto manifest = Manifest(plan, stage.Value().Candidate());
    REQUIRE(publisher.Promote(plan, stage.Value(), manifest).HasValue());

    WriteFile(stage.Value().FinalRoot() / "bin/editor", "change");
    CHECK(publisher.Promote(plan, stage.Value(), manifest).HasError());
    CHECK_FALSE(std::filesystem::exists(stage.Value().StageRoot()));
}

TEST_CASE("Release candidate publisher preserves the stage and final path on invalid inventory", "[release][staging]") {
    TemporaryDirectory output;
    auto plan = Plan(output.path);
    NativeDurableFileSystem files;
    NativeReleaseCandidatePublisher publisher{files};
    auto stage = publisher.Begin(plan, ReleaseCandidateId{43U});
    REQUIRE(stage.HasValue());
    WriteFile(stage.Value().StageRoot() / "bin/editor", "editor");
    WriteFile(stage.Value().StageRoot() / "unlisted.txt", "not declared");
    CHECK(publisher.Promote(plan, stage.Value(), Manifest(plan, ReleaseCandidateId{99U})).HasError());
    auto manifest = Manifest(plan, stage.Value().Candidate());
    CHECK(publisher.Promote(plan, stage.Value(), manifest).HasError());
    CHECK(std::filesystem::is_directory(stage.Value().StageRoot()));
    CHECK_FALSE(std::filesystem::exists(stage.Value().FinalRoot()));
}

TEST_CASE("Release candidate publisher never replaces an existing staged manifest", "[release][staging]") {
    TemporaryDirectory output;
    auto plan = Plan(output.path);
    NativeDurableFileSystem files;
    NativeReleaseCandidatePublisher publisher{files};
    auto stage = publisher.Begin(plan, ReleaseCandidateId{44U});
    REQUIRE(stage.HasValue());
    WriteFile(stage.Value().StageRoot() / "bin/editor", "editor");
    WriteFile(stage.Value().StageRoot() / "manifest.json", "existing metadata");
    auto manifest = Manifest(plan, stage.Value().Candidate());

    CHECK(publisher.Promote(plan, stage.Value(), manifest).HasError());
    std::ifstream existing(stage.Value().StageRoot() / "manifest.json", std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>{existing}, std::istreambuf_iterator<char>{}};
    CHECK(bytes == "existing metadata");
    CHECK_FALSE(std::filesystem::exists(stage.Value().FinalRoot()));
}

TEST_CASE("Release assembly and final verification preserve exact trusted detached metadata",
          "[release][verification][staging][signed-manifest]") {
    using Application::Internal::EncodeDetachedSignatureDocument;
    using Application::Internal::HostReleaseManifestSignatureVerifier;
    TemporaryDirectory output;
    const auto plan = Plan(output.path);
    NativeDurableFileSystem files;
    NativeReleaseCandidatePublisher publisher{files};
    const auto stage = publisher.Begin(plan, ReleaseCandidateId{47U});
    REQUIRE(stage.HasValue());
    WriteFile(stage.Value().StageRoot() / "bin/editor", "editor");
    const auto manifest = Manifest(plan, stage.Value().Candidate());

    Tests::OpenXRProduct::SigningKey signer;
    const auto roots = std::make_shared<Security::TrustedRootStore>();
    REQUIRE(roots->Add(signer.root).HasValue());
    const auto crypto = Security::CreateMbedTlsSignatureProvider();
    REQUIRE(crypto);
    const Security::ArtifactVerifier trusted{crypto, roots};
    const Security::ArtifactVerifier untrusted{crypto, std::make_shared<Security::TrustedRootStore>()};
    const HostReleaseManifestSignatureVerifier trustedHost{trusted};
    const HostReleaseManifestSignatureVerifier untrustedHost{untrusted};
    const auto envelope = EncodeDetachedSignatureDocument(signer.Sign(Bytes(manifest.CanonicalJson())));
    const auto different = EncodeDetachedSignatureDocument(signer.Sign(Bytes("another candidate")));
    const ReleaseManifestSigningInput wrongPublisher{Bytes(envelope), untrustedHost};
    const ReleaseManifestSigningInput wrongManifest{Bytes(different), trustedHost};
    const ReleaseManifestSigningInput signature{Bytes(envelope), trustedHost};
    CHECK(publisher.Promote(plan, stage.Value(), manifest, &wrongPublisher).HasError());
    CHECK(publisher.Promote(plan, stage.Value(), manifest, &wrongManifest).HasError());
    CHECK_FALSE(std::filesystem::exists(stage.Value().StageRoot() / ReleaseManifestFilename));
    CHECK_FALSE(std::filesystem::exists(stage.Value().StageRoot() / ReleaseManifestSignatureFilename));
    REQUIRE(publisher.Promote(plan, stage.Value(), manifest, &signature).HasValue());
    CHECK(publisher.Promote(plan, stage.Value(), manifest, &signature).HasValue());
    CHECK_FALSE(std::filesystem::exists(stage.Value().StageRoot()));
    const auto &final = stage.Value().FinalRoot();

    ManifestSmokeProbe smoke;
    IReleaseCandidateSmokeProbe *probes[]{&smoke};
    const ReleaseCandidateSmokeKind required[]{ReleaseCandidateSmokeKind::ArchiveReadable};
    CHECK(VerifyReleaseArtifactTree(final, manifest).HasError());
    CHECK(VerifyReleaseCandidate(final, manifest, required, nullptr, probes, &untrustedHost).HasError());
    CHECK(smoke.calls == 0);
    const auto verified = VerifyReleaseCandidate(final, manifest, required, nullptr, probes, &trustedHost);
    REQUIRE(verified.HasValue());
    REQUIRE(verified.Value().ManifestSignature());
    CHECK(verified.Value().ManifestSignature()->size == envelope.size());
    CHECK(verified.Value().ManifestSignature()->digest == ComputeSha256(Bytes(envelope)));
    CHECK(smoke.calls == 1);

    SECTION("Even an authenticated envelope cannot be replaced during smoke checks") {
        smoke.replacement = envelope + '\n';
        REQUIRE(trustedHost.Verify(Bytes(manifest.CanonicalJson()), Bytes(smoke.replacement)).HasValue());
        CHECK(VerifyReleaseCandidate(final, manifest, required, nullptr, probes, &trustedHost).HasError());
        CHECK(smoke.calls == 2);
        CHECK(publisher.Promote(plan, stage.Value(), manifest, &signature).HasError());
    }
    SECTION("A same-size changed payload invalidates the signed final tree") {
        WriteFile(final / "bin/editor", "change");
        CHECK(VerifyReleaseCandidate(final, manifest, required, nullptr, probes, &trustedHost).HasError());
        CHECK(smoke.calls == 1);
    }
    SECTION("Extra or missing metadata cannot be treated as a continuation of the signed release") {
        WriteFile(final / "unexpected", "extra");
        CHECK(VerifySignedReleaseArtifactTree(final, manifest, trustedHost).HasError());
        REQUIRE(std::filesystem::remove(final / "unexpected"));
        REQUIRE(std::filesystem::remove(final / ReleaseManifestSignatureFilename));
        CHECK(VerifyReleaseCandidate(final, manifest, required, nullptr, probes, &trustedHost).HasError());
        CHECK(smoke.calls == 1);
    }
    SECTION("The detached envelope is bounded before being parsed") {
        WriteFile(final / ReleaseManifestSignatureFilename, std::string(MaximumReleaseManifestSignatureBytes + 1, 'x'));
        CHECK(VerifyReleaseCandidate(final, manifest, required, nullptr, probes, &trustedHost).HasError());
        CHECK(smoke.calls == 1);
    }
}
