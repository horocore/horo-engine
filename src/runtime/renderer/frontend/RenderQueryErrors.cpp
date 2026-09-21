#include "Horo/Runtime/Render/RenderQueryErrors.h"

#include "RenderTransferErrorDescriptor.h"

namespace Horo::Render::RenderQueryErrors {
    namespace {
        const ErrorDomainId Domain{"render.query"};
        const auto Catalog = detail::MakeTransferErrorCatalog(Domain, detail::TransferErrorProfile::Query);
    }  // namespace

    HORO_DETAIL_DEFINE_TRANSFER_COMMON_ERRORS(Catalog);
    const ErrorCodeDescriptor Unsupported = *Catalog.unsupported;
    const ErrorCodeDescriptor TimestampInvalid = *Catalog.timestampInvalid;
}  // namespace Horo::Render::RenderQueryErrors
