#include "ExtensionPlatformProviderCopy.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace Horo::Extensions::Detail {
    namespace {
        /** @brief Accepts only the exact supported descriptor layouts, including the legacy tail boundary. */
        [[nodiscard]] bool HasProviderAbiLayout(const HoroPlatformServicesProviderDescriptor &descriptor) noexcept {
            return (descriptor.abiVersion == HORO_PLATFORM_SERVICES_PROVIDER_ABI_VERSION &&
                    descriptor.structSize == offsetof(HoroPlatformServicesProviderDescriptor, operations)) ||
                   (descriptor.abiVersion == HORO_PLATFORM_SERVICES_PROVIDER_ABI_VERSION_2 &&
                    descriptor.structSize == sizeof(HoroPlatformServicesProviderDescriptor));
        }

        /** @brief Rejects unbounded permission arrays and missing mandatory provider lifecycle functions. */
        [[nodiscard]] bool HasProviderInputs(const HoroPlatformServicesProviderDescriptor &descriptor) noexcept {
            return descriptor.permissionCount <= 32 && (descriptor.permissionCount == 0 || descriptor.permissions != nullptr) &&
                   descriptor.createCandidate && descriptor.retireCandidate && descriptor.destroyCandidate;
        }

        /** @brief Verifies the complete session/service lifecycle required by the native operations profile. */
        [[nodiscard]] bool HasServiceLifecycle(const HoroPlatformProviderOperations &operations) noexcept {
            return operations.initializeServices && operations.beginSession && operations.stopSession && operations.shutdownServices;
        }

        /** @brief Verifies operation admission, cancellation, and draining remain available together. */
        [[nodiscard]] bool HasOperationLifecycle(const HoroPlatformProviderOperations &operations) noexcept {
            return operations.submit && operations.cancel && operations.closeAdmission && operations.drain;
        }

        /** @brief Rejects truncated tables before checking their complete executable lifecycle. */
        [[nodiscard]] bool ValidOperations(const HoroPlatformProviderOperations *operations) noexcept {
            return operations && operations->structSize == sizeof(HoroPlatformProviderOperations) &&
                   operations->version == HORO_PLATFORM_SERVICES_PROVIDER_OPERATIONS_VERSION && HasServiceLifecycle(*operations) &&
                   HasOperationLifecycle(*operations) && operations->openIngress && operations->closeIngress;
        }
    }  // namespace

    /** @copydoc View */
    std::string_view View(const HoroExtensionStringView value) noexcept {
        return value.data != nullptr ? std::string_view{value.data, value.length} : std::string_view{};
    }

    /** @copydoc IsValidBoundedText */
    bool IsValidBoundedText(const HoroExtensionStringView value) noexcept {
        return (value.data != nullptr || value.length == 0) && value.length <= 256;
    }

    /** @copydoc IsValidPlatformProviderDescriptor */
    bool IsValidPlatformProviderDescriptor(const HoroPlatformServicesProviderDescriptor *descriptor) noexcept {
        return descriptor && HasProviderAbiLayout(*descriptor) && IsValidBoundedText(descriptor->providerKey) &&
               descriptor->providerKey.length != 0 && HasProviderInputs(*descriptor);
    }

    /** @copydoc IsDeclaredPlatformProvider */
    bool IsDeclaredPlatformProvider(const AssetImporterRegistrationSession &session,
                                    const HoroPlatformServicesProviderDescriptor &descriptor) noexcept {
        return std::ranges::any_of(session.manifest->contributions, [&](const auto &contribution) {
            return contribution.type == "platform.services.provider" && contribution.owningModule == session.extensionModule->id &&
                   contribution.id == View(descriptor.providerKey);
        });
    }

    /** @copydoc CopyPlatformProviderCandidate */
    ExtensionPlatformProviderCandidate CopyPlatformProviderCandidate(const AssetImporterRegistrationSession &session,
                                                                     const HoroPlatformServicesProviderDescriptor &descriptor) {
        ExtensionPlatformProviderCandidate candidate;
        candidate.extensionId = session.manifest->id;
        candidate.moduleId = session.extensionModule->id;
        candidate.providerKey = View(descriptor.providerKey);
        candidate.providerId = descriptor.providerId;
        candidate.platformMask = descriptor.platformMask;
        candidate.profileMask = descriptor.profileMask;
        candidate.serviceMask = descriptor.serviceMask;
        candidate.interfaceMajor = descriptor.interfaceMajor;
        candidate.interfaceMinor = descriptor.interfaceMinor;
        candidate.contractMajor = descriptor.contractMajor;
        candidate.contractMinor = descriptor.contractMinor;
        candidate.contractPatch = descriptor.contractPatch;
        for (std::uint32_t index = 0; index < descriptor.permissionCount; ++index) {
            if (!IsValidBoundedText(descriptor.permissions[index]) || descriptor.permissions[index].length == 0)
                throw std::invalid_argument("Invalid provider permission");
            candidate.permissions.emplace_back(View(descriptor.permissions[index]));
        }
        candidate.factoryContext = descriptor.factoryContext;
        candidate.createCandidate = descriptor.createCandidate;
        candidate.retireCandidate = descriptor.retireCandidate;
        candidate.destroyCandidate = descriptor.destroyCandidate;
        if (descriptor.abiVersion == HORO_PLATFORM_SERVICES_PROVIDER_ABI_VERSION_2) {
            if (!ValidOperations(descriptor.operations))
                throw std::invalid_argument("Invalid provider operation profile");
            candidate.operations = *descriptor.operations;
        }
        candidate.moduleCodeLease = session.retirement
                                        ? session.retirement->Acquire(session.extensionModule->id, ExtensionLeaseKind::HostService,
                                                                      candidate.providerKey, session.lifetime)
                                        : std::shared_ptr<void>{session.lifetime};
        if (!candidate.moduleCodeLease)
            throw std::invalid_argument("Provider executable lease admission failed");
        return candidate;
    }
}  // namespace Horo::Extensions::Detail
