#pragma once

/** @file RenderExternalImages.h
 * @brief Non-installed selected-backend bridge for runtime-owned external textures.
 */

#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderGraphSynchronization.h"

#include <memory>

namespace Horo::Render {
    class RenderFrontend;

    namespace Detail {
        /**
         * @brief Immutable acquisition proof and native-free state requirements, specialized only by a matching native adapter.
         * @details The source owns no allocation. Its host/device/runtime owner outlives canonical retirement.
         * Validate fences the exact acquisition generation on every new graph admission. Native subclasses
         * live in dedicated backend-private interfaces; they may not enter installed public headers.
         */
        class IRenderExternalTextureSource {
        public:
            virtual ~IRenderExternalTextureSource() = default;
            /** @brief Validates current external owner, device and acquisition generation without native work or allocation on success. */
            [[nodiscard]] virtual Result<void> Validate() const = 0;
            /** @brief Returns the canonical selected-device incarnation captured before import. */
            [[nodiscard]] virtual std::uint64_t DeviceIncarnation() const noexcept = 0;
            /** @brief Returns the exact immutable physical allocation descriptor. */
            [[nodiscard]] virtual const RenderTextureDescriptor &Descriptor() const noexcept = 0;
            /** @brief Returns actual acquisition ownership/layout evidence from the same qualified source. */
            [[nodiscard]] virtual RenderGraphLogicalState AcquiredState() const noexcept = 0;
            /** @brief Returns the selected native adapter's qualified required external release state. */
            [[nodiscard]] virtual RenderGraphLogicalState RequiredFinalState() const noexcept = 0;
        };

        /** @brief Optional private interface implemented by a backend with actual foreign-allocation admission. */
        class IRenderExternalTextureBackend {
        public:
            virtual ~IRenderExternalTextureBackend() = default;
            /** @brief Validates native source kind and exact device, registers a borrow, and retains canonical native lifetime.
             * @param source Exact private native source; no pointer may escape without its registry-owned shared lease.
             * @param lifetime Single registry-owned retirement authority; no memory allocation charge for the foreign image.
             * @return Nonzero backend instance or typed failure with no native work/borrow retained.
             * @post DestroyTexture unregisters this instance after pins drain; it never destroys the external allocation. */
            [[nodiscard]] virtual Result<std::uint64_t> ImportExternalTexture(const IRenderExternalTextureSource &source,
                                                                              RenderResourceNativeLifetime lifetime) = 0;
        };

        /** @brief Read-only copied facts issued by the exact current external image source. */
        struct RenderExternalTextureFacts final {
            RenderTextureDescriptor descriptor;
            RenderGraphLogicalState acquired;
            RenderGraphLogicalState requiredFinal;
            std::uint64_t deviceIncarnation{};
        };

        /** @brief Explicit host adapter access to the existing frontend registry; never a second resource registry. */
        class RenderExternalTextureAccess final {
        public:
            /** @brief Registers one exact acquisition outside an active frame through the matching selected backend.
             * @param frontend Exact frontend/device owner. @param source Shared immutable acquisition evidence.
             * @return Ready ordinary texture handle, or typed failure after rollback. */
            [[nodiscard]] static Result<RenderTextureHandle> Import(RenderFrontend &frontend,
                                                                    std::shared_ptr<const IRenderExternalTextureSource> source);
            /** @brief Observes exact registration retirement through the canonical registry lease, without CPU waits.
             * @param frontend Original frontend. @param texture Handle issued by this bridge, retained by its native owner.
             * @return True after native backing/queued references are terminal; false while still ready or retiring. */
            [[nodiscard]] static Result<bool> RetirementComplete(const RenderFrontend &frontend, RenderTextureHandle texture);
            /** @brief Reads exact current source/allocation/state facts without adopting an image or retaining a native source pointer. */
            [[nodiscard]] static Result<RenderExternalTextureFacts> Describe(const RenderFrontend &frontend, RenderTextureHandle texture);
        };
    }  // namespace Detail
}  // namespace Horo::Render
