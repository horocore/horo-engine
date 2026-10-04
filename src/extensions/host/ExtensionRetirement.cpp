#include "Horo/Extensions/ExtensionRetirement.h"

#include <algorithm>
#include <mutex>
#include <type_traits>
#include <utility>

namespace Horo::Extensions {
    struct ExtensionRetirementState final {
        std::string extensionId;
        std::vector<std::string> modules;
        std::vector<std::weak_ptr<void>> code;
        std::vector<bool> bound;

        struct Work final {
            std::size_t id;
            ExtensionOutstandingLease attribution;
        };

        struct Publication final {
            std::size_t moduleIndex;
            std::shared_ptr<void> code;
            std::shared_ptr<IExtensionRetirementContribution> owner;
        };

        std::vector<Work> work;
        std::vector<Publication> publications;
        std::size_t nextId{1};
        bool retiring{};
        bool revoking{};
        bool restartRequired{};

    private:
        friend class ExtensionRetirement;
        friend class ExtensionExecutableLease;
        // Protects admission, diagnostics and publication ownership. Revocation and provider destruction run outside this mutex.
        mutable std::mutex mutex;
    };

    namespace {
        constexpr std::size_t MaximumWork = 4096;
        constexpr std::size_t MaximumPublications = 1024;

        /** @brief Copies a diagnostic while the caller holds the state mutex. */
        ExtensionRetirementReport Snapshot(const ExtensionRetirementState &state) {
            using enum ExtensionRetirementDisposition;
            ExtensionRetirementReport report{.extensionId = state.extensionId};
            if (state.retiring)
                report.disposition = state.work.empty() && !state.revoking ? Complete : Draining;
            if (state.restartRequired)
                report.disposition = RestartRequired;
            for (const auto &work : state.work)
                report.outstanding.push_back(work.attribution);
            return report;
        }
    }  // namespace

    /** @copydoc ExtensionExecutableLease::ExtensionExecutableLease */
    ExtensionExecutableLease::ExtensionExecutableLease(ConstructionKey, std::shared_ptr<void> owner) : owner_(std::move(owner)) {}

    ExtensionExecutableLease::~ExtensionExecutableLease() {
        owner_.reset();
        moduleCode_.reset();
        if (!state_)
            return;
        std::scoped_lock lock{state_->mutex};
        std::erase_if(state_->work, [id = id_](const auto &work) {
            return work.id == id;
        });
    }

    /** @copydoc ExtensionRetirement::ExtensionRetirement */
    ExtensionRetirement::ExtensionRetirement(std::string extensionId, std::vector<std::string> dependencyFirstModules)
        : state_(std::make_shared<ExtensionRetirementState>()) {
        state_->extensionId = std::move(extensionId);
        state_->modules = std::move(dependencyFirstModules);
        state_->code.resize(state_->modules.size());
        state_->bound.resize(state_->modules.size());
    }

    /** @copydoc ExtensionRetirement::~ExtensionRetirement */
    ExtensionRetirement::~ExtensionRetirement() {
        CloseAdmission();
    }

    /** @copydoc ExtensionRetirement::BindModuleCode */
    bool ExtensionRetirement::BindModuleCode(const std::string_view moduleId, const std::shared_ptr<void> &codeOwner) const {
        std::scoped_lock lock{state_->mutex};
        const auto foundModule = std::ranges::find(state_->modules, moduleId);
        if (!codeOwner || state_->retiring || foundModule == state_->modules.end())
            return false;
        const auto index = static_cast<std::size_t>(foundModule - state_->modules.begin());
        auto &code = state_->code[index];
        if (state_->bound[index])
            return false;
        state_->bound[index] = true;
        code = codeOwner;
        return true;
    }

