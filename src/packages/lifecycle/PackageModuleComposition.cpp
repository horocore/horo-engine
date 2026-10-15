#include "PackageModuleComposition.h"

#include "Horo/Packages/PackageLifecycleErrors.h"

#include <algorithm>
#include <exception>
#include <tuple>

namespace Horo::Packages::Detail {
    namespace {
        /** @brief Allocates shared dependency ownership before a host can publish executable work. */
        struct ActivationOwnerSlot final {
            std::unique_ptr<IPackageModuleActivation> owner;
        };

        /** @brief Rejects unknown role values before any externally owned host callback. */
        bool KnownRole(const Extensions::ExtensionModuleRole role) noexcept {
            using enum Extensions::ExtensionModuleRole;
            return role == BackendCapability || role == EditorPresentation || role == HeadlessTooling || role == ScriptProvider ||
                   role == RuntimeParticipant;
        }
    }  // namespace

    /** @copydoc PackageModuleComposition::PackageModuleComposition */
    PackageModuleComposition::PackageModuleComposition(std::shared_ptr<const VerifiedPackageInstallRecord> install,
                                                       const std::uint64_t generation, const bool hasRetirementReservation)
        : snapshot_(std::make_shared<PackageActivationSnapshot>()), hasRetirementReservation_(hasRetirementReservation) {
        snapshot_->install = std::move(install);
        snapshot_->generation = generation;
    }

    /** @copydoc PackageModuleComposition::~PackageModuleComposition */
    PackageModuleComposition::~PackageModuleComposition() noexcept {
        Retire();
    }

    /** @copydoc PackageModuleComposition::RejectEntry */
    Result<void> PackageModuleComposition::RejectEntry(Entry &entry, Error error, const PackageModuleOutcome outcome) {
        entry.pending = false;
        entry.report.outcome = outcome;
        entry.report.diagnostic = error;
        entry.report.phases.push_back(outcome);
        if (entry.admission)
            entry.admission->Revoke();
        if (entry.owner)
            entry.owner->Retire();
        if (entry.request.requirement == PackageModuleRequirement::Required)
            return Result<void>::Failure(std::move(error));
        return Result<void>::Success();
    }

    /** @copydoc PackageModuleComposition::PrepareEntries */
    Result<void> PackageModuleComposition::PrepareEntries(const std::span<const EnabledPackageModule> enabled,
                                                          const PackageLifecycleConfiguration &configuration,
                                                          const IPackageExtensionTrust &trust) {
        entries_.reserve(enabled.size());
        activationOrder_.reserve(enabled.size());
        snapshot_->activations.reserve(enabled.size());
        snapshot_->modules.reserve(enabled.size());
        for (const auto &request : enabled) {
            if (!KnownRole(request.role) ||
                (request.requirement != PackageModuleRequirement::Required && request.requirement != PackageModuleRequirement::Optional) ||
                request.module.empty() || request.module.size() > 256 || std::ranges::any_of(entries_, [&request](const auto &entry) {
                return entry.request.package == request.package && entry.request.module == request.module &&
                       entry.request.role == request.role;
            }))
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            Entry entry{request,
                        {request.package,
                         request.module,
                         request.role,
                         {},
                         snapshot_->generation,
                         PackageModuleOutcome::Enabled,
                         {},
                         {PackageModuleOutcome::Installed, PackageModuleOutcome::Enabled}}};
            const auto &packages = snapshot_->install->Graph()->packages;
            const auto package = std::ranges::find(packages, request.package, [](const auto &value) {
                return value.lock.package;
            });
            if (package == packages.end())
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            auto declared = ReadDeclaredPackageDescriptor(*package, request.descriptor);
            if (declared.HasError())
                return Result<void>::Failure(declared.ErrorValue());
            const auto module =
                std::ranges::find(declared.Value().manifest.modules, request.module, &Extensions::ExtensionModuleManifest::id);
            if (module == declared.Value().manifest.modules.end() || std::ranges::find(module->roles, request.role) == module->roles.end())
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            auto prepared = PrepareHost(entry, configuration, trust);
            entries_.push_back(std::move(entry));
            if (prepared.HasError())
                return prepared;
        }
        return Result<void>::Success();
    }

