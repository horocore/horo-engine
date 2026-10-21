#include "ExtensionAbiValidation.h"

#include <cstddef>

namespace Horo::Extensions {
    namespace {
        /** @brief Validate the complete query result before inspecting its requested capabilities. */
        bool HasSupportedRequirements(const HoroExtensionRequirements &requirements, const HoroExtensionHostApi &host) noexcept {
            constexpr auto legacyHostSize = offsetof(HoroExtensionHostApi, abiMinorVersion);
            return requirements.structSize == sizeof(HoroExtensionRequirements) && requirements.abiMajorVersion == host.abiVersion &&
                   requirements.minimumHostMinor <= host.abiMinorVersion && requirements.requiredHostApiSize >= legacyHostSize &&
                   requirements.requiredHostApiSize <= host.structSize && requirements.reserved == 0 &&
                   (requirements.requiredFunctions &
                    ~uint32_t{HORO_EXTENSION_REQUIRES_ASSET_IMPORTER | HORO_EXTENSION_REQUIRES_PLATFORM_PROVIDER |
                              HORO_EXTENSION_REQUIRES_EDITOR_ACTIVITY | HORO_EXTENSION_REQUIRES_EDITOR_PAGE |
                              HORO_EXTENSION_REQUIRES_EDITOR_INSPECTOR | HORO_EXTENSION_REQUIRES_EDITOR_VIEWPORT}) == 0;
        }

        /** @brief Checks optional interactive transport prefixes without granting absent callbacks through older negotiations. */
        HoroExtensionStatus ValidateInteractiveRequirements(const HoroExtensionRequirements &requirements,
                                                            const HoroExtensionHostApi &host) noexcept {
            if ((requirements.requiredFunctions & HORO_EXTENSION_REQUIRES_EDITOR_ACTIVITY) != 0 &&
                (host.structSize < offsetof(HoroExtensionHostApi, registerEditorActivity) + sizeof(host.registerEditorActivity) ||
                 host.registerEditorActivity == nullptr))
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            if ((requirements.requiredFunctions & HORO_EXTENSION_REQUIRES_EDITOR_PAGE) != 0 &&
                (requirements.minimumHostMinor < 5 ||
                 requirements.requiredHostApiSize < offsetof(HoroExtensionHostApi, registerEditorPage) + sizeof(host.registerEditorPage)))
                return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
            if ((requirements.requiredFunctions & HORO_EXTENSION_REQUIRES_EDITOR_PAGE) != 0 &&
                (host.structSize < offsetof(HoroExtensionHostApi, registerEditorPage) + sizeof(host.registerEditorPage) ||
                 host.registerEditorPage == nullptr))
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            if ((requirements.requiredFunctions & HORO_EXTENSION_REQUIRES_EDITOR_INSPECTOR) != 0 &&
                (requirements.minimumHostMinor < 6 ||
                 requirements.requiredHostApiSize <
                     offsetof(HoroExtensionHostApi, registerEditorInspector) + sizeof(host.registerEditorInspector)))
                return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
            if ((requirements.requiredFunctions & HORO_EXTENSION_REQUIRES_EDITOR_INSPECTOR) != 0 &&
                !HoroExtensionHasEditorInspectorTransport(&host))
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            if ((requirements.requiredFunctions & HORO_EXTENSION_REQUIRES_EDITOR_VIEWPORT) != 0) {
                if (requirements.minimumHostMinor < 7 ||
                    requirements.requiredHostApiSize <
                        offsetof(HoroExtensionHostApi, registerEditorViewport) + sizeof(host.registerEditorViewport))
                    return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
                if (!HoroExtensionHasEditorViewportTransport(&host))
                    return HORO_EXTENSION_ERROR_INVALID_ARGS;
            }
            return HORO_EXTENSION_SUCCESS;
        }
    }  // namespace

    /** @copydoc NegotiateModuleAbi */
    HoroExtensionStatus NegotiateModuleAbi(HoroExtensionQueryFunc query,  // NOSONAR(cpp:S5205) Dynamic-library C ABI callback.
                                           const HoroExtensionHostApi &host, HoroExtensionRequirements *negotiated) noexcept {
        if (negotiated != nullptr)
            *negotiated = {};
        if (query == nullptr)
            return HORO_EXTENSION_SUCCESS;
        HoroExtensionRequirements requirements{.structSize = sizeof(HoroExtensionRequirements)};
        try {
            if (const auto status = query(&requirements); status != HORO_EXTENSION_SUCCESS)
                return status;
        } catch (...) {  // NOSONAR(cpp:S1181) Contain a contract-violating native callback at the ABI boundary.
            return HORO_EXTENSION_ERROR_INIT_FAILED;
        }
        if (!HasSupportedRequirements(requirements, host))
            return HORO_EXTENSION_ERROR_VERSION_MISMATCH;
        if ((requirements.requiredFunctions & HORO_EXTENSION_REQUIRES_ASSET_IMPORTER) != 0 && host.registerAssetImporter == nullptr)
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        if ((requirements.requiredFunctions & HORO_EXTENSION_REQUIRES_PLATFORM_PROVIDER) != 0 &&
            (host.structSize <
                 offsetof(HoroExtensionHostApi, registerPlatformServicesProvider) + sizeof(host.registerPlatformServicesProvider) ||
             host.registerPlatformServicesProvider == nullptr))
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        if (const auto status = ValidateInteractiveRequirements(requirements, host); status != HORO_EXTENSION_SUCCESS)
            return status;
        if (negotiated != nullptr)
            *negotiated = requirements;
        return HORO_EXTENSION_SUCCESS;
    }

    /** @copydoc NormalizeModuleApi */
    bool NormalizeModuleApi(HoroExtensionModuleApi &moduleApi) noexcept {
        switch (moduleApi.structSize) {
            case offsetof(HoroExtensionModuleApi, moduleId):
                moduleApi.moduleId = {};
                moduleApi.moduleVersion = {};
                return true;
            case offsetof(HoroExtensionModuleApi, moduleVersion):
                moduleApi.moduleVersion = {};
                return true;
            case sizeof(HoroExtensionModuleApi):
                return true;
            default:
                return false;
        }
    }
}  // namespace Horo::Extensions
