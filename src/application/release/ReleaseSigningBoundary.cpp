#include "Horo/Release/ReleaseSigningBoundary.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>

namespace Horo::Release {
    /** @copydoc SignVerifiedReleaseStage */
    Result<void> SignVerifiedReleaseStage(const ReleaseSigningRequest &request, IReleaseSigningBackend &backend) {
        const auto &plan = request.plan.Request();
        if (!plan.signingSelected || request.credential.value == 0 ||
            std::ranges::find(plan.credentials, request.credential) == plan.credentials.end() || request.inventory.Candidate().value == 0 ||
            request.stageRoot.empty())
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineInputChanged));

        auto verified = VerifyReleaseStagedTree(request.stageRoot, request.inventory);
        if (verified.HasError())
            return verified;
        return backend.Sign(request.stageRoot, request.credential);
    }
}  // namespace Horo::Release
