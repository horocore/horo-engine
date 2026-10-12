#include "../../support/NativePublicationFiles.h"
#include "../packages/PackageArchiveTestSupport.h"
#include "Horo/Application/UiTemplateHost.h"
#include "Horo/Packages/PackageRequest.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateCaptureControls.h"

#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Application::Detail;
    using namespace Horo::Packages;
    using Horo::Tests::Packages::TemporaryDirectory;

    void Write(const std::filesystem::path &path, const std::string_view bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file{path, std::ios::binary | std::ios::trunc};
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        REQUIRE(file.good());
    }

    /** @brief Exercises a real directory link and retains the explicit unavailable-platform warning. */
    void CheckDirectoryControlLink(const std::filesystem::path &root, const std::filesystem::path &target, const bool dangling) {
        const auto control = root / ".horo";
        if (!dangling)
            std::filesystem::create_directory(target);
        std::error_code error;
        std::filesystem::create_directory_symlink(target, control, error);
        if (error) {
            WARN("Directory symlink creation unavailable: " << error.message());
        } else {
            CHECK(ReadTemplateControls(root, nullptr).HasError());
            std::filesystem::remove(control);
        }
        std::filesystem::remove_all(target);
    }

    /** @brief Exercises each actual control file link with the same existing/dangling target coverage. */
    void CheckFileControlLink(const std::filesystem::path &root, const std::filesystem::path &target, const char *name,
                              const bool dangling) {
        const auto control = root / ".horo";
        std::filesystem::create_directories(control);
        if (!dangling)
            Write(target, "{}");
        std::error_code error;
        const auto link = control / name;
        std::filesystem::create_symlink(target, link, error);
        if (error) {
            WARN("File symlink creation unavailable: " << error.message());
        } else {
            CHECK(ReadTemplateControls(root, nullptr).HasError());
            std::filesystem::remove(link);
        }
        std::filesystem::remove(target);
        std::filesystem::remove(control);
    }

    /** @brief Independently verified archive and actual Packages-generated lock for the capture adapter. */
    struct PackageControls final {
        UiTemplateCaptureControls controls;
        std::shared_ptr<PackageRestoreGraph> graph;

        PackageControls() {
            auto request = ValidatedPackageRequest::Parse(
                R"({"sources":{"horo.public":{"kind":"public-registry","registry":"official"}},"dependencies":{"com.horo.assets":{"source":"horo.public","version":"1.0.0"}}})");
            REQUIRE(request.HasValue());
            auto verified = ValidatedPackageArchive::Verify(Horo::Tests::Packages::ValidPackageArchiveBytes());
            REQUIRE(verified.HasValue());
            const auto archive = std::make_shared<const ValidatedPackageArchive>(std::move(verified).Value());
            const auto package = HoroPackageId::Parse("com.horo.assets").Value();
            const auto version = PackageVersion::Parse("1.0.0").Value();
            const auto source = HoroPackageSourceId::Parse("horo.public").Value();
            const PackageResolutionPlan plan{{{package, version, source, archive->Digest(), {}}}};
            const PackageLockArtifact
                evidence{package, version, source,    archive->Digest(), archive->PackageManifestDigest(), archive->Manifest().Digest(),
                         1,       {},      {"assets"}};
            auto lock =
                ValidatedPackageLockfileV1::Generate(plan, std::span{&package, 1U}, request.Value().Digest(), std::span{&evidence, 1U});
            REQUIRE(lock.HasValue());
            graph = std::make_shared<PackageRestoreGraph>(
                PackageRestoreGraph{request.Value().Digest(),
                                    {"linux", "x64", "horo-sdk-2"},
                                    {{lock.Value().Packages().front(), archive, false, std::nullopt}}});
            controls = {request.Value().SerializeCanonical(), lock.Value().SerializeCanonical(), graph};
        }
    };

    /** @brief Observes the production coordinator calling a real native lock while its in-process reservation is active. */
    class ObservedFiles final : public Horo::TestSupport::NativePublicationFiles {
    public:
        const Editor::ProjectMutationCoordinator *coordinator{};
        std::filesystem::path root;
        std::size_t nativeAdmissions{};
        bool rivalDenied{};

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, const std::string_view owner) override {
            ++nativeAdmissions;
            auto lease = native.TryAcquireExclusive(path, owner);
            if (coordinator && lease.HasValue())
                rivalDenied = coordinator->TryAcquire({root, Editor::ProjectMutationOwner::Save, "rival-during-capture"}).HasError();
            return lease;
        }
    };
}  // namespace

