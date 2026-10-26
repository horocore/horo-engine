#include "RuntimeUiPaintHostInternal.h"

#include <algorithm>
#include <format>

namespace Horo::Application::Internal::Detail {
    using namespace Runtime::Ui;

    namespace {
        /** @brief Compares both parts of a typed failure without matching user-facing diagnostic strings. */
        bool Is(const Error &error, const ErrorCodeDescriptor &expected) {
            return error.domain.Value() == expected.domain.Value() && error.code.Value() == expected.code.Value();
        }

        /** @brief Pins the actual pre-realized glyph set; a failed unsealed candidate releases its own pin set. */
        Result<UiGlyphAtlas::FrameLease> SealGlyphs(const RuntimeUiPaintResources &resources) {
            if (resources.fonts.empty())
                return Result<UiGlyphAtlas::FrameLease>::Success({});
            if (!resources.atlas)
                return Result<UiGlyphAtlas::FrameLease>::Failure(MakeError(UiErrors::HandleMalformed));
            auto begun = resources.atlas->BeginFrame();
            if (begun.HasError())
                return Result<UiGlyphAtlas::FrameLease>::Failure(begun.ErrorValue());
            for (const auto &glyph : resources.glyphs) {
                const auto resolved = resources.atlas->Resolve(begun.Value(), glyph);
                if (resolved.HasError()) {
                    (void)resources.atlas->RetireFrame(begun.Value(), UiGlyphAtlasFrameOutcome::Skipped);
                    return Result<UiGlyphAtlas::FrameLease>::Failure(resolved.ErrorValue());
                }
            }
            auto sealed = resources.atlas->SealFrame(begun.Value());
            if (sealed.HasError())
                (void)resources.atlas->RetireFrame(begun.Value(), UiGlyphAtlasFrameOutcome::Skipped);
            return sealed;
        }
    }  // namespace

