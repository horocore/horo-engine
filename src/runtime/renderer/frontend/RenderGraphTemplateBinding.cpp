#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderExternalImages.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphTemplate.h"

#include <algorithm>
#include <type_traits>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Retains the single binding admission flag across provider reentry and every early return. */
        struct BindingScope final {
            explicit BindingScope(bool &flag) : value(flag) {
                flag = true;
            }

            BindingScope(const BindingScope &) = delete;
            BindingScope &operator=(const BindingScope &) = delete;

            ~BindingScope() {
                value = false;
            }

            bool &value;
        };

        /** @brief Compares the entire allocation shape; matching format alone cannot admit a different native view. */
        bool SameDescriptor(const RenderTextureDescriptor &left, const RenderTextureDescriptor &right) noexcept {
            return left.dimension == right.dimension && left.extent == right.extent && left.format == right.format &&
                   left.mipCount == right.mipCount && left.layerCount == right.layerCount && left.sampleCount == right.sampleCount &&
                   left.usage == right.usage && left.depth == right.depth;
        }
    }  // namespace

    /** @copydoc PreparedRenderGraphTemplate::ValidateResidentSuperset */
    Result<void> PreparedRenderGraphTemplate::ValidateResidentSuperset(const RenderFrontend &frontend,
                                                                       const CompiledRenderGraphExecution &execution) const {
        for (std::size_t index = 0; index < execution.resources_.size(); ++index) {
            const auto &resource = execution.resources_[index];
            if (reads_.externalSlots[index] || resource.resourceClass == RenderGraphResourceClass::Transient)
                continue;
            auto ready = std::visit([&frontend]<typename Handle>(const Handle &handle) -> Result<void> {
                if constexpr (std::is_same_v<std::decay_t<decltype(handle)>, std::monostate>)
                    return Result<void>::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
                else {
                    const auto state = frontend.ResourceState(handle);
                    if (state.HasError())
                        return Result<void>::Failure(state.ErrorValue());
                    return state.Value() == RenderResourceState::Ready ? Result<void>::Success()
                                                                       : Result<void>::Failure(MakeError(FrontendErrors::ResourceNotReady));
                }
            }, resource.binding);
            if (ready.HasError())
                return ready;
        }
        return Result<void>::Success();
    }

    /** @copydoc PreparedRenderGraphTemplate::SelectReads */
    Result<void> PreparedRenderGraphTemplate::SelectReads(CompiledRenderGraphExecution &execution,
                                                          const std::span<const RenderGraphOptionalRead> reads) {
        if (reads_.declarations.empty()) {
            return reads.empty() ? Result<void>::Success() : Result<void>::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
        }
        if (reads.size() > reads_.declarations.size())
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        std::ranges::fill(reads_.selected, 0);
        for (const auto &read : reads) {
            if (read.resource.owner != owner_ || read.resource.value == 0 || read.resource.value > reads_.roles.size())
                return Result<void>::Failure(MakeError(FrontendErrors::ResourceWrongOwner));
            const auto index = read.resource.value - 1;
            if (reads_.selected[index] || reads_.roles[index] != static_cast<int>(read.role))
                return Result<void>::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
            reads_.selected[index] = 1;
        }
        // PERF: capacity is the cold complete-use set; selection only shrinks/reuses that backing.
        execution.usages_.clear();
        for (const auto &use : reads_.completeUses) {
            const auto index = use.resource.value - 1;
            if (reads_.roles[index] == -1 || reads_.selected[index])
                execution.usages_.push_back(use);
        }
        execution.passes_.front().usages = {0, execution.usages_.size()};
        return Result<void>::Success();
    }

    /** @copydoc PreparedRenderGraphTemplate::Bind */
    Result<const CompiledRenderGraphExecution *> PreparedRenderGraphTemplate::Bind(
        const RenderFrontend &frontend, const std::size_t instance, const std::span<const RenderGraphExternalTextureBinding> bindings) {
        return Bind(frontend, instance, bindings, reads_.declarations);
    }

    /** @copydoc PreparedRenderGraphTemplate::Bind */
    Result<const CompiledRenderGraphExecution *> PreparedRenderGraphTemplate::Bind(
        const RenderFrontend &frontend, const std::size_t instance, const std::span<const RenderGraphExternalTextureBinding> bindings,
        const std::span<const RenderGraphOptionalRead> reads) {
        using Outcome = Result<const CompiledRenderGraphExecution *>;
        if (std::this_thread::get_id() != lane_ || lane_ != frontend.preparationOwner_ || frontend.activeFrameScope_ || frontend.closing_ ||
            binding_)
            return Outcome::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        if (instance >= instances_.size())
            return Outcome::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        auto &execution = instances_[instance];
        auto &state = *execution.templateBinding_;
        if (state.inFlight_)
            return Outcome::Failure(MakeError(FrontendErrors::ResourceNotReady));

        BindingScope scope{binding_};

        // Failed validation revokes the old unsubmitted binding, never a queued plan.
        execution.owner_ = {};
        state.bound_ = false;
        if (bindings.size() != slots_.size())
            return Outcome::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            const auto &slot = slots_[index];
            if (bindings[index].resource != slot.resource)
                return Outcome::Failure(MakeError(FrontendErrors::ResourceWrongOwner));
            const auto facts = RenderExternalTextureAccess::Describe(frontend, bindings[index].texture);
            if (facts.HasError())
                return Outcome::Failure(facts.ErrorValue());
            if (!SameDescriptor(slot.descriptor, facts.Value().descriptor) || slot.deviceIncarnation != facts.Value().deviceIncarnation ||
                slot.acquired != facts.Value().acquired || slot.requiredFinal != facts.Value().requiredFinal)
                return Outcome::Failure(MakeError(FrontendErrors::ResourceUnsupported));
            for (std::size_t previous = 0; previous < index; ++previous)
                if (bindings[previous].texture == bindings[index].texture)
                    return Outcome::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
        }
        if (auto resident = ValidateResidentSuperset(frontend, execution); resident.HasError())
            return Outcome::Failure(resident.ErrorValue());
        if (auto selected = SelectReads(execution, reads); selected.HasError())
            return Outcome::Failure(selected.ErrorValue());
        // PERF: all storage was sized during Prepare; only typed resident identities change here.
        for (std::size_t index = 0; index < slots_.size(); ++index)
            execution.resources_[slots_[index].resource.value - 1].binding = bindings[index].texture;
        execution.owner_ = owner_;
        state.bound_ = true;
        return Outcome::Success(&execution);
    }
}  // namespace Horo::Render::Detail
