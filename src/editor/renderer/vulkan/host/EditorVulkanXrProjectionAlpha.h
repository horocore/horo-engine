#pragma once
/** @file EditorVulkanXrProjectionAlpha.h
 * @brief Non-installed alpha policy and completion evidence for the selected Vulkan XR viewport.
 */
#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"
#include "Horo/Runtime/Render/RenderResource.h"
#include "Horo/Runtime/Render/RenderSubmission.h"

#include <cstdint>
#include <optional>

namespace Horo::Editor {
    class EditorViewportRendererVulkan;

    /** @brief Color treatment of the selected viewport's final XR projection attachment. */
    enum class EditorXrProjectionAlpha : std::uint8_t {
        Opaque,
        Premultiplied,
    };

    /** @brief Proof of a completed transparent-background projection into one exact acquisition.
     * @details This value proves the selected renderer's scene write. The App still owns acquisition release,
     * GUI retirement, platform consent and native compositor completion.
     */
    class EditorXrPremultipliedProjection final {
    public:
        /** @brief Returns the exact publication written by the renderer. */
        [[nodiscard]] const Render::MultiviewPublication &Publication() const noexcept {
            return publication_;
        }

        /** @brief Returns the exact acquired texture generation written by the renderer. */
        [[nodiscard]] Render::RenderTextureHandle Color() const noexcept {
            return color_;
        }

        /** @brief Returns the graphics completion point observed by the renderer. */
        [[nodiscard]] Render::RenderTimelinePoint Completion() const noexcept {
            return completion_;
        }

    private:
        friend class EditorViewportRendererVulkan;

        EditorXrPremultipliedProjection(Render::MultiviewPublication publication, Render::RenderTextureHandle color,
                                        Render::RenderTimelinePoint completion) noexcept
            : publication_{publication}, color_{color}, completion_{completion} {}

        Render::MultiviewPublication publication_;
        Render::RenderTextureHandle color_;
        Render::RenderTimelinePoint completion_;
    };

    /** @brief Fixed owner-lane evidence storage for a single prepared acquisition. */
    class EditorVulkanXrProjectionAlphaState final {
    public:
        /** @brief Binds premultiplied output identity after the canonical graph binding succeeds. */
        void Bind(EditorXrProjectionAlpha mode, const Render::MultiviewPublication &publication,
                  Render::RenderTextureHandle color) noexcept {
            Reset();
            if (mode == EditorXrProjectionAlpha::Premultiplied && publication.IsValid() && color.IsValid())
                input_ = Input{publication, color};
        }

        /** @brief Reports whether the current bound graph must use transparent black for uncovered pixels. */
        [[nodiscard]] bool IsPremultiplied() const noexcept {
            return input_.has_value();
        }

        /** @brief Associates only the completion address of a successfully submitted projection graph. */
        void Submit(Render::RenderTimelinePoint point) noexcept {
            if (input_ && point.IsValid())
                input_->submitted = point;
        }

        /** @brief Consumes the exact completed binding once, or preserves it when expected identity does not match.
         * @param publication Exact requested Scene/frame/origin/view/device/capability publication.
         * @param color Exact requested acquired texture generation.
         * @param observed Completion actually observed by the selected renderer's canonical graphics queue.
         * @return Exact successful projection completion point, or an empty result.
         */
        [[nodiscard]] std::optional<Render::RenderTimelinePoint> Take(const Render::MultiviewPublication &publication,
                                                                      Render::RenderTextureHandle color,
                                                                      Render::RenderTimelinePoint observed) noexcept {
            if (!input_ || !observed.IsValid() || input_->submitted != observed || input_->publication != publication ||
                input_->color != color)
                return std::nullopt;
            Reset();
            return observed;
        }

        /** @brief Revokes the current binding without releasing the queue or acquisition. */
        void Reset() noexcept {
            input_.reset();
        }

    private:
        struct Input final {
            Render::MultiviewPublication publication;
            Render::RenderTextureHandle color;
            Render::RenderTimelinePoint submitted{};
        };

        std::optional<Input> input_;
    };
}  // namespace Horo::Editor
