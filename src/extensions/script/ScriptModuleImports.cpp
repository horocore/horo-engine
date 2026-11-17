#include "ExtensionAuthorityIdentityValidation.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ScriptModuleRuntime.h"

#include <algorithm>

namespace Horo::Extensions {
    namespace {
        bool Alias(std::string_view text) {
            return Detail::IsCanonicalExtensionAuthorityId(text);
        }
    }  // namespace

    Result<ScriptModuleImport> BindScriptModuleImport(std::string alias, std::shared_ptr<const ScriptRuntimeBackendBinding> backend) {
        if (!Alias(alias) || !backend || !backend->IsUsable())
            return Result<ScriptModuleImport>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        auto resolution = backend->Resolution();
        return Result<ScriptModuleImport>::Success(ScriptModuleImport{std::move(alias), std::move(resolution), std::move(backend), {}});
    }

    Result<ScriptModuleImport> MakeUnavailableScriptModuleImport(std::string alias, const ResolvedExtensionServiceImport &resolved) {
        if (!Alias(alias) || resolved.IsRequired() || resolved.Status() == ExtensionServiceImportStatus::Bound)
            return Result<ScriptModuleImport>::Failure(
                MakeError(ExtensionErrors::ScriptInvocationInvalid, "Only an actual unresolved optional import may project unavailable."));
        const auto &code = resolved.Status() == ExtensionServiceImportStatus::Incompatible ? ExtensionErrors::BackendServiceContractMismatch
                                                                                           : ExtensionErrors::BackendServiceUnavailable;
        auto error = Detail::BackendServiceImportError(resolved, code,
                                                       "Optional script import is unavailable; check its declared service contract/version "
                                                       "and enabled provider.");
        return Result<ScriptModuleImport>::Success(ScriptModuleImport{std::move(alias), resolved, {}, std::move(error)});
    }
}  // namespace Horo::Extensions
