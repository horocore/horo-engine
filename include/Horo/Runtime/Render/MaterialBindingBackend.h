#pragma once
/** @file MaterialBindingBackend.h
 * @brief Narrow renderer-owned native material realization boundary.
 */
#include "Horo/Foundation/Result.h"

#include <memory>

namespace Horo::Render {
    struct MaterialBindingDescriptor;

    /**
     * @brief Owns the native binding table and pins every pipeline/resource generation it references.
     * @details Created and destroyed on the render owner thread. An adapter retains native resources until
     * exact command completion independently of this preparation lease; release never waits for the GPU.
     * The host drains all consumer leases before destroying the selected backend or its resource registry.
     */
    class IResidentMaterialBinding {
    public:
        virtual ~IResidentMaterialBinding() = default;
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
    };
}  // namespace Horo::Render
