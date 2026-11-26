#include "../unit/packages/PackageApplicationServiceTestSupport.h"
#include "EditorUiTestHarness.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Packages/PackageRequest.h"
#include "editor/modals/packages/PackageModal.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <imgui_internal.h>
#include <imgui_test_engine/imgui_te_context.h>
#include <imgui_test_engine/imgui_te_engine.h>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Editor;

    struct PackageUi final {
        Horo::Tests::EditorUiTestHarness harness{Horo::Tests::CreateHeadlessEditorUiTestSurface()};
        Horo::Tests::PackageApplicationService::Fixture application;
        EngineDataBus engineEvents;
        EditorDataBus events;
        LocalizationService localization{LocaleTag{"en-US"}};
        Theme::Fonts fonts{ImGui::GetIO().FontDefault, ImGui::GetIO().FontDefault, ImGui::GetIO().FontDefault, ImGui::GetIO().FontDefault};
        ThemeContext theme{fonts};
        EditorSettingsSnapshot settings;
        EditorGuiContext context{engineEvents, events, localization, theme, settings};
        Input::InputRouter input;
        EditorModalHost modals{events, input};
        bool unavailable{};
        bool missingFactory{};
        bool removalReview{};
        unsigned acquisitions{};
        std::optional<PackageOperationSnapshot> operation;

        explicit PackageUi(const bool unavailableInitially = false, const char *locale = "en-US", const bool noFactory = false,
                           const bool reviewRemoval = false)
            : unavailable(unavailableInitially), missingFactory(noFactory), removalReview(reviewRemoval) {
            LocalizationError error;
            REQUIRE(localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                     (std::string{locale} + ".json"),
                                                 &error));
            REQUIRE(localization.Prepare(LocaleTag{locale}, &error));
            REQUIRE(localization.ActivatePrepared(&error));
            if (removalReview)
                PrepareRemoval();
            Open();
        }

        /** @brief Supplies a locked asset package and a bounded worker reporting authored removal blockers. */
        void PrepareRemoval() {
            using namespace Packages;
            const auto &graph = application.graph;
            auto request = ValidatedPackageRequest::Parse(R"({
                "sources":{"fixture.source":{"kind":"public-registry","registry":"official"}},
                "dependencies":{"com.horo.application-fixture":{"source":"fixture.source","version":"1.0.0"}}
            })");
            REQUIRE(request.HasValue());
            const PackageResolutionPlan plan{.packages = {{graph.package, graph.version, graph.source, graph.archive.Digest(), {}}}};
            const PackageLockArtifact artifact{.package = graph.package,
                                               .version = graph.version,
                                               .source = graph.source,
                                               .artifactDigest = graph.archive.Digest(),
                                               .manifestDigest = graph.archive.PackageManifestDigest(),
                                               .fileManifestDigest = graph.archive.Manifest().Digest(),
                                               .contributions = {"assets"}};
            auto lock = ValidatedPackageLockfileV1::Generate(plan, std::span{&graph.package, 1}, request.Value().Digest(),
                                                             std::span{&artifact, 1});
            REQUIRE(lock.HasValue());
            auto config = application.Configuration();
            config.restore.expectedRequestHash = request.Value().Digest();
            config.restore.lockfileJson = lock.Value().SerializeCanonical();
            config.lockedPlan = std::move(lock).Value();
            config.portableRequest = std::move(request).Value();
            config.prepareRemoval = [](const HoroPackageId &, std::shared_ptr<const PackageRemovalLease> reservation) {
                return Result<PackageRemovalWork>::Success([held = std::move(reservation)](const CancellationToken &) {
                    if (!held)
                        return Result<void>::Failure(Error{ErrorCode{"fixture.package.no_reservation"}, ErrorDomainId{"fixture.package"}});
                    std::vector<Diagnostic> references;
                    for (unsigned attachment = 1; attachment <= 32; ++attachment)
                        references.emplace_back(DiagnosticCode{"packages.host.behavior_in_use"}, DiagnosticSeverity::Error,
                                                 "com.horo.application-fixture", SourceLocation{"Assets/Scene.horoasset"},
                                                 "7/" + std::to_string(attachment));
                    references.emplace_back(DiagnosticCode{"packages.host.decision_subplan_missing"}, DiagnosticSeverity::Error,
                                             "770001", SourceLocation{"Assets/Guard.decision.json"}, "920003");
                    return Result<void>::Failure(Error{ErrorCode{"packages.host.behavior_in_use"}, ErrorDomainId{"packages.host"},
                                                       ErrorSeverity::Error, "", std::move(references)});
                });
            };
            REQUIRE(application.application->Close().HasValue());
            auto created = PackageApplication::Create(application.jobs, *application.restore, application.install,
                                                       *application.lifecycle, std::move(config));
            REQUIRE(created.HasValue());
            application.application = std::move(created).Value();
        }

        void Open() {
            std::function<Result<std::shared_ptr<PackageApplication>>()> acquire;
            if (!missingFactory)
                acquire = [this] {
                    ++acquisitions;
                    if (unavailable)
                        return Result<std::shared_ptr<PackageApplication>>::Failure(
                            Error{ErrorCode{"fixture.package.preparing"}, ErrorDomainId{"fixture.package"}});
                    return Result<std::shared_ptr<PackageApplication>>::Success({application.application.get(), [](PackageApplication *) {
                        // The enclosing fixture retains ownership until the modal and its callbacks have retired.
                    }});
                };
            auto modal = std::make_unique<PackageModal>(context, std::move(acquire), false, [this] { return removalReview; });
            REQUIRE(modals.OpenRoot(std::move(modal)).HasValue());
        }

        void Frame() {
            // Only the real owner/UI thread drives authority state. The test coroutine performs UI gestures.
            REQUIRE(application.application->Pump(Packages::PackageActivationBoundary::Quiescent).HasValue());
            modals.OnUpdate(1.0F / 60.0F);
            ImGui::Begin("Package test host", nullptr, ImGuiWindowFlags_NoSavedSettings);
            if (ImGui::Button("Open packages") && !modals.HasOpenModal())
                Open();
            ImGui::End();
            modals.Draw();
            auto current = application.application->Current();
            REQUIRE(current.HasValue());
            operation = current.Value();
        }

        ~PackageUi() {
            application.source.Unblock();
            static_cast<void>(modals.RequestCloseAllForShutdown());
        }
    };

    /** @brief Delays a real restore source past the former frame cutoff; PackageUi owns teardown and failure-path release. */
    struct DelayedRestoreSource final {
        static constexpr unsigned kBlockedFrames = 350;
        Horo::Tests::PackageApplicationService::ControlledSource &source;
        unsigned blockedFrames{};
        bool entered{};

        explicit DelayedRestoreSource(Horo::Tests::PackageApplicationService::ControlledSource &value) : source(value) {
            source.block.store(true);
        }

        void OnFrame() {
            if (!entered)
                entered = source.entered.try_acquire();
            if (!entered || blockedFrames >= kBlockedFrames)
                return;
            ++blockedFrames;
            if (blockedFrames == kBlockedFrames)
                source.Unblock();
        }
    };

    void Click(ImGuiTestContext &ui, const char *label) {
        const std::string path = std::string{"//**/"} + label;
        ui.ItemClick(path.c_str());
    }

    /** @brief Records owner snapshots only on a failed bounded handoff, without waiting on worker state. */
    void LogAwaitFailure(ImGuiTestContext &ui, const PackageUi &fixture, const PackageOperationKind kind, const unsigned polls,
                         const std::chrono::steady_clock::time_point started) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        ui.LogInfo("Package handoff failed: expectedKind=%u polls=%u elapsedSeconds=%.6f sourceCalls=%llu operationPresent=%d",
                   static_cast<unsigned>(kind), polls, elapsed, static_cast<unsigned long long>(fixture.application.source.calls.load()),
                   fixture.operation.has_value());
        if (fixture.operation) {
            const auto &operation = *fixture.operation;
            ui.LogInfo("Package operation: id=%llu kind=%u outcome=%u phase=%u completion=%.6f packages=%llu/%llu",
                       static_cast<unsigned long long>(operation.id.value), static_cast<unsigned>(operation.kind),
                       static_cast<unsigned>(operation.outcome), static_cast<unsigned>(operation.phase), operation.completion,
                       static_cast<unsigned long long>(operation.completedPackages),
                       static_cast<unsigned long long>(operation.totalPackages));
            if (operation.diagnostic)
                ui.LogInfo("Package diagnostic: %s:%s %s", operation.diagnostic->domain.Value().c_str(),
                           operation.diagnostic->code.Value().c_str(), operation.diagnostic->message.c_str());
        }
        if (const auto jobs = fixture.application.jobs.SnapshotIfChanged(0)) {
            ui.LogInfo("Package retained jobs: count=%llu revision=%llu", static_cast<unsigned long long>(jobs->jobs.size()),
                       static_cast<unsigned long long>(jobs->revision));
            for (const auto &job : jobs->jobs)
                ui.LogInfo("Package job: id=%llu state=%u started=%d finished=%d", static_cast<unsigned long long>(job.id),
                           static_cast<unsigned>(job.state), job.timing.startedAt.has_value(), job.timing.finishedAt.has_value());
        }
    }

    // Leaves room for the 350-frame delayed restore and subsequent real operations, including paced native UI frames.
    constexpr std::size_t kPackageAsyncFrameBudget = 5000;

    /** @brief Advances owner frames while giving independently scheduled package work a bounded wall-time completion window. */
    bool AwaitCondition(ImGuiTestContext &ui, const auto &ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (std::chrono::steady_clock::now() < deadline) {
            if (ready())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
            ui.Yield();
        }
        return ready();
    }

    /** @brief Waits for the exact operation's owner-published terminal snapshot. */
    bool AwaitTerminal(ImGuiTestContext &ui, const PackageUi &fixture, const PackageOperationKind kind) {
        const auto started = std::chrono::steady_clock::now();
        unsigned polls = 0;
        const bool terminal = AwaitCondition(ui, [&fixture, kind, &polls] {
            ++polls;
            return fixture.operation && fixture.operation->kind == kind && fixture.operation->outcome != PackageOperationOutcome::Running;
        });
        if (!terminal)
            LogAwaitFailure(ui, fixture, kind, polls, started);
        return terminal;
    }

    void CheckRun(const Horo::Tests::EditorUiScenarioResult &result) {
        INFO(result.testEngineLog);
        REQUIRE(result.Succeeded());
        CHECK_FALSE(result.frameBudgetExceeded);
    }

    /** @brief Exercises real install controls, denied admission and descriptor rejection after restore. */
    void RunInstallAndSelection(ImGuiTestContext *ctx, PackageUi &fixture) {
        ctx->Yield(2);
        IM_CHECK(!fixture.operation.has_value());
        Click(*ctx, "Restore locked packages");
        IM_CHECK(AwaitTerminal(*ctx, fixture, PackageOperationKind::Restore));
        IM_CHECK(fixture.operation->outcome == PackageOperationOutcome::Succeeded);
        Click(*ctx, "Install verified packages");
        IM_CHECK(AwaitTerminal(*ctx, fixture, PackageOperationKind::Install));
        IM_CHECK(fixture.operation->installedRevision.has_value());
        const auto installedOperation = fixture.operation->id;
        const auto sourceCalls = fixture.application.source.calls.load();
        fixture.application.admissionAllowed = false;
        Click(*ctx, "Install verified packages");
        ctx->Yield(2);
        IM_CHECK(fixture.operation->id == installedOperation);
        IM_CHECK_EQ(fixture.application.source.calls.load(), sourceCalls);
        fixture.application.admissionAllowed = true;
        ctx->ItemInputValue("//**/##PackageIdentity", "INVALID PACKAGE");
        ctx->ItemInputValue("//**/##PackageDescriptor", "extensions/native/extension.json");
        Click(*ctx, "Activate explicit selection");
        ctx->Yield(2);
        IM_CHECK(fixture.operation->kind == PackageOperationKind::Install);
        ctx->ItemInputValue("//**/##PackageIdentity", "com.horo.application-fixture");
        ctx->ItemInputValue("//**/##PackageDescriptor", "../outside.json");
        Click(*ctx, "Activate explicit selection");
        ctx->Yield(2);
        IM_CHECK(fixture.operation->kind == PackageOperationKind::Install);
        ctx->ItemInputValue("//**/##PackageDescriptor", "extensions/native/extension.json");
        Click(*ctx, "Activate explicit selection");
        IM_CHECK(AwaitTerminal(*ctx, fixture, PackageOperationKind::Activate));
        IM_CHECK(fixture.operation->outcome == PackageOperationOutcome::Failed);
        Click(*ctx, "Disable package extensions");
        IM_CHECK(AwaitTerminal(*ctx, fixture, PackageOperationKind::Disable));
        IM_CHECK(fixture.operation->outcome == PackageOperationOutcome::Succeeded);
    }

    /** @brief Retains the running restore across modal reopening until its worker acknowledges cancellation. */
    void RunCloseReopenCancel(ImGuiTestContext *ctx, PackageUi &fixture, PackageOperationId &original) {
        ctx->Yield(2);
        Click(*ctx, "Restore locked packages");
        IM_CHECK(AwaitCondition(*ctx, [&fixture] {
            return fixture.application.source.entered.try_acquire();
        }));
        IM_CHECK_EQ(fixture.application.source.calls.load(), 1U);
        IM_CHECK(fixture.operation.has_value());
        IM_CHECK(fixture.operation->outcome == PackageOperationOutcome::Running);
        original = fixture.operation->id;
        Click(*ctx, "Refresh project package session");
        ctx->Yield(2);
        IM_CHECK(fixture.operation->id == original);
        IM_CHECK(fixture.operation->outcome == PackageOperationOutcome::Running);
        Click(*ctx, "Close");
        ctx->Yield(2);
        IM_CHECK(fixture.operation->id == original);
        IM_CHECK(fixture.operation->outcome == PackageOperationOutcome::Running);
        Click(*ctx, "Open packages");
        ctx->Yield(2);
        IM_CHECK(fixture.operation->id == original);
        Click(*ctx, "Request cancellation");
        fixture.application.source.Unblock();
        IM_CHECK(AwaitTerminal(*ctx, fixture, PackageOperationKind::Restore));
        IM_CHECK(fixture.operation->outcome == PackageOperationOutcome::Cancelled);
    }

    TEST_CASE("Unconfigured package modal remains inert and closable", "[ui][imgui][editor][packages]") {
        PackageUi fixture{false, "en-US", true};
        const auto result = fixture.harness.Run("horo_packages", "no_host", [&fixture](ImGuiTestContext *) {
            fixture.Frame();
        }, [&fixture](ImGuiTestContext *ctx) {
            ctx->Yield(3);
            IM_CHECK(!fixture.operation.has_value());
            Click(*ctx, "Close");
        });
        CheckRun(result);
        CHECK(fixture.acquisitions == 0);
        CHECK(fixture.application.source.calls.load() == 0);
        CHECK_FALSE(fixture.modals.HasOpenModal());
    }

    TEST_CASE("Narrow Turkish package evidence drawing grants no operation or native trust", "[ui][imgui][editor][packages]") {
        PackageUi fixture{false, "tr-TR"};
        ImGui::GetIO().DisplaySize = {420, 800};
        int modalVertices = 0;
        float modalViewportWidth = 0;
        const auto result = fixture.harness.Run("horo_packages", "localized_inert_plan",
                                                [&fixture, &modalVertices, &modalViewportWidth](ImGuiTestContext *) {
            fixture.Frame();
            // Observe this modal's geometry while its owner/UI frame is active, before the test engine drains its queue.
            const ImGuiWindow *modal = ImGui::FindWindowByName("PackageManagement");
            if (modal != nullptr && modal->Active && !modal->Hidden && modal->DrawList->VtxBuffer.Size > 0) {
                modalVertices = modal->DrawList->VtxBuffer.Size;
                modalViewportWidth = ImGui::GetMainViewport()->Size.x;
            }
        }, [](ImGuiTestContext *ctx) {
            ctx->Yield(4);
        });
        CheckRun(result);
        CHECK(modalVertices > 0);
        CHECK(modalViewportWidth == 420.0F);
        REQUIRE(fixture.application.application->Plan().Value());
        CHECK(fixture.application.application->Plan().Value()->packages.front().verification == PackagePlanVerification::LockedOnly);
        CHECK_FALSE(fixture.operation);
        CHECK(fixture.application.source.calls.load() == 0);
        CHECK(fixture.application.trust.calls == 0);
    }

    TEST_CASE("Package modal retries unavailable host acquisition without mutating package authority", "[ui][imgui][editor][packages]") {
        PackageUi fixture{true};
        const auto result = fixture.harness.Run("horo_packages", "retry_host", [&fixture](ImGuiTestContext *) {
            fixture.Frame();
        }, [&fixture](ImGuiTestContext *ctx) {
            ctx->Yield(2);
            IM_CHECK_EQ(fixture.acquisitions, 1U);
            IM_CHECK_EQ(fixture.application.source.calls.load(), 0U);
            IM_CHECK(!fixture.operation.has_value());
            fixture.unavailable = false;
            Click(*ctx, "Refresh project package session");
            ctx->Yield(2);
            IM_CHECK_EQ(fixture.acquisitions, 2U);
            IM_CHECK(!fixture.operation.has_value());
            Click(*ctx, "Close");
        });
        CheckRun(result);
        CHECK_FALSE(fixture.modals.HasOpenModal());
        CHECK(fixture.application.source.calls.load() == 0);
        CHECK(fixture.application.trust.calls == 0);
        CHECK_FALSE(fixture.application.application->Current().Value());
    }

    TEST_CASE("Package modal real controls install verified evidence and preserve descriptor rejection", "[ui][imgui][editor][packages]") {
        PackageUi fixture;
        DelayedRestoreSource delayed{fixture.application.source};
        const auto result = fixture.harness.Run("horo_packages", "install_and_selection", [&fixture, &delayed](ImGuiTestContext *) {
            fixture.Frame();
            delayed.OnFrame();
        }, [&fixture](ImGuiTestContext *ctx) {
            RunInstallAndSelection(ctx, fixture);
        }, Horo::Tests::EditorUiScenarioBudget::Extended(kPackageAsyncFrameBudget));
        CheckRun(result);
        CHECK(delayed.entered);
        CHECK(delayed.blockedFrames == DelayedRestoreSource::kBlockedFrames);
        CHECK(fixture.application.source.calls.load() >= 1);
        REQUIRE(fixture.application.application->Plan().HasValue());
        REQUIRE(fixture.application.application->Plan().Value());
        CHECK(fixture.application.application->Plan().Value()->packages.front().verification == PackagePlanVerification::ArchiveVerified);
        CHECK_FALSE(fixture.application.lifecycle->Active());
    }

    TEST_CASE("Package removal can navigate every page of saved references", "[ui][imgui][editor][packages]") {
        PackageUi fixture{false, "en-US", false, true};
        std::string visibleText;
        const auto result = fixture.harness.Run("horo_packages", "removal_reference_pages", [&](ImGuiTestContext *) {
            ImGui::LogToBuffer();
            fixture.Frame();
            visibleText = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
        }, [&](ImGuiTestContext *ctx) {
            ctx->Yield(2);
            Click(*ctx, "Install verified packages");
            IM_CHECK(AwaitTerminal(*ctx, fixture, PackageOperationKind::Install));
            IM_CHECK(fixture.operation->outcome == PackageOperationOutcome::Succeeded);
            ctx->ItemInputValue("//**/##PackageIdentity", "com.horo.application-fixture");
            Click(*ctx, "Preview removal from the locked graph");
            Click(*ctx, "Remove reviewed package");
            IM_CHECK(AwaitTerminal(*ctx, fixture, PackageOperationKind::Remove));
            IM_CHECK(fixture.operation->outcome == PackageOperationOutcome::Failed);
            IM_CHECK(fixture.operation->diagnostic.has_value());
            IM_CHECK_EQ(fixture.operation->diagnostic->diagnostics.size(), 33U);
            ctx->Yield();
            IM_CHECK((ctx->ItemInfo("//**/Previous references").ItemFlags & ImGuiItemFlags_Disabled) != 0);
            IM_CHECK(visibleText.find("Assets/Guard.decision.json") == std::string::npos);
            Click(*ctx, "Next references");
            ctx->Yield();
            IM_CHECK((ctx->ItemInfo("//**/Next references").ItemFlags & ImGuiItemFlags_Disabled) != 0);
            IM_CHECK(visibleText.find("Decision subplan could not be found") != std::string::npos);
            IM_CHECK(visibleText.find("Assets/Guard.decision.json") != std::string::npos);
            IM_CHECK(visibleText.find("920003") != std::string::npos);
            IM_CHECK(visibleText.find("770001") != std::string::npos);
            Click(*ctx, "Previous references");
            ctx->Yield();
            IM_CHECK((ctx->ItemInfo("//**/Previous references").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        }, Horo::Tests::EditorUiScenarioBudget::Extended(kPackageAsyncFrameBudget));
        CheckRun(result);
    }

    TEST_CASE("Closing and reopening package modal retains running work until its real cancellation acknowledgment",
              "[ui][imgui][editor][packages]") {
        PackageUi fixture;
        fixture.application.source.block = true;
        PackageOperationId original;
        const auto result = fixture.harness.Run("horo_packages", "close_reopen_cancel", [&fixture](ImGuiTestContext *) {
            fixture.Frame();
        }, [&fixture, &original](ImGuiTestContext *ctx) {
            RunCloseReopenCancel(ctx, fixture, original);
        }, Horo::Tests::EditorUiScenarioBudget::Extended(kPackageAsyncFrameBudget));
        CheckRun(result);
        REQUIRE(fixture.operation);
        CHECK(fixture.operation->id == original);
        CHECK(fixture.operation->outcome == PackageOperationOutcome::Cancelled);
        CHECK(fixture.application.source.calls.load() == 1);
    }
}  // namespace