    /** @copydoc PackageModuleComposition::PrepareHost */
    Result<void> PackageModuleComposition::PrepareHost(Entry &entry, const PackageLifecycleConfiguration &configuration,
                                                       const IPackageExtensionTrust &trust) {
        const auto &request = entry.request;
        const bool presentationOmitted = configuration.profile == Extensions::ExtensionHostProfile::Headless &&
                                         request.role == Extensions::ExtensionModuleRole::EditorPresentation;
        for (const auto &host : configuration.moduleHosts)
            if (!presentationOmitted && host->Supports(request.role)) {
                entry.host = host;
                break;
            }
        if (!entry.host) {
            entry.report.host = "composition.unavailable";
            auto rejected = RejectEntry(entry,
                                        MakeError(PackageLifecycleErrors::InvalidCandidate,
                                                  "The selected role has no owning host in this composition."),
                                        PackageModuleOutcome::Unavailable);
            return rejected;
        }
        entry.report.host = entry.host->Identity();
        entry.report.phases.reserve(16);
        auto prepared = PackageModuleCandidateFactory::Prepare(snapshot_->install, request, configuration, trust, snapshot_->generation,
                                                               entry.report.host, entry.admission);
        if (prepared.HasError()) {
            const auto outcome = prepared.ErrorValue().code.Value() == PackageLifecycleErrors::TrustRequired.code.Value()
                                     ? PackageModuleOutcome::Denied
                                     : PackageModuleOutcome::Failed;
            auto rejected = RejectEntry(entry, prepared.ErrorValue(), outcome);
            return rejected;
        }
        entry.module.emplace(std::move(prepared).Value());
        entry.report = entry.module->Identity();
        entry.report.phases.reserve(16);
        entry.pending = true;
        return Result<void>::Success();
    }

    /** @copydoc PackageModuleComposition::DependenciesReady */
    Result<bool> PackageModuleComposition::DependenciesReady(Entry &entry) {
        entry.dependencies.clear();
        for (const auto &required : entry.module->RequiredModules()) {
            bool found = false;
            for (const auto &provider : entries_) {
                if (provider.request.package != entry.request.package || provider.request.module != required)
                    continue;
                found = true;
                if (provider.pending)
                    return Result<bool>::Success(false);
                if (!provider.owner || !provider.owner->IsActive())
                    return Result<bool>::Failure(
                        MakeError(PackageLifecycleErrors::InvalidCandidate, "A required module role is unavailable."));
                entry.dependencies.push_back(provider.owner);
            }
            if (!found)
                return Result<bool>::Failure(
                    MakeError(PackageLifecycleErrors::InvalidCandidate, "A required local module was not enabled."));
        }
        const auto &packages = snapshot_->install->Graph()->packages;
        const auto package = std::ranges::find(packages, entry.request.package, [](const auto &value) {
            return value.lock.package;
        });
        for (const auto &dependency : package->lock.dependencies)
            for (const auto &provider : entries_) {
                if (provider.request.package != dependency.package)
                    continue;
                if (provider.pending)
                    return Result<bool>::Success(false);
                if (provider.owner && provider.owner->IsActive())
                    entry.dependencies.push_back(provider.owner);
            }
        return Result<bool>::Success(true);
    }

    /** @copydoc PackageModuleComposition::ActivateEntry */
    Result<void> PackageModuleComposition::ActivateEntry(Entry &entry, const CancellationToken &cancellation) {
        try {
            const auto slot = std::make_shared<ActivationOwnerSlot>();
            auto activated = entry.host->Activate(*entry.module, entry.dependencies, cancellation);
            if (activated.HasError())
                return RejectEntry(entry, activated.ErrorValue(), PackageModuleOutcome::Failed);
            slot->owner = std::move(activated).Value();
            if (slot->owner)
                entry.owner = std::shared_ptr<IPackageModuleActivation>(slot, slot->owner.get());
            if (entry.owner)
                activationOrder_.push_back(static_cast<std::size_t>(&entry - entries_.data()));
            if (!entry.owner || !entry.owner->IsActive())
                return RejectEntry(entry,
                                   entry.owner && entry.owner->Diagnostic() ? *entry.owner->Diagnostic()
                                                                            : MakeError(PackageLifecycleErrors::InvalidCandidate,
                                                                                        "The owning host returned no live activation."),
                                   PackageModuleOutcome::Failed);
            entry.pending = false;
            entry.report.phases.push_back(PackageModuleOutcome::Loaded);
            entry.report.phases.push_back(PackageModuleOutcome::Active);
            entry.report.outcome = PackageModuleOutcome::Active;
            snapshot_->activations.push_back(entry.module->Activation());
            return Result<void>::Success();
        } catch (const std::exception &error) {
            return RejectEntry(entry, MakeError(PackageLifecycleErrors::InvalidCandidate, error.what()), PackageModuleOutcome::Failed);
        } catch (...) {
            return RejectEntry(entry,
                               MakeError(PackageLifecycleErrors::InvalidCandidate, "The owning module host threw during activation."),
                               PackageModuleOutcome::Failed);
        }
    }

