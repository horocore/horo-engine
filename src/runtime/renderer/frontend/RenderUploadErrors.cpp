#include "Horo/Runtime/Render/RenderUploadErrors.h"

#include "RenderTransferErrorDescriptor.h"

namespace Horo::Render::RenderUploadErrors {
    namespace {
        const ErrorDomainId Domain{"render.upload"};
        const auto Catalog = detail::MakeTransferErrorCatalog(Domain, detail::TransferErrorProfile::Upload);
    }  // namespace

    HORO_DETAIL_DEFINE_TRANSFER_COMMON_ERRORS(Catalog);
    const ErrorCodeDescriptor PayloadSizeMismatch = *Catalog.payloadSizeMismatch;
}  // namespace Horo::Render::RenderUploadErrors
