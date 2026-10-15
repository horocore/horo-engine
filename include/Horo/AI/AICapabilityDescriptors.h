#pragma once

/** @file AICapabilityDescriptors.h
 * @brief Inert bounded task declarations contributed by one gameplay module generation.
 */

#include "Horo/AI/AIIdentity.h"

#include <array>
#include <optional>
#include <span>

namespace Horo::AI {
    inline constexpr std::size_t MaximumAiCapabilityTasks = 128;

    /** @brief Authoring origin; both forms use the same task lifecycle and admission. */
    enum class AiTaskAuthoring : std::uint8_t {
        Native,
        Script,
        Count
    };

    /** @brief Stable task declaration without executable callbacks or runtime state. */
    struct AiCapabilityTaskDescriptor final {
        TaskId task;
        std::optional<BlackboardSchemaId> blackboard; /**< Required agent schema, when declared. */
        AiTaskAuthoring authoring{AiTaskAuthoring::Native};
    };

    /** @brief Owned frozen declarations; copying cannot retain module strings or callbacks. */
    class AiCapabilityDescriptorSet final {
    public:
        /** @brief Find a task declaration. @param task Persistent identity. @return Copied declaration or absence. */
        [[nodiscard]] std::optional<AiCapabilityTaskDescriptor> Find(TaskId task) const noexcept;
        /** @brief Inspect the stable sorted declarations. @return Borrow valid until this value changes. */
        [[nodiscard]] std::span<const AiCapabilityTaskDescriptor> Tasks() const noexcept;

    private:
        friend class AiCapabilityDescriptorRegistry;
        std::array<AiCapabilityTaskDescriptor, MaximumAiCapabilityTasks> tasks_{};
        std::size_t count_{};
    };

    /** @brief Registration-only metadata authority; registration never activates AI or invokes module code. */
    class AiCapabilityDescriptorRegistry final {
    public:
        /** @brief Register one declaration before freeze. @param descriptor Copied inert metadata.
         * @return Success or typed invalid, conflict, capacity or frozen failure; failure retains prior declarations. */
        [[nodiscard]] Result<void> Register(const AiCapabilityTaskDescriptor &descriptor);
        /** @brief Freeze the complete declaration set. @return Owned immutable copy; repeated freezes are equivalent. */
        [[nodiscard]] AiCapabilityDescriptorSet Freeze() noexcept;

    private:
        AiCapabilityDescriptorSet declarations_;
        bool frozen_{};
    };
}  // namespace Horo::AI