TEST_CASE("Template capture accepts absent controls and uses real project admission", "[template][host][capture]") {
    TemporaryDirectory temporary{"horo-template-capture"};
    SECTION("absent control directory") {
        const auto controls = ReadTemplateControls(temporary.Path(), nullptr);
        REQUIRE(controls.HasValue());
        CHECK_FALSE(controls.Value().intent);
        CHECK_FALSE(controls.Value().lock);
        CHECK(ValidateTemplatePackages(controls.Value(), false).HasValue());
    }
    SECTION("existing directory without files") {
        std::filesystem::create_directories(temporary.Path() / ".horo");
        const auto controls = ReadTemplateControls(temporary.Path(), nullptr);
        REQUIRE(controls.HasValue());
        CHECK_FALSE(controls.Value().intent);
        CHECK_FALSE(controls.Value().lock);
    }
    ObservedFiles files;
    Editor::ProjectMutationCoordinator coordinator{files};
    files.coordinator = &coordinator;
    files.root = temporary.Path();
    const UiTemplateInputCapturePolicy policy{.requirePublisher = false, .limits = {}, .maximumBytes = 256U * 1024U * 1024U};
    const auto captured = UiTemplateInputLease::Capture(temporary.Path(), {}, coordinator, nullptr, policy);
    REQUIRE(captured.HasValue());
    CHECK(files.nativeAdmissions == 1);
    CHECK(files.rivalDenied);
    CHECK(coordinator.TryAcquire({temporary.Path(), Editor::ProjectMutationOwner::Save, "after-capture"}).HasValue());
}

TEST_CASE("Template input factory preserves busy admission and releases failed capture", "[template][host][capture]") {
    TemporaryDirectory temporary;
    NativeDurableFileSystem files;
    Editor::ProjectMutationCoordinator coordinator{files};
    {
        const auto writer = coordinator.TryAcquire({temporary.Path(), Editor::ProjectMutationOwner::Save, "owned-save"});
        REQUIRE(writer.HasValue());
        const auto captured = UiTemplateInputLease::Capture(temporary.Path(), {}, coordinator, nullptr);
        REQUIRE(captured.HasError());
        CHECK(captured.ErrorValue().code.Value() == "project.migration.locked");
    }
    Write(temporary.Path() / ".horo/packages.json", "malformed");
    CHECK(UiTemplateInputLease::Capture(temporary.Path(), {}, coordinator, nullptr).HasError());
    CHECK(coordinator.TryAcquire({temporary.Path(), Editor::ProjectMutationOwner::Save, "after-failure"}).HasValue());
}

TEST_CASE("Template controls reject file and directory kind substitutions", "[template][host][capture]") {
    TemporaryDirectory temporary;
    const auto control = temporary.Path() / ".horo";
    SECTION("control directory is a file") {
        Write(control, "not a directory");
        CHECK(ReadTemplateControls(temporary.Path(), nullptr).HasError());
    }
    SECTION("control file is a directory") {
        std::filesystem::create_directories(control / "packages.json");
        CHECK(ReadTemplateControls(temporary.Path(), nullptr).HasError());
    }
    SECTION("lock file is a directory") {
        std::filesystem::create_directories(control / "packages.lock");
        CHECK(ReadTemplateControls(temporary.Path(), nullptr).HasError());
    }
}

TEST_CASE("Template controls reject existing and dangling symlinks", "[template][host][capture]") {
    TemporaryDirectory temporary;
    SECTION("symlink controls") {
        const auto target = temporary.Path() / "target";
        for (const bool dangling : {false, true}) {
            DYNAMIC_SECTION("directory link dangling=" << dangling) {
                CheckDirectoryControlLink(temporary.Path(), target, dangling);
            }
            for (const char *name : {"packages.json", "packages.lock"}) {
                DYNAMIC_SECTION(name << " link dangling=" << dangling) {
                    CheckFileControlLink(temporary.Path(), target, name, dangling);
                }
            }
        }
    }
}

