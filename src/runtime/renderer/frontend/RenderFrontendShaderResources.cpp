#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderShaderResourceErrors.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "RenderFrontendErrors.h"
#include "RenderFrontendResourceAccess.h"
#include "RenderResourceRegistry.h"

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace Horo::Render {
    namespace {
        using Detail::RenderResourceClass;
        using Detail::RenderResourceIdentity;
        using Detail::ResourceReservation;

        /** @brief Deletes only an unpublished preparation candidate, never a registry-owned generation. */
        void DestroyCandidate(IRenderBackend &backend, const RenderResourceClass resourceClass, const std::uint64_t instance) noexcept {
            if (resourceClass == RenderResourceClass::ShaderModule)
                backend.DestroyShaderModule(instance);
            else if (resourceClass == RenderResourceClass::Pipeline)
                backend.DestroyGraphicsPipeline(instance);
            else if (resourceClass == RenderResourceClass::Buffer)
                backend.DestroyBuffer(instance);
            else
                backend.DestroySampler(instance);
        }

        /** @brief Borrows the sole registry/budget while owning a candidate's reservation, native instance and admitted backing. */
        class PublicationGuard final {
        public:
            PublicationGuard(Detail::RenderResourceRegistry &registry, RenderMemoryBudget &budget, IRenderBackend &backend,
                             const RenderResourceClass resourceClass, const ResourceReservation reservation) noexcept
                : resource_(registry, resourceClass, reservation), budget_(budget), backend_(backend), resourceClass_(resourceClass) {}

            ~PublicationGuard() noexcept {
                if (!armed_)
                    return;
                if (native_ != 0)
                    DestroyCandidate(backend_, resourceClass_, native_);
                if (allocation_.IsValid()) {
                    static_cast<void>(budget_.BeginRetire(allocation_));
                    static_cast<void>(budget_.AcknowledgeRetirement(allocation_));
                } else if (memory_.IsValid())
                    static_cast<void>(budget_.Cancel(memory_));
            }

            PublicationGuard(const PublicationGuard &) = delete;
            PublicationGuard &operator=(const PublicationGuard &) = delete;
            PublicationGuard(PublicationGuard &&) = delete;
            PublicationGuard &operator=(PublicationGuard &&) = delete;

            /** @brief Takes an admitted reservation before any placement/result metadata can fail. */
            void OwnMemory(const RenderMemoryReservationId reservation) noexcept {
                memory_ = reservation;
            }

            /** @brief Takes a native candidate before copying errors or committing backing. */
            void OwnNative(const std::uint64_t native) noexcept {
                native_ = native;
            }

            /** @brief Takes committed backing before registry publication. */
            void OwnAllocation(const RenderMemoryAllocationId allocation) noexcept {
                allocation_ = allocation;
            }

            /** @brief Transfers native/backing ownership only after exact registry publication. */
            void Commit() noexcept {
                resource_.Commit();
                armed_ = false;
            }

        private:
            Detail::ResourceReservationGuard resource_;
            RenderMemoryBudget &budget_;
            IRenderBackend &backend_;
            RenderResourceClass resourceClass_;
            RenderMemoryReservationId memory_;
            RenderMemoryAllocationId allocation_;
            std::uint64_t native_{0};
            bool armed_{true};
        };

        static_assert(!std::is_copy_constructible_v<PublicationGuard> && !std::is_copy_assignable_v<PublicationGuard> &&
                      !std::is_move_constructible_v<PublicationGuard> && !std::is_move_assignable_v<PublicationGuard>);

        /** @brief Executes one synchronous loading transaction with rollback guarding every fallible ownership transition. */
        template <typename Query, typename Create>
        [[nodiscard]] Result<ResourceReservation> PrepareResource(Detail::RenderResourceRegistry &registry, RenderMemoryBudget &budget,
                                                                  IRenderBackend &backend, const RenderMemoryScopeId scope,
                                                                  const RenderResourceClass resourceClass,
                                                                  const std::span<const RenderResourceIdentity> dependencies, Query &&query,
                                                                  Create &&create) {
            const auto reserved = registry.Reserve(resourceClass, dependencies);
            if (reserved.HasError())
                return Result<ResourceReservation>::Failure(reserved.ErrorValue());
            PublicationGuard guard{registry, budget, backend, resourceClass, reserved.Value()};
            const auto cost = query();
            if (cost.HasError())
                return Result<ResourceReservation>::Failure(cost.ErrorValue());
            const auto memory = budget.Reserve(scope, reserved.Value().operation, cost.Value());
            if (memory.HasError())
                return Result<ResourceReservation>::Failure(memory.ErrorValue());
            guard.OwnMemory(memory.Value());
            const auto placement = budget.Placement(memory.Value());
            if (placement.HasError())
                return Result<ResourceReservation>::Failure(placement.ErrorValue());
            const auto native = create(placement.Value());
            if (native.HasError())
                return Result<ResourceReservation>::Failure(native.ErrorValue());
            guard.OwnNative(native.Value());
            if (native.Value() == 0)
                return Result<ResourceReservation>::Failure(MakeError(FrontendErrors::ResourceBackendInstanceInvalid));
            const auto allocation = budget.Commit(memory.Value());
            if (allocation.HasError())
                return Result<ResourceReservation>::Failure(allocation.ErrorValue());
            guard.OwnAllocation(allocation.Value().id);
            const auto published = registry.Publish(resourceClass, reserved.Value().identity, native.Value(), allocation.Value().id);
            if (published.HasError())
                return Result<ResourceReservation>::Failure(published.ErrorValue());
            guard.Commit();
            return Result<ResourceReservation>::Success(reserved.Value());
        }

        /** @brief Owns exact registry pins and admitted parameter buffers for one immutable native material table. */
        class FrontendMaterialBinding final : public IResidentMaterialBinding {
        public:
            explicit FrontendMaterialBinding(Detail::RenderResourceRegistry &registry) noexcept : registry_(registry) {}

            ~FrontendMaterialBinding() override {
                native.reset();
                for (std::size_t index = 0; index < bufferCount_; ++index)
                    static_cast<void>(registry_.Release(RenderResourceClass::Buffer, buffers_[index]));
                for (std::size_t index = 0; index < pinCount_; ++index)
                    static_cast<void>(registry_.ReleaseSubmissionPin(pins_[index].resourceClass, pins_[index].identity));
                static_cast<void>(registry_.DrainRetirements());
            }

            FrontendMaterialBinding(const FrontendMaterialBinding &) = delete;
            FrontendMaterialBinding &operator=(const FrontendMaterialBinding &) = delete;

            /** @copydoc IResidentMaterialBinding::NativeBinding */
            const IResidentMaterialBinding &NativeBinding() const noexcept override {
                return native->NativeBinding();
            }

            /** @brief Pins before returning a native instance; fixed storage cannot fail after registry mutation. */
            Result<std::uint64_t> Resolve(const RenderResourceClass resourceClass, const RenderResourceIdentity identity) {
                if (pinCount_ == pins_.size())
                    return Result<std::uint64_t>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
                const auto instance = registry_.BackendInstance(resourceClass, identity);
                if (instance.HasError())
                    return Result<std::uint64_t>::Failure(instance.ErrorValue());
                const auto pinned = registry_.AddSubmissionPin(resourceClass, identity);
                if (pinned.HasError())
                    return Result<std::uint64_t>::Failure(pinned.ErrorValue());
                pins_[pinCount_++] = {resourceClass, identity};
                return instance;
            }

            /** @brief Takes rollback ownership immediately after parameter buffer publication. */
            void OwnBuffer(const RenderResourceIdentity identity) noexcept {
                buffers_[bufferCount_++] = identity;
            }

            std::unique_ptr<IResidentMaterialBinding> native;

        private:
            struct Pin {
                RenderResourceClass resourceClass;
                RenderResourceIdentity identity;
            };

            Detail::RenderResourceRegistry &registry_;
            std::array<Pin, 129> pins_;
            std::array<RenderResourceIdentity, 128> buffers_;
            std::size_t pinCount_{0};
            std::size_t bufferCount_{0};
        };

        /** @brief Removes discovery immediately while retaining the registry's normal dependency/completion retirement rules. */
        [[nodiscard]] Result<void> ReleasePrepared(Detail::RenderResourceRegistry &registry, const RenderResourceClass resourceClass,
                                                   const RenderResourceIdentity identity) {
            const auto released = registry.Release(resourceClass, identity);
            if (released.HasValue())
                static_cast<void>(registry.DrainRetirements());
            return released;
        }
    }  // namespace

    Result<std::uint64_t> RenderFrontend::BackendInstance(const RenderPipelineHandle pipeline) const {
        return resourceRegistry_->BackendInstance(RenderResourceClass::Pipeline, {pipeline.owner, pipeline.slot, pipeline.generation});
    }

    Result<std::uint64_t> RenderFrontend::BackendInstance(const RenderSamplerHandle sampler) const {
        return resourceRegistry_->BackendInstance(RenderResourceClass::Sampler, {sampler.owner, sampler.slot, sampler.generation});
    }

    /** @copydoc Detail::RenderFrontendResourceAccess::BackendInstance(const RenderFrontend &, RenderPipelineHandle) */
    Result<std::uint64_t> Detail::RenderFrontendResourceAccess::BackendInstance(const RenderFrontend &frontend,
                                                                                const RenderPipelineHandle pipeline) {
        return frontend.BackendInstance(pipeline);
    }

    /** @copydoc Detail::RenderFrontendResourceAccess::BackendInstance(const RenderFrontend &, RenderSamplerHandle) */
    Result<std::uint64_t> Detail::RenderFrontendResourceAccess::BackendInstance(const RenderFrontend &frontend,
                                                                                const RenderSamplerHandle sampler) {
        return frontend.BackendInstance(sampler);
    }

    /** @copydoc RenderFrontend::Realize */
    Result<std::unique_ptr<IResidentMaterialBinding>> RenderFrontend::Realize(const MaterialBindingDescriptor &descriptor) {
        using Realization = Result<std::unique_ptr<IResidentMaterialBinding>>;
        if (std::this_thread::get_id() != preparationOwner_)
            return Realization::Failure(MakeError(RenderShaderResourceErrors::WrongThread));
        if (activeFrameScope_)
            return Realization::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        const auto validated = ValidateMaterialBindingDescriptor(descriptor, resourceRegistry_->Owner());
        if (validated.HasError())
            return Realization::Failure(validated.ErrorValue());
        try {
            auto lease = std::make_unique<FrontendMaterialBinding>(*resourceRegistry_);
            const auto pipeline = lease->Resolve(RenderResourceClass::Pipeline,
                                                 {descriptor.pipeline.owner, descriptor.pipeline.slot, descriptor.pipeline.generation});
            if (pipeline.HasError())
                return Realization::Failure(pipeline.ErrorValue());
            std::array<MaterialResidentResource, 128> resources;
            // Resolve all existing generations before allocating any parameter backing. The second bounded pass owns
            // only packed parameter buffers; a stale texture/sampler cannot trigger native preparation first.
            for (unsigned phase = 0; phase < 2; ++phase) {
                for (std::size_t index = 0; index < descriptor.resources.size(); ++index) {
                    const auto &logical = descriptor.resources[index];
                    const bool packedValue = std::holds_alternative<MaterialParameterBinding>(logical.value);
                    if ((phase == 0 && packedValue) || (phase == 1 && !packedValue))
                        continue;
                    auto &resolved = resources[index];
                    if (const auto *packed = std::get_if<MaterialParameterBinding>(&logical.value)) {
                        const auto size = backend_->QueryMaterialParameterBufferSize(pipeline.Value(), descriptor, logical.binding);
                        if (size.HasError())
                            return Realization::Failure(size.ErrorValue());
                        if (size.Value() < packed->bytes.size() || size.Value() > MaterialBindingLimits{}.maximumParameterBytes)
                            return Realization::Failure(MakeError(MaterialBindingErrors::InvalidDescriptor));
                        std::vector<std::byte> padded(size.Value());
                        std::ranges::copy(packed->bytes, padded.begin());
                        const RenderBufferDescriptor buffer{.byteSize = padded.size(), .usage = RenderBufferUsage::Uniform};
                        const auto prepared = PrepareResource(*resourceRegistry_, *memoryBudget_, *backend_,
                                                              memoryConfig_.defaultResourceScope, RenderResourceClass::Buffer, {}, [&] {
                            return backend_->QueryBufferMemoryCost(buffer);
                        }, [&](const RenderMemoryPlacement &placement) {
                            return backend_->CreateBuffer(buffer, padded, placement);
                        });
                        if (prepared.HasError())
                            return Realization::Failure(prepared.ErrorValue());
                        const auto identity = prepared.Value().identity;
                        lease->OwnBuffer(identity);
                        if (identity.slot >= buffers_.size())
                            buffers_.resize(static_cast<std::size_t>(identity.slot) + 1);
                        buffers_[identity.slot] = {.generation = identity.generation, .descriptor = buffer};
                        const auto instance = lease->Resolve(RenderResourceClass::Buffer, identity);
                        if (instance.HasError())
                            return Realization::Failure(instance.ErrorValue());
                        resolved = {MaterialResidentResourceKind::Buffer, instance.Value(), 0, padded.size()};
                    } else if (const auto *buffer = std::get_if<MaterialBufferBinding>(&logical.value)) {
                        const auto instance = lease->Resolve(RenderResourceClass::Buffer,
                                                             {buffer->buffer.owner, buffer->buffer.slot, buffer->buffer.generation});
                        if (instance.HasError())
                            return Realization::Failure(instance.ErrorValue());
                        resolved = {MaterialResidentResourceKind::Buffer, instance.Value(), buffer->offset, buffer->byteCount};
                    } else if (const auto *view = std::get_if<RenderTextureViewHandle>(&logical.value)) {
                        const auto instance = lease->Resolve(RenderResourceClass::TextureView, {view->owner, view->slot, view->generation});
                        if (instance.HasError())
                            return Realization::Failure(instance.ErrorValue());
                        resolved = {MaterialResidentResourceKind::TextureView, instance.Value(), 0, 0};
                    } else {
                        const auto sampler = std::get<RenderSamplerHandle>(logical.value);
                        const auto instance =
                            lease->Resolve(RenderResourceClass::Sampler, {sampler.owner, sampler.slot, sampler.generation});
                        if (instance.HasError())
                            return Realization::Failure(instance.ErrorValue());
                        resolved = {MaterialResidentResourceKind::Sampler, instance.Value(), 0, 0};
                    }
                }
            }
            auto native = backend_->RealizeResolved(descriptor, pipeline.Value(), std::span{resources}.first(descriptor.resources.size()));
            if (native.HasError())
                return Realization::Failure(native.ErrorValue());
            if (!native.Value())
                return Realization::Failure(MakeError(MaterialBindingErrors::InvalidDescriptor));
            lease->native = std::move(native).Value();
            return Realization::Success(std::move(lease));
        } catch (const std::bad_alloc &) {
            return Realization::Failure(MakeError(MaterialBindingErrors::AllocationFailed));
        } catch (const std::length_error &) {
            return Realization::Failure(MakeError(MaterialBindingErrors::AllocationFailed));
        }
    }

    /** @copydoc RenderFrontend::QueryPipelineDeviceIdentity */
    Result<RenderPipelineDeviceIdentity> RenderFrontend::QueryPipelineDeviceIdentity() const {
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<RenderPipelineDeviceIdentity>::Failure(MakeError(RenderShaderResourceErrors::WrongThread));
        if (activeFrameScope_ != nullptr)
            return Result<RenderPipelineDeviceIdentity>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        return backend_->QueryPipelineDeviceIdentity();
    }

    /** @copydoc RenderFrontend::CreateShaderModule */
    Result<ResourceCreation<RenderShaderModuleHandle>> RenderFrontend::CreateShaderModule(
        const RenderShaderModuleDescriptor &descriptor, const std::span<const std::uint8_t> cookedPackage) {
        using Creation = ResourceCreation<RenderShaderModuleHandle>;
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<Creation>::Failure(MakeError(RenderShaderResourceErrors::WrongThread));
        if (activeFrameScope_ != nullptr)
            return Result<Creation>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        const auto valid = ValidateRenderShaderModuleDescriptor(descriptor);
        if (valid.HasError())
            return Result<Creation>::Failure(valid.ErrorValue());
        const auto decoded =
            DecodeShaderArtifactStages(cookedPackage, descriptor.target.backend, descriptor.target.payloadFormat, 64U * 1024U * 1024U);
        if (decoded.HasError())
            return Result<Creation>::Failure(decoded.ErrorValue());
        const auto stages = std::span{decoded.Value().stages}.first(decoded.Value().stageCount);
        const auto selected = std::ranges::find_if(stages, [&](const ShaderArtifactStageView &stage) {
            return stage.stage == descriptor.stage && stage.entryPoint == descriptor.entryPoint;
        });
        if (selected == stages.end())
            return Result<Creation>::Failure(MakeError(RenderShaderResourceErrors::InvalidModule));
        try {
            const auto prepared = PrepareResource(*resourceRegistry_, *memoryBudget_, *backend_, memoryConfig_.defaultResourceScope,
                                                  RenderResourceClass::ShaderModule, {}, [&] {
                return backend_->QueryShaderModuleMemoryCost(descriptor, selected->bytes.size());
            }, [&](const RenderMemoryPlacement &placement) {
                return backend_->CreateShaderModule(descriptor, selected->bytes, placement);
            });
            if (prepared.HasError())
                return Result<Creation>::Failure(prepared.ErrorValue());
            const auto identity = prepared.Value().identity;
            return Result<Creation>::Success({{identity.owner, identity.slot, identity.generation}, prepared.Value().operation});
        } catch (const std::bad_alloc &) {
            return Result<Creation>::Failure(MakeError(RenderShaderResourceErrors::AllocationFailed));
        } catch (const std::length_error &) {
            return Result<Creation>::Failure(MakeError(RenderShaderResourceErrors::AllocationFailed));
        }
    }

    /** @copydoc RenderFrontend::CreateGraphicsPipeline */
    Result<ResourceCreation<RenderPipelineHandle>> RenderFrontend::CreateGraphicsPipeline(
        const RenderGraphicsPipelineDescriptor &descriptor) {
        using Creation = ResourceCreation<RenderPipelineHandle>;
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<Creation>::Failure(MakeError(RenderShaderResourceErrors::WrongThread));
        if (activeFrameScope_ != nullptr)
            return Result<Creation>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        const auto valid = ValidateRenderGraphicsPipelineDescriptor(descriptor);
        if (valid.HasError())
            return Result<Creation>::Failure(valid.ErrorValue());
        const std::array dependencies{RenderResourceIdentity{descriptor.vertex.owner, descriptor.vertex.slot, descriptor.vertex.generation},
                                      RenderResourceIdentity{descriptor.fragment.owner, descriptor.fragment.slot,
                                                             descriptor.fragment.generation}};
        const auto vertex = resourceRegistry_->BackendInstance(RenderResourceClass::ShaderModule, dependencies[0]);
        if (vertex.HasError())
            return Result<Creation>::Failure(vertex.ErrorValue());
        const auto fragment = resourceRegistry_->BackendInstance(RenderResourceClass::ShaderModule, dependencies[1]);
        if (fragment.HasError())
            return Result<Creation>::Failure(fragment.ErrorValue());
        try {
            const auto prepared = PrepareResource(*resourceRegistry_, *memoryBudget_, *backend_, memoryConfig_.defaultResourceScope,
                                                  RenderResourceClass::Pipeline, dependencies, [&] {
                return backend_->QueryGraphicsPipelineMemoryCost(descriptor, vertex.Value(), fragment.Value());
            }, [&](const RenderMemoryPlacement &placement) {
                return backend_->CreateGraphicsPipeline(descriptor, vertex.Value(), fragment.Value(), placement);
            });
            if (prepared.HasError())
                return Result<Creation>::Failure(prepared.ErrorValue());
            const auto identity = prepared.Value().identity;
            return Result<Creation>::Success({{identity.owner, identity.slot, identity.generation}, prepared.Value().operation});
        } catch (const std::bad_alloc &) {
            return Result<Creation>::Failure(MakeError(RenderShaderResourceErrors::AllocationFailed));
        } catch (const std::length_error &) {
            return Result<Creation>::Failure(MakeError(RenderShaderResourceErrors::AllocationFailed));
        }
    }

    /** @copydoc RenderFrontend::CreateSampler */
    Result<ResourceCreation<RenderSamplerHandle>> RenderFrontend::CreateSampler(const RenderSamplerDescriptor &descriptor) {
        using Creation = ResourceCreation<RenderSamplerHandle>;
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<Creation>::Failure(MakeError(RenderShaderResourceErrors::WrongThread));
        if (activeFrameScope_ != nullptr)
            return Result<Creation>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        const auto valid = ValidateRenderSamplerDescriptor(descriptor);
        if (valid.HasError())
            return Result<Creation>::Failure(valid.ErrorValue());
        try {
            const auto prepared = PrepareResource(*resourceRegistry_, *memoryBudget_, *backend_, memoryConfig_.defaultResourceScope,
                                                  RenderResourceClass::Sampler, {}, [&] {
                return backend_->QuerySamplerMemoryCost(descriptor);
            }, [&](const RenderMemoryPlacement &placement) {
                return backend_->CreateSampler(descriptor, placement);
            });
            if (prepared.HasError())
                return Result<Creation>::Failure(prepared.ErrorValue());
            const auto identity = prepared.Value().identity;
            return Result<Creation>::Success({{identity.owner, identity.slot, identity.generation}, prepared.Value().operation});
        } catch (const std::bad_alloc &) {
            return Result<Creation>::Failure(MakeError(RenderShaderResourceErrors::AllocationFailed));
        } catch (const std::length_error &) {
            return Result<Creation>::Failure(MakeError(RenderShaderResourceErrors::AllocationFailed));
        }
    }

    /** @copydoc RenderFrontend::ReleaseShaderModule */
    Result<void> RenderFrontend::ReleaseShaderModule(const RenderShaderModuleHandle handle) {
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<void>::Failure(MakeError(RenderShaderResourceErrors::WrongThread));
        if (activeFrameScope_ != nullptr)
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        return ReleasePrepared(*resourceRegistry_, RenderResourceClass::ShaderModule, {handle.owner, handle.slot, handle.generation});
    }

    /** @copydoc RenderFrontend::ReleaseGraphicsPipeline */
    Result<void> RenderFrontend::ReleaseGraphicsPipeline(const RenderPipelineHandle handle) {
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<void>::Failure(MakeError(RenderShaderResourceErrors::WrongThread));
        if (activeFrameScope_ != nullptr)
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        return ReleasePrepared(*resourceRegistry_, RenderResourceClass::Pipeline, {handle.owner, handle.slot, handle.generation});
    }

    /** @copydoc RenderFrontend::ReleaseSampler */
    Result<void> RenderFrontend::ReleaseSampler(const RenderSamplerHandle handle) {
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<void>::Failure(MakeError(RenderShaderResourceErrors::WrongThread));
        if (activeFrameScope_ != nullptr)
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        return ReleasePrepared(*resourceRegistry_, RenderResourceClass::Sampler, {handle.owner, handle.slot, handle.generation});
    }

    /** @copydoc RenderFrontend::ResourceState(RenderShaderModuleHandle) */
    Result<RenderResourceState> RenderFrontend::ResourceState(const RenderShaderModuleHandle handle) const {
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<RenderResourceState>::Failure(MakeError(RenderShaderResourceErrors::WrongThread));
        return resourceRegistry_->State(RenderResourceClass::ShaderModule, {handle.owner, handle.slot, handle.generation});
    }

    /** @copydoc RenderFrontend::ResourceState(RenderPipelineHandle) */
    Result<RenderResourceState> RenderFrontend::ResourceState(const RenderPipelineHandle handle) const {
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<RenderResourceState>::Failure(MakeError(RenderShaderResourceErrors::WrongThread));
        return resourceRegistry_->State(RenderResourceClass::Pipeline, {handle.owner, handle.slot, handle.generation});
    }

    /** @copydoc RenderFrontend::ResourceState(RenderSamplerHandle) */
    Result<RenderResourceState> RenderFrontend::ResourceState(const RenderSamplerHandle handle) const {
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<RenderResourceState>::Failure(MakeError(RenderShaderResourceErrors::WrongThread));
        return resourceRegistry_->State(RenderResourceClass::Sampler, {handle.owner, handle.slot, handle.generation});
    }
}  // namespace Horo::Render
