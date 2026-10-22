#pragma once

#include "runtime/renderer/modules/metal/MetalBackendModule.h"

#include <SDL3/SDL.h>
#include <memory>

namespace Horo::Editor {
    /** @brief SDL3/macOS owner of the host window's Metal view and layer attachment. */
    class SdlMetalPresentationPort final : public Render::IMetalPresentationPort {
    public:
        /** @brief Borrows the platform window; the window must outlive this port and renderer frontend. */
        explicit SdlMetalPresentationPort(SDL_Window &window) noexcept;
        ~SdlMetalPresentationPort() override;

        SdlMetalPresentationPort(const SdlMetalPresentationPort &) = delete;
        SdlMetalPresentationPort &operator=(const SdlMetalPresentationPort &) = delete;

        [[nodiscard]] Result<void> CreateSurface() override;
        [[nodiscard]] void *Layer() const noexcept override;
        [[nodiscard]] Result<Render::RenderOutputDisplayFacts> OutputDisplayFacts() const override;
        void DestroySurface() noexcept override;

    private:
        struct Impl;
        std::shared_ptr<Impl> impl_;
    };
}  // namespace Horo::Editor
