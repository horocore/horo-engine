#include "Horo/Extensions/ExtensionAbiConformance.h"

#include "ExtensionAbiValidation.h"
#include "Horo/Platform/DynamicLibrary.h"

#include <cstddef>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Extensions {
    namespace {
        constexpr std::uint32_t MaximumIdentityBytes = 256;
        constexpr std::uint32_t MaximumRegistrations = 256;

        struct OwnedImporter final {
            void *context{};
            HoroAssetImporterDestroyFunc destroy{};
        };

        struct RegistrationSession final {
            std::vector<OwnedImporter> importers;
            bool rejected{};
        };

        [[nodiscard]] bool ValidText(const HoroExtensionStringView text, const std::uint32_t maximumBytes,
                                     const bool allowEmpty = true) noexcept {
            return text.length <= maximumBytes && (text.data != nullptr || text.length == 0) && (allowEmpty || text.length != 0);
        }

        [[nodiscard]] bool ValidList(const void *values, const std::uint32_t count) noexcept {
            return count <= MaximumRegistrations && (values != nullptr || count == 0);
        }

        [[nodiscard]] bool ValidImporter(const HoroAssetImporterDescriptor &descriptor) noexcept {
            return descriptor.structSize >= sizeof(HoroAssetImporterDescriptor) &&
                   descriptor.abiVersion == HORO_ASSET_IMPORTER_ABI_VERSION &&
                   ValidText(descriptor.contributionId, MaximumIdentityBytes, false) &&
                   ValidText(descriptor.contributionVersion, MaximumIdentityBytes, false) &&
                   ValidList(descriptor.fileExtensions, descriptor.fileExtensionCount) &&
                   ValidList(descriptor.assetTypes, descriptor.assetTypeCount) && ValidList(descriptor.settings, descriptor.settingCount) &&
                   descriptor.importAsset != nullptr && (descriptor.importerContext == nullptr || descriptor.destroyImporter != nullptr);
        }

        HoroExtensionStatus RegisterImporter(void *context, const HoroAssetImporterDescriptor *descriptor) noexcept {  // NOSONAR(cpp:S5008)
            auto *session = static_cast<RegistrationSession *>(context);
            if (session == nullptr || descriptor == nullptr || session->rejected || !ValidImporter(*descriptor) ||
                session->importers.size() == MaximumRegistrations) {
                if (session != nullptr)
                    session->rejected = true;
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            }
            try {
                session->importers.push_back({descriptor->importerContext, descriptor->destroyImporter});
                return HORO_EXTENSION_SUCCESS;
            } catch (...) {  // NOSONAR(cpp:S1181) No exception may cross the module callback boundary.
                session->rejected = true;
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

        [[nodiscard]] bool DestroyRegistrations(RegistrationSession &session) noexcept {
            bool clean = true;
            for (auto &importer : std::ranges::reverse_view(session.importers)) {
                if (importer.destroy == nullptr)
                    continue;
                try {
                    importer.destroy(importer.context);
                } catch (...) {  // NOSONAR(cpp:S1181) Contain contract-violating cleanup callbacks.
                    clean = false;
                }
            }
            session.importers.clear();
            return clean;
        }

        [[nodiscard]] bool InvokeUnload(const HoroExtensionUnloadFunc unload, HoroExtensionModuleApi &module) noexcept {
            if (unload == nullptr)
                return true;
            try {
                unload(&module);
                return true;
            } catch (...) {  // NOSONAR(cpp:S1181) Contain contract-violating cleanup callbacks.
                return false;
            }
        }

        [[nodiscard]] bool ValidModuleIdentity(const HoroExtensionModuleApi &module) noexcept {
            return ValidText(module.moduleId, MaximumIdentityBytes) && ValidText(module.moduleVersion, MaximumIdentityBytes);
        }

        [[nodiscard]] HoroExtensionStatus InvokeLoad(const HoroExtensionLoadFunc load, const HoroExtensionHostApi &host,
                                                     HoroExtensionModuleApi &module) noexcept {
            try {
                return load(&host, &module);
            } catch (...) {  // NOSONAR(cpp:S1181) Contain contract-violating native callbacks.
                return HORO_EXTENSION_ERROR_INIT_FAILED;
            }
        }
    }  // namespace

    /** @copydoc ExtensionAbiConformanceReport::Passed */
    bool ExtensionAbiConformanceReport::Passed() const noexcept {
        return code == ExtensionAbiConformanceCode::Passed;
    }

    /** @copydoc ExtensionAbiConformanceCodeName */
    std::string_view ExtensionAbiConformanceCodeName(const ExtensionAbiConformanceCode code) noexcept {
        using enum ExtensionAbiConformanceCode;
        switch (code) {
            case Passed:
                return "extension.abi.conformance.passed";
            case LibraryLoadFailed:
                return "extension.abi.library_load_failed";
            case MissingLoadEntryPoint:
                return "extension.abi.load_entry_missing";
            case NegotiationRejected:
                return "extension.abi.negotiation_rejected";
            case RegistrationRejected:
                return "extension.abi.registration_rejected";
            case LoadRejected:
                return "extension.abi.load_rejected";
            case ModuleTableRejected:
                return "extension.abi.module_table_rejected";
            case ModuleIdentityRejected:
                return "extension.abi.module_identity_rejected";
            case CleanupRejected:
                return "extension.abi.cleanup_rejected";
        }
        return "extension.abi.conformance.unknown";
    }

    /** @copydoc RunExtensionAbiConformance */
    ExtensionAbiConformanceReport RunExtensionAbiConformance(const std::string_view modulePath) {
        ExtensionAbiConformanceReport report;
        auto library = Platform::LoadDynamicLibrary(modulePath);
        if (library.HasError())
            return report;

        const auto load = reinterpret_cast<HoroExtensionLoadFunc>(library.Value()->GetSymbol("horo_extension_load"));  // NOSONAR(cpp:S3630)
        if (load == nullptr) {
            report.code = ExtensionAbiConformanceCode::MissingLoadEntryPoint;
            return report;
        }
        const auto query =
            reinterpret_cast<HoroExtensionQueryFunc>(library.Value()->GetSymbol("horo_extension_query"));  // NOSONAR(cpp:S3630)
        const auto unload =
            reinterpret_cast<HoroExtensionUnloadFunc>(library.Value()->GetSymbol("horo_extension_unload"));  // NOSONAR(cpp:S3630)
        report.queryPresent = query != nullptr;
        report.unloadPresent = unload != nullptr;

        RegistrationSession registration;
        constexpr std::string_view engineVersion = "abi-conformance";
        const HoroExtensionHostApi host{
            .structSize = sizeof(HoroExtensionHostApi),
            .abiVersion = HORO_EXTENSION_ABI_VERSION,
            .engineVersion = {engineVersion.data(), static_cast<std::uint32_t>(engineVersion.size())},
            .hostContext = &registration,
            .registerAssetImporter = RegisterImporter,
            .abiMinorVersion = HORO_EXTENSION_ABI_MINOR_VERSION,
        };
        report.status = NegotiateModuleAbi(query, host);
        if (report.status != HORO_EXTENSION_SUCCESS) {
            report.code = ExtensionAbiConformanceCode::NegotiationRejected;
            return report;
        }

        HoroExtensionModuleApi module{.structSize = sizeof(HoroExtensionModuleApi)};
        report.status = InvokeLoad(load, host, module);
        report.registrationCount = static_cast<std::uint32_t>(registration.importers.size());
        ExtensionAbiConformanceCode loadCode = ExtensionAbiConformanceCode::Passed;
        if (registration.rejected)
            loadCode = ExtensionAbiConformanceCode::RegistrationRejected;
        else if (report.status != HORO_EXTENSION_SUCCESS)
            loadCode = ExtensionAbiConformanceCode::LoadRejected;
        else if (!NormalizeModuleApi(module))
            loadCode = ExtensionAbiConformanceCode::ModuleTableRejected;
        else if (!ValidModuleIdentity(module))
            loadCode = ExtensionAbiConformanceCode::ModuleIdentityRejected;

        const bool registrationsClean = DestroyRegistrations(registration);
        report.unloadInvoked = unload != nullptr;
        const bool unloadClean = InvokeUnload(unload, module);
        report.code = registrationsClean && unloadClean ? loadCode : ExtensionAbiConformanceCode::CleanupRejected;
        return report;
    }
}  // namespace Horo::Extensions