TEST_CASE("Template final capture recheck separates stale replacement from typed read failure", "[template][host][capture]") {
    TemporaryDirectory temporary;
    PackageControls packages;
    Write(temporary.Path() / ".horo/packages.json", *packages.controls.intent);
    Write(temporary.Path() / ".horo/packages.lock", *packages.controls.lock);
    const auto captured = ReadTemplateControls(temporary.Path(), packages.graph);
    REQUIRE(captured.HasValue());
    REQUIRE(ValidateTemplatePackages(captured.Value(), false).HasValue());
    REQUIRE(VerifyTemplateControls(temporary.Path(), captured.Value(), packages.graph).HasValue());
    SECTION("intent replaced") {
        Write(temporary.Path() / ".horo/packages.json", "{}");
    }
    SECTION("lock replaced") {
        Write(temporary.Path() / ".horo/packages.lock", "{}");
    }
    SECTION("control removed") {
        std::filesystem::remove(temporary.Path() / ".horo/packages.lock");
    }
    SECTION("equal content graph republished") {
        packages.graph = std::make_shared<PackageRestoreGraph>(*packages.graph);
    }
    const ErrorCodeDescriptor *expected = &Runtime::Ui::UiErrors::RevisionStale;
    SECTION("control became unreadable directory") {
        const auto path = temporary.Path() / ".horo/packages.json";
        std::filesystem::remove(path);
        std::filesystem::create_directory(path);
        expected = &UiTemplates::UiErrors::TemplateGraphInvalid;
    }
    SECTION("control exceeded read ceiling") {
        Write(temporary.Path() / ".horo/packages.lock", std::string(4U * 1024U * 1024U + 1U, ' '));
        expected = &UiTemplates::UiErrors::TemplateGraphBudgetExceeded;
    }
    const auto result = VerifyTemplateControls(temporary.Path(), captured.Value(), packages.graph);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == expected->code.Value());
}

TEST_CASE("Template package admission retains exact lock archive and publisher decisions", "[template][host][capture]") {
    PackageControls packages;
    REQUIRE(ValidateTemplatePackages(packages.controls, false).HasValue());
    SECTION("publisher required but absent") {
        CHECK(ValidateTemplatePackages(packages.controls, true).HasError());
    }
    SECTION("unsigned policy does not qualify required publisher evidence") {
        packages.graph->packages.front().publisher =
            PackagePublisherVerificationDecision{PackagePublisherVerificationOutcome::Unsigned,
                                                 true,
                                                 std::nullopt,
                                                 {},
                                                 packages.graph->packages.front().archive->Digest()};
        CHECK(ValidateTemplatePackages(packages.controls, false).HasValue());
        CHECK(ValidateTemplatePackages(packages.controls, true).HasError());
    }
    SECTION("accepted publisher retains exact artifact decision") {
        packages.graph->packages.front().publisher =
            PackagePublisherVerificationDecision{PackagePublisherVerificationOutcome::Accepted,
                                                 true,
                                                 std::nullopt,
                                                 {},
                                                 packages.graph->packages.front().archive->Digest()};
        CHECK(ValidateTemplatePackages(packages.controls, true).HasValue());
    }
    SECTION("publisher denied or revoked") {
        for (const auto outcome :
             {PackagePublisherVerificationOutcome::RevokedPublisher, PackagePublisherVerificationOutcome::UnknownPublisher}) {
            packages.graph->packages.front().publisher =
                PackagePublisherVerificationDecision{outcome, false, std::nullopt, {}, packages.graph->packages.front().archive->Digest()};
            CHECK(ValidateTemplatePackages(packages.controls, false).HasError());
            CHECK(ValidateTemplatePackages(packages.controls, true).HasError());
        }
    }
    SECTION("accepted publisher pinned to wrong archive") {
        packages.graph->packages.front().publisher = PackagePublisherVerificationDecision{PackagePublisherVerificationOutcome::Accepted,
                                                                                          true,
                                                                                          std::nullopt,
                                                                                          {},
                                                                                          ComputeSha256(std::span<const std::byte>{})};
        CHECK(ValidateTemplatePackages(packages.controls, true).HasError());
    }
    SECTION("archive absent") {
        packages.graph->packages.front().archive.reset();
        CHECK(ValidateTemplatePackages(packages.controls, false).HasError());
    }
}

TEST_CASE("Template package admission rejects mismatched locked evidence", "[template][host][capture]") {
    PackageControls packages;
    SECTION("exact locked evidence differs") {
        auto &entry = packages.graph->packages.front();
        SECTION("archive digest") {
            entry.lock.artifactDigest = ComputeSha256(std::span<const std::byte>{});
        }
        SECTION("package manifest digest") {
            entry.lock.manifestDigest = ComputeSha256(std::span<const std::byte>{});
        }
        SECTION("file manifest digest") {
            entry.lock.fileManifestDigest = ComputeSha256(std::span<const std::byte>{});
        }
        SECTION("format") {
            ++entry.lock.packageFormatVersion;
        }
        SECTION("source") {
            entry.lock.source = HoroPackageSourceId::Parse("other.source").Value();
        }
        SECTION("version") {
            entry.lock.version = PackageVersion::Parse("1.0.1").Value();
        }
        SECTION("contributions") {
            entry.lock.contributions.clear();
        }
        CHECK(ValidateTemplatePackages(packages.controls, false).HasError());
    }
}
