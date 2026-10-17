#include "editor/modals/settings/SettingsModalCollisionInternal.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "support/CollisionSettingsTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>
#include <memory>
#include <string_view>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using Kind = SettingsModalInternal::CollisionItemKind;

    /** @brief Keeps the real project and session alive through every rendered frame and queued keyboard gesture. */
    struct CollisionPresentationFixture final {
        Horo::Tests::CollisionSettings::Project project;
        std::shared_ptr<CollisionProjectSettingsSession> owner{project.Open()};
        Editor::Tests::EditorGuiContextFixture gui{{1200, 3800}};
        SettingsState settings;
        ImVec2 size{1100, 3600};

        CollisionPresentationFixture() {
            const auto catalog = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/en-US.json";
            LocalizationError error;
            REQUIRE(gui.localization.LoadCatalogFile(catalog, &error));
            settings.collision.owner = owner;
            RefreshCollisionDraft(settings.collision, *owner);
        }

        /** @brief Draws real shared controls and queues navigation activation, without directly invoking an authoring command. */
        void Frame(const std::string_view action = {}) {
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            ImGui::Begin("CollisionPresentation", nullptr, ImGuiWindowFlags_NoSavedSettings);
            const auto label = action.empty() ? std::string{} : gui.localization.Get("editor", action);
            const auto id = ImGui::GetID(label.c_str());
            SettingsModalInternal::DrawCollision(settings, gui.context);
            if (!action.empty())
                ImGui::ActivateItemByID(id);
            ImGui::End();
            gui.imgui.EndFrame();
            REQUIRE(ImGui::GetDrawData() != nullptr);
            REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
        }

        /** @brief Consumes a queued keyboard gesture in a separately rendered frame. */
        void Page(const std::string_view action = {}) {
            Frame(action);
            if (!action.empty())
                Frame();
        }
    };

    /** @brief Restores the shared theme while its ImGui context is still alive. */
    struct RestoreCollisionTheme final {
        Theme::Preset preset{Theme::GetThemePreset()};

        ~RestoreCollisionTheme() {
            Theme::SetThemePreset(preset);
        }
    };
}  // namespace

TEST_CASE("Collision presentation and disabled actions never publish canonical project settings", "[editor][gui][collision]") {
    CollisionPresentationFixture fixture;
    const auto before = fixture.owner->Snapshot().Value();
    for (const auto kind : {Kind::Layer, Kind::Profile, Kind::Channel}) {
        fixture.settings.collision.kind = static_cast<int>(kind);
        fixture.Page();
        CHECK(fixture.owner->Snapshot().Value() == before);
        CHECK_FALSE(fixture.settings.collision.pending);
    }
    fixture.Page("settings.collision.create");
    fixture.Page("settings.collision.save");
    CHECK(fixture.owner->Snapshot().Value() == before);
    CHECK_FALSE(fixture.owner->IsDirty());
    CHECK_FALSE(fixture.settings.collision.error);
    fixture.settings.collision.kind = static_cast<int>(Kind::Layer);
    const bool dynamic = fixture.settings.collision.input->schema.layers.front().admitsDynamic;
    fixture.Page("settings.collision.dynamic");
    CHECK(fixture.settings.collision.input->schema.layers.front().admitsDynamic != dynamic);
    CHECK(fixture.settings.collision.pending);
    CHECK(fixture.owner->Snapshot().Value() == before);
    fixture.Page("settings.collision.reset_candidate");
    CHECK_FALSE(fixture.settings.collision.pending);
}

TEST_CASE("Collision controls require explicit review commit replay and durable save gestures", "[editor][gui][collision]") {
    CollisionPresentationFixture fixture;
    const auto before = fixture.owner->Snapshot().Value();
    fixture.settings.collision.newName = "Actors";
    fixture.Page("settings.collision.create");
    REQUIRE(fixture.settings.collision.pending);
    REQUIRE(fixture.settings.collision.input->layers.size() == 2);
    CHECK(fixture.owner->Snapshot().Value() == before);
    fixture.Page("settings.collision.review");
    REQUIRE(fixture.settings.collision.review);
    REQUIRE(fixture.settings.collision.review->CanCommit());
    fixture.Page("settings.collision.commit");
    REQUIRE(fixture.owner->IsDirty());
    CHECK(fixture.owner->Snapshot().Value()->Input().layers.size() == 2);
    fixture.Page("settings.collision.undo");
    CHECK(fixture.owner->Snapshot().Value()->Input().layers.size() == 1);
    fixture.Page("settings.collision.redo");
    CHECK(fixture.owner->Snapshot().Value()->Input().layers.size() == 2);
    fixture.Page("settings.collision.save");
    REQUIRE_FALSE(fixture.owner->IsDirty());
    fixture.owner->Close();
    fixture.Page();
    const auto reopened = fixture.project.Open();
    CHECK(reopened->Snapshot().Value()->Input().layers.size() == 2);
}

