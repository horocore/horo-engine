#include "../unit/packages/PackageApplicationServiceTestSupport.h"
#include "EditorUiTestHarness.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/modals/packages/PackageModal.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui_test_engine/imgui_te_context.h>

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
        unsigned acquisitions{};
        std::optional<PackageOperationSnapshot> operation;

        explicit PackageUi(const bool unavailableInitially = false, const char *locale = "en-US", const bool noFactory = false)
            : unavailable(unavailableInitially), missingFactory(noFactory) {
            LocalizationError error;
            REQUIRE(localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                     (std::string{locale} + ".json"),
                                                 &error));
            REQUIRE(localization.Prepare(LocaleTag{locale}, &error));
            REQUIRE(localization.ActivatePrepared(&error));
            Open();
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
                    }});
                };
            auto modal = std::make_unique<PackageModal>(context, std::move(acquire));
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

    void Click(ImGuiTestContext &ui, const char *label) {
        const std::string path = std::string{"//**/"} + label;
        ui.ItemClick(path.c_str());
    }

    bool AwaitTerminal(ImGuiTestContext &ui, const PackageUi &fixture, const PackageOperationKind kind) {
        for (unsigned frame = 0; frame < 300 && (!fixture.operation || fixture.operation->kind != kind ||
                                                 fixture.operation->outcome == PackageOperationOutcome::Running);
             ++frame)
            ui.Yield();
        return fixture.operation && fixture.operation->kind == kind && fixture.operation->outcome != PackageOperationOutcome::Running;
    }

    void CheckRun(const Horo::Tests::EditorUiScenarioResult &result) {
        INFO(result.testEngineLog);
        REQUIRE(result.Succeeded());
        CHECK_FALSE(result.frameBudgetExceeded);
    }

    TEST_CASE("Unconfigured package modal remains inert and closable", "[ui][imgui][editor][packages]") {
        PackageUi fixture{false, "en-US", true};
        const auto result = fixture.harness.Run("horo_packages", "no_host", [&](ImGuiTestContext *) {
            fixture.Frame();
        }, [&](ImGuiTestContext *ctx) {
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
        const auto result = fixture.harness.Run("horo_packages", "localized_inert_plan", [&](ImGuiTestContext *) {
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
        const auto result = fixture.harness.Run("horo_packages", "retry_host", [&](ImGuiTestContext *) {
            fixture.Frame();
        }, [&](ImGuiTestContext *ctx) {
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
        const auto result = fixture.harness.Run("horo_packages", "install_and_selection", [&](ImGuiTestContext *) {
            fixture.Frame();
        }, [&](ImGuiTestContext *ctx) {
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
            IM_CHECK(fixture.operation->kind == PackageOperationKind::Activate);
            IM_CHECK(fixture.operation->outcome == PackageOperationOutcome::Failed);
            Click(*ctx, "Disable package extensions");
            IM_CHECK(AwaitTerminal(*ctx, fixture, PackageOperationKind::Disable));
            IM_CHECK(fixture.operation->kind == PackageOperationKind::Disable);
            IM_CHECK(fixture.operation->outcome == PackageOperationOutcome::Succeeded);
        });
        CheckRun(result);
        CHECK(fixture.application.source.calls.load() >= 1);
        REQUIRE(fixture.application.application->Plan().HasValue());
        REQUIRE(fixture.application.application->Plan().Value());
        CHECK(fixture.application.application->Plan().Value()->packages.front().verification == PackagePlanVerification::ArchiveVerified);
        CHECK_FALSE(fixture.application.lifecycle->Active());
    }

    TEST_CASE("Closing and reopening package modal retains running work until its real cancellation acknowledgment",
              "[ui][imgui][editor][packages]") {
        PackageUi fixture;
        fixture.application.source.block = true;
        PackageOperationId original;
        const auto result = fixture.harness.Run("horo_packages", "close_reopen_cancel", [&](ImGuiTestContext *) {
            fixture.Frame();
        }, [&](ImGuiTestContext *ctx) {
            ctx->Yield(2);
            Click(*ctx, "Restore locked packages");
            for (unsigned frame = 0; frame < 100 && fixture.application.source.calls.load() == 0; ++frame)
                ctx->Yield();
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
        });
        CheckRun(result);
        REQUIRE(fixture.operation);
        CHECK(fixture.operation->id == original);
        CHECK(fixture.operation->outcome == PackageOperationOutcome::Cancelled);
        CHECK(fixture.application.source.calls.load() == 1);
    }
}  // namespace
