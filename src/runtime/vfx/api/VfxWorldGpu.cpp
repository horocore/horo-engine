#include "Horo/Vfx/VfxWorld.h"
#include "VfxWorldCoordinates.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Vfx {
    namespace {
        /** @brief Matches the captured semantic generations without consulting a live world. */
        bool SameBinding(EffectSystemId effect, EmitterId emitter, const Render::VfxGpuSimulationIdentity &id) noexcept {
            return effect.scope.Value() == id.scene && effect.slot == id.activation.slot && effect.generation == id.activation.generation &&
                   emitter.scope.Value() == id.scene && emitter.slot == id.emitter.slot && emitter.generation == id.emitter.generation;
        }

        /** @brief Requires real admitted initialization and the implemented additive recipe before ownership can move. */
        bool ValidGpuBinding(VfxIdentityScope scene, const VfxGpuEmitterPreparation &binding) noexcept {
            if (!binding.simulator || binding.blend != Render::VfxParticlePass::Additive)
                return false;
            const auto state = binding.simulator->RenderState();
            return scene == binding.effect.scope && state.active && state.nextTick == 0 && state.preparation.IsLive() &&
                   !state.lifetimeCancellation.IsCancellationRequested() &&
                   SameBinding(binding.effect, binding.emitter, state.preparation.Descriptor().identity);
        }

        /** @brief Rejects aliases in scene identity slots while permitting emitters of one exact effect. */
        bool Compatible(EffectSystemId a, EmitterId ae, std::uint32_t bufferA, EffectSystemId b, EmitterId be,
                        std::uint32_t bufferB) noexcept {
            return ae.slot != be.slot && bufferA != bufferB && (a.slot != b.slot || a == b);
        }

        /** @brief Checks the complete CPU inventory before admitting a GPU binding. */
        bool CompatibleCpu(const VfxGpuEmitterPreparation &gpu, std::span<const VfxEmitterPreparation> cpu) noexcept {
            const auto buffer = gpu.simulator->Preparation().descriptor.identity.buffer.slot;
            for (const auto &other : cpu)
                if (!Compatible(gpu.effect, gpu.emitter, buffer, other.effect, other.emitter, other.particles.buffer.slot))
                    return false;
            return true;
        }

        /** @brief Checks earlier GPU bindings before any ownership transfer. */
        bool CompatibleGpu(const VfxGpuEmitterPreparation &gpu, std::span<VfxGpuEmitterPreparation> previous) noexcept {
            const auto buffer = gpu.simulator->Preparation().descriptor.identity.buffer.slot;
            for (const auto &other : previous)
                if (!Compatible(gpu.effect, gpu.emitter, buffer, other.effect, other.emitter,
                                other.simulator->Preparation().descriptor.identity.buffer.slot))
                    return false;
            return true;
        }

        /** @brief Preflights every GPU owner and identity relation without mutating caller storage. */
        bool ValidGpuBindings(VfxIdentityScope scene, std::span<const VfxEmitterPreparation> cpu,
                              std::span<VfxGpuEmitterPreparation> gpu) noexcept {
            for (std::size_t i = 0; i < gpu.size(); ++i) {
                if (!ValidGpuBinding(scene, gpu[i]))
                    return false;
                if (!CompatibleCpu(gpu[i], cpu) || !CompatibleGpu(gpu[i], gpu.first(i)))
                    return false;
            }
            return true;
        }

        /** @brief Bounds both lengths before addition so admission work and arithmetic remain finite. */
        bool ValidWorldInventory(VfxIdentityScope scene, std::size_t cpu, std::size_t gpu) noexcept {
            return scene.IsValid() && cpu + gpu > 0 && cpu + gpu <= 4096;
        }

        /** @brief Charges CPU and GPU particle lanes against one scene capacity. */
        bool CombinedCapacity(std::span<const VfxEmitterPreparation> cpu, std::span<VfxGpuEmitterPreparation> gpu) noexcept {
            std::uint64_t count{};
            for (const auto &binding : cpu)
                count += binding.particles.capacity;
            for (const auto &binding : gpu)
                count += binding.simulator->Preparation().descriptor.layout.capacity;
            return count <= CpuParticleBufferHardLimits::Particles;
        }

        /** @brief Captures identity values independently of mutable emitter storage. */
        Render::VfxBatchId BatchId(EffectSystemId effect, EmitterId emitter) noexcept {
            return {effect.scope.Value(), effect.slot, effect.generation, emitter.slot, emitter.generation};
        }

        /** @brief Preserves the first static failure while counting every processed result. */
        void RecordStatus(VfxGpuUpdateReport &report, GpuParticleStepStatus status) noexcept {
            if (status == GpuParticleStepStatus::Complete) {
                ++report.processed;
                return;
            }
            ++report.rejected;
            if (report.status == GpuParticleStepStatus::Complete)
                report.status = status;
        }

        /** @brief Uses the shared overflow-safe relative coordinate conversion. */
        float OriginAxis(Math::WorldCoordinateAxis64 simulation, Math::WorldCoordinateAxis64 render) noexcept {
            return Detail::VfxRelativeAxis(simulation, render);
        }

        /** @brief Captures a render-origin offset without retaining coordinate references. */
        Math::Vec3 OriginDelta(const Math::WorldCoordinate64 &simulation, const Math::WorldCoordinate64 &render) noexcept {
            return {OriginAxis(simulation.X(), render.X()), OriginAxis(simulation.Y(), render.Y()), OriginAxis(simulation.Z(), render.Z())};
        }
    }  // namespace

    Result<std::unique_ptr<VfxWorld>> VfxWorld::Prepare(VfxIdentityScope scene, std::span<const VfxEmitterPreparation> cpu,
                                                        std::span<VfxGpuEmitterPreparation> gpu, VfxExtractionBudget budget) {
        using Output = Result<std::unique_ptr<VfxWorld>>;
        if (!ValidWorldInventory(scene, cpu.size(), gpu.size()))
            return Output::Failure(MakeError(VfxErrors::IdentityInvalid));
        if (auto valid = ValidateCpuBindings(scene, cpu); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        if (!ValidGpuBindings(scene, cpu, gpu))
            return Output::Failure(MakeError(VfxErrors::IdentityInvalid));
        if (!CombinedCapacity(cpu, gpu))
            return Output::Failure(MakeError(VfxErrors::LimitExceeded));
        auto extractor = VfxRenderExtractor::Prepare(budget, static_cast<std::uint32_t>(gpu.size()));
        if (extractor.HasError())
            return Output::Failure(extractor.ErrorValue());
        try {
            auto world = std::make_unique<VfxWorld>(VfxWorld{scene, std::move(extractor).Value()});
            if (auto prepared = world->PrepareCpuEmitters(cpu); prepared.HasError())
                return Output::Failure(prepared.ErrorValue());
            world->gpuEmitters_.resize(gpu.size());
            for (std::size_t i = 0; i < gpu.size(); ++i) {
                auto &slot = world->gpuEmitters_[i];
                slot.effect = gpu[i].effect;
                slot.emitter = gpu[i].emitter;
                slot.capacity = gpu[i].simulator->Preparation().descriptor.layout.capacity;
                slot.simulator = std::move(gpu[i].simulator);
            }
            return Output::Success(std::move(world));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(VfxErrors::ParticleBufferAllocationFailed));
        }
    }

    Result<void> VfxWorld::CheckCpuPlay(EffectSystemId effect, bool &found) const {
        for (const auto &entry : emitters_)
            if (entry.binding.effect == effect) {
                if (entry.retired)
                    return Result<void>::Failure(MakeError(VfxErrors::GenerationExhausted));
                if (entry.live)
                    return Result<void>::Failure(MakeError(VfxErrors::IdentityInvalid));
                found = true;
            }
        return Result<void>::Success();
    }

    bool VfxWorld::HasLiveEffect(EffectSystemId effect) const noexcept {
        const bool cpu = std::ranges::any_of(emitters_, [effect](const auto &e) {
            return e.live && e.binding.effect == effect;
        });
        return cpu || HasLiveGpu(effect);
    }

    bool VfxWorld::CanPlayGpu(EffectSystemId effect, bool &found) const noexcept {
        for (const auto &entry : gpuEmitters_) {
            if (entry.effect != effect)
                continue;
            const auto state = entry.simulator->RenderState();
            if (entry.live || entry.retired || !state.active || !state.preparation.IsLive())
                return false;
            if (!SameBinding(effect, entry.emitter, state.preparation.Descriptor().identity))
                return false;
            found = true;
        }
        return true;
    }

    void VfxWorld::SetGpuLive(EffectSystemId effect) noexcept {
        for (auto &entry : gpuEmitters_)
            if (entry.effect == effect)
                entry.live = true;
    }

    bool VfxWorld::HasLiveGpu(EffectSystemId effect) const noexcept {
        return std::ranges::any_of(gpuEmitters_, [effect](const auto &entry) {
            return entry.live && entry.effect == effect;
        });
    }

    void VfxWorld::StopGpu(EffectSystemId effect, EffectSystemId next, bool exhausted) noexcept {
        for (auto &entry : gpuEmitters_)
            if (entry.effect == effect) {
                (void)entry.simulator->Stop();
                entry.live = false;
                entry.retired = exhausted;
                entry.effect = next;
            }
    }

    void VfxWorld::ShutdownGpu() noexcept {
        for (auto &entry : gpuEmitters_) {
            (void)entry.simulator->Shutdown();
            entry.live = false;
        }
    }

    VfxGpuUpdateReport VfxWorld::ApplyGpuAcknowledgements(std::span<const Render::GpuVfxStepAcknowledgement> acknowledgements) noexcept {
        VfxGpuUpdateReport report;
        if (owner_ != std::this_thread::get_id()) {
            report.status = GpuParticleStepStatus::ThreadViolation;
            return report;
        }
        if (shutDown_) {
            report.status = GpuParticleStepStatus::ShutDown;
            return report;
        }
        if (acknowledgements.size() > MaximumGpuAcknowledgements) {
            report.status = GpuParticleStepStatus::CapacityExceeded;
            return report;
        }
        for (const auto &ack : acknowledgements) {
            const auto found = std::ranges::find_if(gpuEmitters_, [&ack](const auto &entry) {
                return entry.simulator->RenderState().preparation.Id() == ack.Step().simulation;
            });
            if (found == gpuEmitters_.end())
                RecordStatus(report, GpuParticleStepStatus::StaleAcknowledgement);
            else
                RecordStatus(report, found->simulator->ApplyAcknowledgement(ack));
        }
        return report;
    }

    VfxGpuUpdateReport VfxWorld::PrepareGpuSteps(float deltaSeconds, const CancellationToken &cancellation) noexcept {
        VfxGpuUpdateReport report;
        if (owner_ != std::this_thread::get_id()) {
            report.status = GpuParticleStepStatus::ThreadViolation;
            return report;
        }
        if (shutDown_) {
            report.status = GpuParticleStepStatus::ShutDown;
            return report;
        }
        for (auto &entry : gpuEmitters_) {
            if (!entry.live)
                continue;
            if (!entry.simulator->PendingWork().empty()) {
                ++report.pending;
                continue;
            }
            const auto state = entry.simulator->RenderState();
            RecordStatus(report, entry.simulator->PrepareStep(state.nextTick, deltaSeconds, cancellation).status);
        }
        return report;
    }

    bool VfxWorld::CompatibleGpuReplacements(EffectSystemId effect, std::span<VfxGpuEmitterPreparation> replacements) const noexcept {
        for (const auto &replacement : replacements) {
            const auto buffer = replacement.simulator->Preparation().descriptor.identity.buffer.slot;
            for (const auto &entry : emitters_)
                if (!Compatible(replacement.effect, replacement.emitter, buffer, entry.binding.effect, entry.binding.emitter,
                                entry.binding.particles.buffer.slot))
                    return false;
            for (const auto &entry : gpuEmitters_) {
                if (entry.effect == effect)
                    continue;
                if (!Compatible(replacement.effect, replacement.emitter, buffer, entry.effect, entry.emitter,
                                entry.simulator->Preparation().descriptor.identity.buffer.slot))
                    return false;
            }
        }
        return true;
    }

    Result<void> VfxWorld::ValidateGpuReplacements(EffectSystemId effect, std::span<VfxGpuEmitterPreparation> replacements) const {
        std::size_t count{};
        for (const auto &entry : gpuEmitters_) {
            if (entry.effect != effect)
                continue;
            ++count;
            const auto found = std::ranges::find_if(replacements, [&entry](const auto &r) {
                return r.effect == entry.effect && r.emitter == entry.emitter;
            });
            if (entry.live || entry.retired || found == replacements.end())
                return Result<void>::Failure(MakeError(VfxErrors::IdentityInvalid));
            if (found->simulator->Preparation().descriptor.layout.capacity != entry.capacity)
                return Result<void>::Failure(MakeError(VfxErrors::LimitExceeded));
        }
        if (!count || count != replacements.size())
            return Result<void>::Failure(MakeError(VfxErrors::IdentityInvalid));
        return Result<void>::Success();
    }

    Result<void> VfxWorld::ReprepareGpuEffect(EffectSystemId effect, std::span<VfxGpuEmitterPreparation> replacements) {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(VfxErrors::ParticleBufferThreadViolation));
        if (shutDown_ || replacements.size() > gpuEmitters_.size())
            return Result<void>::Failure(MakeError(VfxErrors::IdentityInvalid));
        if (!ValidGpuBindings(scene_, {}, replacements) || !CompatibleGpuReplacements(effect, replacements))
            return Result<void>::Failure(MakeError(VfxErrors::IdentityInvalid));
        if (auto valid = ValidateGpuReplacements(effect, replacements); valid.HasError())
            return valid;
        for (auto &entry : gpuEmitters_)
            if (entry.effect == effect) {
                const auto found = std::ranges::find_if(replacements, [&entry](const auto &r) {
                    return r.emitter == entry.emitter;
                });
                entry.simulator = std::move(found->simulator);
            }
        return Result<void>::Success();
    }

    bool VfxRenderExtractor::AppendGpuEmitters(const VfxWorld &world, Render::RenderWorldFrameData &data,
                                               const Math::WorldCoordinate64 &origin, VfxExtractionReport &report) const noexcept {
        for (const auto &entry : world.gpuEmitters_)
            if (entry.live && !AppendGpuEmitter(entry.effect, entry.emitter, *entry.simulator, entry.capacity, data, origin, report))
                return false;
        return true;
    }

    bool VfxRenderExtractor::AppendGpuEmitter(EffectSystemId effect, EmitterId emitter, const GpuParticleSimulator &simulator,
                                              std::uint32_t capacity, Render::RenderWorldFrameData &data,
                                              const Math::WorldCoordinate64 &origin, VfxExtractionReport &report) const noexcept {
        const auto state = simulator.RenderState();
        if (!state.active || !state.preparation.IsLive()) {
            report.status = VfxExtractionStatus::InvalidParticleData;
            return false;
        }
        if (report.admittedBatches >= budget_.maximumBatches || capacity > budget_.maximumInstances - report.admittedInstances ||
            report.reservedDrawCalls == budget_.maximumDrawCalls) {
            if (!report.omittedBatches)
                report.firstOmitted = BatchId(effect, emitter);
            ++report.omittedBatches;
            report.omittedInstances += capacity;
            report.status = VfxExtractionStatus::BudgetExceeded;
            return true;
        }
        const auto delta = OriginDelta(state.preparation.Descriptor().simulationOrigin, origin);
        if (!Math::IsFinite(delta)) {
            report.status = VfxExtractionStatus::InvalidParticleData;
            return false;
        }
        Render::GpuVfxRenderBatch batch{BatchId(effect, emitter),   state.preparation,
                                        state.generation,           {},
                                        state.lifetimeCancellation, delta};
        const auto work = simulator.PendingWork();
        if (!work.empty())
            batch.pending = work.front();
        data.gpuBatches_.push_back(std::move(batch));
        ++report.admittedBatches;
        report.admittedInstances += capacity;
        ++report.reservedDrawCalls;
        return true;
    }
}  // namespace Horo::Vfx
