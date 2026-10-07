#pragma once
/** @file EditorActivityProviderAction.h @brief Private typed invocation boundary for one borrowed native activity endpoint. */

#include "Horo/Extensions/EditorActivityAbi.h"

#include <type_traits>

namespace Horo::Extensions {
    /**
     * @brief Keeps a provider's C callback and its opaque context inseparable; owns neither native code nor context.
     * @details The caller must hold the exact provider's retirement lease until Invoke returns. This standard-layout
     * adapter is the only C++ action state retaining C ABI context; copied presentation and jobs remain strongly typed.
     */
    class EditorActivityProviderAction final {
    public:
        EditorActivityProviderAction() noexcept = default;

        /** @brief Borrows the native endpoint without retaining any descriptor data. @param descriptor Live registration input. */
        explicit EditorActivityProviderAction(const HoroEditorActivityDescriptor &descriptor) noexcept
            : invoke_(descriptor.invokeAction), context_(descriptor.moduleContext) {}

        /** @brief Checks whether this endpoint admits invocation. @return Whether a callback was supplied. */
        [[nodiscard]] bool IsAvailable() const noexcept {
            return invoke_ != nullptr;
        }

        /**
         * @brief Contains every provider exception at the native call, with no allocation or diagnostic work.
         * @param action Borrowed bounded request, live for this call only.
         * @param sink Host-owned copied-result sink, live for this call only.
         * @return Provider status or INIT_FAILED for an absent/throwing endpoint.
         * @pre The caller holds the provider's executable retirement lease.
         */
        [[nodiscard]] HoroExtensionStatus Invoke(const HoroEditorActivityAction &action,
                                                 const HoroEditorActivitySnapshotSink &sink) const noexcept;

    private:
        HoroEditorActivityActionFunc invoke_{};
        void *context_{};
    };

    static_assert(std::is_standard_layout_v<EditorActivityProviderAction>);
    static_assert(std::is_trivially_copyable_v<EditorActivityProviderAction>);
}  // namespace Horo::Extensions
