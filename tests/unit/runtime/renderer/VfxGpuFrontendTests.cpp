#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "VfxGpuProtocolProvider.h"
#include "support/AllocationProbe.h"

#include <catch2/generators/catch_generators.hpp>
#include <thread>

namespace Horo::Render::ProtocolFake {
    namespace {
        using Vfx::GpuParticleSimulator;
        using Vfx::GpuParticleStepStatus;

        struct Attempt final {
            bool scheduled{}, dispatched{}, executed{}, presented{};
            std::string error;
        };

        struct Drained final {
            std::array<GpuVfxStepAcknowledgement, 64> acknowledgements;
            std::array<GpuParticleStepStatus, 64> statuses;
            std::size_t count{};
        };

        /** @brief Drives real public frontend admission; the clearly named provider below is protocol-only and never GPU evidence. */
        class Fixture final {
        public:
            explicit Fixture(bool observe = false, bool bindColdLifetime = true) : frontend(Create(audit)) {
                auto info = seed.Info();
                if (observe) {
                    const auto bytes = 2 * VfxGpuObservationWords::Bytes;
                    Value(frontend->PrepareVfxGpuReadbackStorage({bytes, bytes, bytes, 16, 1}, {1, bytes, 16}, 1024U * 1024U,
                                                                 std::chrono::seconds{5}));
                    info.readback =
                        {true,  1, VfxGpuReadbackPurpose::DiagnosticsCapture, VfxGpuReadbackRequirement::RequiredForVisualEffect, 1, 2,
                         bytes, 1};
                }
                logical = Value(GpuParticleSimulator::Create(Vfx::Tests::ParticleDescriptor(seed.Source()), info));
                Prepare(bindColdLifetime);
                color = Value(frontend->CreateTexture({.extent = {16, 16}, .usage = RenderTextureUsage::RenderAttachment})).handle;
                Value(frontend->ProcessResourceRequests());
                const auto issued = Value(Detail::RenderFrontendResourceAccess::QueueAssignments(*frontend));
                queues.assign(issued.begin(), issued.end());
            }

