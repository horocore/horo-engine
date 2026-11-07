#include "ExtensionReferenceConformance.h"

#include "EditorActivityAbiConversion.h"
#include "NativeExtensionServiceInternal.h"

namespace Horo::Extensions {
    namespace {
        HoroExtensionStatus NoInvocation(void *, const HoroExtensionServiceRequest *, HoroExtensionServiceResult *) {
            return HORO_EXTENSION_ERROR_BUSY;
        }

        HoroExtensionStatus NoPublication(void *, const HoroEditorActivitySnapshot *) {
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        }

        bool Text(const HoroExtensionStringView value) noexcept {
            return value.data && value.length > 0 && value.length <= 256;
        }

        Result<void> ActivityShape(const HoroEditorActivityDescriptor &descriptor) {
            if (descriptor.structSize != sizeof(descriptor) || descriptor.schemaVersion != 1 || !descriptor.invokeAction ||
                !Text(descriptor.activityId) || !Text(descriptor.drawerId) || !Text(descriptor.labelKey))
                return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected));
            auto form = Detail::CopyActivityForm(descriptor.initialSnapshot, {descriptor.drawerId.data, descriptor.drawerId.length},
                                                 {descriptor.labelKey.data, descriptor.labelKey.length});
            if (form.HasError())
                return Result<void>::Failure(form.ErrorValue());
            return Result<void>::Success();
        }
    }  // namespace

    HoroExtensionStatus ExtensionReferenceConformance::Service(const HoroExtensionServiceDescriptor *descriptor) noexcept {
        if (!descriptor || providers_.size() >= 32)
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        try {
            if (CopyNativeServiceDescriptor(*descriptor).HasError())
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            providers_.push_back(*descriptor);
            ++count_;
            return HORO_EXTENSION_SUCCESS;
        } catch (...) {
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        }
    }

    HoroExtensionStatus ExtensionReferenceConformance::Import(const HoroExtensionStringView id,
                                                              HoroExtensionServiceClient *client) noexcept {
        if (!Text(id) || !client || client->structSize != sizeof(*client) || count_ >= 256)
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        *client = {sizeof(*client), this, NoInvocation};
        ++count_;
        return HORO_EXTENSION_SUCCESS;
    }

    HoroExtensionStatus ExtensionReferenceConformance::Activity(const HoroEditorActivityDescriptor *descriptor,
                                                                HoroEditorActivitySessionApi *session) noexcept {
        if (!descriptor || !session || session->structSize != sizeof(*session) || count_ >= 256)
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        try {
            if (ActivityShape(*descriptor).HasError())
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            *session = {sizeof(*session), HORO_EDITOR_ACTIVITY_SESSION_VERSION, UINT32_MAX, this, NoPublication};
            ++count_;
            return HORO_EXTENSION_SUCCESS;
        } catch (...) {
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        }
    }

    bool ExtensionReferenceConformance::Destroy() noexcept {
        bool clean = true;
        for (auto provider = providers_.rbegin(); provider != providers_.rend(); ++provider) {
            try {
                provider->shutdown(provider->provider);
            } catch (...) {
                clean = false;
            }
        }
        providers_.clear();
        return clean;
    }
}  // namespace Horo::Extensions
