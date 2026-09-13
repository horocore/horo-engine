#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"

namespace Horo::Render::Detail {
    /** @brief Internal adapter used by matching integrations without publishing native resource identity. */
    class RenderFrontendResourceAccess final {
    public:
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderBufferHandle buffer);
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderMeshHandle mesh);
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderTextureViewHandle view);
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderTargetHandle target);
    };
}  // namespace Horo::Render::Detail