    /** @copydoc RuntimeUiPaintSource::AdmitResources */
    Result<void> RuntimeUiPaintSource::AdmitResources() {
        if (resources.images.size() > Render::MaximumUiSubmissionImages || resources.fonts.size() > Render::MaximumUiSubmissionFonts ||
            resources.glyphs.size() > Render::MaximumUiSubmissionGlyphs || (!resources.fonts.empty() && !resources.atlas))
            return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
        textures.reserve(resources.images.size() + resources.fonts.size());
        const auto append = [&](const Render::RenderTextureHandle texture) {
            if (std::ranges::find(textures, texture) == textures.end())
                textures.push_back(texture);
        };
        for (const auto &image : resources.images) {
            if (!image.registry || !image.realization.Creation().handle.IsValid())
                return Result<void>::Failure(MakeError(UiErrors::HandleMalformed));
            append(image.realization.Creation().handle);
        }
        for (const auto &font : resources.fonts) {
            if (!font.face || !font.realization.Creation().handle.IsValid())
                return Result<void>::Failure(MakeError(UiErrors::HandleMalformed));
            append(font.realization.Creation().handle);
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintSource::Refresh */
    Result<void> RuntimeUiPaintSource::Refresh(Render::RenderFrontend &frontend, const CancellationToken &cancellation) {
        if (!refresh || !frame.snapshot)
            return Result<void>::Success();
        auto candidate =
            Render::UiPaintProgram::Create(frontend, programs.policy, *frame.snapshot, textures, programs.kernel, cancellation);
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        if (const auto realized = frontend.ProcessResourceRequests(); realized.HasError())
            return Result<void>::Failure(realized.ErrorValue());
        // All creation/compilation precedes publication. Old queued passes retain their canonical completion leases.
        programs.active = std::move(candidate).Value();
        refresh = false;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintSource::BindResources */
    Result<void> RuntimeUiPaintSource::BindResources() {
        for (std::size_t index = 0; index < resources.images.size(); ++index) {
            const auto &image = resources.images[index];
            auto resource = programs.active->TextureResource(frame.slot, image.realization.Creation().handle);
            if (resource.HasError())
                return Result<void>::Failure(resource.ErrorValue());
            frame.images[index] = {image.registry.get(), image.realization.Image(), image.realization.Page(), resource.Value(),
                                   &image.realization};
        }
        for (std::size_t index = 0; index < resources.fonts.size(); ++index) {
            const auto &font = resources.fonts[index];
            auto resource = programs.active->TextureResource(frame.slot, font.realization.Creation().handle);
            if (resource.HasError())
                return Result<void>::Failure(resource.ErrorValue());
            frame.fonts[index] = {font.face.get(), resource.Value(), &font.realization};
        }
        auto atlas = SealGlyphs(resources);
        if (atlas.HasError())
            return Result<void>::Failure(atlas.ErrorValue());
        frame.submission = {&*frame.geometry,
                            {frame.images.data(), resources.images.size()},
                            {frame.fonts.data(), resources.fonts.size()},
                            std::move(atlas).Value()};
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintSource::ReleaseFrame */
    void RuntimeUiPaintSource::ReleaseFrame() noexcept {
        frame.submission = {};
        frame.prepared.reset();
        frame.geometry.reset();
        frame.snapshot.reset();
        frame.animation = {};
        frame.encoded.reset();
    }

    /** @copydoc RuntimeUiPaintSource::Extract */
    Result<void> RuntimeUiPaintSource::Extract(const RuntimeUiService &service, const std::uint64_t frameNumber,
                                               const CancellationToken &cancellation) {
        ReleaseFrame();
        auto current = animation->Acquire();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        frame.animation = std::move(current).Value();
        auto extracted = projection->Extract(service, frame.animation);
        if (extracted.HasError()) {
            auto error = extracted.ErrorValue();
            error.message = "Runtime UI source extraction: " + error.message;
            return Result<void>::Failure(std::move(error));
        }
        frame.snapshot.emplace(std::move(extracted).Value());
        if (!programs.active || refresh)
            return Result<void>::Success();  // The explicit next owner safe point consumes this actual prototype.
        auto built = arena.Build(*frame.snapshot, programs.policy.projection);
        if (built.HasError())
            return Result<void>::Failure(built.ErrorValue());
        frame.geometry.emplace(std::move(built).Value());
        frame.slot = static_cast<std::uint32_t>(frameNumber % programs.policy.frameSlots);
        if (const auto bindings = BindResources(); bindings.HasError())
            return bindings;
        auto uploaded = programs.active->Prepare(frame.slot, frame.submission, cancellation);
        if (uploaded.HasError()) {
            frame.submission = {};
            if (Is(uploaded.ErrorValue(), Render::UiPaintProgramErrors::PreparationRequired)) {
                refresh = true;
                return Result<void>::Success();
            }
            if (Is(uploaded.ErrorValue(), Render::UiPaintFrameErrors::Pending))
                return Result<void>::Success();
            return Result<void>::Failure(uploaded.ErrorValue());
        }
        frame.prepared.emplace(std::move(uploaded).Value());
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintSource::Retire */
    Result<void> RuntimeUiPaintSource::Retire() {
        retiring = true;
        animation->Shutdown();
        ReleaseFrame();
        projection.reset();
        if (programs.active) {
            if (const auto closed = programs.active->Shutdown(); closed.HasError())
                return closed;
            programs.active.reset();
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintSource::OnPhase */
    Result<void> RuntimeUiPaintSource::OnPhase(const RuntimeUiService &service, Render::RenderFrontend &frontend,
                                               const Runtime::RuntimePhase phase, const Runtime::FrameContext &context,
                                               const CancellationToken &cancellation) {
        if (phase == Runtime::RuntimePhase::ApplyQueuedOwnerThreadCommands) {
            if (const auto applied = ApplyOutput(); applied.HasError())
                return applied;
            if (refresh)
                if (const auto refreshed = Refresh(frontend, cancellation); refreshed.HasError())
                    return refreshed;
        }
        if (const auto forwarded = animation->OnPhase(phase, context); forwarded.HasError()) {
            auto error = forwarded.ErrorValue();
            error.message = std::format("Runtime UI animation phase {}: {}", static_cast<unsigned>(phase), error.message);
            return Result<void>::Failure(std::move(error));
        }
        return phase == Runtime::RuntimePhase::RenderExtraction ? Extract(service, context.frameNumber, cancellation)
                                                                : Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintHostState::Sources */
    std::array<RuntimeUiPaintSource *, 1 + MaximumSceneUiSources + MaximumRetiredSceneUiSources> RuntimeUiPaintHostState::Sources(
        const bool includeRetired) noexcept {
        std::array<RuntimeUiPaintSource *, 1 + MaximumSceneUiSources + MaximumRetiredSceneUiSources> result{};
        std::size_t index = 0;
        const auto append = [&](const auto &entry) {
            if (entry && (includeRetired || !entry->retiring))
                result[index++] = entry.get();
        };
        append(source);
        for (const auto &entry : sceneSources)
            append(entry);
        if (includeRetired)
            for (const auto &entry : retiredSources)
                append(entry);
        return result;
    }

    /** @copydoc RuntimeUiPaintHostState::Retire */
    Result<void> RuntimeUiPaintHostState::Retire() {
        phase = RuntimeUiPaintHostPhase::Retiring;
        for (auto *entry : Sources(true))
            if (entry)
                if (const auto retired = entry->Retire(); retired.HasError())
                    return retired;
        if (std::ranges::none_of(Sources(true), [](const auto *entry) {
            return entry != nullptr;
        }))
            phase = RuntimeUiPaintHostPhase::NoSource;
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal::Detail