            ~Fixture() {
                (void)logical->Shutdown();
                if (frontend->BeginShutdown().HasError())
                    std::terminate();
                const auto drained = frontend->PollDrain();
                if (drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                    std::terminate();
            }

            /** @brief Retries only an exact pending real-registry cold preparation transaction. */
            void Prepare(bool lifetime) {
                for (unsigned retry = 0; retry < 8; ++retry) {
                    auto prepared = frontend->PrepareVfxGpuSimulation(logical->Preparation(), {},
                                                                      lifetime ? logical->LifetimeCancellation() : CancellationToken{});
                    if (prepared.HasValue()) {
                        receipt = std::move(prepared).Value();
                        REQUIRE(logical->Activate(receipt).HasValue());
                        return;
                    }
                    REQUIRE(ErrorChainContains(prepared.ErrorValue(), VfxFrameErrors::Pending.domain, VfxFrameErrors::Pending.code));
                    Value(frontend->ProcessResourceRequests());
                }
                FAIL("Bounded fake-protocol resource preparation did not complete.");
            }

            /** @brief Creates immutable logical work without native dispatch or physics replay. */
            void Next(std::uint64_t tick = 0) {
                REQUIRE(logical->PrepareStep(tick, .25F).HasValue());
            }

            /** @brief Applies only acknowledgements minted by the real frontend from fake provider protocol facts. */
            Drained Drain() {
                Drained result;
                result.count = Value(frontend->DrainVfxGpuAcknowledgements(result.acknowledgements));
                for (std::size_t i = 0; i < result.count; ++i)
                    result.statuses[i] = logical->ApplyAcknowledgement(result.acknowledgements[i]);
                return result;
            }

            /** @brief Uses actual frontend graph and issued submission admission while controlling protocol-only outcomes. */
            Attempt Run(bool dispatch = true, bool present = true, bool tamper = false, bool includeWork = true) {
                auto frame = Value(frontend->BeginFrame({.frameNumber = ++frameNumber, .outputExtent = {16, 16}}));
                auto builder =
                    Value(RenderGraphBuilder::Create({.maxPasses = 8, .maxResources = 8, .maxUsages = 16, .maxDependencies = 16}));
                const auto target = Value(builder.ImportTexture(color, RenderGraphResourceClass::Persistent));
                const auto anchor = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                const auto output = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                REQUIRE(builder.AddUsage({anchor, target, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}).HasValue());
                const auto contribution = frame.ScheduleVfxGpu(builder, Request(target, anchor, output, includeWork));
                if (contribution.HasError())
                    return {.error = contribution.ErrorValue().code.Value()};
                Attempt result{.scheduled = true, .dispatched = contribution.Value().dispatched};
                if (!dispatch)
                    return result;
                auto plan = Compile(builder, contribution.Value(), target, anchor, output);
                if (tamper)
                    for (auto &record : plan.workloads)
                        if (auto *compute = std::get_if<RenderGraphVfxGpuCompute>(&record.workload))
                            compute->work.deltaSeconds = .125F;
                const auto issued = Value(frame.ReserveSubmission(RenderQueueRole::Graphics, contribution.Value().RequiredWaits()));
                const std::array submissions{RenderGraphQueueSubmission{issued, {0, plan.graph.Passes().size()}}};
                const auto encoded = frame.ExecuteSubmittedGraph(plan.graph, plan.workloads, submissions);
                if (encoded.HasError()) {
                    result.error = encoded.ErrorValue().code.Value();
                    return result;
                }
                result.executed = true;
                if (!present)
                    return result;
                const auto submitted = frame.Present();
                result.presented = submitted.HasValue();
                if (submitted.HasError())
                    result.error = submitted.ErrorValue().code.Value();
                return result;
            }

            /** @brief Issues a real unrelated frontend submission to exercise bounded fake global history. */
            void UnrelatedFrame() {
                auto frame = Value(frontend->BeginFrame({.frameNumber = ++frameNumber, .outputExtent = {16, 16}}));
                auto builder = Value(RenderGraphBuilder::Create({.maxPasses = 1, .maxResources = 1, .maxUsages = 1}));
                const auto target = Value(builder.ImportTexture(color, RenderGraphResourceClass::Persistent));
                const auto pass = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                REQUIRE(builder.AddUsage({pass, target, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}).HasValue());
                const auto graph = Value(builder.Finalize());
                const auto schedule = Value(CompileRenderGraph(graph));
                const std::array initial{RenderGraphImportedState{target, {}, RenderGraphInitialContents::Discarded}};
                const auto sync = Value(SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial));
                const auto compiled = Value(CompileRenderGraphExecution(graph, schedule, sync, queues));
                const std::array work{RenderGraphPassWorkload{pass, RenderGraphColorAttachment{target, {}}}};
                const auto issued = Value(frame.ReserveSubmission(RenderQueueRole::Graphics));
                const std::array submissions{RenderGraphQueueSubmission{issued, {0, 1}}};
                REQUIRE(frame.ExecuteSubmittedGraph(compiled, work, submissions).HasValue());
                REQUIRE(frame.Present().HasValue());
            }

            /** @brief Measures expected observation pressure at the real frontend boundary only. */
            void RequiredPressureIsAllocationFree() {
                auto frame = Value(frontend->BeginFrame({.frameNumber = ++frameNumber, .outputExtent = {16, 16}}));
                auto builder =
                    Value(RenderGraphBuilder::Create({.maxPasses = 8, .maxResources = 8, .maxUsages = 16, .maxDependencies = 16}));
                const auto target = Value(builder.ImportTexture(color, RenderGraphResourceClass::Persistent));
                const auto anchor = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                const auto output = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                const auto request = Request(target, anchor, output, true);
                const ErrorCodeDescriptor *failure{};
                std::size_t allocations{};
                {
                    Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
                    const auto result = frame.ScheduleVfxGpu(builder, request);
                    failure = result.FailureDescriptor();
                    allocations = measurement.Snapshot().requests;
                }
                REQUIRE(failure);
                CHECK(failure->code.Value() == VfxFrameErrors::Pending.code.Value());
                CHECK(allocations == 0);
            }

            std::shared_ptr<Audit> audit{std::make_shared<Audit>()};
            std::unique_ptr<RenderFrontend> frontend;
            Vfx::Tests::GpuSeedFixture seed;
            std::unique_ptr<GpuParticleSimulator> logical;
            VfxGpuPreparationReceipt receipt;

        private:
            struct Plan final {
                CompiledRenderGraphExecution graph;
                std::vector<RenderGraphPassWorkload> workloads;
            };

