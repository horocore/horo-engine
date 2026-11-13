#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "SourceEditorSession.h"
#include "editor/project_model/RendererAvailability.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "support/OwnedTestDirectory.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Stores real document owners and the production presentation session across host rollback. */
    struct SessionState final {
        std::filesystem::path root;
        SourceEditorSession views;
        std::unique_ptr<SourceFileOpenService> documents;
        SourceDocumentSnapshot source;
        unsigned enters{};
        unsigned leaves{};
        bool deny{};
    };

    /** @brief Uses the real owner's teardown ordering at the real GuiScreenHost leave/re-enter boundary. */
    class SessionScreen final : public GuiScreen {
    public:
        SessionScreen(SessionState &state, const bool reject) : GuiScreen("test.SessionScreen"), state_(state), reject_(reject) {}

        ScreenId Id() const override {
            return reject_ ? 2 : 1;
        }

        Result<void> OnEnter(const GuiRoute &) override {
            if (reject_)
                return Result<void>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
            state_.views.Reset();
            state_.documents = std::make_unique<SourceFileOpenService>(state_.root);
            const char *path = state_.enters++ == 0 ? "first.cpp" : "second.cpp";
            const auto opened = state_.documents->Open({.path = path, .mode = SourceOpenMode::EmbeddedOnly});
            if (opened.HasError())
                return Result<void>::Failure(opened.ErrorValue());
            if (!opened.Value().sourceSnapshot)
                return Result<void>::Failure(MakeError(SourceDocumentErrors::Invalid));
            state_.source = *opened.Value().sourceSnapshot;
            auto *view = state_.views.Acquire(state_.source.Identity().instance);
            if (!view)
                return Result<void>::Failure(MakeError(SourceDocumentErrors::Invalid));
            view->Publish(state_.source);
            return Result<void>::Success();
        }

        void OnUpdate(float) override {}

        void Draw(const GuiContentRegion &) override {}

        LeaveDecision CanLeave(const LeaveTarget &) const override {
            return {.disposition = state_.deny ? LeaveDisposition::Deny : LeaveDisposition::Allow, .requirement = std::nullopt};
        }

        Result<LeaveDecision> ResolveLeave(const LeaveTarget &, const LeaveResolution &) override {
            return Result<LeaveDecision>::Success({.disposition = LeaveDisposition::Allow, .requirement = std::nullopt});
        }

        void OnLeave() override {
            state_.views.Reset();
            state_.documents.reset();
            ++state_.leaves;
        }

    private:
        SessionState &state_;
        bool reject_{};
    };

    /** @brief Composes a real route host without a native renderer or production project setup. */
    struct SessionHostFixture final {
        Editor::Tests::EditorGuiContextFixture gui;
        Horo::Tests::OwnedTestDirectory directory{"source-session-route"};
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8}};
        ProjectCreationService creation{jobs, gui.engineEvents};
        ConfigurationService configuration = CreateEditorConfigurationService(DefaultEditorSettings());
        EditorSettingsService settings{DefaultEditorSettings(), configuration, gui.editorEvents, gui.localization};
        EditorModalHost modals{gui.editorEvents, gui.input};
        RendererAvailabilitySnapshot renderers{{}, {}};
        SessionState state{.root = directory.Path()};
        GuiScreenHost host{gui.context,
                           {.modalHost = modals,
                            .settingsService = settings,
                            .localization = gui.localization,
                            .engineEvents = gui.engineEvents,
                            .creationService = creation,
                            .jobs = jobs,
                            .inputRouter = gui.input,
                            .rendererAvailability = renderers}};

        SessionHostFixture() {
            std::ofstream(directory.Path() / "first.cpp") << "first";
            std::ofstream(directory.Path() / "second.cpp") << "second";
            host.Screens().Register(GuiRouteKind::Welcome, [&](const EditorServiceRegistry &, const GuiRoute &) {
                return std::make_unique<SessionScreen>(state, false);
            });
            host.Screens().Register(GuiRouteKind::ProjectBrowser, [&](const EditorServiceRegistry &, const GuiRoute &) {
                return std::make_unique<SessionScreen>(state, true);
            });
            REQUIRE(host.Start({GuiRouteKind::Welcome, WelcomeRouteParameters{}}).HasValue());
        }

        ~SessionHostFixture() {
            host.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Cancel);
        }
    };
}  // namespace

TEST_CASE("Rejected route leave retains source views and failed destination entry fences the rollback owner",
          "[editor][source][widget][lifecycle]") {
    SessionHostFixture fixture;
    const auto instance = fixture.state.source.Identity().instance;
    auto *original = fixture.state.views.Acquire(instance);
    REQUIRE(original);
    const auto identity = original->widget.Identity();
    CHECK(original->widget.HasHighlight(fixture.state.source.Revision()));
    REQUIRE(original->widget.Select({1, 3}));
    fixture.state.views.Presented(instance);
    fixture.state.deny = true;
    static_cast<void>(fixture.host.Navigate({GuiRouteKind::ProjectBrowser, ProjectBrowserRouteParameters{}}));
    CHECK(fixture.state.enters == 1);
    CHECK(fixture.state.leaves == 0);
    CHECK(fixture.state.views.Acquire(instance)->widget.Identity() == identity);
    CHECK(fixture.state.views.Acquire(instance)->widget.Selection() == SourceWidgetSelection{1, 3});
    fixture.state.deny = false;
    REQUIRE(fixture.host.Navigate({GuiRouteKind::ProjectBrowser, ProjectBrowserRouteParameters{}}).HasValue());
    CHECK(fixture.state.enters == 2);
    CHECK(fixture.state.leaves == 1);
    CHECK(fixture.state.source.Identity().instance == instance);
    CHECK(fixture.state.source.Identity().key.source.Value() == "second.cpp");
    auto *rolledBack = fixture.state.views.Acquire(instance);
    REQUIRE(rolledBack);
    CHECK(rolledBack->widget.Identity().value > identity.value);
    CHECK(rolledBack->widget.HasHighlight(fixture.state.source.Revision()));
    CHECK(rolledBack->widget.Selection() == SourceWidgetSelection{0, 0});
    CHECK(fixture.state.views.NeedsFocus(instance));
}
