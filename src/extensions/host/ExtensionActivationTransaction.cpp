#include "ExtensionActivationTransaction.h"

#include "Horo/Foundation/Diagnostics.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <iterator>

namespace Horo::Extensions {
    namespace {
        constexpr std::string_view RollbackDiagnosticCode = "extension.activation.rollback_cleanup_failed";

        void AppendRollbackDiagnostic(Error &error, const std::string_view moduleId) {
            try {
                error.diagnostics.push_back({
                    .code = DiagnosticCode{std::string{RollbackDiagnosticCode}},
                    .severity = DiagnosticSeverity::Error,
                    .message = "Module unload failed while rolling back activation: " + std::string{moduleId},
                });
            } catch (...) {  // NOSONAR(cpp:S1181) Rollback reporting must not prevent remaining cleanup.
                LOG_WARN("extensions", "Could not retain the rollback failure diagnostic for module %.*s.",
                         static_cast<int>(moduleId.size()), moduleId.data());
            }
        }
    }  // namespace

    /** @copydoc ExtensionActivationTransaction::~ExtensionActivationTransaction */
    ExtensionActivationTransaction::~ExtensionActivationTransaction() {
        RollbackWithoutDiagnostics();
    }

    /** @copydoc ExtensionActivationTransaction::Stage */
    void ExtensionActivationTransaction::Stage(std::shared_ptr<ExtensionModuleLifetime> lifetime,
                                               std::vector<Assets::AssetImporterContribution> contributions) {
        lifetimes_.push_back(std::move(lifetime));
        contributions_.insert(contributions_.end(), std::make_move_iterator(contributions.begin()),
                              std::make_move_iterator(contributions.end()));
    }

    /** @copydoc ExtensionActivationTransaction::Contributions */
    std::vector<Assets::AssetImporterContribution> &ExtensionActivationTransaction::Contributions() noexcept {
        return contributions_;
    }

    /** @copydoc ExtensionActivationTransaction::Rollback */
    Error ExtensionActivationTransaction::Rollback(Error primary) {
        DiscardContributions();
        RollbackLifetimes(&primary);
        return primary;
    }

    /** @copydoc ExtensionActivationTransaction::ReleaseLifetimes */
    std::vector<std::shared_ptr<ExtensionModuleLifetime>> ExtensionActivationTransaction::ReleaseLifetimes() noexcept {
        released_ = true;
        return std::move(lifetimes_);
    }

    void ExtensionActivationTransaction::DiscardContributions() noexcept {
        while (!contributions_.empty())
            contributions_.pop_back();
    }

    void ExtensionActivationTransaction::RollbackLifetimes(Error *const primary) noexcept {
        while (!lifetimes_.empty()) {
            std::shared_ptr<ExtensionModuleLifetime> lifetime = std::move(lifetimes_.back());
            lifetimes_.pop_back();
            if (lifetime != nullptr)
                if (const auto retirement = lifetime->retirement.lock())
                    retirement->CloseAdmission();
            if (lifetime == nullptr || lifetime->UnloadNow())
                continue;
            if (primary != nullptr)
                AppendRollbackDiagnostic(*primary, lifetime->moduleId);
            else
                LOG_WARN("extensions", "Module unload failed during activation transaction rollback: %s", lifetime->moduleId.c_str());
        }
        released_ = true;
    }

    void ExtensionActivationTransaction::RollbackWithoutDiagnostics() noexcept {
        if (released_)
            return;
        DiscardContributions();
        RollbackLifetimes(nullptr);
    }
}  // namespace Horo::Extensions
