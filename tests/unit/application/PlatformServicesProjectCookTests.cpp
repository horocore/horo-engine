#include "../../support/NativePublicationFiles.h"
#include "../../support/project/PlatformServicesMigrationFixture.h"
#include "Horo/Application/PlatformServicesProjectCook.h"

#include <functional>
#include <stdexcept>

namespace Horo::Tests::PlatformMigration {
    namespace {
        /** @brief Injects faults around real native staging and commit while retaining the native monotonic receipt. */
        class CookFiles final : public TestSupport::NativePublicationFiles {
        public:
            std::function<void()> staged;
            bool failPreparation{};
            bool failCleanup{};
            bool failCommit{};
            bool failAfterCommit{};
            bool throwAfterCommit{};
            std::uint32_t commits{};

            Result<void> RemoveDurable(const std::filesystem::path &path) override {
                return failCleanup ? Failure() : native.RemoveDurable(path);
            }

            Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                             std::filesystem::perms permissions) override {
                auto written = native.WritePrivateDurable(path, bytes, created, permissions);
                if (written.HasError())
                    return written;
                if (staged)
                    staged();
                if (failPreparation)
                    return Failure();
                return written;
            }

            Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                              AtomicFileReplacementReceipt &receipt) override {
                if (failCommit)
                    return Failure();
                auto replaced = native.AtomicReplaceTracked(prepared, destination, receipt);
                if (!receipt.WasCommitted())
                    return replaced;
                ++commits;
                if (throwAfterCommit)
                    throw std::runtime_error("injected after commit");
                if (failAfterCommit)
                    return Failure();
                return replaced;
            }

