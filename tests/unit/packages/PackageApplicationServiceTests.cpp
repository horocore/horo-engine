#include "PackageApplicationServiceTestSupport.h"

#include <array>

namespace Horo::Tests::PackageApplicationService {
    namespace {
        /** @brief Explicit digest-bound test approval for the real role-aware lifecycle. */
        class ExactRoleTrust final : public IPackageExtensionTrust {
        public:
            Result<PackageExtensionTrustApproval> Approve(const PackageRestorePackage &package,
                                                          const Extensions::ExtensionManifest &) const override {
                ++calls;
                return Result<PackageExtensionTrustApproval>::Success(
                    {package.archive->Digest(), package.archive->PackageManifestDigest(), {.revision = 1}});
            }

            mutable unsigned calls{};
        };

        /** @brief Observes the role handoff and real lifecycle retirement without loading fixture bytes. */
        class TestRoleHost final : public IPackageModuleHost {
        public:
            [[nodiscard]] std::string_view Identity() const noexcept override {
                return "fixture.application.role";
            }

            [[nodiscard]] bool Supports(const Extensions::ExtensionModuleRole role) const noexcept override {
                return role == Extensions::ExtensionModuleRole::BackendCapability;
            }

            Result<std::unique_ptr<IPackageModuleActivation>> Activate(const VerifiedPackageModule &module,
                                                                       std::span<const std::shared_ptr<IPackageModuleActivation>>,
                                                                       const CancellationToken &) override {
                ++activated;
                if (onActivate)
                    onActivate();
                return Result<std::unique_ptr<IPackageModuleActivation>>::Success(std::make_unique<Receipt>(*this, module.Activation()));
            }

            std::function<void()> onActivate;
            unsigned activated{}, retired{}, finalized{};
            bool drainAllowed{true};

        private:
            class Receipt final : public IPackageModuleActivation {
            public:
                Receipt(TestRoleHost &host, Extensions::ExtensionActivationLease proof) : host_(host), proof_(std::move(proof)) {}

                bool IsActive() const noexcept override {
                    return !retired_ && proof_.IsUsable();
                }

                void Retire() noexcept override {
                    if (!retired_) {
                        retired_ = true;
                        ++host_.retired;
                    }
                }

                bool Finalize() noexcept override {
                    if (!retired_ || !host_.drainAllowed)
                        return false;
                    ++host_.finalized;
                    return true;
                }

            private:
                TestRoleHost &host_;
                Extensions::ExtensionActivationLease proof_;
                bool retired_{};
            };
        };

        /** @brief Selects a role explicitly declared in the same verified fixture archive. */
        EnabledPackageModule SelectedRole(const Fixture &fixture) {
            return {fixture.graph.package, Parse<PackagePath>("extensions/native/extension.json"), "com.horo.application-fixture.native",
                    Extensions::ExtensionModuleRole::BackendCapability};
        }

