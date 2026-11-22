#pragma once

/** @file ImmersiveAuthoringContext.h @brief Editor-owned immersive planning generations. */

#include "Horo/Scene/SceneSourceModel.h"
#include "Horo/XR/XRIdentity.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace Horo::Editor {
    enum class ImmersiveAuthoringMode : std::uint8_t {
        Edit,
        Simulate,
        Play
    };
    enum class ImmersiveRequestKind : std::uint8_t {
        Plan,
        Diagnostics
    };
    enum class ImmersiveAdmission : std::uint8_t {
        Admitted,
        StaleContext,
        ModeDenied,
        PermissionDenied,
        InvalidRequest
    };

    /** @brief Values captured from the authenticated editor, document and XR owners. */
    struct ImmersiveContextIdentity final {
        std::uint64_t projectOpening{};
        std::uint64_t documentOpening{};
        std::uint64_t documentRevision{};
        XR::XRSessionId xrSession;
        XR::XRSpaceId referenceSpace;
        std::uint64_t user{};
        std::uint64_t conversation{};
        std::uint64_t toolContext{};
        XR::XRCapabilityRevision capabilities;
        ImmersiveAuthoringMode mode{};
        std::uint64_t worldGeneration{}; /**< Required for a disposable simulation or Play world. */
        bool planningAllowed{};
        bool diagnosticsAllowed{};
        bool operator==(const ImmersiveContextIdentity &) const noexcept = default;
    };

    struct ImmersiveContextEpoch;

    /** @brief Immutable bounded capture. The epoch is only a revocation signal and contains no owner reference. */
    class ImmersivePlanningContext final {
    public:
        static constexpr std::size_t MaximumSelection = 32;

        [[nodiscard]] const ImmersiveContextIdentity &Identity() const & noexcept {
            return identity_;
        }

        [[nodiscard]] const ImmersiveContextIdentity &Identity() const && = delete;

        [[nodiscard]] std::uint64_t Generation() const noexcept {
            return generation_;
        }

        [[nodiscard]] std::span<const SceneSource::SceneObjectId> Selection() const & noexcept {
            return std::span{selection_}.first(selectionCount_);
        }

        [[nodiscard]] std::span<const SceneSource::SceneObjectId> Selection() const && = delete;
        [[nodiscard]] bool IsCurrent() const noexcept;

    private:
        friend class ImmersiveAuthoringContext;
        ImmersiveContextIdentity identity_;
        std::array<SceneSource::SceneObjectId, MaximumSelection> selection_{};
        std::size_t selectionCount_{};
        std::uint64_t generation_{};
        std::shared_ptr<const ImmersiveContextEpoch> epoch_;
    };

    /** @brief Cold owner-lane publication and admission for one Editor opening. */
    class ImmersiveAuthoringContext final {
    public:
        ImmersiveAuthoringContext();
        ~ImmersiveAuthoringContext();
        ImmersiveAuthoringContext(const ImmersiveAuthoringContext &) = delete;
        ImmersiveAuthoringContext &operator=(const ImmersiveAuthoringContext &) = delete;

        /** @brief Publishes the current owner observation. Invalid observations revoke the last generation. */
        [[nodiscard]] std::optional<ImmersivePlanningContext> Publish(const ImmersiveContextIdentity &identity,
                                                                      std::span<const SceneSource::SceneObjectId> selection) noexcept;
        /** @brief Revokes all previously issued contexts before project, document or XR teardown. */
        void Revoke() noexcept;
        /** @brief Rechecks one captured request before any tool authority is consulted.
         * Admitted planning is non-authoritative, including in Simulate. It grants no mutation or approval.
         */
        [[nodiscard]] ImmersiveAdmission Admit(const ImmersivePlanningContext &context, ImmersiveRequestKind kind) const noexcept;

    private:
        std::shared_ptr<ImmersiveContextEpoch> epoch_;
        std::optional<ImmersivePlanningContext> current_;
        std::uint64_t generation_{};
    };
}  // namespace Horo::Editor