            /** @brief Captures the logical unit lifetime gate explicitly, independent of the cold parent token. */
            VfxGpuGraphRequest Request(RenderGraphResourceId target, RenderGraphPassRef anchor, RenderGraphPassRef output,
                                       bool include) const {
                const auto current = logical->RenderState();
                VfxGpuGraphRequest request{.preparation = receipt,
                                           .generation = current.generation,
                                           .lifetimeCancellation = current.lifetimeCancellation,
                                           .view = {.extent = {16, 16}},
                                           .color = target,
                                           .colorOperations = {.loadOperation = AttachmentLoadOperation::Load},
                                           .lightingComplete = anchor,
                                           .outputTransform = output,
                                           .remainingDrawCalls = 4,
                                           .remainingPasses = 6,
                                           .remainingResources = 7,
                                           .remainingUsages = 15};
                if (include && !logical->PendingWork().empty())
                    request.pending = logical->PendingWork().front();
                return request;
            }

            /** @brief Builds exact imported states and ordered graph workload records without guessed resource mappings. */
            Plan Compile(RenderGraphBuilder &builder, const VfxGpuGraphSchedule &contribution, RenderGraphResourceId color,
                         RenderGraphPassRef anchor, RenderGraphPassRef output) {
                const auto graph = Value(builder.Finalize());
                const auto schedule = Value(CompileRenderGraph(graph));
                std::vector<RenderGraphImportedState> initial{{color, {}, RenderGraphInitialContents::Discarded}};
                initial.insert(initial.end(), contribution.InitialStates().begin(), contribution.InitialStates().end());
                const auto sync = Value(SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial));
                Plan plan{Value(CompileRenderGraphExecution(graph, schedule, sync, queues)), {}};
                for (const auto &pass : plan.graph.Passes()) {
                    if (pass.pass == anchor)
                        plan.workloads.push_back({anchor, RenderGraphColorAttachment{color, {}}});
                    else if (pass.pass == output)
                        plan.workloads.push_back({output, std::monostate{}});
                    else {
                        const auto found = std::ranges::find(contribution.Workloads(), pass.pass, &RenderGraphPassWorkload::pass);
                        REQUIRE(found != contribution.Workloads().end());
                        plan.workloads.push_back(*found);
                    }
                }
                return plan;
            }

