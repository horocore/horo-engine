#include "../../../apps/HoroEditor/app/EditorCloudModelProfile.h"
#include "../../../apps/HoroEditor/app/EditorLocalAgentContext.h"
#include "../../../apps/HoroEditor/app/EditorLocalAgentPanel.h"
#include "../../../apps/HoroEditor/app/EditorLocalAgentPatch.h"
#include "../../../apps/HoroEditor/app/EditorLocalAgentPatchCheckpoint.h"
#include "../../support/AllocationProbe.h"
#include "../../support/OwnedTestDirectory.h"
#include "../../support/SourceDocumentTestProject.h"
#include "EditorWorkspaceViewModel.h"
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Editor/WorkspaceMcpControls.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Platform/SecureRandom.h"
#include "Horo/Security/CredentialStore.h"
#include "Horo/Security/SecurityErrors.h"
#include "editor/source_editor/SourceEditorSession.h"
#include "editor/source_editor/SourceReviewApplication.h"
#include "editor/source_editor/SourceReviewOverlay.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <imgui_internal.h>
#include <span>
#include <thread>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct LocalAgentFixture final {
        Editor::Tests::EditorGuiContextFixture gui;
        Editor::Tests::ScopedJobSystem jobs;
        NativeDurableFileSystem files;
        WorkspacePanelRegistry panels;
        Input::InputContextToken workspace{
            gui.input.PushContext(Input::InputContextId{"workspace"}, Input::InputContextKind::EditorWorkspace)};
        Input::RawInputCollector collector;
        EditorWorkspaceViewModel model;
        EditorWorkspaceViewCommandData command;
        EditorLocalAgentPanelHost agent;

        explicit LocalAgentFixture(const std::string locale, const std::function<void(PanelContext &)> &configure = {},
                                   std::shared_ptr<EditorCloudModelProfileStore> profiles = {})
            : agent(MakeEditorLocalAgentPanel(jobs.Get(), files, std::move(profiles))) {
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            const auto catalog = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" / (locale + ".json");
            REQUIRE(gui.localization.LoadCatalogFile(catalog));
            REQUIRE(gui.localization.Prepare(LocaleTag{locale}));
            REQUIRE(gui.localization.ActivatePrepared());
            panels.RegisterPanel(agent.panel);
            PanelContext ctx{.dataBus = gui.editorEvents,
                             .inputRouter = &gui.input,
                             .workspaceInputContext = &workspace,
                             .projectIdentity = "layout-test"};
            if (configure)
                configure(ctx);
            panels.AttachAll(ctx);
            InputFrame(1);
        }

        ~LocalAgentFixture() {
            panels.DetachAll();
            Theme::ApplyUiScale(100, 1.0F);
        }

        void InputFrame(const Input::FrameNumber number) {
            collector.BeginFrame(number);
            gui.input.BeginFrame(collector.Commit());
        }

        std::string Draw(const float width, const bool show = true, const bool log = false) {
            agent.poll();
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({width, 750});
            ImGui::Begin("Chat dock fixture", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            if (log)
                ImGui::LogToBuffer();
            if (show)
                agent.panel->DrawPanel(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail(), model, command, gui.context);
            std::string drawn;
            if (log) {
                drawn = ImGui::GetCurrentContext()->LogBuffer.c_str();
                ImGui::LogFinish();
            }
            ImGui::End();
            gui.imgui.EndFrame();
            return drawn;
        }

        ImGuiWindow *ChatWindow() const {
            for (auto *window : ImGui::GetCurrentContext()->Windows) {
                if (window->Active && window->ParentWindow && std::string_view{window->Name}.find("##LocalAgent") != std::string_view::npos)
                    return window;
            }
            return nullptr;
        }

        void Activate(const char *id, const bool text = false) {
            auto *chat = ChatWindow();
            REQUIRE(chat);
            ImGui::FocusWindow(chat);
            ImGui::ActivateItemByID(chat->GetID(id));
            if (text)
                ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
            Draw(420);
        }

        void Enter(const char *id, const char *value) {
            Activate(id, true);
            REQUIRE(ImGui::GetActiveID() == ChatWindow()->GetID(id));
            ImGui::GetIO().AddInputCharactersUTF8(value);
            Draw(420);
        }

        std::string DrawnText() {
            return Draw(420, true, true);
        }
    };

    /** @brief Pumps until an observable transport or presentation condition occurs, with a finite test deadline. */
    template <typename Condition> bool Await(Condition condition) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        do {
            if (condition())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    /** @brief Observes credential admission, then prevents network access by rejecting each lookup. */
    struct CloudCredentialProbe final : Security::CredentialBackend {
        static constexpr std::string_view Key = "fixture-model-key";
        std::atomic_uint resolutions{};
        std::atomic_bool enrolled{};
        std::vector<CredentialReference> stored;

        bool Available() const noexcept override {
            return true;
        }

        Result<void> Put(const Security::CredentialReference &reference, Security::SecureBytes secret) override {
            enrolled = std::ranges::equal(secret.View(), std::as_bytes(std::span{Key}));
            stored.push_back(reference);
            return Result<void>::Success();
        }

        Result<Security::SecureBytes> Resolve(const Security::CredentialReference &reference) override {
            if (std::ranges::find(stored, reference) == stored.end())
                return Result<Security::SecureBytes>::Failure(MakeError(SecurityErrors::CredentialNotFound));
            ++resolutions;
            return Result<Security::SecureBytes>::Failure(MakeError(SecurityErrors::CredentialRevoked));
        }

        Result<void> Remove(const Security::CredentialReference &reference) noexcept override {
            std::erase(stored, reference);
            enrolled = !stored.empty();
            return Result<void>::Success();
        }
    };

    TEST_CASE("Cloud profile preserves the active binding through stale and failed writes", "[editor][agent][cloud]") {
        Horo::Tests::OwnedTestDirectory directory{"cloud-profile-write"};
        const auto path = directory.Path() / "connection.profile";
        const auto probe = std::make_shared<CloudCredentialProbe>();
        EditorCloudModelProfileStore
            store{path, std::make_shared<Security::PersistentCredentialStore>(probe, Platform::CreateNativeSecureRandomSource())};
        const auto key = [] {
            return Security::SecureBytes{std::as_bytes(std::span{CloudCredentialProbe::Key})};
        };
        CHECK(store.Enroll(std::nullopt, "https://example.invalid/?other=route", "model-a", key()).HasError());
        std::ofstream{path} << "horo-cloud-model-v1\nhttps://first.invalid\nmodel-a\ninvalid-reference\n\n";
        CHECK(store.Load().HasError());
        CHECK(store.Enroll(std::nullopt, "https://first.invalid", "model-a", key()).HasError());
        CHECK(probe->stored.empty());
        REQUIRE(std::filesystem::remove(path));
        const auto original = store.Enroll(std::nullopt, "https://first.invalid", "model-a", key());
        REQUIRE(original.HasValue());
        CHECK(store.Enroll(std::nullopt, "https://second.invalid", "model-b", key()).HasError());
        REQUIRE(probe->stored.size() == 1);

        auto blocked = path;
        blocked += ".pending";
        REQUIRE(std::filesystem::create_directory(blocked));
        std::ofstream{blocked / "active"} << "owned fixture";
        CHECK(store.Enroll(original.Value(), "https://second.invalid", "model-b", key()).HasError());
        REQUIRE(probe->stored.size() == 1);
        CHECK(probe->stored.front() == original.Value().credential);
        const auto retained = store.Load();
        REQUIRE(retained.HasValue());
        CHECK(retained.Value() == original.Value());

        REQUIRE(std::filesystem::remove(blocked / "active"));
        REQUIRE(std::filesystem::remove(blocked));
        const auto replaced = store.Enroll(original.Value(), "https://second.invalid", "model-b", key());
        REQUIRE(replaced.HasValue());
        REQUIRE(probe->stored.size() == 1);
        CHECK(probe->stored.front() == replaced.Value().credential);
        CHECK(store.Revoke(original.Value()).HasError());
        REQUIRE(store.Revoke(replaced.Value()).HasValue());
        CHECK(probe->stored.empty());
    }

    TEST_CASE("Cloud chat asks before looking up credentials for either approved operation", "[editor][agent][gui][cloud]") {
        Horo::Tests::OwnedTestDirectory directory{"cloud-profile"};
        const auto path = directory.Path() / "connection.profile";
        const auto probe = std::make_shared<CloudCredentialProbe>();
        const auto makeStore = [&] {
            return std::make_shared<
                EditorCloudModelProfileStore>(path,
                                              std::make_shared<
                                                  Security::PersistentCredentialStore>(probe, Platform::CreateNativeSecureRandomSource()));
        };
        const auto switchToCloud = [](LocalAgentFixture &fixture) {
            fixture.Draw(420);
            fixture.Activate("###cloud_use");
            REQUIRE(Await([&] {
                return fixture.DrawnText().find("Updating cloud connection") == std::string::npos;
            }));
            CHECK(fixture.DrawnText().find("Could not update the saved cloud connection") == std::string::npos);
        };
        const auto store = makeStore();
        {
            LocalAgentFixture first{"en-US", {}, store};
            ImGui::GetPlatformIO().Platform_GetClipboardTextFn = [](ImGuiContext *) {
                return CloudCredentialProbe::Key.data();
            };
            switchToCloud(first);
            first.Enter("##CloudEndpoint", "https://provider.invalid");
            first.Activate("###cloud_key_enroll");
            REQUIRE(Await([&] {
                return first.DrawnText().find("Access key saved") != std::string::npos;
            }));
            REQUIRE(probe->enrolled);
            first.Enter("##CloudModel", "fixture-model");
            first.Activate("###cloud_model_save");
            REQUIRE(Await([&] {
                return first.DrawnText().find("Model saved for this cloud connection") != std::string::npos;
            }));
        }
        const auto retained = store->Load();
        REQUIRE(retained.HasValue());
        REQUIRE(retained.Value());
        CHECK(retained.Value()->model == "fixture-model");
        CHECK(retained.Value()->endpoint == "https://provider.invalid");
        std::ifstream profile{path, std::ios::binary};
        const std::string serialized{std::istreambuf_iterator<char>{profile}, std::istreambuf_iterator<char>{}};
        profile.close();
        CHECK(serialized.find(CloudCredentialProbe::Key) == std::string::npos);
        CHECK(serialized.find(retained.Value()->credential.Value()) != std::string::npos);

        LocalAgentFixture fixture{"en-US", {}, makeStore()};
        switchToCloud(fixture);
        CHECK(probe->resolutions == 0);
        fixture.Enter("##CloudPrompt", "Review this exact prompt");
        fixture.Activate("###cloud_review");
        const auto review = fixture.DrawnText();
        CHECK(review.find("https://provider.invalid") != std::string::npos);
        CHECK(review.find("fixture-model") != std::string::npos);
        CHECK(review.find("Review this exact prompt") != std::string::npos);
        CHECK(review.find(CloudCredentialProbe::Key) == std::string::npos);
        CHECK(probe->resolutions == 0);
        fixture.Activate("###cloud_discard");
        CHECK(probe->resolutions == 0);

        fixture.Activate("###discover");
        CHECK(probe->resolutions == 0);
        fixture.Activate("###cloud_approve");
        const auto failed = [&] {
            return fixture.DrawnText().find("Cloud authentication failed") != std::string::npos;
        };
        REQUIRE(Await(failed));
        CHECK(probe->resolutions == 1);

        fixture.Activate("###cloud_review");
        CHECK(probe->resolutions == 1);
        fixture.Activate("###cloud_approve");
        REQUIRE(Await(failed));
        CHECK(probe->resolutions == 2);
        fixture.Activate("###cloud_key_revoke");
        REQUIRE(Await([&] {
            return fixture.DrawnText().find("Cloud access key removed") != std::string::npos;
        }));
        CHECK_FALSE(probe->enrolled);
        CHECK_FALSE(std::filesystem::exists(path));
    }

#if HORO_ENABLE_TELEMETRY
    /** @brief Captures production MCP events for the duration of one chat scenario. */
    struct SceneChatAuditLog {
        Horo::Tests::OwnedTestDirectory directory{"scene-chat-audit"};
        std::shared_ptr<Log::StructuredLogStore> records{std::make_shared<Log::StructuredLogStore>(32)};

        SceneChatAuditLog() {
            REQUIRE(Log::Logger::Init({.logDirectory = directory.Path(), .writeSessionMarkers = false, .echoToStderr = false}));
            Log::Logger::SetStructuredLogStore(records);
        }

        ~SceneChatAuditLog() {
            Log::Logger::Shutdown();
            Log::Logger::SetStructuredLogStore(nullptr);
        }

        bool ContainsCompletion(const WorkspaceMcpAuditReference &reference) const {
            const auto snapshot = records->SnapshotIfChanged(0);
            if (!snapshot)
                return false;
            return std::ranges::any_of(snapshot->records, [&](const auto &entry) {
                const auto matches = [&](const std::string_view name, const std::uint64_t value) {
                    const auto found = std::ranges::find(entry->fields, name, &Telemetry::Field::key);
                    return found != entry->fields.end() && found->value == Telemetry::FieldValue{value};
                };
                return entry->category == "mcp.audit" && entry->message == "operation.finished" &&
                       matches("principal_id", reference.session.principal) && matches("session_id", reference.session.session) &&
                       matches("session_generation", reference.session.generation) && matches("operation_id", reference.operation);
            });
        }
    };
#endif

    TEST_CASE("Idle local chat keeps frame allocations bounded", "[editor][agent][gui][performance]") {
        const auto *locale = GENERATE("en-US", "tr-TR");
        LocalAgentFixture local{locale};
        for (int warmup = 0; warmup < 8; ++warmup)
            local.Draw(420);

        constexpr std::size_t frames = 120;
        Horo::Tests::AllocationProbe::Measurement idle;
        {
            const Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
            for (std::size_t frame = 0; frame < frames; ++frame)
                local.agent.poll();
            idle = measurement.Snapshot();
        }
        CHECK(idle.requests == 0);
        Horo::Tests::AllocationProbe::Measurement visible;
        {
            const Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
            for (std::size_t frame = 0; frame < frames; ++frame)
                local.Draw(420);
            visible = measurement.Snapshot();
        }
        CAPTURE(locale, visible.requests, visible.requestedBytes);
        CHECK(visible.requests <= 4 * frames);
        CHECK(visible.requestedBytes <= 256 * frames);
    }

    TEST_CASE("Local chat owns keyboard input only while its text field is presented", "[editor][agent][gui]") {
        LocalAgentFixture fixture{"en-US"};
        fixture.Draw(420);
        auto *chat = fixture.ChatWindow();
        REQUIRE(chat);
        ImGui::FocusWindow(chat);
        ImGui::ActivateItemByID(chat->GetID("##Prompt"));
        ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
        fixture.Draw(420);
        REQUIRE_FALSE(fixture.gui.input.IsContextActive(fixture.workspace));

        auto modal = fixture.gui.input.PushContext(Input::InputContextId{"modal"}, Input::InputContextKind::ModalRoot);
        fixture.command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        fixture.agent.panel->OnPresentationHidden(fixture.command);
        CHECK(fixture.command.command == EditorWorkspaceViewCommand::ChangeActivePanel);
        CHECK(fixture.gui.input.IsContextActive(modal));
        modal.Reset();
        fixture.InputFrame(2);
        fixture.Draw(420, false);
        fixture.InputFrame(3);
        fixture.Draw(420, false);
        CHECK(fixture.gui.input.IsContextActive(fixture.workspace));
        CHECK_FALSE(fixture.ChatWindow());
        fixture.Draw(420);
        CHECK(fixture.ChatWindow() == chat);
    }

    TEST_CASE("Local chat can move, close and restore as a workspace dock tab", "[editor][agent][gui]") {
        LocalAgentFixture fixture{"en-US"};
        auto &host = fixture.model.workspacePanelHost;
        const auto id = fixture.agent.panel->GetId();
        REQUIRE(host.OpenPanel(id, "workspace.right").Succeeded());
        REQUIRE(host.MovePanel(id, {"workspace.left", 0}).Succeeded());
        CHECK(host.Layout().FindTabStack("workspace.left")->activeTab == id);
        REQUIRE(host.ClosePanel(id).Succeeded());
        REQUIRE(host.OpenPanel(id, "workspace.right").Succeeded());

        const Horo::Tests::OwnedTestDirectory directory{"local-chat-layout"};
        const auto path = directory.Path() / "layout.json";
        AtomicFileReplacementReceipt receipt;
        REQUIRE(host.SaveLayout(fixture.files, receipt, path));
        WorkspacePanelHost restored;
        REQUIRE(restored.RestoreLayout(path));
        CHECK(restored.Layout().FindTabStack("workspace.right")->activeTab == id);
        CHECK(restored.Layout().FindTabStack("workspace.document")->activeTab == "horo.viewport");
        CHECK(restored.DocumentTabs().empty());
    }

    TEST_CASE("Local chat stays in the right dock and keeps localized controls within its resized width", "[editor][agent][gui]") {
        const std::string locale = GENERATE("en-US", "tr-TR");
        const float displayScale = GENERATE(1.0F, 1.5F, 2.0F);
        LocalAgentFixture fixture{locale};
        Theme::ApplyUiScale(100, displayScale);
        ImGui::GetIO().DisplayFramebufferScale = {displayScale, displayScale};
        const auto &right = fixture.panels.GetPanelsForArea(WorkspaceDockArea::Right);
        REQUIRE(right.size() == 1);
        CHECK(right.front()->GetId() == "horo.local_agent");
        for (const auto width : {430.0F, 185.0F, 430.0F}) {
            CAPTURE(locale, displayScale, width);
            // ContentSizeIdeal is from the preceding frame. Allow the vertical scrollbar allocation to settle first.
            fixture.Draw(width);
            fixture.Draw(width);
            fixture.Draw(width);
            const auto *chat = fixture.ChatWindow();
            REQUIRE(chat);
            CHECK(chat->ContentSizeIdeal.x <= chat->InnerRect.GetWidth());
            CHECK(chat->Size.x < width);
        }
    }

    TEST_CASE("Play contact review works without a speech provider and clears when its scene retires", "[editor][agent][gui][play]") {
        unsigned reads = 0;
        bool current = true;
        bool playing = false;
        LocalAgentFixture fixture{"en-US", [&](PanelContext &context) {
            context.canReviewAgentSceneContacts = [&] { return playing; };
            context.readAgentSceneContacts = [&] {
                ++reads;
                AgentSceneContactReview review{.object = 301, .completedTick = 405};
                review.contacts[0] = {507, AgentSceneContactKind::TriggerEnter};
                review.count = 1;
                return std::optional{review};
            };
            context.agentSceneContactsCurrent = [&](const AgentSceneContactReview &review) {
                return current && review.object == 301;
            };
        }};
        constexpr std::string_view button = "Review selected object's physics events";
        constexpr std::string_view contact = "Trigger entry #507";
        CHECK(fixture.DrawnText().find(button) == std::string::npos);
        playing = true;
        CHECK(fixture.DrawnText().find(button) != std::string::npos);
        CHECK(reads == 0);

        fixture.Activate("###xr_contacts_review");
        const auto reviewed = fixture.DrawnText();
        CHECK(reviewed.find("Completed physics tick 405") != std::string::npos);
        CHECK(reviewed.find(contact) != std::string::npos);
        CHECK(reads == 1);

        current = false;
        const auto withdrawn = fixture.DrawnText();
        CHECK(withdrawn.find(contact) == std::string::npos);
        CHECK(withdrawn.find("A complete physics event review is unavailable") != std::string::npos);
        current = true;
        playing = false;
        CHECK(fixture.DrawnText().find(button) == std::string::npos);
        playing = true;
        CHECK(fixture.DrawnText().find(contact) == std::string::npos);
        CHECK(reads == 1);
    }

    TEST_CASE("Viewport numbers are captured only after a user opens their review", "[editor][agent][gui][viewport]") {
        CancellationSource owner;
        int captured{};
        LocalAgentFixture fixture{"en-US", [&](PanelContext &context) {
            context.captureAgentViewport = [&](AgentViewportComponents) {
                ++captured;
                return AgentSceneSelectionSnapshot{.documentEpoch = 1,
                                                   .lifetime = owner.Token(),
                                                   .viewport = AgentViewportOverview{.revision = 1,
                                                                                     .position = {4, 5, 6},
                                                                                     .target = {},
                                                                                     .up = {0, 1, 0},
                                                                                     .verticalExtent = 1,
                                                                                     .nearPlane = 0.1F,
                                                                                     .farPlane = 100}};
            };
        }};
        fixture.Draw(420);
        fixture.Enter("##Endpoint", "http://127.0.0.1:11434");
        fixture.Enter("##Model", "local-test");
        fixture.Enter("##Prompt", "Explain this view");
        CHECK(captured == 0);
        CHECK(fixture.DrawnText().find("camera.position") == std::string::npos);
        fixture.Activate("###context_viewport_offer");
        CHECK(captured == 1);
        CHECK(fixture.DrawnText().find("camera.position = [4, 5, 6]") != std::string::npos);
        owner.RequestCancellation();
        fixture.Activate("###context_approve");
        CHECK(fixture.DrawnText().find("camera.position") == std::string::npos);
        CHECK(fixture.DrawnText().find("Stop") == std::string::npos);
        CHECK(captured == 1);
    }

    TEST_CASE("A viewport action opens an editable chat preview with opt-in transforms", "[editor][agent][gui][viewport]") {
        CancellationSource owner;
        CancellationSource replacement;
        bool replaceSelection = false;
        AgentSceneSelectionSnapshot selection{.documentEpoch = 1,
                                              .objects = {42},
                                              .objectCount = 1,
                                              .lifetime = owner.Token(),
                                              .viewport = AgentViewportOverview{.revision = 1,
                                                                                .position = {4, 5, 6},
                                                                                .up = {0, 1, 0},
                                                                                .verticalExtent = 1,
                                                                                .nearPlane = 0.1F,
                                                                                .farPlane = 100}};
        auto offered = std::optional{selection};
        LocalAgentFixture fixture{"en-US", [&](PanelContext &context) {
            context.takeAgentViewportReview = [&] {
                return std::exchange(offered, std::nullopt);
            };
            context.captureAgentViewport = [&](const AgentViewportComponents scope) {
                auto result = selection;
                if (replaceSelection) {
                    owner.RequestCancellation();
                    result.lifetime = replacement.Token();
                    result.selectionRevision = 2;
                    result.objects.front() = 77;
                }
                if (scope == AgentViewportComponents::LocalTransforms) {
                    result.transforms[0] = {result.objects.front(), {7, 8, 9}, {0, 0, 0, 1}, {1, 1, 1}};
                    result.transformCount = 1;
                }
                return result;
            };
        }};
        CHECK(fixture.DrawnText().find("camera.position = [4, 5, 6]") != std::string::npos);
        CHECK_FALSE(offered);
        CHECK(fixture.DrawnText().find("local transform:") == std::string::npos);
        fixture.Enter("##Prompt", "Explain the selected transform");
        SECTION("the same capture can add and remove component values") {
            fixture.Activate("###context_viewport_add_transforms");
            CHECK(fixture.DrawnText().find("object 42 local transform: translation = [7, 8, 9]") != std::string::npos);
            fixture.Activate("###context_viewport_remove_transforms");
            CHECK(fixture.DrawnText().find("local transform:") == std::string::npos);
        }
        SECTION("a replacement selected during the component read needs its own review") {
            replaceSelection = true;
            fixture.Activate("###context_viewport_add_transforms");
            const auto drawn = fixture.DrawnText();
            CHECK(drawn.find("camera.position") == std::string::npos);
            CHECK(drawn.find("object 77 local transform") == std::string::npos);
            CHECK(drawn.find(fixture.gui.localization.Get("editor", "workspace.local_agent.context_stale")) != std::string::npos);
        }
    }

    TEST_CASE("Scene query approval discloses the exact count and retires a denied or changed review", "[editor][agent][gui]") {
        std::size_t count = 3;
        LocalAgentFixture fixture{"en-US", [&](PanelContext &context) {
            context.sceneObjectCount = [&] {
                return count;
            };
        }};
        fixture.Draw(420);
        fixture.Enter("##Endpoint", "http://127.0.0.1:11434");
        fixture.Enter("##Model", "local-test");
        fixture.Enter("##Prompt", "Count the objects");
        fixture.Activate("###ask_with_scene_count");
        CHECK(fixture.DrawnText().find("Stop") == std::string::npos);
        fixture.Activate("###scene_query_review");
        const auto review = fixture.DrawnText();
        CHECK(review.find("scene.inspect") != std::string::npos);
        CHECK(review.find("Arguments: none") != std::string::npos);
        CHECK(review.find("layout-test") != std::string::npos);
        CHECK(review.find("Only the displayed count can be sent to the local model.") != std::string::npos);
        CHECK(review.find("Current scene objects 3") != std::string::npos);
        fixture.Activate("###scene_query_deny");
        CHECK(fixture.DrawnText().find("Scene object count query was denied.") != std::string::npos);
        CHECK(fixture.DrawnText().find("scene.inspect") == std::string::npos);

        fixture.Activate("###scene_query_review");
        fixture.Draw(420);
        ++count;
        fixture.Activate("###ask_with_scene_count");
        CHECK(fixture.DrawnText().find("scene.inspect") == std::string::npos);
        CHECK(fixture.DrawnText().find("Stop") == std::string::npos);
        fixture.Activate("###scene_query_review");
        CHECK(fixture.DrawnText().find("Current scene objects 4") != std::string::npos);
    }

    TEST_CASE("An explicitly allowed scene query runs once against the disclosed count", "[.][chat-http]") {
        const auto *endpoint = std::getenv("HORO_MODEL_TEST_ENDPOINT");
        REQUIRE(endpoint);
        std::optional<WorkspaceMcpAuditReference> receipt;
#if HORO_ENABLE_TELEMETRY
        SceneChatAuditLog audit{};
#endif
        LocalAgentFixture fixture{"en-US", [&](PanelContext &context) {
            context.sceneObjectCount = [] {
                return std::size_t{3};
            };
            context.requestMcpAudit = [&](const WorkspaceMcpAuditReference &reference, EditorWorkspaceViewCommandData &) {
                receipt = reference;
            };
        }};
        fixture.Draw(420);
        fixture.Enter("##Endpoint", endpoint);
        fixture.Enter("##Model", "editor-approved-tool");
        fixture.Enter("##Prompt", "Count the objects");
        fixture.Activate("###scene_query_review");
        CHECK(fixture.DrawnText().find("Stop") == std::string::npos);
        fixture.Activate("###ask_with_scene_count");
        REQUIRE(Await([&] {
            const auto text = fixture.DrawnText();
            return text.find("Three objects") != std::string::npos && text.find("Scene object count query completed.") != std::string::npos;
        }));
        fixture.Activate("###scene_query_audit");
        REQUIRE(receipt);
        CHECK(receipt->session.principal > 0);
        CHECK(receipt->operation > 0);
#if HORO_ENABLE_TELEMETRY
        REQUIRE(Log::Logger::Flush());
        CHECK(audit.ContainsCompletion(*receipt));
#endif
    }

    TEST_CASE("Revoking the scene query stops the admitted turn before the model can run the tool", "[.][chat-http]") {
        const auto *endpoint = std::getenv("HORO_MODEL_TEST_ENDPOINT");
        const auto *gateName = std::getenv("HORO_MODEL_TEST_CHAT_GATE");
        REQUIRE(endpoint);
        REQUIRE(gateName);
        const auto gate = std::filesystem::path{gateName} / "scene-query";
        LocalAgentFixture fixture{"en-US", [](PanelContext &context) {
            context.sceneObjectCount = [] {
                return std::size_t{3};
            };
            context.requestMcpAudit = [](const WorkspaceMcpAuditReference &, EditorWorkspaceViewCommandData &) {
                FAIL("Revoked tool was offered as an accepted audit record");
            };
        }};
        fixture.Draw(420);
        fixture.Enter("##Endpoint", endpoint);
        fixture.Enter("##Model", "editor-approved-tool");
        fixture.Enter("##Prompt", "Hold the approved scene query");
        fixture.Activate("###scene_query_review");
        fixture.Activate("###ask_with_scene_count");
        REQUIRE(Await([&] {
            return std::filesystem::is_regular_file(gate / "started") &&
                   fixture.DrawnText().find("Revoke scene query permission") != std::string::npos;
        }));
        fixture.Activate("###scene_query_revoke");
        std::ofstream{gate / "release"} << "release";
        REQUIRE(Await([&] {
            return fixture.DrawnText().find("Request stopped.") != std::string::npos;
        }));
        const auto text = fixture.DrawnText();
        CHECK(text.find("Three objects") == std::string::npos);
        CHECK(text.find("View MCP audit record") == std::string::npos);
    }

    TEST_CASE("A real local model reply survives dock hiding and completes while hidden", "[.][chat-http]") {
        const auto *endpoint = std::getenv("HORO_MODEL_TEST_ENDPOINT");
        const auto *gateName = std::getenv("HORO_MODEL_TEST_CHAT_GATE");
        REQUIRE(endpoint);
        REQUIRE(gateName);
        const std::filesystem::path gate{gateName};
        REQUIRE(gate.is_absolute());
        REQUIRE(std::filesystem::is_directory(gate));

        LocalAgentFixture fixture{"en-US"};
        auto &host = fixture.model.workspacePanelHost;
        const auto id = fixture.agent.panel->GetId();
        REQUIRE(host.OpenPanel(id, "workspace.right").Succeeded());
        fixture.Draw(420);
        fixture.Enter("##Endpoint", endpoint);
        fixture.Enter("##Model", "editor-dock-lifecycle");
        fixture.Enter("##Prompt", "Keep this request running");
        fixture.Activate("###ask");
        REQUIRE(Await([&] {
            const auto text = fixture.DrawnText();
            return std::filesystem::is_regular_file(gate / "started") && text.find("First visible line") != std::string::npos;
        }));
        CHECK_FALSE(std::filesystem::exists(gate / "finished"));

        REQUIRE(host.ClosePanel(id).Succeeded());
        fixture.agent.panel->OnPresentationHidden(fixture.command);
        fixture.Draw(420, false);
        CHECK_FALSE(fixture.ChatWindow());
        REQUIRE(host.OpenPanel(id, "workspace.left").Succeeded());
        CHECK(fixture.DrawnText().find("First visible line") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(gate / "finished"));

        REQUIRE(host.ClosePanel(id).Succeeded());
        fixture.agent.panel->OnPresentationHidden(fixture.command);
        fixture.Draw(420, false);
        {
            std::ofstream release{gate / "release"};
            REQUIRE(release.good());
        }
        REQUIRE(Await([&] {
            fixture.Draw(420, false);
            return std::filesystem::is_regular_file(gate / "finished");
        }));
        CHECK_FALSE(fixture.ChatWindow());
        REQUIRE(host.OpenPanel(id, "workspace.right").Succeeded());
        REQUIRE(Await([&] {
            return fixture.DrawnText().find("Final answer after hiding the dock") != std::string::npos;
        }));
    }

    TEST_CASE("Project reattachment abandons the old HTTP turn and accepts a fresh turn", "[.][chat-http]") {
        const auto *endpoint = std::getenv("HORO_MODEL_TEST_ENDPOINT");
        const auto *gateName = std::getenv("HORO_MODEL_TEST_CHAT_GATE");
        REQUIRE(endpoint);
        REQUIRE(gateName);
        const auto gate = std::filesystem::path{gateName} / "project";
        LocalAgentFixture fixture{"en-US"};
        fixture.Draw(420);
        fixture.Enter("##Endpoint", endpoint);
        fixture.Enter("##Model", "editor-project-lifecycle");
        fixture.Enter("##Prompt", "Reply for retiring workspace");
        fixture.Activate("###ask");
        REQUIRE(Await([&] {
            const auto text = fixture.DrawnText();
            return std::filesystem::is_regular_file(gate / "started") && text.find("Retiring workspace started") != std::string::npos;
        }));
        fixture.panels.DetachAll();
        PanelContext replacement{.dataBus = fixture.gui.editorEvents,
                                 .inputRouter = &fixture.gui.input,
                                 .workspaceInputContext = &fixture.workspace,
                                 .projectIdentity = "replacement"};
        fixture.panels.AttachAll(replacement);
        {
            std::ofstream release{gate / "release"};
            REQUIRE(release.good());
        }
        bool leaked = false;
        REQUIRE(Await([&] {
            const auto text = fixture.DrawnText();
            leaked |= text.find("Retired workspace answer") != std::string::npos;
            return text.find("Stop") == std::string::npos;
        }));
        fixture.Enter("##Prompt", "Reply for replacement workspace");
        fixture.Activate("###ask");
        REQUIRE(Await([&] {
            const auto text = fixture.DrawnText();
            leaked |= text.find("Retired workspace answer") != std::string::npos;
            return text.find("Replacement workspace answer") != std::string::npos;
        }));
        CHECK_FALSE(leaked);
    }

    TEST_CASE("A completed source reply opens only its exact publication for review", "[.][chat-http]") {
        const auto *endpoint = std::getenv("HORO_MODEL_TEST_ENDPOINT");
        REQUIRE(endpoint);
        TestSupport::SourceProject project;
        project.Put("int value = 1;\n");
        SourceDocumentService owner{project.directory};
        const auto source = owner.Open(project.Identity(), project.directory / "code.cpp").Value();
        SourceEditorSession session;
        std::shared_ptr<const SourceReviewOverlay> queued;
        LocalAgentFixture fixture{"en-US", [&](PanelContext &context) {
            context.activeSourceSnapshot = [&](SourceDocumentSnapshot &output) {
                output = source;
                return true;
            };
            context.requestSourceReview = [&](const Proposals::ChangeSet &proposal, const Proposals::WorkspaceEditPlan &plan,
                                              const std::size_t index, EditorWorkspaceViewCommandData &) {
                auto overlay = SourceReviewOverlay::Capture(proposal, plan, index);
                REQUIRE(overlay.HasValue());
                queued = std::make_shared<const SourceReviewOverlay>(std::move(overlay).Value());
                return true;
            };
        }};
        fixture.Draw(420);
        fixture.Enter("##Endpoint", endpoint);
        fixture.Enter("##Model", "editor-inline-patch");
        fixture.Enter("##Prompt", "Propose one source edit");
        fixture.Activate("###context_offer");
        REQUIRE(fixture.DrawnText().find("int value = 1;") != std::string::npos);
        fixture.Activate("###context_approve");
        REQUIRE(Await([&] {
            return fixture.DrawnText().find("Review changes in source editor") != std::string::npos;
        }));
        fixture.Draw(350);
        const auto narrow = fixture.Draw(350, true, true);
        const auto *chat = fixture.ChatWindow();
        REQUIRE(chat);
        CHECK(chat->ContentSizeIdeal.x <= chat->InnerRect.GetWidth());
        const auto reference = narrow.rfind("code.cpp");
        REQUIRE(reference != std::string::npos);
        CHECK(reference > narrow.find("int value = 2;"));
        CHECK(narrow.find("Edit file") != std::string::npos);
        CHECK(narrow.find("bytes") != std::string::npos);
        CHECK(narrow.find("Awaiting review") != std::string::npos);
        CHECK_FALSE(queued);
        SECTION("an explicit click queues the correct tab and exact hunk") {
            fixture.Activate("###source_patch_review");
            REQUIRE(queued);
            REQUIRE(session.StageReview(queued));
            auto *view = session.Acquire(source.Identity().instance);
            REQUIRE(view);
            view->Publish(source);
            CHECK(view->widget.ReviewStatus() == SourceReviewStatus::Ready);
            fixture.Activate("###source_patch_review");
            REQUIRE(session.StageReview(queued));
            CHECK(session.Acquire(source.Identity().instance) == view);
            CHECK(owner.Snapshot(source.Identity().instance).Value().SharesStateWith(source));
        }
        SECTION("a revised buffer prevents the review from opening") {
            REQUIRE(owner.Edit(source.Identity().instance, {source.Revision(), 12, 1, "9"}).HasValue());
            fixture.Activate("###source_patch_review");
            CHECK_FALSE(queued);
            CHECK(fixture.DrawnText().find("The source changed after this request.") != std::string::npos);
        }
    }

    TEST_CASE("Local source review rejects prose and edits outside the offered file", "[editor][agent][source]") {
        TestSupport::SourceProject project;
        project.Put("one\n");
        SourceDocumentService owner{project.directory};
        const auto source = owner.Open(project.Identity(), project.directory / "code.cpp").Value();
        const EditorLocalAgentPatchSource offered{{source}, "example/model:latest", 1};
        const auto patch =
            R"({"version":1,"files":[{"kind":"edit","path":"code.cpp","hunks":[{"offset":0,"before":"one","after":"two"}]}]})";
        REQUIRE(PrepareEditorLocalPatchReview(patch, offered, "workspace", 1).HasValue());
        CHECK_FALSE(IsEditorLocalPatchReply("An example:\n```json\n{}\n```"));
        const auto foreign =
            R"({"version":1,"files":[{"kind":"edit","path":"other.cpp","hunks":[{"offset":0,"before":"one","after":"two"}]}]})";
        CHECK(PrepareEditorLocalPatchReview(foreign, offered, "workspace", 1).HasError());
        const auto remove = R"({"version":1,"files":[{"kind":"delete","path":"code.cpp"}]})";
        const auto removal = PrepareEditorLocalPatchReview(remove, offered, "workspace", 1);
        REQUIRE(removal.HasValue());
        CHECK(removal.Value().files.front().kind == EditorPatchFileKind::Delete);
        CHECK(SourceReviewOverlay::Capture(removal.Value().proposal, *removal.Value().plan, 0).HasValue());
        CHECK(owner.Snapshot(source.Identity().instance).Value().SharesStateWith(source));
        const auto edited = owner.Edit(source.Identity().instance, {source.Revision(), 0, 3, "authored"});
        REQUIRE(edited.HasValue());
        const EditorLocalAgentPatchSource dirty{{edited.Value()}, "example/model:latest", 2};
        const auto dirtyRemoval = PrepareEditorLocalPatchReview(remove, dirty, "workspace", 1);
        REQUIRE(dirtyRemoval.HasValue());
        CHECK(std::get<Proposals::PlanConflict>(dirtyRemoval.Value().plan->entries.front()).reason ==
              Proposals::PlanConflictReason::DirtyDelete);
        CHECK(edited.Value().IsCurrent());
    }

    TEST_CASE("An explicitly selected structural destination is inspected without publishing files", "[editor][agent][source]") {
        using namespace Proposals;
        TestSupport::SourceProject project;
        project.Put("base");
        SourceDocumentService owner{project.directory};
        const auto source = owner.Open(project.Identity(), project.directory / "code.cpp").Value();
        const EditorLocalAgentPatchSource offered{{source}, "example/model:latest", 1};
        NativeDurableFileSystem files;
        const auto path = SourceDocumentId::Parse("src/new.cpp").Value();
        const EditorPatchDestination destination{path, project.directory, files, {}};
        constexpr auto create = R"({"version":1,"files":[{"kind":"create","path":"src/new.cpp","text":"new text"}]})";
        constexpr auto rename = R"({"version":1,"files":[{"kind":"rename","path":"code.cpp","destination":"src/new.cpp","hunks":[]}]})";
        CHECK(PrepareEditorLocalPatchReview(create, offered, "workspace", 1).HasError());
        CHECK(PrepareEditorLocalPatchReview(rename, offered, "workspace", 1).HasError());
        auto expected = std::optional<PlanConflictReason>{};
        SECTION("Missing parent") {
            expected = PlanConflictReason::DestinationParentMissing;
        }
        SECTION("Occupied destination") {
            REQUIRE(std::filesystem::create_directory(project.directory / "src"));
            project.Put("existing", "src/new.cpp");
            expected = PlanConflictReason::DestinationOccupied;
        }
        SECTION("Portable alias collision") {
            REQUIRE(std::filesystem::create_directory(project.directory / "src"));
            project.Put("existing", "src/New.cpp");
            expected = PlanConflictReason::DestinationAlias;
        }
        SECTION("A symbolic link does not contribute a valid parent") {
            REQUIRE(std::filesystem::create_directory(project.directory / "other"));
            std::error_code error;
            std::filesystem::create_directory_symlink(project.directory / "other", project.directory / "src", error);
            if (error)
                SKIP("Directory symlink creation is unavailable to the test process");
            expected = PlanConflictReason::DestinationRedirect;
        }
        SECTION("The exact absent destination has a review and recoverable decisions") {
            REQUIRE(std::filesystem::create_directory(project.directory / "src"));
            auto prepared = PrepareEditorLocalPatchReview(create, offered, "workspace", 1, &destination);
            REQUIRE(prepared.HasValue());
            auto review = std::move(prepared).Value();
            REQUIRE(review.decisions.Decide(*review.plan, {0}, ReviewChoice::Accept).HasValue());
            REQUIRE(SaveEditorPatchCheckpoint(files, project.directory, {review.proposal, review.decisions, review.offeredPaths}, {})
                        .HasValue());
            const std::array sources{source};
            const auto restored = LoadEditorPatchCheckpoint(project.directory, "workspace", sources, {}, &files);
            REQUIRE(restored.HasValue());
            REQUIRE(restored.Value());
            CHECK(restored.Value()->decisions.State().Value() == ReviewState::Accepted);
            CHECK_FALSE(std::filesystem::exists(project.directory / "src/new.cpp"));
            project.Put("appeared", "src/new.cpp");
            CHECK(LoadEditorPatchCheckpoint(project.directory, "workspace", sources, {}, &files).HasError());
            REQUIRE(std::filesystem::remove(project.directory / "src/new.cpp"));
        }
        for (const auto reply : {create, rename}) {
            const auto review = PrepareEditorLocalPatchReview(reply, offered, "workspace", 1, &destination);
            REQUIRE(review.HasValue());
            const auto &entry = review.Value().plan->entries.front();
            if (expected)
                CHECK(std::get<PlanConflict>(entry).reason == *expected);
            else
                CHECK(std::get<FilePreview>(entry).after == (reply == create ? "new text" : "base"));
        }
        CHECK(source.IsCurrent());
        CHECK(source.Text() == "base");
    }

    TEST_CASE("Chat summarizes every admitted operation without retaining patch text", "[editor][agent][source]") {
        using namespace Proposals;
        const auto path = [](std::string_view name) {
            return SourceDocumentId::Parse(name).Value();
        };
        const auto policy = ProjectPolicy::Capture("workspace", 1, {{path("src"), true}}).Value();
        const auto hash = ComputeSha256(std::as_bytes(std::span{"abc", 3}));
        const FileBase edit{path("src/edit.cpp"), 1, 1, 3, false, hash, hash};
        const FileBase remove{path("src/remove.cpp"), 1, 1, 3, false, hash, hash};
        const FileBase rename{path("src/rename.cpp"), 1, 1, 3, false, hash, hash};
        const auto proposal = ChangeSet::Capture(policy, {"review",
                                                          1,
                                                          {"provider", "model", "request"},
                                                          {RenameFile{rename, path("src/target.cpp"), {}}, EditFile{edit, {{1, "b", "BB"}}},
                                                           CreateFile{path("src/create.cpp"), "new\n"}, DeleteFile{remove}}});
        REQUIRE(proposal.HasValue());
        const auto cards = DescribeEditorPatchFiles(proposal.Value());
        REQUIRE(cards.size() == 4);
        CHECK(cards[0].kind == EditorPatchFileKind::Create);
        CHECK(cards[0].addedBytes == 4);
        CHECK(cards[1].kind == EditorPatchFileKind::Edit);
        CHECK(cards[1].hunks == 1);
        CHECK(cards[1].removedBytes == 1);
        CHECK(cards[1].addedBytes == 2);
        CHECK(cards[2].kind == EditorPatchFileKind::Delete);
        CHECK(cards[2].removedBytes == 3);
        CHECK(cards[3].kind == EditorPatchFileKind::Rename);
        CHECK(cards[3].destination == path("src/target.cpp"));
    }

    TEST_CASE("An explicit file selection can plan and route two independent source edits", "[editor][agent][source]") {
        TestSupport::SourceProject project;
        project.Put("one\n", "one.cpp");
        project.Put("two\n", "two.cpp");
        SourceDocumentService owner{project.directory};
        const auto one = owner.Open(project.Identity("one.cpp", 1), project.directory / "one.cpp").Value();
        const auto two = owner.Open(project.Identity("two.cpp", 2), project.directory / "two.cpp").Value();
        CancellationSource lifetime;
        auto prepared = EditorLocalAgentContext::PrepareOpenFiles({two, one}, "workspace", 1, lifetime.Token(), 1);
        REQUIRE(prepared.HasValue());
        auto context = std::move(prepared).Value();
        CHECK(context.Items()[0].provenance.origin == "two.cpp");
        const std::array both{one, two};
        CHECK(context.MatchesOpenSource(both));
        CHECK_FALSE(context.MatchesOpenSource(std::span{&one, 1}));
        REQUIRE(context.Approve());
        const EditorLocalAgentPatchSource offered{context.PatchSources(), "model", 1};
        REQUIRE(offered.sources.size() == 2);
        const auto patch = R"({"version":1,"files":[
            {"kind":"edit","path":"two.cpp","hunks":[{"offset":0,"before":"two","after":"new"}]},
            {"kind":"edit","path":"one.cpp","hunks":[{"offset":0,"before":"one","after":"first"}]}]})";
        const auto review = PrepareEditorLocalPatchReview(patch, offered, "workspace", 1);
        REQUIRE(review.HasValue());
        REQUIRE(review.Value().files.size() == 2);
        CHECK(review.Value().files[0].path == one.Identity().key.source);
        CHECK(review.Value().files[1].path == two.Identity().key.source);
        const auto selected = SourceReviewOverlay::Capture(review.Value().proposal, *review.Value().plan, 1);
        REQUIRE(selected.HasValue());
        CHECK(selected.Value().Source().SharesStateWith(two));
        REQUIRE(owner.Edit(two.Identity().instance, {two.Revision(), 0, 3, "new"}).HasValue());
        CHECK_FALSE(context.IsCurrent());
        CHECK_FALSE(context.MatchesOpenSource(both));
        CHECK(SourceReviewOverlay::Capture(review.Value().proposal, *review.Value().plan, 1).HasError());
        CHECK(SourceReviewOverlay::Capture(review.Value().proposal, *review.Value().plan, 0).HasValue());
    }

    TEST_CASE("A source review checkpoint restores after reopening exact content and rejects different evidence", "[editor][agent][gui]") {
        using namespace Proposals;
        TestSupport::SourceProject project;
        project.Put("one two\n", "file.cpp");
        const auto path = project.directory / "file.cpp";
        SourceDocumentService first{project.directory};
        const auto identity = project.Identity("file.cpp", 1);
        REQUIRE(first.Open(identity, path).HasValue());
        REQUIRE(first.Edit(identity.instance, {1, 0, 3, "new"}).HasValue());
        auto original = first.Undo(identity.instance, 2).Value().snapshot;
        REQUIRE(original.Revision() == 3);
        auto review = PrepareEditorLocalPatchReview(
                          R"({"version":1,"files":[{"kind":"edit","path":"file.cpp","hunks":[
                            {"offset":0,"before":"one","after":"three"},{"offset":4,"before":"two","after":"four"}
                          ]}]})",
                          {{original}, "test", 1}, "layout-test", 3)
                          .Value();
        NativeDurableFileSystem files;
        const auto stored = project.directory / ".horo/local/agent/source-review";
        REQUIRE_FALSE(LoadEditorPatchCheckpoint(project.directory, "layout-test", {}, {}).Value());
        REQUIRE(review.decisions.Decide(*review.plan, {0, 0}, ReviewChoice::Accept).HasValue());
        REQUIRE(review.decisions.Decide(*review.plan, {0, 1}, ReviewChoice::Reject).HasValue());
        CHECK_FALSE(std::filesystem::exists(stored));
        REQUIRE(
            SaveEditorPatchCheckpoint(files, project.directory, {review.proposal, review.decisions, review.offeredPaths}, {}).HasValue());

        SourceDocumentService reopened{project.directory};
        const auto latest = reopened.Open(identity, path).Value();
        CHECK(latest.Revision() == 1);
        auto loaded = LoadEditorPatchCheckpoint(project.directory, "layout-test", std::span{&latest, 1}, {});
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value());
        auto restored = std::move(loaded).Value().value();
        CHECK(restored.proposal.Digest() == review.proposal.Digest());
        CHECK(restored.decisions.State().Value() == ReviewState::Mixed);
        CHECK(restored.decisions.State({0, 0}).Value() == ReviewState::Accepted);
        CHECK(restored.decisions.State({0, 1}).Value() == ReviewState::Rejected);
        REQUIRE(SourceReviewOverlay::Capture(restored.proposal, *restored.plan, 0).HasValue());
        REQUIRE(restored.decisions.Decide(*restored.plan, {0, 1}, ReviewChoice::Accept).HasValue());
        REQUIRE(restored.decisions.Undo(*restored.plan).Value());
        CHECK(restored.decisions.State().Value() == ReviewState::Mixed);
        CHECK(latest.IsCurrent());
        CHECK(latest.Text() == "one two\n");
        CHECK(LoadEditorPatchCheckpoint(project.directory, "other-project", std::span{&latest, 1}, {}).HasError());
        CHECK(LoadEditorPatchCheckpoint(project.directory, "layout-test", {}, {}).HasError());

        const auto changed = reopened.Edit(identity.instance, {1, 0, 3, "new"}).Value();
        CHECK(LoadEditorPatchCheckpoint(project.directory, "layout-test", std::span{&changed, 1}, {}).HasError());
        {
            std::ofstream corrupt{stored, std::ios::binary | std::ios::app};
            corrupt << "trailing bytes";
            REQUIRE(corrupt.good());
        }
        CHECK(LoadEditorPatchCheckpoint(project.directory, "layout-test", std::span{&latest, 1}, {}).HasError());
    }

    TEST_CASE("Chat checks detached proposed HoroScript and hides results after the source changes", "[editor][agent][source]") {
        TestSupport::SourceProject project;
        project.Put("return 1", "check.lua");
        SourceFileOpenService owner{project.directory};
        const auto base = *owner.Open({.path = "check.lua"}).Value().sourceSnapshot;
        const std::array sources{base};
        const bool invalid = GENERATE(false, true);
        const auto *replacement = invalid ? "local =" : "error('must not execute')";
        const auto *expected = invalid ? "The proposed script has a syntax error." : "Proposed script syntax passed.";
        const std::string patch =
            std::string{R"({"version":1,"files":[{"kind":"edit","path":"check.lua","hunks":[{"offset":0,"before":"return 1","after":")"} +
            replacement + R"("}]}]})";
        auto prepared = PrepareEditorLocalPatchReview(patch, {{base}, "test", 1}, "layout-test", 1);
        REQUIRE(prepared.HasValue());
        auto review = std::move(prepared).Value();
        NativeDurableFileSystem files;
        REQUIRE(
            SaveEditorPatchCheckpoint(files, project.directory, {review.proposal, review.decisions, review.offeredPaths}, {}).HasValue());
        LocalAgentFixture fixture{"en-US", [&project, &sources](PanelContext &panel) {
            panel.projectFileRoot = &project.directory;
            panel.openSourceSnapshots = [&sources] {
                return std::span<const SourceDocumentSnapshot>{sources};
            };
            panel.requestSourceReview = [](const auto &, const auto &, auto, auto &) {
                return false;
            };
        }};
        fixture.Draw(420);
        fixture.Activate("###source_patch_checkpoint_load");
        REQUIRE(Await([&fixture] {
            return fixture.DrawnText().find("Check proposed script syntax") != std::string::npos;
        }));
        fixture.Activate("###source_patch_check_syntax");
        REQUIRE(Await([&fixture, expected] {
            return fixture.DrawnText().find(expected) != std::string::npos;
        }));
        CHECK(fixture.command.command == EditorWorkspaceViewCommand::None);
        CHECK(owner.Documents().Snapshot(base.Identity().instance).Value().SharesStateWith(base));
        std::ifstream disk{project.directory / "check.lua"};
        CHECK(std::string(std::istreambuf_iterator<char>{disk}, {}) == "return 1");
        REQUIRE(owner.Documents().Edit(base.Identity().instance, {base.Revision(), 7, 1, "2"}).HasValue());
        const auto stale = fixture.DrawnText();
        CHECK(stale.find(expected) == std::string::npos);
        CHECK(stale.find("The source changed after this request.") != std::string::npos);
    }

    TEST_CASE("Source review validation requires distinct approvals for build and tests", "[editor][agent][source]") {
        using enum Proposals::SourceReviewValidation;
        Proposals::SourceTransactionStatus state{.audit = std::make_shared<const Proposals::SourceReviewAudit>(),
                                                 .disposition = Proposals::SourceReviewDisposition::Saved,
                                                 .validation = Ready};
        std::optional<SourceReviewValidationAction> requested;
        LocalAgentFixture fixture{"en-US", [&](PanelContext &panel) {
            panel.sourceReviewTransaction = [&] {
                return state;
            };
            panel.requestSourceReviewValidation = [&](const auto action, const auto &review, const auto &lifetime, auto &) {
                CHECK(review.audit == state.audit);
                CHECK_FALSE(lifetime.IsCancellationRequested());
                requested = action;
            };
        }};
        const auto ready = fixture.DrawnText();
        CHECK(ready.find("Approve build") != std::string::npos);
        CHECK(ready.find("Approve tests") == std::string::npos);
        CHECK_FALSE(requested);
        fixture.Activate("###source_validation_approve_build");
        CHECK(requested == SourceReviewValidationAction::Build);
        requested.reset();
        state.validation = BuildPassed;
        state.canRunTests = true;
        CHECK(fixture.DrawnText().find("Approve tests") != std::string::npos);
        CHECK_FALSE(requested);
        fixture.Activate("###source_validation_approve_test");
        CHECK(requested == SourceReviewValidationAction::Test);
        requested.reset();
        state.validation = Stale;
        state.canRunTests = false;
        const auto stale = fixture.DrawnText();
        CHECK(stale.find("Validation is stale.") != std::string::npos);
        CHECK(stale.find("Approve tests") == std::string::npos);
    }

    TEST_CASE("Structural confirmation follows accepted files in a restored review", "[editor][agent][source]") {
        using namespace Proposals;
        TestSupport::SourceProject project;
        project.Put("one", "edit.cpp");
        project.Put("remove", "remove.cpp");
        SourceFileOpenService owner{project.directory};
        const auto edited = *owner.Open({.path = "edit.cpp"}).Value().sourceSnapshot;
        const auto removed = *owner.Open({.path = "remove.cpp"}).Value().sourceSnapshot;
        const std::array sources{edited, removed};
        auto prepared = PrepareEditorLocalPatchReview(
            R"({"version":1,"files":[{"kind":"edit","path":"edit.cpp","hunks":[{"offset":0,"before":"one","after":"two"}]},
                {"kind":"delete","path":"remove.cpp"}]})",
            {{edited, removed}, "test", 1}, "layout-test", 1);
        REQUIRE(prepared.HasValue());
        auto review = std::move(prepared).Value();
        REQUIRE(review.decisions.Decide(*review.plan, {}, ReviewChoice::Accept).HasValue());
        NativeDurableFileSystem files;
        REQUIRE(
            SaveEditorPatchCheckpoint(files, project.directory, {review.proposal, review.decisions, review.offeredPaths}, {}).HasValue());
        SourceReviewTransactions transactions;
        const auto context = [&](PanelContext &panel) {
            panel.projectFileRoot = &project.directory;
            panel.openSourceSnapshots = [&] {
                return std::span<const SourceDocumentSnapshot>{sources};
            };
            panel.requestSourceReview = [](const auto &, const auto &, const auto, auto &) {
                return false;
            };
            panel.requestSourceReviewApply = [](const auto &decisions, const auto &policy, const auto &plan, const auto &lifetime,
                                                auto &command) {
                return QueueSourceReviewApplication("layout-test", decisions, policy, plan, lifetime, command);
            };
            panel.sourceReviewTransaction = [&] {
                return transactions.Status();
            };
        };
        LocalAgentFixture fixture{"en-US", context};
        fixture.Draw(420);
        fixture.Activate("###source_patch_checkpoint_load");
        REQUIRE(Await([&] {
            return fixture.DrawnText().find("Saved source review restored.") != std::string::npos;
        }));
        fixture.Activate("###source_patch_apply");
        CHECK(fixture.command.command == EditorWorkspaceViewCommand::None);
        bool mixed{};
        SECTION("A selected deletion keeps its dedicated confirmation") {
            REQUIRE(fixture.ChatWindow());
            ImGui::FocusWindow(fixture.ChatWindow());
            ImGui::ActivateItemByID(ImHashStr("###source_patch_reject_file", 0, fixture.ChatWindow()->GetID(0)));
            fixture.Draw(420);
            fixture.Activate("###source_patch_confirm_delete");
        }
        SECTION("The group confirmation applies the deletion and text edit together") {
            mixed = true;
            fixture.Activate("###source_patch_confirm_moves");
        }
        REQUIRE(fixture.command.command == EditorWorkspaceViewCommand::ApplySourceReview);
        CHECK(std::filesystem::is_regular_file(project.directory / "remove.cpp"));
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .ioWorkerCount = 1}};
        ProjectMutationCoordinator mutations{files};
        REQUIRE(transactions.Process(std::exchange(fixture.command, {}), owner.Documents(), &jobs, project.directory, &files, &owner));
        REQUIRE(Await([&] {
            return transactions.Poll(owner.Documents(), &files, &owner, nullptr, &mutations);
        }));
        CHECK(transactions.Status().feedback == (mixed ? SourceTransactionFeedback::Applied : SourceTransactionFeedback::Deleted));
        CHECK_FALSE(std::filesystem::exists(project.directory / "remove.cpp"));
        CHECK(edited.IsCurrent() != mixed);
        const auto current = owner.Documents().Snapshot(edited.Identity().instance);
        REQUIRE(current.HasValue());
        CHECK(current.Value().Text() == (mixed ? "two" : "one"));
    }

    TEST_CASE("The file picker sends the selected sources and routes each returned card", "[.][chat-http]") {
        const auto *endpoint = std::getenv("HORO_MODEL_TEST_ENDPOINT");
        REQUIRE(endpoint);
        TestSupport::SourceProject project;
        constexpr auto longPath = "two_very_long_source_name_for_a_narrow_review_card_in_the_local_agent_panel.cpp";
        project.Put("one\n", "one.cpp");
        project.Put("two\n", longPath);
        SourceDocumentService owner{project.directory};
        const auto one = owner.Open(project.Identity("one.cpp", 1), project.directory / "one.cpp").Value();
        const auto two = owner.Open(project.Identity(longPath, 2), project.directory / longPath).Value();
        const std::array open{one, two};
        std::vector<DocumentInstanceId> selections;
        SourceReviewTransactions transactions;
        const auto configure = [&](PanelContext &context) {
            context.projectFileRoot = &project.directory;
            context.openSourceSnapshots = [&]() -> std::span<const SourceDocumentSnapshot> {
                return open;
            };
            context.requestSourceReview = [&](const Proposals::ChangeSet &proposal, const Proposals::WorkspaceEditPlan &plan,
                                              const std::size_t operation, EditorWorkspaceViewCommandData &) {
                const auto overlay = SourceReviewOverlay::Capture(proposal, plan, operation);
                REQUIRE(overlay.HasValue());
                selections.push_back(overlay.Value().Source().Identity().instance);
                return true;
            };
            context.requestSourceReviewApply = [](const auto &decisions, const auto &policy, const auto &plan, const auto &lifetime,
                                                  auto &command) {
                return QueueSourceReviewApplication("layout-test", decisions, policy, plan, lifetime, command);
            };
            context.requestSourceReviewUndo = [&](const auto &lifetime, auto &command) {
                return transactions.QueueUndo(lifetime, command);
            };
            context.sourceReviewTransaction = [&] {
                return transactions.Status();
            };
        };
        LocalAgentFixture fixture{"en-US", configure};
        JobSystem sourceJobs{JobSystemConfig{.workerCount = 1, .ioWorkerCount = 1}};
        fixture.Draw(420);
        fixture.Enter("##Endpoint", endpoint);
        fixture.Enter("##Model", "editor-multi-inline-patch");
        fixture.Enter("##Prompt", "Propose two source edits");
        fixture.Activate("###context_file_offer");
        const auto select = [&](const ImGuiID seed, const char *id) {
            ImGui::FocusWindow(fixture.ChatWindow());
            ImGui::ActivateItemByID(ImHashStr(id, 0, seed));
            fixture.Draw(420);
        };
        select(fixture.ChatWindow()->GetID("2"), "###context_file_add");
        select(fixture.ChatWindow()->GetID("1"), "###context_file_add");
        fixture.Activate("###context_files_preview");
        REQUIRE(fixture.DrawnText().find(longPath) != std::string::npos);
        fixture.Activate("###context_approve");
        REQUIRE(Await([&] {
            const auto response = fixture.DrawnText();
            const auto first = response.find("Awaiting review");
            return first != std::string::npos && response.find("Awaiting review", first + 1) != std::string::npos;
        }));
        fixture.Draw(350);
        const auto narrow = fixture.Draw(350, true, true);
        REQUIRE(fixture.ChatWindow());
        CHECK(fixture.ChatWindow()->ContentSizeIdeal.x <= fixture.ChatWindow()->InnerRect.GetWidth());
        CHECK(narrow.find(longPath) != std::string::npos);
        select(fixture.ChatWindow()->GetID(1), "###source_patch_review");
        select(fixture.ChatWindow()->GetID(0), "###source_patch_review");
        CHECK(selections == std::vector{two.Identity().instance, one.Identity().instance});
        select(fixture.ChatWindow()->GetID(0), "###source_patch_hunks");
        CHECK(fixture.DrawnText().find("Change 1/2") != std::string::npos);
        select(fixture.ChatWindow()->GetID(0), "###source_patch_next_hunk");
        CHECK(fixture.DrawnText().find("Change 2/2") != std::string::npos);
        select(fixture.ChatWindow()->GetID(0), "###source_patch_accept_hunk");
        CHECK(fixture.DrawnText().find("Mixed decisions") != std::string::npos);
        fixture.Activate("###source_patch_undo");
        CHECK(fixture.DrawnText().find("Mixed decisions") == std::string::npos);
        select(fixture.ChatWindow()->GetID(0), "###source_patch_accept_file");
        CHECK(fixture.DrawnText().find("Accepted") != std::string::npos);
        fixture.Activate("###source_patch_reject");
        CHECK(fixture.DrawnText().find("Rejected") != std::string::npos);
        select(fixture.ChatWindow()->GetID(0), "###source_patch_review");
        CHECK(selections.size() == 2);
        fixture.Activate("###source_patch_undo");
        CHECK(fixture.DrawnText().find("Accepted") != std::string::npos);
        fixture.Activate("###source_patch_checkpoint_save");
        REQUIRE(Await([&] {
            return fixture.DrawnText().find("Source review saved in this project.") != std::string::npos;
        }));
        fixture.panels.DetachAll();
        PanelContext next{.dataBus = fixture.gui.editorEvents,
                          .inputRouter = &fixture.gui.input,
                          .workspaceInputContext = &fixture.workspace,
                          .projectIdentity = "layout-test"};
        configure(next);
        fixture.panels.AttachAll(next);
        CHECK(fixture.DrawnText().find("Accepted") == std::string::npos);
        fixture.Activate("###source_patch_checkpoint_load");
        REQUIRE(Await([&] {
            return fixture.DrawnText().find("Saved source review restored.") != std::string::npos;
        }));
        CHECK(fixture.DrawnText().find("Accepted") != std::string::npos);
        CHECK(one.IsCurrent());
        CHECK(two.IsCurrent());
        select(fixture.ChatWindow()->GetID(0), "###source_patch_hunks");
        select(fixture.ChatWindow()->GetID(0), "###source_patch_reject_hunk");
        select(fixture.ChatWindow()->GetID(1), "###source_patch_accept_file");
        fixture.Activate("###source_patch_apply");
        REQUIRE(fixture.command.command == EditorWorkspaceViewCommand::ApplySourceReview);
        CHECK(one.IsCurrent());
        CHECK(two.IsCurrent());
        REQUIRE(transactions.Process(std::exchange(fixture.command, {}), owner, &sourceJobs, project.directory, &fixture.files));
        CHECK(fixture.DrawnText().find("Preparing accepted source changes") != std::string::npos);
        CHECK(one.IsCurrent());
        CHECK(two.IsCurrent());
        REQUIRE(Await([&] {
            return transactions.Poll(owner, &fixture.files);
        }));
        CHECK(fixture.DrawnText().find("Accepted changes applied to the open files.") != std::string::npos);
        const auto appliedOne = owner.Snapshot(one.Identity().instance).Value();
        const auto appliedTwo = owner.Snapshot(two.Identity().instance).Value();
        CHECK(appliedOne.Text() == "onE\n");
        CHECK(appliedTwo.Text() == "new two\n");
        CHECK(appliedOne.Dirty());
        CHECK(appliedTwo.Dirty());
        const auto audit = transactions.Status().audit;
        REQUIRE(audit);
        CHECK(fixture.DrawnText().find("Last applied proposal") != std::string::npos);
        REQUIRE(audit->choices.size() == 2);
        CHECK(audit->choices[0] == std::vector{Proposals::ReviewState::Rejected, Proposals::ReviewState::Accepted});
        CHECK(audit->choices[1] == std::vector{Proposals::ReviewState::Accepted});
        REQUIRE(audit->revisions.size() == 2);
        CHECK(audit->revisions[0].file == 0);
        CHECK(audit->revisions[0].before == one.Revision());
        CHECK(audit->revisions[0].after == appliedOne.Revision());
        CHECK(audit->revisions[1].file == 1);
        CHECK(audit->revisions[1].before == two.Revision());
        CHECK(audit->revisions[1].after == appliedTwo.Revision());
        const auto redacted = Proposals::EncodeAudit(audit->proposal);
        REQUIRE(redacted.HasValue());
        CHECK(redacted.Value().find("one.cpp") == std::string::npos);
        CHECK(redacted.Value().find("new two") == std::string::npos);
        CHECK(redacted.Value().find("ollama") == std::string::npos);
        SourceDocumentService disk(project.directory);
        CHECK(disk.Open(project.Identity("one.cpp", 1), project.directory / "one.cpp").Value().Text() == one.Text());
        CHECK(disk.Open(project.Identity(longPath, 2), project.directory / longPath).Value().Text() == two.Text());
        fixture.Activate("###source_patch_revert");
        REQUIRE(fixture.command.command == EditorWorkspaceViewCommand::UndoSourceReview);
        fixture.panels.DetachAll();
        REQUIRE(transactions.Process(std::exchange(fixture.command, {}), owner, &sourceJobs, project.directory, &fixture.files));
        CHECK(transactions.Status().feedback == Proposals::SourceTransactionFeedback::Cancelled);
        CHECK(appliedOne.IsCurrent());
        CHECK(appliedTwo.IsCurrent());
        fixture.panels.AttachAll(next);
        CHECK(fixture.DrawnText().find("The source review action was cancelled.") != std::string::npos);
        fixture.Activate("###source_patch_revert");
        REQUIRE(fixture.command.command == EditorWorkspaceViewCommand::UndoSourceReview);
        REQUIRE(transactions.Process(std::exchange(fixture.command, {}), owner, &sourceJobs, project.directory, &fixture.files));
        CHECK(fixture.DrawnText().find("Applied changes were undone together.") != std::string::npos);
        CHECK_FALSE(transactions.Status().canUndo);
        CHECK(transactions.Status().audit == audit);
        CHECK(owner.Snapshot(one.Identity().instance).Value().Text() == one.Text());
        CHECK(owner.Snapshot(two.Identity().instance).Value().Text() == two.Text());
    }
}  // namespace
