#pragma once
#include "Horo/Runtime/Render/RenderGraphSynchronization.h"
#include "Horo/Runtime/Render/UiPaintProgram.h"
#include "UiPaintFrontendState.h"
#include "UiRenderSubmissionRetention.h"

#include <optional>

namespace Horo::Render {
    namespace Detail {
        /** @brief Private non-reusable preparation identity; issued observations hold only weak references. */
        struct UiPaintProgramIdentity final {};

        inline constexpr std::uint32_t NoPaintRole = UINT32_MAX;

        struct UiPaintMaskScope final {
            std::uint32_t leaf{NoPaintRole}, boundary{NoPaintRole}, parent{NoPaintRole};
        };

        struct UiPaintPassRange final {
            std::uint32_t offset{}, count{};
        };

        /** @brief Bounded copies of existing issued provenance, never a second resource or source publisher. */
        struct UiPaintSubmissionEvidence final {
            struct Image final {
                const Runtime::Ui::UiImageResourceRegistry *registry{};
                Runtime::Ui::UiImageResourceHandle image;
                std::uint32_t page{};
                RenderGraphResourceId texture;
                std::optional<UiRenderImageTexture> realization;
            };

            struct Font final {
                RenderGraphResourceId texture;
                std::optional<Runtime::Ui::UiFontFace> face;
                std::optional<UiRenderAtlasTexture> realization;
            };

            std::array<Image, MaximumUiSubmissionImages> images;
            std::array<Font, MaximumUiSubmissionFonts> fonts;
            std::size_t imageCount{}, fontCount{};
            Runtime::Ui::UiGlyphAtlasFrameId atlasFrame;
            Runtime::Ui::UiGlyphAtlasRevision atlasRevision;
            /** @brief Copies already validated finite immutable provenance before native slot publication. */
            void Capture(const UiRenderSubmission &source) noexcept;
            /** @brief Compares exact issuance, allowing copies and reordered equivalent binding tables. */
            [[nodiscard]] bool Matches(const UiRenderSubmission &source) const noexcept;
        };

        /** @brief Reserved inactive CPU candidate; swapping the accepted index publishes all arrays together. */
        struct UiPaintCpuFrame final {
            std::vector<UiPaintVertex> vertices;
            std::vector<std::uint32_t> indices;
            std::vector<UiPaintParameters> parameters;
            std::vector<UiPaintDraw> draws;
            std::vector<UiPaintPassRange> ranges;
            std::optional<Runtime::Ui::UiRenderGeometryPlan> geometry;
            UiPaintSubmissionEvidence evidence;
        };

        struct UiPaintSlotProgram final {
            std::vector<ResourceCreation<RenderTextureHandle>> surfaces;
            ResourceCreation<RenderTextureHandle> stencil;
            std::vector<RenderGraphResourceId> textures;
            std::vector<std::vector<RenderGraphResourceId>> sampled;
            std::optional<CompiledRenderGraphExecution> graph;
            std::vector<RenderGraphPassWorkload> workloads;
            std::array<UiPaintCpuFrame, 2> frames;
            std::uint32_t accepted{};
            std::uint64_t revision{};
        };
    }  // namespace Detail

    struct UiPaintProgram::Storage final {
        struct SourceRoles final {
            Runtime::Ui::UiRenderSnapshot prototype;
            std::vector<RenderTextureHandle> external;
            std::vector<Detail::UiPaintMaskScope> masks;
            std::vector<std::uint32_t> commandScopes, groupScopes;
        } source;

        struct NativePreparation final {
            std::unique_ptr<UiPaintFrameBufferPool> buffers;
            std::shared_ptr<IResidentUiPaintKernel> primary, offscreen;
            std::vector<Detail::UiPaintSlotProgram> slots;
        } native;

        UiPaintProgramDescriptor descriptor;
        std::weak_ptr<Detail::UiPaintFrontendState> frontend;
        std::shared_ptr<const Detail::UiPaintProgramIdentity> identity;
        std::thread::id owner;
        bool closed{};
        Detail::UiRenderSubmissionRetention validation;

        Storage(const Runtime::Ui::UiRenderSnapshot &prototype, const UiPaintProgramDescriptor &policy)
            : source{.prototype = prototype}, descriptor(policy), owner(std::this_thread::get_id()) {}

        [[nodiscard]] Result<void> BuildRoles();
        [[nodiscard]] Result<std::uint32_t> MaskScope(std::uint32_t leaf, std::uint32_t group);
        [[nodiscard]] Result<std::uint32_t> AddMaskScope(std::uint32_t leaf, std::uint32_t boundary, std::uint32_t depth = 0);
        [[nodiscard]] bool IsMaskAncestor(std::uint32_t ancestor, std::uint32_t leaf) const noexcept;
        [[nodiscard]] Result<void> CreateSlot(RenderFrontend &renderer, std::uint32_t slot, const UiPaintFrameBuffers &buffers,
                                              RenderTextureFormat stencilFormat);
        [[nodiscard]] Result<void> CompileSlot(std::uint32_t slot, const UiPaintFrameBuffers &buffers);
        [[nodiscard]] Result<void> ValidateSource(const UiRenderSubmission &submission) const;
        [[nodiscard]] Result<void> BuildFrame(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &candidate,
                                              const UiRenderSubmission &submission);
        [[nodiscard]] Result<void> BuildMask(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &candidate,
                                             const UiRenderSubmission &submission, std::uint32_t scope);
        [[nodiscard]] Result<void> BuildGroup(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &candidate,
                                              const UiRenderSubmission &submission, std::uint32_t group);
        [[nodiscard]] Result<void> Leaf(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &candidate,
                                        const UiRenderSubmission &submission, const Runtime::Ui::UiRenderGeometryBatch &batch,
                                        std::uint32_t pass);
        [[nodiscard]] Result<void> Composite(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &candidate,
                                             const UiRenderSubmission &submission, std::uint32_t group, std::uint32_t pass);
        [[nodiscard]] Result<void> MaskedDraw(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &candidate,
                                              const Runtime::Ui::UiRenderSnapshot &snapshot, UiPaintDraw draw, std::uint32_t scope,
                                              std::uint32_t pass);
        [[nodiscard]] Result<std::uint32_t> Quad(Detail::UiPaintCpuFrame &candidate, Runtime::Ui::UiLogicalRect rect,
                                                 const Runtime::Ui::UiLogicalTransform &transform, bool targetUv);
        [[nodiscard]] Result<std::uint32_t> Fullscreen(Detail::UiPaintCpuFrame &candidate);
        [[nodiscard]] Result<void> Draw(Detail::UiPaintCpuFrame &candidate, const UiPaintDraw &draw);
        [[nodiscard]] Result<UiPaintScissor> Scissor(const Runtime::Ui::UiRenderSnapshot &snapshot, std::uint32_t clip) const;
        [[nodiscard]] Result<std::uint32_t> Sample(const Detail::UiPaintSlotProgram &slot, std::uint32_t pass,
                                                   RenderGraphResourceId resource) const;
        [[nodiscard]] Result<std::uint32_t> SourceSample(const Detail::UiPaintSlotProgram &slot, std::uint32_t pass,
                                                         const UiRenderSubmission &submission, std::uint32_t reference,
                                                         const Runtime::Ui::UiPositionedGlyph *glyph = nullptr) const;
        void Publish(Detail::UiPaintSlotProgram &slot, Detail::UiPaintCpuFrame &frame, std::uint64_t revision) noexcept;
        [[nodiscard]] Result<void> Shutdown();
    };
}  // namespace Horo::Render
