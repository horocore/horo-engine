#pragma once
/** @file VfxGpuProtocolProvider.h @brief Explicit FAKE GPU protocol provider for frontend state tests only.
 * No GPU dispatch, physics, raster pixels, shader payload or native completion proof is provided here.
 */
#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderFrontendResourceAccess.h"
#include "RenderMemoryTestSupport.h"
#include "support/VfxGpuTestSupport.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

namespace Horo::Render::ProtocolFake {
    /** @brief Checks typed frontend results outside allocation measurements; no native execution is asserted. */
    template <class T> T Value(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message
                                : "Protocol operation accepted (not native GPU proof)."));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Identifies injected protocol failure without manufacturing shader or native evidence. */
    inline Error Failure() {
        return Error{ErrorCode{"render.test.vfx_protocol_failure"},
                     ErrorDomainId{"horo.render.test"},
                     ErrorSeverity::Error,
                     "Explicit fake protocol failure.",
                     {}};
    }

    struct Kernel final : IResidentVfxGpuKernel {};

    /** @brief Owns copied real registry inventory and bounded fake event delivery, with honest CPU payload accounting. */
    struct Simulation final : IResidentVfxGpuSimulation {
        explicit Simulation(const NativeVfxGpuSimulationPreparation &p, std::size_t metadataBytes)
            : id(p.id), device(p.deviceIncarnation), descriptor(p.preparation.descriptor), states(p.states.begin(), p.states.end()),
              observations(p.observations.begin(), p.observations.end()), forces(p.forces), parameterArena(p.parameterArena),
              preparationResources(p.preparationResources), lease(p.resourceLease), metadata(metadataBytes),
              opaque(p.preparation.descriptor.readback.enabled
                         ? p.preparation.descriptor.readback.maximumRecords * VfxGpuObservationWords::Bytes
                         : 0) {}

        /** @brief Charges the fake simulation object and its actual cold vector capacities honestly. */
        std::size_t RetainedPayloadBytes() const noexcept override {
            return sizeof(*this) + states.capacity() * sizeof(NativeVfxGpuBuffer) + observations.capacity() * sizeof(NativeVfxGpuBuffer) +
                   opaque.capacity() + metadata.capacity();
        }

        /** @brief Queues a bounded fake protocol fact using only the registered step and captured covering signal. */
        void Emit(VfxGpuSubmissionState state) noexcept {
            if (eventCount == events.size())
                std::terminate();
            const auto point = (state == VfxGpuSubmissionState::RejectedUnsubmitted || state == VfxGpuSubmissionState::CancelledUnsubmitted)
                                   ? RenderTimelinePoint{}
                                   : signal;
            events[(eventHead + eventCount) % events.size()] = {step, state, point};
            ++eventCount;
        }

        /** @brief Models delayed scoped delivery only; this function is not GPU completion evidence. */
        void Complete() noexcept {
            completed = true;
            Emit(VfxGpuSubmissionState::Completed);
        }

        VfxGpuSimulationId id;
        std::uint64_t device;
        VfxGpuSimulationDescriptor descriptor;
        std::vector<NativeVfxGpuBuffer> states, observations;
        NativeVfxGpuBuffer forces, parameterArena;
        NativeVfxGpuPreparationResources preparationResources;
        std::shared_ptr<IRenderGraphResourceLease> lease;
        std::vector<std::byte> metadata;
        bool everEncoded{};
        std::vector<std::byte> opaque; /**< Zero-initialized opaque protocol bytes; never simulated particle output. */
        std::array<NativeGpuVfxStepReceipt, 16> events;
        std::size_t eventHead{}, eventCount{};
        VfxGpuStepIdentity step;
        RenderTimelinePoint signal;
        bool completed{}, observationConsumed{};
    };
    enum class ColdCall {
        Realize,
        Reaped,
        QueryBuffer,
        CreateBuffer,
        DestroyBuffer,
        NativePrepare
    };
    enum class Commit {
        Accept,
        AcceptThenFail,
        UncertainThenFail
    };

    /** @brief Controls only fake provider transitions; every step/signal originates in an actual frontend execution request. */
    struct Audit final {
        std::weak_ptr<Simulation> simulation;
        std::array<std::shared_ptr<Simulation>, 4> canonicalInventory;
        std::array<ColdCall, 128> coldCalls;
        std::size_t coldCallCount{}, extraMetadataBytes{}, nativePreparations{}, reaped{}, preparationQueries{};
        NativeVfxGpuPreparationResources preparationResources;
        std::optional<Error> preparationQueryError;
        bool overflowArenaCost{};

        /** @brief Records bounded cold call order without allocating during resource callbacks. */
        void Log(ColdCall call) noexcept {
            if (coldCallCount == coldCalls.size())
                std::terminate();
            coldCalls[coldCallCount++] = call;
        }

        /** @brief Retains the fake canonical native inventory independently of a rejected frontend transaction. */
        bool Keep(const std::shared_ptr<Simulation> &sim) noexcept {
            const auto free = std::ranges::find(canonicalInventory, nullptr);
            if (free == canonicalInventory.end())
                return false;
            *free = sim;
            return true;
        }

        /** @brief Models cold native canonical inventory maintenance before any new real registry buffer admission. */
        void Reap() noexcept {
            for (auto &sim : canonicalInventory) {
                if (!sim || sim.use_count() != 1)
                    continue;
                if (sim->everEncoded && !sim->completed)
                    continue;
                sim.reset();
                ++reaped;
                Log(ColdCall::Reaped);
            }
        }

        /** @brief Releases fake canonical owners only under this provider explicit teardown quiescence. */
        void DropInventory() noexcept {
            for (auto &sim : canonicalInventory)
                sim.reset();
        }

        std::shared_ptr<Simulation> active;
        IRenderGraphResourceLease *activeLease{};
        RenderTimelinePoint activeSignal;
        std::array<RenderTimelinePoint, 8> globalHistory;
        std::size_t historyCount{}, encodes{}, copies{}, discards{};
        Commit commit{Commit::Accept};
        bool failEncode{}, rejectPending{};

        /** @brief Releases the fake backend completion-owned graph lease once. */
        void ReleaseGraph() noexcept {
            if (activeLease)
                std::exchange(activeLease, nullptr)->Release();
        }

        /** @brief Models finite provider-wide timeline observation history independently of scoped step facts. */
        void Remember() noexcept {
            globalHistory[historyCount++ % globalHistory.size()] = activeSignal;
        }

        /** @brief Borrows only the actual mock preparation object issued through the provider boundary. */
        std::shared_ptr<Simulation> Sim() const {
            auto sim = simulation.lock();
            REQUIRE(sim);
            return sim;
        }
    };

    /** @brief Delegates real frontend resource creation to Null; advertised GPU flags belong ONLY to this clearly named fake test provider.
     */
    class Backend final : public IRenderBackend {
    public:
        Backend(std::unique_ptr<IRenderBackend> base, std::shared_ptr<Audit> audit) : base_(std::move(base)), audit_(std::move(audit)) {}

        /** @brief Starts the delegated Null resource owner and explicitly enables fake protocol features for these tests only. */
        Result<void> Initialize(const RenderBackendConfig &config) override {
            auto result = base_->Initialize(config);
            caps_ = base_->Capabilities();
            caps_.backend = RenderBackendId{"vfx-protocol-fake"};
            caps_.supportsExplicitGraphSubmissions = true;
            caps_.supportsSubmissionObservation = true;
            caps_.support.features.Enable(RenderCapability::VfxGpuSimulation);
            caps_.support.features.Enable(RenderCapability::VfxGpuDraw);
            caps_.support.features.Enable(RenderCapability::VfxGpuReadback);
            return result;
        }

        const RenderBackendCapabilities &Capabilities() const noexcept override {
            return caps_;
        }

        /** @brief Uses the real delegated frame identity while resetting only uncommitted fake transaction state. */
        Result<FrameToken> BeginFrame(const FrameDescriptor &d) override {
            audit_->active.reset();
            audit_->activeSignal = {};
            return base_->BeginFrame(d);
        }

        /** @brief Keeps unrelated legacy execution delegated to the existing Null backend. */
        Result<void> Execute(const RenderExecutionPlan &p) override {
            return base_->Execute(p);
        }

        /** @brief Captures real frontend work registration before controlled protocol acceptance or rejection. */
        Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request) override {
            ++audit_->encodes;
            Capture(request);
            if (audit_->failEncode) {
                audit_->rejectPending = true;
                return Result<void>::Failure(Failure());
            }
            audit_->activeLease = request.lease;
            if (request.transfer)
                request.transfer->authority = RenderGraphLeaseAuthority::BackendCompletion;
            return Result<void>::Success();
        }

        /** @brief Models fake commit or uncertainty only after an actual frontend execution request. */
        Result<void> Present(FrameToken frame) override {
            if (audit_->active) {
                if (audit_->commit == Commit::UncertainThenFail)
                    audit_->active->Emit(VfxGpuSubmissionState::SubmissionUncertain);
                else
                    audit_->active->Emit(VfxGpuSubmissionState::Accepted);
            }
            audit_->Remember();
            audit_->ReleaseGraph();
            audit_->active.reset();
            auto result = base_->Present(frame);
            return audit_->commit == Commit::Accept ? std::move(result) : Result<void>::Failure(Failure());
        }

        /** @brief Rejects unsubmitted fake work without releasing committed transaction evidence. */
        void AbortFrame(FrameToken frame) noexcept override {
            Reject();
            base_->AbortFrame(frame);
        }

        /** @brief Abandons only the current unsubmitted fake transaction. */
        void AbortActiveFrame() noexcept override {
            Reject();
            base_->AbortActiveFrame();
        }

        /** @brief Keeps ordinary resource lifecycle delegated to Null. */
        Result<void> Resize(FramebufferExtent extent) override {
            return base_->Resize(extent);
        }

        /** @brief Stops pending fake admission before delegating the existing owner shutdown. */
        Result<void> BeginShutdown() override {
            Reject();
            return base_->BeginShutdown();
        }

        /** @brief Proves only fake protocol owner quiescence, never hardware retirement. */
        Result<RenderBackendDrainProgress> PollDrain() override {
            audit_->ReleaseGraph();
            audit_->DropInventory();
            return base_->PollDrain();
        }

        /** @brief Models bounded global history so scoped receipts cannot depend on an immortal cache. */
        Result<bool> ObserveSubmission(const RenderCpuWait &wait) override {
            if (std::ranges::find(audit_->globalHistory, wait.completion) == audit_->globalHistory.end())
                return Result<bool>::Failure(Failure());
            return Result<bool>::Success(true);
        }

        /** @brief Returns an authored fake resource plan; actual registry allocation and limits remain frontend-owned. */
        Result<NativeVfxGpuPreparationResources> QueryVfxGpuNativePreparationResources(const VfxGpuSimulationDescriptor &) const override {
            ++audit_->preparationQueries;
            if (audit_->preparationQueryError)
                return Result<NativeVfxGpuPreparationResources>::Failure(*audit_->preparationQueryError);
            return Result<NativeVfxGpuPreparationResources>::Success(audit_->preparationResources);
        }

        /** @brief Delegates exact resource identity, memory and lifetime handling to the existing Null owner. */
        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &d) const override {
            audit_->Log(ColdCall::QueryBuffer);
            if (audit_->overflowArenaCost && d.usage == RenderBufferUsage::Uniform)
                return Result<RenderMemoryCostPlan>::Success(TestSupport::DedicatedMemoryCost(std::numeric_limits<std::size_t>::max(), 1));
            return base_->QueryBufferMemoryCost(d);
        }

        /** @brief Delegates exact resource identity, memory and lifetime handling to the existing Null owner. */
        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &d) const override {
            return base_->QueryTextureMemoryCost(d);
        }

        /** @brief Delegates exact resource identity, memory and lifetime handling to the existing Null owner. */
        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &d, std::span<const std::byte> bytes,
                                           const RenderMemoryPlacement &p, RenderResourceNativeLifetime life = {}) override {
            audit_->Log(ColdCall::CreateBuffer);
            return base_->CreateBuffer(d, bytes, p, std::move(life));
        }

        /** @brief Delegates exact resource identity, memory and lifetime handling to the existing Null owner. */
        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &d, std::uint64_t v, std::uint64_t i,
                                         RenderResourceNativeLifetime life = {}) override {
            return base_->CreateMesh(d, v, i, std::move(life));
        }

        /** @brief Delegates exact resource identity, memory and lifetime handling to the existing Null owner. */
        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &d, std::span<const std::byte> b, const RenderMemoryPlacement &p,
                                            RenderResourceNativeLifetime life = {}) override {
            return base_->CreateTexture(d, b, p, std::move(life));
        }

        /** @brief Delegates exact resource identity, memory and lifetime handling to the existing Null owner. */
        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &d, std::uint64_t t,
                                                RenderResourceNativeLifetime life = {}) override {
            return base_->CreateTextureView(d, t, std::move(life));
        }

        /** @brief Delegates exact resource identity, memory and lifetime handling to the existing Null owner. */
        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &d, std::uint64_t c, std::uint64_t z,
                                                 RenderResourceNativeLifetime life = {}) override {
            return base_->CreateRenderTarget(d, c, z, std::move(life));
        }

        /** @brief Delegates exact resource identity, memory and lifetime handling to the existing Null owner. */
        void DestroyBuffer(std::uint64_t id) noexcept override {
            audit_->Log(ColdCall::DestroyBuffer);
            base_->DestroyBuffer(id);
        }

        /** @brief Delegates exact resource identity, memory and lifetime handling to the existing Null owner. */
        void DestroyMesh(std::uint64_t id) noexcept override {
            base_->DestroyMesh(id);
        }

        /** @brief Delegates exact resource identity, memory and lifetime handling to the existing Null owner. */
        void DestroyTexture(std::uint64_t id) noexcept override {
            base_->DestroyTexture(id);
        }

        /** @brief Delegates exact resource identity, memory and lifetime handling to the existing Null owner. */
        void DestroyTextureView(std::uint64_t id) noexcept override {
            base_->DestroyTextureView(id);
        }

        /** @brief Delegates exact resource identity, memory and lifetime handling to the existing Null owner. */
        void DestroyRenderTarget(std::uint64_t id) noexcept override {
            base_->DestroyRenderTarget(id);
        }

        /** @brief Accepts only opaque empty unit-test family input; real shader bytes are never fabricated. */
        Result<std::shared_ptr<IResidentVfxGpuKernel>> RealizeVfxGpuPrograms(const CookedVfxGpuPrograms &programs) override {
            audit_->Log(ColdCall::Realize);
            audit_->Reap();
            if (!programs.simulation.artifact.payload.empty() || !programs.draw.artifact.payload.empty())
                return Result<std::shared_ptr<IResidentVfxGpuKernel>>::Failure(Failure());
            return Result<std::shared_ptr<IResidentVfxGpuKernel>>::Success(std::make_shared<Kernel>());
        }

        /** @brief Copies real registry-issued inventory and retains its actual ownership lease. */
        Result<std::shared_ptr<IResidentVfxGpuSimulation>> PrepareNativeVfxGpuSimulation(
            const NativeVfxGpuSimulationPreparation &p) override {
            audit_->Log(ColdCall::NativePrepare);
            ++audit_->nativePreparations;
            auto sim = std::make_shared<Simulation>(p, audit_->extraMetadataBytes);
            if (!audit_->Keep(sim))
                return Result<std::shared_ptr<IResidentVfxGpuSimulation>>::Failure(Failure());
            audit_->simulation = sim;
            return Result<std::shared_ptr<IResidentVfxGpuSimulation>>::Success(std::move(sim));
        }

        /** @brief Drains bounded fake facts without allocations or loss. */
        Result<std::size_t> PollNativeVfxGpuReceipts(const std::shared_ptr<IResidentVfxGpuSimulation> &owner,
                                                     std::span<NativeGpuVfxStepReceipt> out) override {
            auto sim = std::dynamic_pointer_cast<Simulation>(owner);
            if (!sim)
                return Result<std::size_t>::Failure(Failure());
            std::size_t count{};
            while (count < out.size() && sim->eventCount) {
                out[count++] = sim->events[sim->eventHead];
                sim->eventHead = (sim->eventHead + 1) % sim->events.size();
                --sim->eventCount;
            }
            return Result<std::size_t>::Success(count);
        }

        /** @brief Copies opaque protocol-only bytes to the real prepared queue after the controlled scoped event. */
        Result<std::size_t> CopyCompletedVfxGpuObservation(const std::shared_ptr<IResidentVfxGpuSimulation> &owner,
                                                           const VfxGpuStepIdentity &step, std::span<std::byte> destination) override {
            auto sim = std::dynamic_pointer_cast<Simulation>(owner);
            if (!sim || !sim->completed || sim->observationConsumed || sim->step != step || destination.size() != sim->opaque.size())
                return Result<std::size_t>::Failure(Failure());
            std::ranges::copy(sim->opaque, destination.begin());
            sim->observationConsumed = true;
            ++audit_->copies;
            return Result<std::size_t>::Success(destination.size());
        }

        /** @brief Models exact terminal hold consumption once without producing particle values. */
        Result<void> DiscardCompletedVfxGpuObservation(const std::shared_ptr<IResidentVfxGpuSimulation> &owner,
                                                       const VfxGpuStepIdentity &step) override {
            auto sim = std::dynamic_pointer_cast<Simulation>(owner);
            if (!sim || !sim->completed || sim->observationConsumed || sim->step != step)
                return Result<void>::Failure(Failure());
            sim->observationConsumed = true;
            ++audit_->discards;
            return Result<void>::Success();
        }

    private:
        /** @brief Copies only real registered work and its actual covering frontend-issued partition signal. */
        void Capture(const RenderGraphExecutionRequest &request) {
            for (std::size_t index = 0; index < request.workloads.size(); ++index) {
                const auto *work = std::get_if<RenderGraphVfxGpuCompute>(&request.workloads[index].workload);
                if (!work)
                    continue;
                audit_->active = std::dynamic_pointer_cast<Simulation>(work->simulationLease);
                REQUIRE(audit_->active);
                audit_->active->everEncoded = true;
                audit_->active->step = work->work.step;
                audit_->active->completed = false;
                audit_->active->observationConsumed = false;
                for (const auto &partition : request.submissions)
                    if (index >= partition.passes.offset && index - partition.passes.offset < partition.passes.count)
                        audit_->active->signal = partition.submission.signal;
            }
            REQUIRE_FALSE(request.submissions.empty());
            audit_->activeSignal = request.submissions.back().submission.signal;
        }

        /** @brief Emits explicit unsubmitted facts and releases only fake uncommitted graph ownership. */
        void Reject() noexcept {
            if (audit_->active)
                audit_->active->Emit(audit_->rejectPending ? VfxGpuSubmissionState::RejectedUnsubmitted
                                                           : VfxGpuSubmissionState::CancelledUnsubmitted);
            audit_->rejectPending = false;
            audit_->active.reset();
            audit_->ReleaseGraph();
        }

        std::unique_ptr<IRenderBackend> base_;
        std::shared_ptr<Audit> audit_;
        RenderBackendCapabilities caps_;
    };

    class Provider final : public IRenderBackendProvider {
    public:
        explicit Provider(std::shared_ptr<Audit> audit) : audit_(std::move(audit)) {}

        /** @brief Constructs the explicitly fake provider around the real Null resource implementation. */
        Result<std::unique_ptr<IRenderBackend>> Create() const override {
            RenderBackendRegistry inner;
            REQUIRE(RegisterNullRenderBackend(inner).HasValue());
            REQUIRE(inner.Seal().HasValue());
            return Result<std::unique_ptr<IRenderBackend>>::Success(
                std::make_unique<Backend>(Value(inner.Create(RenderBackendId{"null"})), audit_));
        }

    private:
        std::shared_ptr<Audit> audit_;
    };

    inline std::unique_ptr<RenderFrontend> Create(const std::shared_ptr<Audit> &audit, const RenderFrontendMemoryConfig &memory = {}) {
        RenderBackendRegistry registry;
        REQUIRE(registry
                    .Register({.id = RenderBackendId{"vfx-protocol-fake"},
                               .displayName = "FAKE VFX protocol only",
                               .provider = std::make_unique<Provider>(audit)})
                    .HasValue());
        REQUIRE(registry.Seal().HasValue());
        return Value(RenderFrontend::Create(registry, RenderBackendId{"vfx-protocol-fake"}, {}, {}, memory));
    }
}  // namespace Horo::Render::ProtocolFake
