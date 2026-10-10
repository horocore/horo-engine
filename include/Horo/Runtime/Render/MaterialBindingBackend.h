#pragma once
/** @file MaterialBindingBackend.h
 * @brief Narrow renderer-owned native material realization boundary.
 */
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/ShaderManifest.h"

#include <memory>
#include <span>

namespace Horo::Render {
    struct MaterialBindingDescriptor;

    /** @brief Kind of an exact frontend-resolved resource; native instances remain opaque outside renderer implementation. */
    enum class MaterialResidentResourceKind : std::uint8_t {
        Buffer,
        TextureView,
        Sampler
    };

    /** @brief Synchronous selected-backend material input resolved by the sole frontend registry.
     * @details One entry per logical descriptor resource, in the same order. Packed parameters are realized as admitted
     * uniform buffers by the frontend; this boundary never allocates unbudgeted native backing. */
    struct MaterialResidentResource {
        MaterialResidentResourceKind kind{MaterialResidentResourceKind::Buffer};
        std::uint64_t instance{0};
        std::size_t offset{0};
        std::size_t byteCount{0};
    };

    /**
     * @brief Owns the native binding table and pins every pipeline/resource generation it references.
     * @details Created and destroyed on the render owner thread. An adapter retains native resources until
     * exact command completion independently of this preparation lease; release never waits for the GPU.
     * The host drains all consumer leases before destroying the selected backend or its resource registry.
     */
    class IResidentMaterialBinding {
    public:
        virtual ~IResidentMaterialBinding() = default;

        /** @brief Borrows the selected native adapter beneath a frontend pin-owning wrapper.
         * @return Adapter interface valid for this lease's lifetime; no native API handle is exposed. */
        [[nodiscard]] virtual const IResidentMaterialBinding &NativeBinding() const noexcept {
            return *this;
        }
    };

    /** @brief Selected renderer's explicit preparation boundary; no feature-side backend discovery. */
    class IMaterialBindingBackend {
    public:
        virtual ~IMaterialBindingBackend() = default;
        /**
         * @brief Resolves exact resident generations and realizes a native table at a non-frame-hot safe point.
         * @param descriptor Validated owned logical layout, packed values, and exact resource generations.
         * @return Owned table/resource lease or original typed failure; unsupported adapters use MaterialBindingErrors::Unsupported.
         * @details Must verify pipeline/layout compatibility, residency, ranges and usages before native mutation.
         * Must not reenter the owning table during realization. Failure publishes no table. Cancellation/shutdown are host-serialized
         * before/after this synchronous call.
         */
        [[nodiscard]] virtual Result<std::unique_ptr<IResidentMaterialBinding>> Realize(const MaterialBindingDescriptor &descriptor);
        /** @brief Queries the actual linked parameter block size before frontend backing admission.
         * @param pipeline Exact ready selected-backend pipeline instance. @param descriptor Complete material for exact layout admission.
         * @param binding Active logical uniform buffer binding.
         * @return Required byte size, including native ABI tail padding, or typed failure. */
        [[nodiscard]] virtual Result<std::size_t> QueryMaterialParameterBufferSize(std::uint64_t pipeline,
                                                                                   const MaterialBindingDescriptor &descriptor,
                                                                                   ShaderBindingId binding) const;
        /** @brief Prepares a native table from registry-resolved, pinned generations without allocating resource backing.
         * @param descriptor Validated owned logical material. @param pipeline Exact ready native pipeline instance.
         * @param resources Exact resources in descriptor order, retained by the calling frontend lease.
         * @return Native table or original typed failure. Only preparation metadata allocations may occur. */
        [[nodiscard]] virtual Result<std::unique_ptr<IResidentMaterialBinding>> RealizeResolved(
            const MaterialBindingDescriptor &descriptor, std::uint64_t pipeline, std::span<const MaterialResidentResource> resources);
    };
}  // namespace Horo::Render