        private:
            static Result<void> Failure() {
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            }
        };

        /** @brief Captures the same bounded models that actual editor/CLI admission uses. */
        PlatformServicesProjectSourceSnapshot Source(Project &project) {
            auto source = CapturePlatformServicesProjectSources(project.root, InspectProjectCompatibility(project.root),
                                                                {.profile = PlatformServicesHostProfile::Cook});
            REQUIRE(source.HasValue());
            return std::move(source).Value();
        }

        /** @brief Reads the sole committed pair without consulting private staging. */
        Assets::AssetSourceCapture Output(Project &project) {
            auto current = Assets::CaptureAssetSource(project.root, project.root / PlatformServicesProjectCookPath,
                                                      PlatformProviderManifestCookGenerationMaximumBytes);
            REQUIRE(current.HasValue());
            return std::move(current).Value();
        }

        /** @brief Holds the genuine exact-output native writer lease for every attempted publication. */
        Result<PlatformServicesProjectCookPublication> Publish(Project &project, const PlatformServicesProjectSourceSnapshot &source,
                                                               CookFiles &files, const Assets::AssetSourceIdentity &expected,
                                                               CancellationToken cancellation = {},
                                                               std::function<Result<void>()> mappingFence = {},
                                                               const std::uint64_t mappingRevision = 0) {
            auto lockPath = Assets::AssetSourcePublicationLockPath(project.root, project.root / PlatformServicesProjectCookPath);
            REQUIRE(lockPath.HasValue());
            auto lease = files.TryAcquireExclusive(lockPath.Value(), "platform-cook-fault-regression");
            REQUIRE(lease.HasValue());
            return CookAndPublishPlatformServicesProject({.sources = source,
                                                          .mappingRevision = mappingRevision,
                                                          .validateMappingRevision = std::move(mappingFence)},
                                                         expected, lease.Value(), files, cancellation);
        }
    }  // namespace

    TEST_CASE("Cook preserves the previous pair across preparation, stale source, cancellation and precommit failures",
              "[platform-cook-publication]") {
        Project project;
        project.InitializeCurrent();
        CookFiles files;
        const auto source = Source(project);
        REQUIRE(Publish(project, source, files, {}).HasValue());
        const auto previous = Output(project);
        CancellationSource cancellation;
        SECTION("Preparation failure after private creation") {
            files.failPreparation = true;
        }
        SECTION("Failed cleanup retains preparation evidence and the old committed pair") {
            files.failPreparation = true;
            files.failCleanup = true;
        }
        SECTION("Native replacement failure") {
            files.failCommit = true;
        }
        SECTION("Root changed after staging") {
            files.staged = [&] {
                project.Write(".horo/project.json", project.Read(".horo/project.json") + " ");
            };
        }
        SECTION("Ledger changed after staging") {
            files.staged = [&] {
                project.Write(PlatformServicesDocumentPaths::Ledger, project.Read(PlatformServicesDocumentPaths::Ledger) + " ");
            };
        }
        SECTION("Definitions changed after staging") {
            files.staged = [&] {
                project.Write(PlatformServicesDocumentPaths::Presence, project.Read(PlatformServicesDocumentPaths::Presence) + " ");
            };
        }
        SECTION("Achievements changed after staging") {
            files.staged = [&] {
                project.Write(PlatformServicesDocumentPaths::Achievements, project.Read(PlatformServicesDocumentPaths::Achievements) + " ");
            };
        }
        SECTION("Stats changed after staging") {
            files.staged = [&] {
                project.Write(PlatformServicesDocumentPaths::Stats, project.Read(PlatformServicesDocumentPaths::Stats) + " ");
            };
        }
        SECTION("Leaderboards changed after staging") {
            files.staged = [&] {
                project.Write(PlatformServicesDocumentPaths::Leaderboards, project.Read(PlatformServicesDocumentPaths::Leaderboards) + " ");
            };
        }
        SECTION("Cancellation after staging") {
            files.staged = [&] {
                cancellation.RequestCancellation();
            };
        }
        const auto published = Publish(project, source, files, previous.identity, cancellation.Token());
        REQUIRE(published.HasError());
        CHECK(Output(project).bytes == previous.bytes);
        CHECK(files.commits == 1);
        CHECK(std::filesystem::exists(project.root / (std::string{PlatformServicesProjectCookPath} + ".source-publication.tmp")) ==
              files.failCleanup);
        if (files.failCleanup) {
            files.failPreparation = false;
            files.failCleanup = false;
            CHECK(Publish(project, source, files, previous.identity).HasError());
            CHECK(Output(project).bytes == previous.bytes);
        }
    }

    TEST_CASE("Cook refuses missing writer authority and non-cook host profiles before derived writes", "[platform-cook-publication]") {
        Project project;
        project.InitializeCurrent();
        CookFiles files;
        const auto source = Source(project);
        const ExclusiveFileLock absent;
        const auto denied = CookAndPublishPlatformServicesProject({.sources = source}, {}, absent, files);
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == Assets::AssetSourceErrors::Authority.code.Value());
        CHECK_FALSE(std::filesystem::exists(project.root / "build"));
        auto interactive = CapturePlatformServicesProjectSources(project.root, InspectProjectCompatibility(project.root), {});
        REQUIRE(interactive.HasValue());
        const auto wrongProfile = CookAndPublishPlatformServicesProject({.sources = interactive.Value()}, {}, absent, files);
        REQUIRE(wrongProfile.HasError());
        CHECK(wrongProfile.ErrorValue().code.Value() == PlatformProviderManifestCookErrors::InvalidInput.code.Value());
        CHECK_FALSE(std::filesystem::exists(project.root / "build"));
    }

    TEST_CASE("Cook rechecks host revision authority after staging and preserves an obsolete expected-output base",
              "[platform-cook-publication]") {
        Project project;
        project.InitializeCurrent();
        CookFiles files;
        const auto source = Source(project);
        REQUIRE(Publish(project, source, files, {}).HasValue());
        const auto previous = Output(project);
        std::uint32_t checks{};
        const auto stale = Publish(project, source, files, previous.identity, {}, [&] {
            ++checks;
            return checks == 1 ? Result<void>::Success()
                               : Result<void>::Failure(MakeError(PlatformProviderManifestCookErrors::StaleGeneration));
        });
        REQUIRE(stale.HasError());
        CHECK(checks == 2);
        CHECK(stale.ErrorValue().code.Value() == PlatformProviderManifestCookErrors::StaleGeneration.code.Value());
        CHECK(Output(project).bytes == previous.bytes);
        CHECK(Publish(project, source, files, {}).HasError());
        CHECK(Output(project).bytes == previous.bytes);
    }

    TEST_CASE("Exact-provider cook requires the private host revision witness and rechecks it before commit",
              "[platform-cook-publication]") {
        Project project;
        project.InitializeCurrent();
        auto metadata = nlohmann::json::parse(project.Read(".horo/project.json"));
        metadata["settings"]["platformServices"]["provider"] = {{"mode", "exact"}, {"key", "platform.fixture"}};
        project.Write(".horo/project.json", metadata.dump());
        PlatformServicesProjectValidationContext context{.profile = PlatformServicesHostProfile::Cook};
        PlatformProviderModuleContribution contribution{.module = ModuleId{"horo.platform.fixture"},
                                                        .providerKey = "platform.fixture",
                                                        .provider = PlatformProviderId{41},
                                                        .interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                             PlatformServicesBackendInterfaceMinor},
                                                        .allowedProfiles = PlatformServicesHostProfileMask::Cook};
        contribution.supportedServices.fill(true);
        context.contributions.push_back(contribution);
        context.trustedModules.push_back(contribution.module);
        auto source = CapturePlatformServicesProjectSources(project.root, InspectProjectCompatibility(project.root), context);
        REQUIRE(source.HasValue());
        CookFiles files;
        const auto noWitness = Publish(project, source.Value(), files, {}, {}, {}, 7);
        REQUIRE(noWitness.HasError());
        CHECK(noWitness.ErrorValue().code.Value() == PlatformProviderManifestCookErrors::StaleGeneration.code.Value());
        CHECK_FALSE(std::filesystem::exists(project.root / "build"));
        std::uint32_t checks{};
        const auto stale = Publish(project, source.Value(), files, {}, {}, [&] {
            return ++checks == 1 ? Result<void>::Success()
                                 : Result<void>::Failure(MakeError(PlatformProviderManifestCookErrors::StaleGeneration));
        }, 7);
        REQUIRE(stale.HasError());
        CHECK(checks == 2);
        CHECK_FALSE(Output(project).identity.exists);
        CHECK(files.commits == 0);
        const auto valid = Publish(project, source.Value(), files, {}, {}, [] {
            return Result<void>::Success();
        }, 7);
        REQUIRE(valid.HasValue());
        CHECK(Output(project).identity.digest == valid.Value().generationFingerprint);
    }

    TEST_CASE("Cook retains the complete committed pair when native durability fails or throws after replacement",
              "[platform-cook-publication]") {
        Project project;
        project.InitializeCurrent();
        CookFiles files;
        SECTION("Typed failure after commit") {
            files.failAfterCommit = true;
        }
        SECTION("Exception after commit") {
            files.throwAfterCommit = true;
        }
        const auto published = Publish(project, Source(project), files, {});
        REQUIRE(published.HasValue());
        CHECK(published.Value().publication.status == Assets::AssetSourcePublicationStatus::ReconciliationRequired);
        REQUIRE(published.Value().publication.durabilityError.has_value());
        const auto current = Output(project);
        CHECK(current.identity.digest == published.Value().generationFingerprint);
        CHECK(DecodePlatformProviderManifestCookGeneration(current.bytes, published.Value().generationFingerprint).HasValue());
        CHECK(files.commits == 1);
    }
}  // namespace Horo::Tests::PlatformMigration
