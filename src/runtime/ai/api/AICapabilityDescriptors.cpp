#include "Horo/AI/AICapabilityDescriptors.h"

#include <algorithm>

namespace Horo::AI {
    /** @copydoc AiCapabilityDescriptorSet::Find */
    std::optional<AiCapabilityTaskDescriptor> AiCapabilityDescriptorSet::Find(const TaskId task) const noexcept {
        const auto values = Tasks();
        const auto found = std::ranges::find(values, task, &AiCapabilityTaskDescriptor::task);
        return found == values.end() ? std::nullopt : std::optional{*found};
    }

    /** @copydoc AiCapabilityDescriptorSet::Tasks */
    std::span<const AiCapabilityTaskDescriptor> AiCapabilityDescriptorSet::Tasks() const noexcept {
        return {tasks_.data(), count_};
    }

    /** @copydoc AiCapabilityDescriptorRegistry::Register */
    Result<void> AiCapabilityDescriptorRegistry::Register(const AiCapabilityTaskDescriptor &descriptor) {
        if (frozen_ || !descriptor.task.IsValid() || descriptor.authoring >= AiTaskAuthoring::Count ||
            (descriptor.blackboard && !descriptor.blackboard->IsValid()))
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        if (declarations_.Find(descriptor.task))
            return Result<void>::Failure(MakeError(AIErrors::DescriptorConflict));
        if (declarations_.count_ == MaximumAiCapabilityTasks)
            return Result<void>::Failure(MakeError(AIErrors::DescriptorLimitExceeded));
        declarations_.tasks_[declarations_.count_++] = descriptor;
        return Result<void>::Success();
    }

    /** @copydoc AiCapabilityDescriptorRegistry::Freeze */
    AiCapabilityDescriptorSet AiCapabilityDescriptorRegistry::Freeze() noexcept {
        std::ranges::sort(std::span{declarations_.tasks_.data(), declarations_.count_}, {}, &AiCapabilityTaskDescriptor::task);
        frozen_ = true;
        return declarations_;
    }
}  // namespace Horo::AI
