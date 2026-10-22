#include "editor/renderer/EditorGuiComposition.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace Horo::Editor {
    namespace {
        struct GuiPreparationFixture {
            struct DestroyContext {
                void operator()(ImGuiContext *context) const noexcept {
                    ImGui::DestroyContext(context);
                }
            };

            std::unique_ptr<ImGuiContext, DestroyContext> context{ImGui::CreateContext()};
            EditorGuiComposition composition;
            std::size_t resolved{};
            const Render::RenderTextureViewHandle view{{7}, 9, 3};

            GuiPreparationFixture() {
                ImGui::SetCurrentContext(context.get());
                auto &io = ImGui::GetIO();
                io.DisplaySize = {320, 240};
                io.DeltaTime = 1.0F / 60.0F;
                REQUIRE(io.Fonts->Build());
                io.Fonts->SetTexID(1);
                ImGui::NewFrame();
            }

            void Image(const std::uintptr_t identity) {
                ImGui::GetForegroundDrawList()->AddImage(static_cast<ImTextureID>(identity), {0, 0}, {32, 32});
            }

            Result<Render::GuiCompositionPreparation> Prepare() {
                ImGui::Render();
                return composition.Prepare(Render::FrameToken{11}, Render::GuiCompositionMode::BorrowedActiveRecording, 1,
                                           [this](const std::uintptr_t identity) -> Result<Render::GuiSampledResource> {
                    ++resolved;
                    return identity == 2 ? Result<Render::GuiSampledResource>::Success({view, {}})
                                         : Result<Render::GuiSampledResource>::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
                });
            }
        };
    }  // namespace

    TEST_CASE_METHOD(GuiPreparationFixture, "GUI preparation resolves drawn frontend generations once and rejects stale request identity",
                     "[editor][gui][resource]") {
        Image(2);
        Image(2);
        const auto prepared = Prepare();
        REQUIRE(prepared.HasValue());
        REQUIRE(prepared.Value().resources.size() == 1);
        CHECK(prepared.Value().resources[0].view == view);
        CHECK(resolved == 1);
        const std::array<Render::GuiSampledResourceInstance, 1> instances{{{42, 0}}};
        Render::GuiCompositionRequest request{.frame = {11}, .resources = instances};
        CHECK(composition.Matches(request));
        request.frame = {12};
        CHECK_FALSE(composition.Matches(request));
        request.frame = {11};
        composition.Reset();
        CHECK_FALSE(composition.Matches(request));
    }

    TEST_CASE_METHOD(GuiPreparationFixture, "Only the exact privately owned font identity bypasses frontend sampled pins",
                     "[editor][gui][resource]") {
        SECTION("current native font is known") {
            Image(1);
            const auto prepared = Prepare();
            REQUIRE(prepared.HasValue());
            CHECK(prepared.Value().resources.empty());
            CHECK(resolved == 0);
        }
        SECTION("unknown image cannot pretend to be font") {
            Image(3);
            CHECK(Prepare().HasError());
            CHECK(resolved == 1);
        }
        SECTION("zero image is invalid") {
            Image(0);
            CHECK(Prepare().HasError());
            CHECK(resolved == 0);
        }
    }

    TEST_CASE_METHOD(GuiPreparationFixture, "GUI preparation rejects callbacks that could introduce undeclared native samples",
                     "[editor][gui][resource]") {
        ImGui::GetForegroundDrawList()->AddCallback([](const ImDrawList *, const ImDrawCmd *) {
        }, nullptr);
        CHECK(Prepare().HasError());
    }
}  // namespace Horo::Editor
