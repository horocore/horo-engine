#include "Horo/Navigation/Backends/RecastDetourCrowdProvider.h"
#include "Horo/Navigation/NavigationErrors.h"

namespace Horo::Navigation {
    /** @copydoc CreateRecastDetourCrowdBackend */
    Result<std::unique_ptr<INavigationCrowdBackend>> CreateRecastDetourCrowdBackend(const RecastDetourCrowdLimits &) {
        return Result<std::unique_ptr<INavigationCrowdBackend>>::Failure(MakeError(NavigationErrors::OperationUnsupported));
    }
}  // namespace Horo::Navigation