    /** @copydoc ExtensionRetirement::Acquire */
    std::shared_ptr<ExtensionExecutableLease> ExtensionRetirement::Acquire(const std::string_view moduleId, const ExtensionLeaseKind kind,
                                                                           std::string subject, std::shared_ptr<void> owner) const {
        if (!owner || subject.empty())
            return {};
        auto lease = std::make_shared<ExtensionExecutableLease>(ExtensionExecutableLease::ConstructionKey{}, std::move(owner));
        std::shared_ptr<void> code;
        std::scoped_lock lock{state_->mutex};
        const auto foundModule = std::ranges::find(state_->modules, moduleId);
        if (state_->retiring || state_->work.size() >= MaximumWork || state_->nextId == 0 || foundModule == state_->modules.end())
            return {};
        code = state_->code[static_cast<std::size_t>(foundModule - state_->modules.begin())].lock();
        if (!code)
            return {};
        const std::size_t id = state_->nextId++;
        state_->work.push_back({id, {.moduleId = std::string{moduleId}, .subject = std::move(subject), .kind = kind}});
        lease->state_ = state_;
        lease->id_ = id;
        lease->moduleCode_ = std::move(code);
        return lease;
    }

    /** @copydoc ExtensionRetirement::RegisterContribution */
    bool ExtensionRetirement::RegisterContribution(const std::string_view moduleId,
                                                   std::shared_ptr<IExtensionRetirementContribution> contribution) const {
        std::shared_ptr<void> code;
        std::scoped_lock lock{state_->mutex};
        const auto foundModule = std::ranges::find(state_->modules, moduleId);
        if (!contribution || state_->retiring || foundModule == state_->modules.end() || state_->publications.size() >= MaximumPublications)
            return false;
        const auto index = static_cast<std::size_t>(foundModule - state_->modules.begin());
        code = state_->code[index].lock();
        if (!code)
            return false;
        static_assert(std::is_nothrow_move_constructible_v<ExtensionRetirementState::Publication>);
        // Allocate before moving native owners into a temporary under the state mutex.
        // On failure the lock guard unwinds before either caller ownership or the code pin.
        state_->publications.reserve(state_->publications.size() + 1);
        state_->publications.emplace_back(index, std::move(code), std::move(contribution));
        return true;
    }

    /** @copydoc ExtensionRetirement::BeginRetirement */
    ExtensionRetirementReport ExtensionRetirement::BeginRetirement() const {
        CloseAdmission();
        return Inspect();
    }

    /** @copydoc ExtensionRetirement::CloseAdmission */
    void ExtensionRetirement::CloseAdmission() const noexcept {
        std::vector<ExtensionRetirementState::Publication> publications;
        {
            std::scoped_lock lock{state_->mutex};
            if (state_->retiring)
                return;
            state_->retiring = true;
            state_->revoking = true;
            publications = std::move(state_->publications);
        }
        for (std::size_t moduleIndex = state_->modules.size(); moduleIndex > 0; --moduleIndex) {
            for (std::size_t index = publications.size(); index > 0; --index) {
                auto &publication = publications[index - 1];
                if (publication.moduleIndex == moduleIndex - 1) {
                    publication.owner->Revoke();
                    publication.owner.reset();
                    publication.code.reset();
                }
            }
        }
        std::scoped_lock lock{state_->mutex};
        state_->revoking = false;
    }

    /** @copydoc ExtensionRetirement::Inspect */
    ExtensionRetirementReport ExtensionRetirement::Inspect() const {
        std::scoped_lock lock{state_->mutex};
        return Snapshot(*state_);
    }

    /** @copydoc ExtensionRetirement::IsDrained */
    bool ExtensionRetirement::IsDrained() const noexcept {
        std::scoped_lock lock{state_->mutex};
        return state_->retiring && !state_->revoking && !state_->restartRequired && state_->work.empty();
    }

    /** @copydoc ExtensionRetirement::RequireRestart */
    void ExtensionRetirement::RequireRestart() const noexcept {
        std::scoped_lock lock{state_->mutex};
        state_->restartRequired = true;
    }
}  // namespace Horo::Extensions
