#include "PackageApplicationServiceTestSupport.h"

#include <array>

namespace Horo::Tests::PackageApplicationService {
    namespace {
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
