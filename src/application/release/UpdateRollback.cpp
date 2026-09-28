#include "Horo/Release/UpdateRollback.h"

#include "Horo/Release/UpdateRollbackErrors.h"

#include <type_traits>

namespace Horo::Release {
    namespace {
        /** @brief Returns the typed release version without accepting a mismatched product version alternative. */
        [[nodiscard]] bool EarlierVersion(const ReleaseProductVersion &candidate, const ReleaseProductVersion &installed) {
            if (candidate.index() != installed.index())
                return false;
            return std::visit([]<typename Left, typename Right>(const Left &left, const Right &right) {
                if constexpr (std::is_same_v<Left, Right>)
                    return CompareReleaseVersionPrecedence(left.value, right.value) < 0;
                return false;
            }, candidate, installed);
        }

        /** @brief Checks a signed-policy floor without weakening same-product version typing. */
        [[nodiscard]] bool BelowFloor(const ReleaseProductVersion &candidate, const ReleaseProductVersion &minimum) {
            if (candidate.index() != minimum.index())
                return true;
            return std::visit([]<typename Left, typename Right>(const Left &left, const Right &right) {
                if constexpr (std::is_same_v<Left, Right>)
                    return CompareReleaseVersionPrecedence(left.value, right.value) < 0;
                return true;
            }, candidate, minimum);
        }
    }  // namespace

    /** @copydoc RollbackVerifiedUpdate */
    Result<UpdateActivationOutcome> RollbackVerifiedUpdate(const UpdateRollbackRequest &request, NativeDurableFileSystem &files,
                                                           const Security::ArtifactVerifier &verifier, IUpdateActivationHost &host) {
        using enum UpdateRollbackReason;
        using enum UpdateRollbackAuthority;
        const auto &installed = request.activation.current.package.selection.artifact;
        const auto &prior = request.activation.staged.package.selection.artifact;
        if (installed.product != prior.product || installed.installation != prior.installation ||
            !EarlierVersion(prior.version, installed.version))
            return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateRollbackErrors::InvalidTarget));
        if (request.reason != FailedActivation && request.reason != ExplicitUserRequest)
            return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateRollbackErrors::InvalidTarget));
        if (request.authority != NormalPolicy && request.authority != AdministratorRecovery)
            return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateRollbackErrors::PolicyDenied));
        if (request.reason == ExplicitUserRequest && !request.explicitWarningAcknowledged)
            return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateRollbackErrors::ConfirmationRequired));
        if (request.authority == NormalPolicy &&
            (!request.minimumAllowedVersion || BelowFloor(prior.version, *request.minimumAllowedVersion)))
            return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateRollbackErrors::PolicyDenied));
        return ActivateVerifiedUpdate(request.activation, files, verifier, host);
    }
}  // namespace Horo::Release
