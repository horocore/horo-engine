#pragma once

/** @file ViewportPanelRenderSupport.h
 * @brief Private render-surface and localization fixtures shared by one viewport test target.
 */
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/renderer/EditorViewportRenderer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Horo::Editor::Tests::ViewportRenderSupport {
    /** @brief Retain stable text for actual ImGui controls without a project catalog dependency. */
    class TestLocalization final : public Horo::Editor::ILocalizationService {
    public:
        [[nodiscard]] const std::string &Get(const std::string_view, const std::string_view localKey) const override {
            std::string_view value = localKey;
            if (localKey == "workspace.viewport.object_count") {
                value = "{} objects";
            }
            const auto [entry, inserted] = values_.try_emplace(std::string(localKey), value);
            static_cast<void>(inserted);
            return entry->second;
        }

    private:
        mutable std::unordered_map<std::string, std::string> values_;
    };

    /** @brief Supply only the viewport texture surface and capture neutral renderer requests; no UI behavior is mocked. */
    class FakeViewportRenderer final : public Horo::Editor::IEditorViewportRenderer {
    public:
        void RequestExtent(const Horo::Editor::EditorViewportExtent extent) noexcept override {
            requestedExtent = extent;
        }

        void RequestGrid(const Horo::Editor::EditorViewportGridOptions &options) noexcept override {
            gridOptions = options;
        }

        void RequestLightVisualizer(const Horo::Editor::EditorViewportLightVisualizerOptions &options) noexcept override {
            lightVisualizerOptions = options;
        }

        [[nodiscard]] Horo::Editor::EditorViewportExtent RequestedExtent() const noexcept override {
            return requestedExtent;
        }

        [[nodiscard]] Horo::Math::ClipDepthRange ClipDepthRange() const noexcept override {
            return depthRange;
        }

        [[nodiscard]] Horo::Result<void> ExecuteStaticMeshPass(const Horo::Render::StaticMeshPassDescriptor &) override {
            return Horo::Result<void>::Success();
        }

        [[nodiscard]] Horo::Editor::EditorViewportTextureView TextureView() const noexcept override {
            return Horo::Editor::EditorViewportTextureView{
                .textureId = textureId,
                .u0 = 0.25F,
                .v0 = 0.75F,
                .u1 = 0.75F,
                .v1 = 0.25F,
            };
        }

        [[nodiscard]] bool IsReady() const noexcept override {
            return ready;
        }

        bool ready{true};
        Horo::Editor::EditorViewportExtent requestedExtent{};
        Horo::Editor::EditorViewportGridOptions gridOptions{};
        Horo::Editor::EditorViewportLightVisualizerOptions lightVisualizerOptions{};
        Horo::Math::ClipDepthRange depthRange{Horo::Math::ClipDepthRange::ZeroToOne};
        static constexpr std::uintptr_t textureId = 42;
    };
}  // namespace Horo::Editor::Tests::ViewportRenderSupport