    /** @copydoc PackageModuleComposition::Prepare */
    Result<void> PackageModuleComposition::Prepare(const std::span<const EnabledPackageModule> enabled,
                                                   const PackageLifecycleConfiguration &configuration, const IPackageExtensionTrust &trust,
                                                   const CancellationToken &cancellation, const bool &shutdownRequested) {
        if (auto prepared = PrepareEntries(enabled, configuration, trust); prepared.HasError())
            return prepared;
        while (std::ranges::any_of(entries_, [](const auto &entry) {
            return entry.pending;
        })) {
            bool advanced = false;
            for (auto &entry : entries_) {
                if (!entry.pending)
                    continue;
                if (cancellation.IsCancellationRequested() || shutdownRequested)
                    return Result<void>::Failure(MakeError(PackageLifecycleErrors::Cancelled));
                auto ready = DependenciesReady(entry);
                if (ready.HasError()) {
                    if (auto rejected = RejectEntry(entry, ready.ErrorValue(), PackageModuleOutcome::Unavailable); rejected.HasError())
                        return rejected;
                    advanced = true;
                    continue;
                }
                if (!ready.Value())
                    continue;
                if (auto activated = ActivateEntry(entry, cancellation); activated.HasError())
                    return activated;
                advanced = true;
            }
            if (!advanced)
                return Result<void>::Failure(
                    MakeError(PackageLifecycleErrors::InvalidCandidate, "The enabled module dependency graph cannot make progress."));
        }
        for (const auto &entry : entries_)
            snapshot_->modules.push_back(entry.report);
        return Result<void>::Success();
    }

    /** @copydoc PackageModuleComposition::Retire */
    void PackageModuleComposition::Retire() noexcept {
        if (retired_)
            return;
        retired_ = true;
        // Close every grant before the first host callback, then withdraw dependents before their providers.
        for (auto &entry : entries_) {
            if (entry.admission)
                entry.admission->Revoke();
            if (entry.report.outcome == PackageModuleOutcome::Active) {
                entry.report.outcome = PackageModuleOutcome::Retiring;
                entry.report.phases.push_back(PackageModuleOutcome::Retiring);
            }
        }
        for (auto index = activationOrder_.rbegin(); index != activationOrder_.rend(); ++index)
            if (entries_[*index].owner)
                entries_[*index].owner->Retire();
        snapshot_.reset();
    }

    /** @copydoc PackageModuleComposition::Finalize */
    bool PackageModuleComposition::Finalize() {
        if (!retired_)
            return false;
        // A host call that returned no receipt has finished; it cannot retain executable work under the host contract.
        // Its prepared dependency references must not prevent an actual provider receipt from reaching its final owner.
        for (auto &entry : entries_)
            if (!entry.owner)
                entry.dependencies.clear();
        // A dependent retains each provider; only the final framework owner may finalize it.
        // Actual activation order also covers an inactive but still draining returned receipt.
        for (auto iterator = activationOrder_.rbegin(); iterator != activationOrder_.rend(); ++iterator) {
            auto &entry = entries_[*iterator];
            if (!entry.owner || entry.owner.use_count() != 1 || !entry.owner->Finalize())
                continue;
            entry.owner.reset();
            entry.dependencies.clear();
            entry.module.reset();
        }
        return std::ranges::none_of(entries_, [](const auto &entry) {
            return static_cast<bool>(entry.owner);
        });
    }

    /** @copydoc PackageModuleComposition::Reports */
    std::vector<PackageModuleReport> PackageModuleComposition::Reports() const {
        std::vector<PackageModuleReport> result;
        result.reserve(entries_.size());
        for (const auto &entry : entries_)
            result.push_back(entry.report);
        return result;
    }

    /** @copydoc PackageModuleComposition::Snapshot */
    std::shared_ptr<const PackageActivationSnapshot> PackageModuleComposition::Snapshot() const noexcept {
        return snapshot_;
    }
}  // namespace Horo::Packages::Detail