TEST_CASE("Collision removal keeps canonical owners until review and reset restores each definition kind", "[editor][gui][collision]") {
    for (const auto kind : {Kind::Layer, Kind::Profile, Kind::Channel}) {
        DYNAMIC_SECTION("definition kind " << static_cast<int>(kind)) {
            CollisionPresentationFixture fixture;
            const auto before = fixture.owner->Snapshot().Value();
            fixture.settings.collision.kind = static_cast<int>(kind);
            fixture.Page("settings.collision.remove_candidate");
            REQUIRE(fixture.settings.collision.pending);
            CHECK(fixture.owner->Snapshot().Value() == before);
            if (kind == Kind::Layer) {
                fixture.Page("settings.collision.review");
                REQUIRE(fixture.settings.collision.error);
                CHECK_FALSE(fixture.settings.collision.review);
                CHECK_FALSE(fixture.owner->IsDirty());
            }
            fixture.Page("settings.collision.reset_candidate");
            REQUIRE(fixture.settings.collision.input);
            const auto restored = Application::ProjectCollisionSettings::Create(*fixture.settings.collision.input);
            REQUIRE(restored.HasValue());
            const auto encoded = Application::EncodeProjectCollisionSettings(restored.Value());
            REQUIRE(encoded.HasValue());
            CHECK(encoded.Value() == Application::EncodeProjectCollisionSettings(*before).Value());
            CHECK_FALSE(fixture.settings.collision.pending);
            CHECK_FALSE(fixture.settings.collision.error);
        }
    }
}

TEST_CASE("Collision feedback renders missing projection unavailable and revoked owners without adopting authority",
          "[editor][gui][collision]") {
    CollisionPresentationFixture fixture;
    const auto before = fixture.owner->Snapshot().Value();
    SECTION("missing projection") {
        fixture.settings.collision.input.reset();
        fixture.Page();
        CHECK(fixture.owner->Snapshot().Value() == before);
    }
    SECTION("unavailable owner") {
        fixture.settings.collision.owner.reset();
        fixture.settings.collision.error = MakeError(CollisionSettingsError(CollisionSettingsFailure::Unsupported));
        fixture.Page();
        CHECK(fixture.owner->Snapshot().Value() == before);
    }
    SECTION("revoked owner") {
        fixture.owner->Close();
        fixture.Page();
        CHECK(fixture.owner->Snapshot().HasError());
    }
}

TEST_CASE("Collision controls preserve runtime authority in narrow Turkish layouts across themes", "[editor][gui][collision]") {
    for (const auto preset : {Theme::Preset::HoroDark, Theme::Preset::Light}) {
        DYNAMIC_SECTION("theme " << static_cast<int>(preset)) {
            CollisionPresentationFixture fixture;
            RestoreCollisionTheme restore;
            Theme::SetThemePreset(preset);
            if (preset == Theme::Preset::Light)
                ImGui::StyleColorsLight();
            else
                ImGui::StyleColorsDark();
            const auto catalog = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/tr-TR.json";
            LocalizationError error;
            REQUIRE(fixture.gui.localization.LoadCatalogFile(catalog, &error));
            REQUIRE(fixture.gui.localization.Prepare(LocaleTag{"tr-TR"}, &error));
            REQUIRE(fixture.gui.localization.ActivatePrepared(&error));
            fixture.size = {420, 3600};
            ImGui::GetIO().FontGlobalScale = 1.25F;
            const auto before = fixture.owner->Snapshot().Value();
            std::uint64_t revision = 2;
            for (const auto state : {CollisionActivationState::NoPhysics, CollisionActivationState::Inactive,
                                     CollisionActivationState::Unknown, CollisionActivationState::Active}) {
                CollisionActivationEvidence evidence{.revision = revision++, .state = state};
                if (state == CollisionActivationState::Active) {
                    evidence.projectId = "collision-project";
                    evidence.documentRevision = before->Input().revision;
                    evidence.generation = 8;
                    evidence.fingerprint = before->Schema()->SemanticFingerprint();
                    evidence.playRunning = true;
                }
                REQUIRE(fixture.owner->ObserveActivation(evidence).HasValue());
                fixture.Page();
                if (state == CollisionActivationState::Active) {
                    fixture.Page("settings.collision.dynamic");
                    fixture.Page("settings.collision.review");
                    REQUIRE(fixture.settings.collision.review);
                    CHECK_FALSE(fixture.settings.collision.review->CanCommit());
                    fixture.Page("settings.collision.commit");
                }
                CHECK(fixture.owner->Snapshot().Value() == before);
                CHECK_FALSE(fixture.owner->IsDirty());
            }
        }
    }
}
