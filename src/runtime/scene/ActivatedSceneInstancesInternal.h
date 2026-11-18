#pragma once

/** @file ActivatedSceneInstancesInternal.h @brief Private child-runtime ownership of one canonical Scene service. */
#include "Horo/Runtime/Scene/SceneInstances.h"

namespace Horo::Runtime::Detail {
    /** @brief Owns only independent child services, bounded during explicit parent-lane admission. */
    class ActivatedSceneInstances final {
    public:
        [[nodiscard]] static Result<ActivatedSceneInstanceOperation> Queue(
            RuntimeSceneService &parent, const RuntimeSceneDefinition &definition, const ActivatedSceneInstanceRequest &request,
            std::unique_ptr<ScenePublicationCheck> source, std::vector<std::unique_ptr<SceneActivationParticipant>> participants,
            const ActivatedSceneInstanceOperation *replaces);
        [[nodiscard]] static Result<void> Cancel(RuntimeSceneService &parent, const ActivatedSceneInstanceOperation &pending);
        [[nodiscard]] static Result<void> Remove(RuntimeSceneService &parent, const ActivatedSceneInstanceOperation &current);
        [[nodiscard]] static std::optional<RuntimeSceneView> Find(const RuntimeSceneService &parent,
                                                                  const ActivatedSceneInstanceOperation &current) noexcept;
        void Advance(RuntimeSceneService &parent, const FrameContext &context);
        void CancelPending() noexcept;
        /** @brief Revalidates the actual pending owner after child participant validation and before publication. */
        [[nodiscard]] Result<void> ValidateChildPublication(const RuntimeSceneService &child) const;

    private:
        struct Entry final {
            SceneInstanceAddress address;
            SceneRuntimeId identity;
            SceneDefinitionRevision revision;
            CancellationToken parentCancellation;
            std::unique_ptr<RuntimeSceneService> service;
            ScenePublicationReceipt receipt;
        };

        struct Pending final {
            Entry entry;
            CancellationToken cancellation;
            SceneRuntimeId replaces;
        };

        /** @brief Returns true only for the exact non-reusable child operation of this owner. */
        [[nodiscard]] static bool Matches(const Entry &entry, const ActivatedSceneInstanceOperation &operation) noexcept;
        [[nodiscard]] static Result<void> CheckMutation(const RuntimeSceneService &parent, SceneRuntimeId expected);
        void AdvanceRemoval(RuntimeSceneService &parent, const FrameContext &context);
        void AdvancePublication(RuntimeSceneService &parent, const FrameContext &context);

        std::vector<Entry> active_;
        std::optional<Pending> pending_;
        SceneRuntimeId removal_;
    };
}  // namespace Horo::Runtime::Detail
