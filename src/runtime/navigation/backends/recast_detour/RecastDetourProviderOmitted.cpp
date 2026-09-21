#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationErrors.h"

namespace Horo::Navigation {
    /** @copydoc CreateRecastDetourNavigationQueryBackend */
    Result<std::unique_ptr<INavigationQueryBackend>> CreateRecastDetourNavigationQueryBackend(const RecastDetourProviderCreateInfo &) {
        return Result<std::unique_ptr<INavigationQueryBackend>>::Failure(MakeError(NavigationErrors::OperationUnsupported));
    }

    /** @copydoc CreateRecastDetourNavigationMeshBuilder */
    Result<std::unique_ptr<INavigationMeshBuilder>> CreateRecastDetourNavigationMeshBuilder() {
        return Result<std::unique_ptr<INavigationMeshBuilder>>::Failure(MakeError(NavigationErrors::OperationUnsupported));
    }
}  // namespace Horo::Navigation
