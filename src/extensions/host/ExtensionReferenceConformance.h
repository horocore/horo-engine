#pragma once

#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/ExtensionServiceAbi.h"

#include <vector>

namespace Horo::Extensions {
    /** @brief ABI-shape checker only; it grants no service invocation, UI publication or module permission. */
    class ExtensionReferenceConformance final {
    public:
        HoroExtensionStatus Service(const HoroExtensionServiceDescriptor *descriptor) noexcept;
        HoroExtensionStatus Import(HoroExtensionStringView id, HoroExtensionServiceClient *client) noexcept;
        HoroExtensionStatus Activity(const HoroEditorActivityDescriptor *descriptor, HoroEditorActivitySessionApi *session) noexcept;
        /** @brief Calls accepted native provider shutdown while its actual library is still mapped. */
        bool Destroy() noexcept;

        std::uint32_t Count() const noexcept {
            return count_;
        }

    private:
        std::vector<HoroExtensionServiceDescriptor> providers_;
        std::uint32_t count_{};
    };
}  // namespace Horo::Extensions