            std::vector<RenderQueueAssignment> queues;
            RenderTextureHandle color;
            std::uint64_t frameNumber{};
        };
    }  // namespace

    TEST_CASE("FAKE GPU protocol uses actual issued inventory and abandoned builders do not advance logical ticks",
              "[vfx-gpu][frontend][protocol-fake]") {
        Fixture f;
        const auto sim = f.audit->Sim();
        CHECK(sim->id == f.receipt.Id());
        CHECK(sim->states.size() == VfxGpuStateBufferCount(sim->descriptor));
        CHECK(sim->lease);
        CHECK(sim->RetainedPayloadBytes() >= sizeof(Simulation));
        for (const auto &buffer : sim->states) {
            CHECK(buffer.handle.IsValid());
            CHECK(buffer.instance != 0);
        }
        f.Next();
        CHECK(f.Run(false).scheduled);
        CHECK(f.audit->encodes == 0);
        CHECK(f.Drain().count == 0);
        CHECK(f.logical->RenderState().nextTick == 0);
        REQUIRE(f.Run().presented);
        const auto accepted = f.Drain();
        REQUIRE(accepted.count == 1);
        CHECK(accepted.acknowledgements[0].State() == VfxGpuSubmissionState::Accepted);
        CHECK(f.logical->RenderState().nextTick == 1);
        CHECK(f.logical->ApplyAcknowledgement(accepted.acknowledgements[0]) == GpuParticleStepStatus::StaleAcknowledgement);
        CHECK(f.logical->RenderState().generation == 2);
        sim->Complete();
        const auto completed = f.Drain();
        CHECK(completed.count == 1);
        CHECK(f.logical->RenderState().nextTick == 1);
    }

    TEST_CASE("FAKE GPU protocol rejects mutated work before provider dispatch and retries explicit unsubmitted failures",
              "[vfx-gpu][frontend][protocol-fake]") {
        Fixture f;
        f.Next();
        SECTION("mutated scheduled work") {
            const auto result = f.Run(true, true, true);
            CHECK_FALSE(result.executed);
            CHECK(f.audit->encodes == 0);
        }
        SECTION("provider pre-submit rejection") {
            f.audit->failEncode = true;
            const auto result = f.Run();
            CHECK_FALSE(result.executed);
            CHECK(result.error == "render.test.vfx_protocol_failure");
            const auto rejected = f.Drain();
            REQUIRE(rejected.count == 1);
            CHECK(rejected.acknowledgements[0].State() == VfxGpuSubmissionState::RejectedUnsubmitted);
            CHECK(f.logical->RenderState().nextTick == 0);
            f.audit->failEncode = false;
            f.Next();
        }
        REQUIRE(f.Run().presented);
        CHECK(f.Drain().count == 1);
        CHECK(f.logical->RenderState().nextTick == 1);
    }

    TEST_CASE("FAKE GPU protocol never rewinds accepted or uncertain submissions after frame failure",
              "[vfx-gpu][frontend][protocol-fake]") {
        Fixture f;
        f.Next();
        SECTION("accepted then later presentation failure") {
            f.audit->commit = Commit::AcceptThenFail;
            const auto result = f.Run();
            CHECK(result.executed);
            CHECK_FALSE(result.presented);
            REQUIRE(f.Drain().count == 1);
            CHECK(f.logical->RenderState().nextTick == 1);
            f.audit->Sim()->Emit(VfxGpuSubmissionState::DeviceLost);
            const auto lost = f.Drain();
            REQUIRE(lost.count == 1);
            CHECK(lost.statuses[0] == GpuParticleStepStatus::DeviceLost);
            CHECK(f.logical->RenderState().nextTick == 1);
        }
        SECTION("uncertainty is terminal, not an unsubmitted retry") {
            f.audit->commit = Commit::UncertainThenFail;
            CHECK_FALSE(f.Run().presented);
            const auto uncertain = f.Drain();
            REQUIRE(uncertain.count == 1);
            CHECK(uncertain.statuses[0] == GpuParticleStepStatus::SubmissionUncertain);
            CHECK(f.logical->PrepareStep(0, .25F).status == GpuParticleStepStatus::SubmissionUncertain);
            CHECK_FALSE(f.Run().scheduled);
        }
    }

    TEST_CASE("FAKE scoped completion survives unrelated provider history eviction", "[vfx-gpu][frontend][protocol-fake]") {
        Fixture f;
        f.Next();
        REQUIRE(f.Run().presented);
        REQUIRE(f.Drain().count == 1);
        const auto signal = f.audit->Sim()->signal;
        for (unsigned i = 0; i < 12; ++i)
            f.UnrelatedFrame();
        REQUIRE(std::ranges::find(f.audit->globalHistory, signal) == f.audit->globalHistory.end());
        f.audit->Sim()->Complete();
        const auto completed = f.Drain();
        REQUIRE(completed.count == 1);
        CHECK(completed.acknowledgements[0].State() == VfxGpuSubmissionState::Completed);
        CHECK(completed.acknowledgements[0].Submission() == signal);
        CHECK(completed.statuses[0] == GpuParticleStepStatus::Complete);
    }

    TEST_CASE("FAKE GPU protocol retains required readback quota until the real prepared queue consumer lease retires",
              "[vfx-gpu][frontend][protocol-fake]") {
        Fixture f(true);
        f.Next();
        REQUIRE(f.Run().presented);
        f.Drain();
        const auto step = f.audit->Sim()->step;
        f.audit->Sim()->Complete();
        RenderReadbackResult lease;
        std::array<GpuVfxStepAcknowledgement, 64> facts;
        bool drained{}, processed{};
        RenderReadbackPreparedStatus acquired{};
        std::size_t allocations{};
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
            const auto completion = f.frontend->DrainVfxGpuAcknowledgements(facts);
            drained = completion.HasValue();
            if (drained)
                for (std::size_t i = 0; i < completion.Value(); ++i)
                    (void)f.logical->ApplyAcknowledgement(facts[i]);
            processed = f.frontend->ProcessVfxGpuReadbacks().HasValue();
            acquired = f.frontend->AcquireVfxGpuObservation(f.receipt, step, lease);
            allocations = measurement.Snapshot().requests;
        }
        REQUIRE(drained);
        REQUIRE(processed);
        CHECK(allocations == 0);
        REQUIRE(acquired == RenderReadbackPreparedStatus::Complete);
        CHECK(lease.Id().IsValid());
        CHECK(lease.Completion() == f.audit->Sim()->signal);
        f.Next(1);
        f.RequiredPressureIsAllocationFree();
        CHECK(f.audit->copies == 1);
        std::thread consumer([owned = std::move(lease)]() mutable {
            owned = RenderReadbackResult{};
        });
        consumer.join();
        Value(f.frontend->ProcessVfxGpuReadbacks());
        REQUIRE(f.Run().presented);
        f.Drain();
        CHECK(f.audit->Sim()->step.tick == 1);
    }

    TEST_CASE("FAKE GPU readback cancellation retains exact submitted ownership until scoped completion",
              "[vfx-gpu][frontend][protocol-fake]") {
        Fixture f(true);
        f.Next();
        REQUIRE(f.Run().presented);
        REQUIRE(f.Drain().count == 1);
        const auto step = f.audit->Sim()->step;
        REQUIRE(f.frontend->CancelVfxGpuObservation(f.receipt, step) == RenderReadbackPreparedStatus::Complete);
        Value(f.frontend->ProcessVfxGpuReadbacks());
        CHECK(Value(f.frontend->GpuReadbackDiagnostics()).queue.pendingBytes == 2 * VfxGpuObservationWords::Bytes);
        CHECK(f.audit->copies == 0);
        CHECK(f.audit->discards == 0);
        f.audit->Sim()->Complete();
        f.Drain();
        Value(f.frontend->ProcessVfxGpuReadbacks());
        CHECK(f.audit->copies == 0);
        CHECK(f.audit->discards == 1);
        CHECK(Value(f.frontend->GpuReadbackDiagnostics()).queue.pendingBytes == 0);
    }

    TEST_CASE("FAKE GPU protocol cannot weaken captured lifetime or reuse a released receipt", "[vfx-gpu][frontend][protocol-fake]") {
        Fixture f(false, false);
        f.Next();
        REQUIRE(f.Run(false).scheduled);
        SECTION("captured unit lifetime cancels despite absent cold parent") {
            (void)f.logical->Stop();
            CHECK_FALSE(f.Run().scheduled);
        }
        SECTION("explicit frontend release revokes new scheduling") {
            REQUIRE(f.frontend->ReleaseVfxGpuSimulation(f.receipt).HasValue());
            CHECK_FALSE(f.Run().scheduled);
        }
        CHECK(f.audit->encodes == 0);
    }

    TEST_CASE("FAKE provider CPU metadata cannot exceed the real preparation payload ledger", "[vfx-gpu][frontend][protocol-fake]") {
        auto audit = std::make_shared<Audit>();
        auto frontend = Create(audit);
        Vfx::Tests::GpuSeedFixture seed;
        auto info = seed.Info();
        const auto base = seed.Prepare()->Preparation().descriptor;
        info.maximumRetainedBytes = base.layout.byteCount * VfxGpuStateBufferCount(base) + GpuParticleForceWords::Bytes + 4;
        auto logical = Value(GpuParticleSimulator::Create(Vfx::Tests::ParticleDescriptor(seed.Source()), info));
        auto prepared = frontend->PrepareVfxGpuSimulation(logical->Preparation(), {}, logical->LifetimeCancellation());
        if (prepared.HasError() &&
            ErrorChainContains(prepared.ErrorValue(), VfxFrameErrors::Pending.domain, VfxFrameErrors::Pending.code)) {
            Value(frontend->ProcessResourceRequests());
            prepared = frontend->PrepareVfxGpuSimulation(logical->Preparation(), {}, logical->LifetimeCancellation());
        }
        CHECK(prepared.HasError());
        CHECK(audit->encodes == 0);
        CHECK_FALSE(logical->RenderState().active);
        REQUIRE(frontend->BeginShutdown().HasValue());
        CHECK(Value(frontend->PollDrain()) == RenderBackendDrainProgress::Drained);
    }

    /** @brief Drives only real cold preparation retries; expected pending remains in the same frontend transaction. */
    Result<VfxGpuPreparationReceipt> PrepareCold(RenderFrontend &frontend, const GpuParticleSimulator &logical) {
        for (unsigned retry = 0; retry < 8; ++retry) {
            auto result = frontend.PrepareVfxGpuSimulation(logical.Preparation(), {}, logical.LifetimeCancellation());
            if (result.HasValue() || !ErrorChainContains(result.ErrorValue(), VfxFrameErrors::Pending.domain, VfxFrameErrors::Pending.code))
                return result;
            const auto processed = frontend.ProcessResourceRequests();
            if (processed.HasError())
                return Result<VfxGpuPreparationReceipt>::Failure(processed.ErrorValue());
        }
        return Result<VfxGpuPreparationReceipt>::Failure(Failure());
    }

    TEST_CASE("FAKE cold canonical inventory rollback reaps actual registry pins before a frame-free retry",
              "[vfx-gpu][frontend][protocol-fake][rollback]") {
        Vfx::Tests::GpuSeedFixture seed;
        const auto seedOwner = seed.Prepare();
        const auto descriptor = seedOwner->Preparation().descriptor;
        const auto physical = descriptor.layout.byteCount * VfxGpuStateBufferCount(descriptor) + GpuParticleForceWords::Bytes + 4;
        RenderFrontendMemoryConfig memory;
        memory.budget = {.hardCapBytes = physical,
                         .defaultBlockBytes = physical,
                         .maximumBlockBytes = physical,
                         .maximumAlignment = 16,
                         .maximumPools = 4,
                         .maximumBlocks = 16,
                         .maximumReservations = 16,
                         .maximumAllocations = 16};
        auto audit = std::make_shared<Audit>();
        audit->extraMetadataBytes = 256U * 1024U;
        auto frontend = Create(audit, memory);
        auto small = seed.Info();
        small.maximumRetainedBytes = 128U * 1024U;
        auto rejectedOwner = Value(GpuParticleSimulator::Create(Vfx::Tests::ParticleDescriptor(seed.Source()), small));
        auto *sameOwner = rejectedOwner.get();
        const auto rejected = PrepareCold(*frontend, *rejectedOwner);
        REQUIRE(rejected.HasError());
        REQUIRE(audit->nativePreparations == 1);
        CHECK(rejectedOwner.get() == sameOwner);
        CHECK_FALSE(rejectedOwner->RenderState().active);
        REQUIRE_FALSE(audit->simulation.expired());
        const std::weak_ptr<Simulation> abandoned = audit->simulation;
        CHECK(audit->Sim()->RetainedPayloadBytes() > small.maximumRetainedBytes);
        REQUIRE(frontend->MemorySnapshot().committedBackingBytes == physical);
        const auto traceStart = audit->coldCallCount;
        auto corrected = seed.Info();
        auto retryOwner = Value(GpuParticleSimulator::Create(Vfx::Tests::ParticleDescriptor(seed.Source()), corrected));
        CHECK(SameVfxGpuIdentity(rejectedOwner->Preparation().descriptor.identity, retryOwner->Preparation().descriptor.identity));
        CHECK(rejectedOwner->Preparation().descriptor.initializationDigest != retryOwner->Preparation().descriptor.initializationDigest);
        const auto prepared = PrepareCold(*frontend, *retryOwner);
        INFO((prepared.HasError() ? prepared.ErrorValue().code.Value() + ": " + prepared.ErrorValue().message
                                  : "Corrected cold retry admitted."));
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value().IsLive());
        CHECK(abandoned.expired());
        CHECK(audit->reaped == 1);
        CHECK(audit->nativePreparations == 2);
        CHECK(audit->encodes == 0);
        const auto trace = std::span{audit->coldCalls}.subspan(traceStart, audit->coldCallCount - traceStart);
        const auto realize = std::ranges::find(trace, ColdCall::Realize);
        const auto destroyed = std::ranges::find(trace, ColdCall::DestroyBuffer);
        const auto reaped = std::ranges::find(trace, ColdCall::Reaped);
        const auto query = std::ranges::find(trace, ColdCall::QueryBuffer);
        REQUIRE(realize != trace.end());
        REQUIRE(destroyed != trace.end());
        REQUIRE(reaped != trace.end());
        REQUIRE(query != trace.end());
        // Native inventory maintenance releases pins; the subsequent existing frontend drain owns actual destruction.
        CHECK(realize < reaped);
        CHECK(reaped < destroyed);
        CHECK(destroyed < query);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == physical);
        CHECK(retryOwner->Activate(prepared.Value()).HasValue());
        REQUIRE(frontend->BeginShutdown().HasValue());
        CHECK(Value(frontend->PollDrain()) == RenderBackendDrainProgress::Drained);
    }

    /** @brief Provides a valid large fake arena so double charging exceeds the deliberately tight per-simulation budget. */
    NativeVfxGpuPreparationResources ArenaPlan() {
        return {{512U * 1024U, RenderBufferUsage::Uniform, RenderBufferAccess::HostVisible}, 2048, 256};
    }

    /** @brief Computes only exact delegated Null buffer bytes, excluding separately admitted CPU ownership payload. */
    std::size_t InventoryBytes(const VfxGpuSimulationDescriptor &descriptor, const NativeVfxGpuPreparationResources &plan) {
        return descriptor.layout.byteCount * VfxGpuStateBufferCount(descriptor) + GpuParticleForceWords::Bytes + 4 +
               plan.parameterArena.byteSize;
    }

    /** @brief Uses the existing real memory-budget owner with exactly one inventory worth of physical backing. */
    RenderFrontendMemoryConfig InventoryMemory(const std::size_t bytes) {
        RenderFrontendMemoryConfig memory;
        memory.budget = {.hardCapBytes = bytes,
                         .defaultBlockBytes = bytes,
                         .maximumBlockBytes = bytes,
                         .maximumAlignment = 16,
                         .maximumPools = 4,
                         .maximumBlocks = 16,
                         .maximumReservations = 16,
                         .maximumAllocations = 16};
        return memory;
    }

    /** @brief Drains the real frontend under this explicit fake provider's quiescence contract. */
    void CloseCold(RenderFrontend &frontend) {
        REQUIRE(frontend.BeginShutdown().HasValue());
        CHECK(Value(frontend.PollDrain()) == RenderBackendDrainProgress::Drained);
    }

    TEST_CASE("FAKE native uniform arena is real registry inventory charged once and pinned through logical release",
              "[vfx-gpu][frontend][protocol-fake][native-arena][ledger]") {
        Vfx::Tests::GpuSeedFixture seed;
        auto info = seed.Info();
        info.maximumRetainedBytes = 640U * 1024U;
        auto logical = Value(GpuParticleSimulator::Create(Vfx::Tests::ParticleDescriptor(seed.Source()), info));
        auto audit = std::make_shared<Audit>();
        audit->preparationResources = ArenaPlan();
        const auto physical = InventoryBytes(logical->Preparation().descriptor, audit->preparationResources);
        auto frontend = Create(audit, InventoryMemory(physical));
        const auto receipt = Value(PrepareCold(*frontend, *logical));
        auto simulation = audit->Sim();
        REQUIRE(simulation->parameterArena.handle.IsValid());
        CHECK(simulation->parameterArena.instance ==
              Value(Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, simulation->parameterArena.handle)));
        CHECK(simulation->preparationResources.parameterArena.byteSize == ArenaPlan().parameterArena.byteSize);
        CHECK(simulation->preparationResources.operationSlots == ArenaPlan().operationSlots);
        CHECK(simulation->preparationResources.parameterStrideBytes == ArenaPlan().parameterStrideBytes);
        CHECK(simulation->lease);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == physical);
        const auto firstQueries = audit->preparationQueries;
        CHECK(Value(PrepareCold(*frontend, *logical)).Id() == receipt.Id());
        CHECK(audit->preparationQueries == firstQueries);
        CHECK(audit->nativePreparations == 1);
        REQUIRE(frontend->ReleaseVfxGpuSimulation(receipt).HasValue());
        Value(frontend->ProcessResourceRequests());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == physical);
        simulation.reset();
        audit->Reap();
        Value(frontend->ProcessResourceRequests());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CloseCold(*frontend);
        CHECK(audit->simulation.expired());
        CHECK(std::ranges::count(std::span{audit->coldCalls}.first(audit->coldCallCount), ColdCall::CreateBuffer) ==
              VfxGpuStateBufferCount(logical->Preparation().descriptor) + 3);
        CHECK(std::ranges::count(std::span{audit->coldCalls}.first(audit->coldCallCount), ColdCall::DestroyBuffer) ==
              VfxGpuStateBufferCount(logical->Preparation().descriptor) + 3);
    }

    /** @brief Mutates only returned plan metadata, never native handles or shader artifacts. */
    NativeVfxGpuPreparationResources MalformedArena(const unsigned mode) {
        auto plan = ArenaPlan();
        switch (mode) {
            case 0:
                plan.parameterArena.usage = RenderBufferUsage::Storage;
                break;
            case 1:
                plan.parameterArena.access = RenderBufferAccess::DeviceLocal;
                break;
            case 2:
                plan.operationSlots = 0;
                break;
            case 3:
                plan.parameterStrideBytes = 127;
                break;
            case 4:
                plan.parameterStrideBytes = 129;
                break;
            case 5:
                --plan.parameterArena.byteSize;
                break;
            case 6:
                plan.parameterArena.byteSize = 0;
                break;
            default:
                plan.operationSlots = std::numeric_limits<std::uint32_t>::max();
                plan.parameterStrideBytes = std::numeric_limits<std::uint32_t>::max() - 15;
                plan.parameterArena.byteSize = 1;
        }
        return plan;
    }

    TEST_CASE("FAKE native arena rejects malformed plans before buffer reservation or native preparation",
              "[vfx-gpu][frontend][protocol-fake][native-arena][invalid]") {
        const unsigned mode = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U);
        Vfx::Tests::GpuSeedFixture seed;
        auto logical = seed.Prepare();
        auto audit = std::make_shared<Audit>();
        audit->preparationResources = MalformedArena(mode);
        auto frontend = Create(audit);
        CHECK(PrepareCold(*frontend, *logical).HasError());
        CHECK(audit->preparationQueries == 1);
        CHECK(audit->nativePreparations == 0);
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CHECK(std::ranges::count(std::span{audit->coldCalls}.first(audit->coldCallCount), ColdCall::CreateBuffer) == 0);
        CloseCold(*frontend);
    }

    TEST_CASE("FAKE native arena query preserves its original typed error without allocation",
              "[vfx-gpu][frontend][protocol-fake][native-arena][query-error]") {
        Vfx::Tests::GpuSeedFixture seed;
        auto logical = seed.Prepare();
        auto audit = std::make_shared<Audit>();
        audit->preparationQueryError = Failure();
        auto frontend = Create(audit);
        const auto result = PrepareCold(*frontend, *logical);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().domain.Value() == audit->preparationQueryError->domain.Value());
        CHECK(result.ErrorValue().code.Value() == audit->preparationQueryError->code.Value());
        CHECK(result.ErrorValue().message == audit->preparationQueryError->message);
        CHECK(audit->nativePreparations == 0);
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CloseCold(*frontend);
    }

    TEST_CASE("FAKE native arena cost overflow rejects the whole cold inventory before any resource queueing",
              "[vfx-gpu][frontend][protocol-fake][native-arena][overflow]") {
        Vfx::Tests::GpuSeedFixture seed;
        auto logical = seed.Prepare();
        auto audit = std::make_shared<Audit>();
        audit->preparationResources = ArenaPlan();
        audit->overflowArenaCost = true;
        auto frontend = Create(audit);
        CHECK(PrepareCold(*frontend, *logical).HasError());
        CHECK(audit->nativePreparations == 0);
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CHECK(std::ranges::count(std::span{audit->coldCalls}.first(audit->coldCallCount), ColdCall::CreateBuffer) == 0);
        CloseCold(*frontend);
    }

    TEST_CASE("FAKE native arena cannot bypass the actual global physical budget",
              "[vfx-gpu][frontend][protocol-fake][native-arena][global-budget]") {
        Vfx::Tests::GpuSeedFixture seed;
        auto logical = seed.Prepare();
        auto audit = std::make_shared<Audit>();
        audit->preparationResources = ArenaPlan();
        auto frontend = Create(audit, InventoryMemory(InventoryBytes(logical->Preparation().descriptor, audit->preparationResources) - 1));
        CHECK(PrepareCold(*frontend, *logical).HasError());
        CHECK(audit->nativePreparations == 0);
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        CloseCold(*frontend);
    }

    TEST_CASE("FAKE native arena rollback reclaims its real pinned allocation before a frame-free retry",
              "[vfx-gpu][frontend][protocol-fake][native-arena][rollback]") {
        Vfx::Tests::GpuSeedFixture seed;
        auto small = seed.Info();
        small.maximumRetainedBytes = 128U * 1024U;
        auto rejected = Value(GpuParticleSimulator::Create(Vfx::Tests::ParticleDescriptor(seed.Source()), small));
        auto audit = std::make_shared<Audit>();
        audit->preparationResources = {{4096, RenderBufferUsage::Uniform, RenderBufferAccess::HostVisible}, 16, 256};
        audit->extraMetadataBytes = 256U * 1024U;
        const auto physical = InventoryBytes(rejected->Preparation().descriptor, audit->preparationResources);
        auto frontend = Create(audit, InventoryMemory(physical));
        CHECK(PrepareCold(*frontend, *rejected).HasError());
        REQUIRE(audit->nativePreparations == 1);
        const std::weak_ptr<Simulation> abandoned = audit->simulation;
        REQUIRE_FALSE(abandoned.expired());
        REQUIRE(audit->Sim()->parameterArena.handle.IsValid());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == physical);
        auto retry = seed.Prepare();
        const auto receipt = Value(PrepareCold(*frontend, *retry));
        CHECK(receipt.IsLive());
        CHECK(abandoned.expired());
        CHECK(audit->reaped == 1);
        CHECK(audit->nativePreparations == 2);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == physical);
        const auto buffers = VfxGpuStateBufferCount(retry->Preparation().descriptor) + 3;
        CHECK(std::ranges::count(std::span{audit->coldCalls}.first(audit->coldCallCount), ColdCall::DestroyBuffer) == buffers);
        REQUIRE(frontend->ReleaseVfxGpuSimulation(receipt).HasValue());
        audit->Reap();
        Value(frontend->ProcessResourceRequests());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CloseCold(*frontend);
        CHECK(audit->simulation.expired());
        CHECK(std::ranges::count(std::span{audit->coldCalls}.first(audit->coldCallCount), ColdCall::DestroyBuffer) == 2 * buffers);
    }

}  // namespace Horo::Render::ProtocolFake
