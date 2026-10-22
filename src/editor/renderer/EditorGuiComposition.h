#pragma once

/** @file EditorGuiComposition.h
 * @brief Bounded private preparation of actual drawn image identities before canonical GUI admission. */
#include "editor/renderer/EditorGuiRenderer.h"
#include "editor/renderer/EditorRendererErrors.h"

#include <algorithm>
#include <array>
#include <imgui.h>

namespace Horo::Editor {
    /** @brief Synchronous borrowed draw snapshot and exact frontend sample declarations; owns no image resource. */
    class EditorGuiComposition final {
    public:
        /** @brief Resolves every drawn image before recording, admitting only the exact known native-font identity as private.
         * @param frame Current renderer frame. @param mode Selected adapter's typed recording mode.
         * @param knownNativeFont Exact atlas created by this adapter; zero requires all images to have frontend identities.
         * @param resolve Callable resolving an opaque GUI image to its existing typed owner/generation.
         * @return Borrowed bounded declaration, or typed unknown/stale/callback/capacity failure. */
        template <typename Resolver>
        [[nodiscard]] Result<Render::GuiCompositionPreparation> Prepare(const Render::FrameToken frame,
                                                                        const Render::GuiCompositionMode mode,
                                                                        const std::uintptr_t knownNativeFont, const Resolver &resolve) {
            using Outcome = Result<Render::GuiCompositionPreparation>;
            count_ = 0;
            frame_ = {};
            draw_ = ImGui::GetDrawData();
            if (!frame.IsValid() || draw_ == nullptr || draw_->TotalVtxCount < 0 || draw_->TotalIdxCount < 0)
                return Outcome::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
            for (const ImDrawList *list : draw_->CmdLists) {
                if (list == nullptr)
                    return Outcome::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
                for (const ImDrawCmd &command : list->CmdBuffer) {
                    if (command.UserCallback != nullptr && command.UserCallback != ImDrawCallback_ResetRenderState)
                        return Outcome::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
                    if (command.UserCallback != nullptr || command.ElemCount == 0)
                        continue;
                    const auto id = static_cast<std::uintptr_t>(command.GetTexID());
                    if (id != 0 && id == knownNativeFont)
                        continue;
                    if (auto retained = Append(id, resolve); retained.HasError())
                        return Outcome::Failure(retained.ErrorValue());
                }
            }
            frame_ = frame;
            mode_ = mode;
            vertices_ = draw_->TotalVtxCount;
            indices_ = draw_->TotalIdxCount;
            return Outcome::Success({frame, mode, std::span{resources_}.first(count_)});
        }

        /** @brief Rejects stale or substituted requests before an adapter touches native command state. */
        [[nodiscard]] bool Matches(const Render::GuiCompositionRequest &request) const noexcept {
            return frame_.IsValid() && request.frame == frame_ && request.mode == mode_ && request.resources.size() == count_ &&
                   draw_ == ImGui::GetDrawData() && draw_ != nullptr && draw_->TotalVtxCount == vertices_ &&
                   draw_->TotalIdxCount == indices_;
        }

        /** @brief Resolves an already prepared opaque image to its exact request-array index. */
        [[nodiscard]] std::optional<std::size_t> Index(const std::uintptr_t identity) const noexcept {
            const auto identities = std::span{identities_}.first(count_);
            const auto found = std::ranges::find(identities, identity);
            return found == identities.end() ? std::nullopt
                                             : std::optional<std::size_t>{static_cast<std::size_t>(found - identities.begin())};
        }

        /** @brief Withdraws the synchronous snapshot on close, abort or a new GUI frame. */
        void Reset() noexcept {
            frame_ = {};
            draw_ = nullptr;
            count_ = 0;
        }

    private:
        template <typename Resolver> [[nodiscard]] Result<void> Append(const std::uintptr_t identity, const Resolver &resolve) {
            if (identity == 0)
                return Result<void>::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
            if (std::ranges::find(std::span{identities_}.first(count_), identity) != std::span{identities_}.first(count_).end())
                return Result<void>::Success();
            if (count_ == resources_.size())
                return Result<void>::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
            const auto resource = resolve(identity);
            if (resource.HasError())
                return Result<void>::Failure(resource.ErrorValue());
            if (!resource.Value().view.IsValid())
                return Result<void>::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
            identities_[count_] = identity;
            resources_[count_++] = resource.Value();
            return Result<void>::Success();
        }

        Render::FrameToken frame_;
        Render::GuiCompositionMode mode_{Render::GuiCompositionMode::BorrowedActiveRecording};
        ImDrawData *draw_{};
        int vertices_{};
        int indices_{};
        std::array<std::uintptr_t, Render::GuiCompositionPreparation::MaximumResources> identities_{};
        std::array<Render::GuiSampledResource, Render::GuiCompositionPreparation::MaximumResources> resources_{};
        std::size_t count_{};
    };
}  // namespace Horo::Editor