        /** @brief Advances install work while retaining a running product boundary. */
        PackageOperationSnapshot WaitForActivation(Fixture &fixture, PackageOperationId operation) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < deadline) {
                REQUIRE(fixture.application->Pump(PackageActivationBoundary::Running).HasValue());
                auto snapshot = fixture.application->Query(operation);
                REQUIRE(snapshot.HasValue());
                if (snapshot.Value().phase == PackageOperationPhase::Activating ||
                    snapshot.Value().outcome != PackageOperationOutcome::Running)
                    return std::move(snapshot).Value();
                std::this_thread::yield();
            }
            FAIL("Restore and install did not reach the activation boundary.");
            return {};
        }

        TEST_CASE("Package application creation and plan projection do not restore install or approve execution",
                  "[packages][application][ownership]") {
            Fixture fixture;
            auto current = fixture.application->Current();
            REQUIRE(current.HasValue());
            REQUIRE_FALSE(current.Value());
            auto plan = fixture.application->Plan();
            REQUIRE(plan.HasValue());
            REQUIRE(plan.Value()->packages.size() == 1);
            CHECK(plan.Value()->packages.front().verification == PackagePlanVerification::LockedOnly);
            CHECK(fixture.source.calls.load() == 0);
            CHECK_FALSE(fixture.install.InstalledRecord());
            CHECK_FALSE(fixture.lifecycle->Active());
            CHECK(fixture.trust.calls == 0);
        }

        TEST_CASE("Package application restores exact locked bytes without installing or executing them",
                  "[packages][application][restore]") {
            Fixture fixture;
            auto started = fixture.application->Restore();
            REQUIRE(started.HasValue());
            const auto result = WaitForTerminal(fixture, started.Value());
            REQUIRE(result.outcome == PackageOperationOutcome::Succeeded);
            CHECK(result.kind == PackageOperationKind::Restore);
            CHECK(result.completedPackages == 1);
            CHECK_FALSE(result.installedRevision);
            CHECK_FALSE(result.activeGeneration);
            CHECK(fixture.source.calls.load() == 1);
            CHECK_FALSE(fixture.install.InstalledRecord());
            CHECK_FALSE(fixture.lifecycle->Active());
            const auto plan = fixture.application->Plan();
            REQUIRE(plan.HasValue());
            REQUIRE(plan.Value()->packages.size() == 1);
            CHECK(plan.Value()->packages.front().verification == PackagePlanVerification::ArchiveVerified);
            CHECK(fixture.trust.calls == 0);
        }

        TEST_CASE("Package plans report every direct dependent before and after verifying the archives", "[packages][application][plan]") {
            Fixture fixture;
            const auto &asset = fixture.graph;
            const auto first = Parse<HoroPackageId>("com.horo.a-dependent");
            const auto last = Parse<HoroPackageId>("com.horo.z-dependent");
            const auto &provider = asset.lock->Packages().front();
            const PackageResolutionPlan plan{
                .packages = {{last, provider.version, provider.source, provider.artifactDigest, {provider.package}},
                             {provider.package, provider.version, provider.source, provider.artifactDigest, {}},
                             {first, provider.version, provider.source, provider.artifactDigest, {provider.package}}}};
            const std::array roots{last, first};
            const std::array artifacts{PackageLockArtifact{first, provider.version, provider.source, provider.artifactDigest,
                                                           provider.manifestDigest, provider.fileManifestDigest},
                                       PackageLockArtifact{provider.package, provider.version, provider.source, provider.artifactDigest,
                                                           provider.manifestDigest, provider.fileManifestDigest},
                                       PackageLockArtifact{last, provider.version, provider.source, provider.artifactDigest,
                                                           provider.manifestDigest, provider.fileManifestDigest}};
            auto lock = ValidatedPackageLockfileV1::Generate(plan, roots, asset.requestHash, artifacts);
            REQUIRE(lock.HasValue());
            auto configuration = fixture.Configuration();
            configuration.restore.lockfileJson = lock.Value().SerializeCanonical();
            configuration.lockedPlan = std::move(lock).Value();
            auto application =
                PackageApplication::Create(fixture.jobs, *fixture.restore, fixture.install, *fixture.lifecycle, std::move(configuration));
            REQUIRE(application.HasValue());
            fixture.application = std::move(application).Value();

            const auto check = [&](const PackagePlanVerification verification) {
                const auto visible = fixture.application->Plan();
                REQUIRE(visible.HasValue());
                const auto &packages = visible.Value()->packages;
                REQUIRE(packages.size() == 3);
                CHECK(packages[0].requiredBy.empty());
                CHECK(packages[2].requiredBy.empty());
                REQUIRE(packages[1].locked.package == provider.package);
                CHECK(packages[1].verification == verification);
                CHECK(packages[1].requiredBy == std::vector<LockedPackageReference>{{first, provider.version}, {last, provider.version}});
            };
            check(PackagePlanVerification::LockedOnly);
            const auto preview = fixture.application->PreviewRemoval(first);
            REQUIRE(preview.HasValue());
            CHECK(preview.Value().removed == std::vector<LockedPackageReference>{{first, provider.version}});
            CHECK(fixture.source.calls.load() == 0);
            const auto restore = fixture.application->Restore();
            REQUIRE(restore.HasValue());
            REQUIRE(WaitForTerminal(fixture, restore.Value()).outcome == PackageOperationOutcome::Succeeded);
            check(PackagePlanVerification::ArchiveVerified);
            CHECK_FALSE(fixture.install.InstalledRecord());
            fixture.admissionAllowed = false;
            const auto stale = fixture.application->PreviewRemoval(first);
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == "fixture.metadata.stale");
        }

        TEST_CASE("Package application serializes mutations and fences replaced operation identities",
                  "[packages][application][generation]") {
            Fixture fixture;
            const auto installed = fixture.application->Install();
            REQUIRE(installed.HasValue());
            const auto busy = fixture.application->Restore();
            REQUIRE(busy.HasError());
            CHECK(busy.ErrorValue().code.Value() == "packages.application.busy");
            const auto result = WaitForTerminal(fixture, installed.Value());
            REQUIRE(result.outcome == PackageOperationOutcome::Succeeded);
            const auto record = fixture.install.InstalledRecord();
            REQUIRE(record);
            REQUIRE(result.installedRevision);
            CHECK(*result.installedRevision == record->Revision());
            CHECK_FALSE(result.activeGeneration);
            CHECK_FALSE(fixture.lifecycle->Active());
            const auto replacement = fixture.application->Restore();
            REQUIRE(replacement.HasValue());
            CHECK(replacement.Value() != installed.Value());
            const auto stale = fixture.application->Query(installed.Value());
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == "packages.application.stale_operation");
            const auto cancelled = fixture.application->RequestCancel(installed.Value());
            REQUIRE(cancelled.HasError());
            CHECK(cancelled.ErrorValue().code.Value() == "packages.application.stale_operation");
            CHECK(WaitForTerminal(fixture, replacement.Value()).outcome == PackageOperationOutcome::Succeeded);
        }

        TEST_CASE("Package application rejects changed metadata before restored bytes become installed availability",
                  "[packages][application][metadata]") {
            Fixture fixture;
            fixture.source.block.store(true);
            const auto started = fixture.application->Install();
            REQUIRE(started.HasValue());
            REQUIRE(fixture.source.entered.try_acquire_for(std::chrono::seconds{5}));
            fixture.admissionAllowed = false;
            fixture.source.Unblock();
            const auto result = WaitForTerminal(fixture, started.Value());
            REQUIRE(result.outcome == PackageOperationOutcome::Failed);
            REQUIRE(result.diagnostic);
            CHECK(result.diagnostic->code.Value() == "fixture.metadata.stale");
            CHECK_FALSE(result.installedRevision);
            CHECK_FALSE(fixture.install.InstalledRecord());
            CHECK_FALSE(fixture.lifecycle->Active());
            CHECK(fixture.trust.calls == 0);
        }

        TEST_CASE("Package application keeps committed availability when safe point activation loses metadata admission",
                  "[packages][application][activation]") {
            Fixture fixture;
            const auto installed = fixture.application->Install();
            REQUIRE(installed.HasValue());
            const auto available = WaitForTerminal(fixture, installed.Value());
            REQUIRE(available.outcome == PackageOperationOutcome::Succeeded);
            const std::array enabled{
                EnabledPackageExtension{fixture.graph.package, Parse<PackagePath>("extensions/native/extension.json")}};
            const auto activated = fixture.application->Activate(enabled);
            REQUIRE(activated.HasValue());
            fixture.admissionAllowed = false;
            REQUIRE(fixture.application->Pump(PackageActivationBoundary::Running).HasValue());
            REQUIRE(fixture.application->Query(activated.Value()).Value().outcome == PackageOperationOutcome::Running);
            const auto result = WaitForTerminal(fixture, activated.Value());
            REQUIRE(result.outcome == PackageOperationOutcome::Failed);
            REQUIRE(result.diagnostic);
            CHECK(result.diagnostic->code.Value() == "fixture.metadata.stale");
            CHECK(result.installedRevision == available.installedRevision);
            CHECK_FALSE(result.activeGeneration);
            CHECK(fixture.install.InstalledRecord()->Revision() == *available.installedRevision);
            CHECK_FALSE(fixture.lifecycle->Active());
            CHECK(fixture.trust.calls == 0);
        }

        TEST_CASE("Package application exposes only current verified execution trust denial evidence",
                  "[packages][application][activation]") {
            Fixture fixture{true};
            const auto installed = fixture.application->Install();
            REQUIRE(installed.HasValue());
            REQUIRE(WaitForTerminal(fixture, installed.Value()).outcome == PackageOperationOutcome::Succeeded);
            CHECK_FALSE(fixture.application->ActivationTrustRequired().Value());
            const std::array enabled{
                EnabledPackageExtension{fixture.graph.package, Parse<PackagePath>("extensions/native/extension.json")}};
            const auto pending = fixture.application->Activate(enabled);
            REQUIRE(pending.HasValue());
            CHECK_FALSE(fixture.application->ActivationTrustRequired().Value());
            const auto outcome = WaitForTerminal(fixture, pending.Value());
            REQUIRE(outcome.outcome == PackageOperationOutcome::Failed);
            REQUIRE(outcome.diagnostic);
            CHECK(outcome.diagnostic->code.Value() == PackageLifecycleErrors::TrustRequired.code.Value());
            CHECK(fixture.trust.calls == 1);
            CHECK(fixture.application->ActivationTrustRequired().Value());
            CHECK_FALSE(fixture.lifecycle->Active());

            const auto replacement = fixture.application->Restore();
            REQUIRE(replacement.HasValue());
            CHECK_FALSE(fixture.application->ActivationTrustRequired().Value());
            CHECK(WaitForTerminal(fixture, replacement.Value()).outcome == PackageOperationOutcome::Succeeded);
            CHECK_FALSE(fixture.application->ActivationTrustRequired().Value());
        }

        TEST_CASE("Package application commits role-aware selections at its real boundary and retires them through Disable",
                  "[packages][application][activation][modules]") {
            ExactRoleTrust trust;
            auto host = std::make_shared<TestRoleHost>();
            Fixture fixture{true, host, &trust};
            auto selected = SelectedRole(fixture);
            const auto started = fixture.application->ActivateModules(std::span{&selected, 1});
            REQUIRE(started.HasValue());
            selected.module.clear();
            const auto queued = WaitForActivation(fixture, started.Value());
            REQUIRE(queued.phase == PackageOperationPhase::Activating);
            CHECK(queued.outcome == PackageOperationOutcome::Running);
            CHECK(queued.installedRevision);
            CHECK_FALSE(queued.attemptedActivationGeneration);
            CHECK(queued.moduleReports.empty());
            CHECK(trust.calls == 0);
            CHECK(host->activated == 0);
            bool reentryRejected{};
            host->onActivate = [&] {
                const auto nested = fixture.application->Pump(PackageActivationBoundary::Quiescent);
                reentryRejected = nested.HasError() && nested.ErrorValue().code.Value() == "packages.application.busy";
            };
            const auto result = WaitForTerminal(fixture, started.Value());
            REQUIRE(result.outcome == PackageOperationOutcome::Succeeded);
            REQUIRE(result.attemptedActivationGeneration);
            CHECK(result.attemptedActivationGeneration == result.activeGeneration);
            REQUIRE(result.moduleReports.size() == 1);
            CHECK(result.moduleReports[0].module == SelectedRole(fixture).module);
            CHECK(result.moduleReports[0].host == host->Identity());
            CHECK(result.moduleReports[0].outcome == PackageModuleOutcome::Active);
            CHECK(host->activated == 1);
            CHECK(reentryRejected);
            CHECK(trust.calls == 1);

            auto missing = SelectedRole(fixture);
            missing.module = "com.horo.application-fixture.missing";
            const auto rejected = fixture.application->ActivateModules(std::span{&missing, 1});
            REQUIRE(rejected.HasValue());
            const auto failure = WaitForTerminal(fixture, rejected.Value());
            CHECK(failure.outcome == PackageOperationOutcome::Failed);
            CHECK(failure.activeGeneration == result.activeGeneration);
            CHECK(failure.attemptedActivationGeneration > failure.activeGeneration);
            CHECK(failure.moduleReports.empty());
            CHECK(host->retired == 0);

            const auto disable = fixture.application->Disable();
            REQUIRE(disable.HasValue());
            REQUIRE(fixture.application->Pump(PackageActivationBoundary::Running).HasValue());
            CHECK(host->retired == 0);
            const auto disabled = WaitForTerminal(fixture, disable.Value());
            CHECK(disabled.kind == PackageOperationKind::Disable);
            REQUIRE(disabled.outcome == PackageOperationOutcome::Succeeded);
            CHECK(disabled.installedRevision == result.installedRevision);
            CHECK(disabled.activeGeneration > result.activeGeneration);
            CHECK(disabled.moduleReports.empty());
            CHECK(fixture.lifecycle->Active()->modules.empty());
            CHECK(host->retired == 1);
            CHECK(host->finalized == 1);
        }

        TEST_CASE("Package installation keeps active and draining package owners in the committed graph",
                  "[packages][application][removal]") {
            ExactRoleTrust trust;
            auto host = std::make_shared<TestRoleHost>();
            Fixture fixture{true, host, &trust};

            struct AllowDrainOnExit {
                TestRoleHost &host;

                ~AllowDrainOnExit() {
                    host.drainAllowed = true;
                }
            } allowDrain{*host};

            const auto selected = SelectedRole(fixture);
            const auto activated = fixture.application->ActivateModules(std::span{&selected, 1});
            REQUIRE(activated.HasValue());
            REQUIRE(WaitForTerminal(fixture, activated.Value()).outcome == PackageOperationOutcome::Succeeded);
            const auto previous = fixture.install.InstalledRecord();
            REQUIRE(previous);

            const auto replacement = Parse<HoroPackageId>("com.horo.replacement");
            const auto &original = fixture.graph.lock->Packages().front();
            const PackageResolutionPlan plan{.packages = {{replacement, original.version, original.source, original.artifactDigest, {}}}};
            const PackageLockArtifact artifact{replacement,
                                               original.version,
                                               original.source,
                                               original.artifactDigest,
                                               original.manifestDigest,
                                               original.fileManifestDigest};
            auto configuration = fixture.Configuration();
            configuration.restore.expectedRequestHash = ComputeSha256(std::as_bytes(std::span{std::string_view{"replacement-request"}}));
            auto lock = ValidatedPackageLockfileV1::Generate(plan, std::span{&replacement, 1}, configuration.restore.expectedRequestHash,
                                                             std::span{&artifact, 1});
            REQUIRE(lock.HasValue());
            configuration.restore.lockfileJson = lock.Value().SerializeCanonical();
            configuration.lockedPlan = std::move(lock).Value();
            auto rebound =
                PackageApplication::Create(fixture.jobs, *fixture.restore, fixture.install, *fixture.lifecycle, std::move(configuration));
            REQUIRE(rebound.HasValue());
            fixture.application = std::move(rebound).Value();

            const auto tryInstall = [&] {
                const auto started = fixture.application->Install();
                REQUIRE(started.HasValue());
                return WaitForTerminal(fixture, started.Value());
            };
            const auto checkBlocked = [&](const PackageOperationSnapshot &result) {
                CHECK(result.outcome == PackageOperationOutcome::Failed);
                REQUIRE(result.diagnostic);
                CHECK(result.diagnostic->code.Value() == PackageLifecycleErrors::PackageInUse.code.Value());
                CHECK(result.diagnostic->message == "Packages still in use: " + selected.package.Value());
                CHECK(fixture.install.InstalledRecord() == previous);
            };
            checkBlocked(tryInstall());
            host->drainAllowed = false;
            const auto disabled = fixture.application->Disable();
            REQUIRE(disabled.HasValue());
            REQUIRE(WaitForTerminal(fixture, disabled.Value()).outcome == PackageOperationOutcome::Succeeded);
            CHECK(host->retired == 1);
            CHECK(host->finalized == 0);
            checkBlocked(tryInstall());

            host->drainAllowed = true;
            fixture.lifecycle->FinalizeRetirements();
            REQUIRE(tryInstall().outcome == PackageOperationOutcome::Succeeded);
            REQUIRE(fixture.install.InstalledRecord() != previous);
            CHECK(fixture.install.ActiveGraph()->packages.front().lock.package == replacement);
        }

        TEST_CASE("Package application reports role trust decisions without reusing stale attempt evidence",
                  "[packages][application][activation][modules]") {
            auto host = std::make_shared<TestRoleHost>();
            Fixture fixture{true, host};
            auto selected = SelectedRole(fixture);
            SECTION("required trust denial rejects the composition") {}
            SECTION("optional trust denial remains a visible independent outcome") {
                selected.requirement = PackageModuleRequirement::Optional;
            }
            const auto started = fixture.application->ActivateModules(std::span{&selected, 1});
            REQUIRE(started.HasValue());
            const auto result = WaitForTerminal(fixture, started.Value());
            const bool optional = selected.requirement == PackageModuleRequirement::Optional;
            CHECK(result.outcome == (optional ? PackageOperationOutcome::Succeeded : PackageOperationOutcome::Failed));
            CHECK(result.activeGeneration.has_value() == optional);
            REQUIRE(result.attemptedActivationGeneration);
            REQUIRE(result.moduleReports.size() == 1);
            const auto &report = result.moduleReports[0];
            CHECK(report.generation == *result.attemptedActivationGeneration);
            CHECK(report.outcome == PackageModuleOutcome::Denied);
            REQUIRE(report.diagnostic);
            CHECK(report.diagnostic->code.Value() == PackageLifecycleErrors::TrustRequired.code.Value());
            CHECK(fixture.application->ActivationTrustRequired().Value());
            CHECK(host->activated == 0);

            const auto pending = fixture.application->ActivateModules(std::span{&selected, 1});
            REQUIRE(pending.HasValue());
            REQUIRE(fixture.application->RequestCancel(pending.Value()).HasValue());
            const auto cancelled = WaitForTerminal(fixture, pending.Value());
            CHECK(cancelled.outcome == PackageOperationOutcome::Cancelled);
            CHECK_FALSE(cancelled.attemptedActivationGeneration);
            CHECK(cancelled.moduleReports.empty());
            CHECK_FALSE(fixture.application->ActivationTrustRequired().Value());
            CHECK(fixture.trust.calls == 1);
        }

        TEST_CASE("Package application Close retains blocked work and leaves borrowed authorities open after cancellation",
                  "[packages][application][shutdown]") {
            Fixture fixture;
            fixture.source.block.store(true);
            const auto started = fixture.application->Install();
            REQUIRE(started.HasValue());
            REQUIRE(fixture.source.entered.try_acquire_for(std::chrono::seconds{5}));
            const auto closing = fixture.application->Close();
            REQUIRE(closing.HasError());
            CHECK(closing.ErrorValue().code.Value() == "packages.application.busy");
            const auto rejected = fixture.application->Restore();
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == "packages.application.closed");
            fixture.source.Unblock();
            CHECK(WaitForTerminal(fixture, started.Value()).outcome == PackageOperationOutcome::Cancelled);
            REQUIRE(fixture.application->Close().HasValue());
            const auto closed = fixture.application->Pump(PackageActivationBoundary::Quiescent);
            REQUIRE(closed.HasError());
            CHECK(closed.ErrorValue().code.Value() == "packages.application.closed");
            CHECK_FALSE(fixture.install.InstalledRecord());
            auto restored = fixture.restore->Start(fixture.Configuration().restore);
            REQUIRE(restored.HasValue());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            std::optional<PackageRestoreProgressSnapshot> snapshot;
            do {
                snapshot = fixture.restore->Query(restored.Value().Id());
                REQUIRE(snapshot);
                if (snapshot->outcome != PackageRestoreOutcome::Running)
                    break;
                std::this_thread::yield();
            } while (std::chrono::steady_clock::now() < deadline);
            CHECK(snapshot->outcome == PackageRestoreOutcome::Ready);
        }

        TEST_CASE("Package application preserves a failed source cause without publishing installation",
                  "[packages][application][errors]") {
            Fixture fixture;
            fixture.source.failFetch = true;
            const auto started = fixture.application->Restore();
            REQUIRE(started.HasValue());
            const auto result = WaitForTerminal(fixture, started.Value());
            REQUIRE(result.outcome == PackageOperationOutcome::Failed);
            REQUIRE(result.diagnostic);
            bool found = false;
            for (const Error *error = &*result.diagnostic; error != nullptr; error = error->cause.Get())
                found |= error->code.Value() == "fixture.source.unavailable";
            CHECK(found);
            CHECK_FALSE(result.installedRevision);
            CHECK_FALSE(result.activeGeneration);
        }

        TEST_CASE("Package application rejects foreign lane work before touching borrowed authorities",
                  "[packages][application][ownership]") {
            Fixture fixture;
            std::optional<Result<void>> pumped;
            std::optional<Result<std::optional<PackageOperationSnapshot>>> queried;
            std::jthread worker([&] {
                pumped = fixture.application->Pump(PackageActivationBoundary::Quiescent);
                queried = fixture.application->Current();
            });
            worker.join();
            REQUIRE(pumped);
            REQUIRE(pumped->HasError());
            CHECK(pumped->ErrorValue().code.Value() == "packages.application.owner_lane");
            REQUIRE(queried);
            REQUIRE(queried->HasError());
            CHECK(queried->ErrorValue().code.Value() == "packages.application.owner_lane");
            CHECK(fixture.source.calls.load() == 0);
            CHECK_FALSE(fixture.install.InstalledRecord());
        }

        TEST_CASE("Package application rejects mismatched project authority composition without side effects",
                  "[packages][application][composition]") {
            Fixture fixture;
            auto configuration = fixture.Configuration();
            configuration.restore.projectRoot = std::filesystem::canonical(fixture.content.Path());
            const auto mismatched =
                PackageApplication::Create(fixture.jobs, *fixture.restore, fixture.install, *fixture.lifecycle, std::move(configuration));
            REQUIRE(mismatched.HasError());
            CHECK(mismatched.ErrorValue().code.Value() == "packages.application.invalid_input");
            CHECK(fixture.source.calls.load() == 0);
            CHECK_FALSE(fixture.install.InstalledRecord());
        }

        TEST_CASE("Package application disabling an empty composition is idempotent and does not restore",
                  "[packages][application][disable]") {
            Fixture fixture;
            for (int iteration = 0; iteration < 2; ++iteration) {
                const auto disabled = fixture.application->Disable();
                REQUIRE(disabled.HasValue());
                const auto result = WaitForTerminal(fixture, disabled.Value());
                REQUIRE(result.outcome == PackageOperationOutcome::Succeeded);
                CHECK(result.kind == PackageOperationKind::Disable);
                CHECK_FALSE(result.activeGeneration);
            }
            CHECK(fixture.source.calls.load() == 0);
            CHECK(fixture.trust.calls == 0);
        }
    }  // namespace
}  // namespace Horo::Tests::PackageApplicationService
