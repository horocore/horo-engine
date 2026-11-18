#include "Horo/Runtime/Scene/SceneInstances.h"

#include "RuntimeSceneErrors.h"

#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Durable occurrence equality and non-reusable operation ownership are deliberately independent. */
        class EmbeddedSceneOwnership final : public SceneBaselineOwnership {
        public:
            explicit EmbeddedSceneOwnership(std::shared_ptr<const SceneInstanceAddress> occurrence) noexcept
                : occurrence_(std::move(occurrence)) {}

            bool HasAddress(const SceneInstanceAddress &address) const noexcept {
                return *occurrence_ == address;
            }

            const std::shared_ptr<const SceneInstanceAddress> &Occurrence() const noexcept {
                return occurrence_;
            }

            bool Matches(const SceneBaselineOwnership &other) const noexcept override {
                const auto *child = dynamic_cast<const EmbeddedSceneOwnership *>(&other);
                return child && child->occurrence_ == occurrence_;
            }

            bool MatchesOperation(const SceneBaselineOwnership &other) const noexcept override {
                // A caller can reuse the same child revision for a new queue after cancellation.
                return this == &other;
            }

        private:
            std::shared_ptr<const SceneInstanceAddress> occurrence_;
        };

        /** @brief Resolves the exact parent and fails before staging any child from a stale or unsupported request. */
        Result<void> CheckParent(const RuntimeSceneService &service, const SceneInstanceAddress &address,
                                 const SceneStructuralAdmission &admission) {
            if (!address.parent.IsValid() || !address.definition.IsValid() || !address.instance.IsValid() ||
                address.mode > SceneInstanceMode::Activated)
                return Result<void>::Failure(MakeError(SceneErrors::BaselineInvalid));
            if (address.mode != SceneInstanceMode::Embedded)
                return Result<void>::Failure(MakeError(SceneErrors::BaselineUnsupported));
            const auto active = service.ActiveScene();
            if (!active || active->RuntimeId() != address.parent || admission.scene != address.parent)
                return Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
            if (active->DefinitionId() == address.definition)
                return Result<void>::Failure(MakeError(SceneErrors::BaselineInvalid, "A Scene cannot embed itself"));
            return Result<void>::Success();
        }

        /** @brief Requires the exact resident operation even after a detach and same-revision reattach. */
        bool ResidentOperation(const RuntimeSceneService &service, const SceneInstanceAddress &address,
                               const SceneDefinitionRevision revision, const SceneBaselineOwnership *ownership) noexcept {
            const auto parent = service.ActiveScene();
            const auto resident = parent ? FindEmbeddedSceneInstance(*parent, address) : std::nullopt;
            return resident && resident->revision == revision && resident->ownership == ownership;
        }

        /** @brief Retiring an exact child is allowed even after its source stopped publishing. */
        class EmbeddedRemovalCheck final : public ScenePublicationCheck {
        public:
            EmbeddedRemovalCheck(const RuntimeSceneService &service, SceneInstanceAddress address, SceneDefinitionRevision revision,
                                 std::shared_ptr<const SceneBaselineOwnership> ownership) noexcept
                : service_(service), address_(address), revision_(revision), ownership_(std::move(ownership)) {}

            Result<void> ValidatePublication() const override {
                return ResidentOperation(service_, address_, revision_, ownership_.get())
                           ? Result<void>::Success()
                           : Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
            }

        private:
            const RuntimeSceneService &service_;
            SceneInstanceAddress address_;
            SceneDefinitionRevision revision_;
            std::shared_ptr<const SceneBaselineOwnership> ownership_;
        };
    }  // namespace

    SceneInstanceOperation::SceneInstanceOperation(SceneInstanceAddress address, SceneDefinitionRevision revision,
                                                   std::shared_ptr<const SceneBaselineOwnership> ownership,
                                                   std::shared_ptr<const SceneStructuralReceipt> receipt) noexcept
        : address_(address), revision_(revision), ownership_(std::move(ownership)), receipt_(std::move(receipt)) {}

    /** @copydoc SceneInstanceOperation::Cancel */
    Result<void> SceneInstanceOperation::Cancel(RuntimeSceneService &service) const {
        if (!ownership_)
            return Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
        return service.CancelPendingBaseline(address_.parent, address_.definition, *ownership_);
    }

    /** @copydoc QueueEmbeddedSceneInstance */
    Result<SceneInstanceOperation> QueueEmbeddedSceneInstance(RuntimeSceneService &service, const SceneInstanceAddress &address,
                                                              RuntimeSceneDefinition definition,
                                                              std::vector<RuntimeGroupAssetLease> resources,
                                                              const SceneStructuralAdmission &admission,
                                                              SceneBaselineAttachmentLimits limits, const SceneInstanceOperation *replaces,
                                                              std::shared_ptr<const ScenePublicationCheck> sourceCheck) {
        using Outcome = Result<SceneInstanceOperation>;
        if (auto parent = CheckParent(service, address, admission); parent.HasError())
            return Outcome::Failure(parent.ErrorValue());
        if (definition.Id() != address.definition || !sourceCheck)
            return Outcome::Failure(MakeError(SceneErrors::BaselineInvalid));
        if (replaces &&
            (replaces->address_ != address || !ResidentOperation(service, address, replaces->revision_, replaces->ownership_.get())))
            return Outcome::Failure(MakeError(SceneErrors::BaselineStale));
        const auto revision = definition.Revision();
        try {
            const auto occurrence = replaces ? static_cast<const EmbeddedSceneOwnership &>(*replaces->ownership_).Occurrence()
                                             : std::make_shared<const SceneInstanceAddress>(address);
            const auto ownership = std::make_shared<const EmbeddedSceneOwnership>(occurrence);
            const auto expectedRevision = replaces ? replaces->revision_ : SceneDefinitionRevision{};
            SceneCommandBuffer commands;
            if (auto staged = commands.AttachBaseline(std::move(definition), std::move(resources), admission, limits, expectedRevision,
                                                      std::move(sourceCheck), ownership);
                staged.HasError())
                return Outcome::Failure(staged.ErrorValue());
            auto receipt = service.QueueTrackedStructuralCommands(std::move(commands));
            if (receipt.HasError())
                return Outcome::Failure(receipt.ErrorValue());
            return Outcome::Success(SceneInstanceOperation{address, revision, ownership, std::move(receipt).Value()});
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(SceneErrors::BaselineCapacityExceeded));
        }
    }

    /** @copydoc QueueEmbeddedSceneRemoval */
    Result<SceneInstanceOperation> QueueEmbeddedSceneRemoval(RuntimeSceneService &service, const SceneInstanceOperation &current,
                                                             const SceneStructuralAdmission &admission) {
        using Outcome = Result<SceneInstanceOperation>;
        const auto &address = current.address_;
        if (auto parent = CheckParent(service, address, admission); parent.HasError())
            return Outcome::Failure(parent.ErrorValue());
        if (!ResidentOperation(service, address, current.revision_, current.ownership_.get()))
            return Outcome::Failure(MakeError(SceneErrors::BaselineStale));
        try {
            const auto &source = static_cast<const EmbeddedSceneOwnership &>(*current.ownership_);
            const auto ownership = std::make_shared<const EmbeddedSceneOwnership>(source.Occurrence());
            auto check = std::make_shared<const EmbeddedRemovalCheck>(service, address, current.revision_, current.ownership_);
            SceneCommandBuffer commands;
            if (auto staged = commands.DetachBaseline(address.definition, current.revision_, admission, std::move(check), ownership);
                staged.HasError())
                return Outcome::Failure(staged.ErrorValue());
            auto receipt = service.QueueTrackedStructuralCommands(std::move(commands));
            if (receipt.HasError())
                return Outcome::Failure(receipt.ErrorValue());
            return Outcome::Success(SceneInstanceOperation{address, {}, ownership, std::move(receipt).Value()});
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(SceneErrors::BaselineCapacityExceeded));
        }
    }

    /** @copydoc FindEmbeddedSceneInstance */
    std::optional<SceneBaselineAttachmentView> FindEmbeddedSceneInstance(const RuntimeSceneView &parent,
                                                                         const SceneInstanceAddress &address) noexcept {
        if (!parent.IsCurrent() || parent.RuntimeId() != address.parent || address.mode != SceneInstanceMode::Embedded)
            return std::nullopt;
        const auto found = parent.FindBaseline(address.definition);
        const auto *ownership = found ? dynamic_cast<const EmbeddedSceneOwnership *>(found->ownership) : nullptr;
        return ownership && ownership->HasAddress(address) ? found : std::nullopt;
    }
}  // namespace Horo::Runtime
