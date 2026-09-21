#include "Horo/Runtime/Render/RenderReadbackErrors.h"

#include "RenderTransferErrorDescriptor.h"

namespace Horo::Render::RenderReadbackErrors {
    namespace {
        const ErrorDomainId Domain{"render.readback"};
        const auto Catalog = detail::MakeTransferErrorCatalog(Domain, detail::TransferErrorProfile::Readback);
    }  // namespace

    HORO_DETAIL_DEFINE_TRANSFER_COMMON_ERRORS(Catalog);
    const ErrorCodeDescriptor MappingSizeMismatch = *Catalog.mappingSizeMismatch;
}  // namespace Horo::Render::RenderReadbackErrors
