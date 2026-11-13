#include "MetalVfxGpuEndToEndFixture.h"

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecution.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Vfx/CpuParticleSimulator.h"
#include "Horo/Vfx/VfxWorld.h"
#include "VfxGpuCook.h"
#include "editor/renderer/metal/SdlMetalPresentationPort.h"
#include "runtime/renderer/frontend/RenderFrontendResourceAccess.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"
#include "support/VfxGpuQualification.h"
#include "support/VfxTestSupport.h"

#import <Metal/Metal.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_approx.hpp>
#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Horo::Render::Test {
    namespace {
        using namespace Vfx;
        using namespace std::chrono_literals;
        constexpr std::uint32_t Count = 8;
        constexpr float Delta = .1F;
        constexpr FramebufferExtent Extent{64, 64};

        /** @brief Keeps original errors visible at every actual public admission boundary. */
        template <class T> T Value(Result<T> result) {
            INFO(
                (result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Actual operation accepted."));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Retains the original typed failure without altering the public operation. */
        void Accepted(const Result<void> &result) {
            INFO(
                (result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Actual operation accepted."));
            REQUIRE(result.HasValue());
        }

        /** @brief Rebuilds reflection candidates only from the genuine bound normalized records. */
        ShaderReflectionCandidate Candidate(const NormalizedShaderReflection &r) {
            return {r.backend, r.interfaceSchemaVersion, r.bindings, r.parameters, r.stageInterface, r.targetBindings, r.sourceMap};
        }

        /** @brief Reads exact bounded packaged files through the existing offline input reader, never a runtime compiler. */
        CookedVfxGpuProgram LoadProgram(const std::filesystem::path &root, VfxGpuProgramRole role, std::string_view prefix) {
            const auto target = Editor::Offline::VfxGpuMetalCookTarget();
            auto manifest = MakeVfxGpuShaderManifest(role, target);
            auto payload =
                Value(Application::Internal::ReadShaderCookInput(root / (std::string{prefix} + ".horoshdr"), 16U * 1024U * 1024U));
            auto metadata =
                Value(Application::Internal::ReadShaderCookInput(root / (std::string{prefix} + ".reflection.json"), 16U * 1024U * 1024U));
            auto bound = Value(DecodeReflectedShaderArtifact(manifest, target, payload, metadata));
            CompiledShaderArtifact artifact{.backend = target.backend,
                                            .payloadFormat = target.payloadFormat,
                                            .artifactKey = bound.compiledArtifactKey,
                                            .payload = std::move(payload),
                                            .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(bound))};
            return {std::move(manifest), target, artifact, Candidate(artifact.reflection->reflection)};
        }

        /** @brief Requires both actual canonical bound programs, including all three native stages. */
        CookedVfxGpuPrograms LoadFamily(const std::filesystem::path &root) {
            const auto prefixes = Editor::Offline::VfxGpuCookPrefixes();
            CookedVfxGpuPrograms family{LoadProgram(root, VfxGpuProgramRole::Simulation, prefixes[0]),
                                        LoadProgram(root, VfxGpuProgramRole::Draw, prefixes[1])};
            Accepted(ValidateCookedVfxGpuPrograms(family, ShaderTargetBackend::Metal));
            return family;
        }

        /** @brief Waits only in this explicitly bounded native fixture; production frame submission never waits here. */
        bool Terminal(id<MTLCommandBuffer> commands) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (commands && commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(1ms);
            }
            return !commands || commands.status == MTLCommandBufferStatusCompleted;
        }

        /** @brief Retains all owners until the actual public shutdown path proves native retirement. */
        bool Drain(RenderFrontend &frontend) {
            if (frontend.BeginShutdown().HasError())
                return false;
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto state = frontend.PollDrain();
                if (state.HasError())
                    return false;
                if (state.Value() == RenderBackendDrainProgress::Drained)
                    return true;
                std::this_thread::sleep_for(1ms);
            }
            return false;
        }

        /** @brief Defines authored seed inputs shared by two real CPU simulators and the actual GPU preparation. */
        ParticleSystemDescriptor Source(SimulationPreference preference, VfxGpuReadbackDescriptor readback = {}, bool variedSeed = false,
                                        std::uint32_t count = Count) {
            auto d = Vfx::Tests::ValidParticleDescriptorData();
            d.maximumParticles = count;
            d.shape = ParticleEmitterShape::Point;
            d.spawnRate = {0, 0};
            d.initialSpeed = {0, 0};
            d.initialSize = {.3, .3};
            d.initialOpacity = {1, 1};
            d.lifetimeSeconds = {10, 10};
            // This authored fixture is the PreparedParticles force/integration subset, not the generic source helper's SceneDepth recipe.
            d.collisionMode = ParticleCollisionMode::None;
            d.lifetimeKind = ParticleLifetimeKind::Finite;
            d.killCondition = ParticleKillCondition::Lifetime;
            d.renderMode = ParticleRenderMode::Billboard;
            d.sortMode = ParticleSortMode::None;
            d.simulationPreference = preference;
            d.gpuReadback = readback;
            if (variedSeed) {
                d.shape = ParticleEmitterShape::Box;
                d.initialSpeed = {.1, .3};
                d.initialSize = {.2, .4};
                d.lifetimeSeconds = {9, 10};
            }
            return Vfx::Tests::ParticleDescriptor(d);
        }

        /** @brief Requires every authored readback field to survive actual logical and native preparation without a host override. */
        void CheckReadbackPolicy(const VfxGpuReadbackDescriptor &actual, const VfxGpuReadbackDescriptor &source) {
            CHECK(actual.enabled == source.enabled);
            CHECK(actual.schemaVersion == source.schemaVersion);
            CHECK(actual.purpose == source.purpose);
            CHECK(actual.requirement == source.requirement);
            CHECK(actual.cadenceSteps == source.cadenceSteps);
            CHECK(actual.maximumRecords == source.maximumRecords);
            CHECK(actual.maximumBytesPerResult == source.maximumBytesPerResult);
            CHECK(actual.maximumPendingResults == source.maximumPendingResults);
        }

        /** @brief Obtains genuine cook admission for this exact validated fixture source and declared fixture dependency.
         * @details Available asserts only this fixture's authored material identity; it is not an AssetRegistry snapshot
         * or native material qualification. Native GPU program/resource evidence is exercised independently below.
         */
        ParticleCookAdmission CookAdmission(const ParticleSystemDescriptor &source) {
            const auto profile = Value(GetParticleCookProfile(ParticleCookTier::Standard));
            const ParticleMaterialEvidence material{source.Data().material, ParticleMaterialAvailability::Available};
            auto cooked = Value(BuildParticleSystemCookPlan(source, profile, material, Vfx::Tests::ParticleErrorRegistry(),
                                                            "native-vfx-prepared-particles-fixture"));
            REQUIRE(cooked.Accepted());
            REQUIRE(cooked.admission.has_value());
            CHECK(cooked.admission->Descriptor().Data() == source.Data());
            CHECK(cooked.admission->Plan().material == source.Data().material);
            CheckReadbackPolicy(cooked.admission->Plan().gpuReadback, source.Data().gpuReadback);
            return std::move(*cooked.admission);
        }

        const std::array Forces{CpuParticleForceModule{.kind = CpuParticleForceKind::Gravity, .vector = {0, -.1F, 0}},
                                CpuParticleForceModule{.kind = CpuParticleForceKind::Wind, .vector = {1, 0, 0}, .strength = .1F},
                                CpuParticleForceModule{.kind = CpuParticleForceKind::Attraction,
                                                       .center = {0, .5F, 0},
                                                       .strength = .05F,
                                                       .falloff = .5F},
                                CpuParticleForceModule{.kind = CpuParticleForceKind::Noise,
                                                       .strength = .05F,
                                                       .frequency = 2,
                                                       .randomChannel = 19}};

        /** @brief Provides authored logical fixture identity, never a native submission identity. */
        EffectSystemId Effect() {
            return {VfxIdentityScope::Create(42).Value(), 2, 1};
        }

        /** @brief Provides authored logical buffer identity independently of renderer-issued resource handles. */
        ParticleBufferId Buffer() {
            return {Effect().scope, 5, 1};
        }

        /** @brief Creates the genuine canonical CPU oracle; it never produces runtime GPU output. */
        CpuParticleSimulator Cpu(std::span<const CpuParticleForceModule> forces = Forces, bool variedSeed = false,
                                 std::uint32_t count = Count) {
            return Value(CpuParticleSimulator::Create(Source(SimulationPreference::RequireCPU, {}, variedSeed, count),
                                                      {.buffer = Buffer(),
                                                       .activation = Effect(),
                                                       .effectSeed = 0x123456789abcdef0ULL,
                                                       .maximumBurstParticles = count,
                                                       .forces = forces}));
        }

        /** @brief Projects complete CPU seed streams, without private slot layout or native pointers. */
        CpuParticleSoAConstView SeedView(const CpuParticleExtractView &v) {
            CpuParticleSoAConstView a;
            a.positionX = v.positionX;
            a.positionY = v.positionY;
            a.positionZ = v.positionZ;
            a.velocityX = v.velocityX;
            a.velocityY = v.velocityY;
            a.velocityZ = v.velocityZ;
            a.sizeX = v.sizeX;
            a.sizeY = v.sizeY;
            a.rotation = v.rotation;
            a.angularVelocity = v.angularVelocity;
            a.age = v.age;
            a.maximumAge = v.maximumAge;
            a.packedColor = v.packedColor;
            a.customFlags = v.customFlags;
            return a;
        }

        /** @brief Requires actual selected frontend feature admission before constructing the host's immutable resolver facts. */
        VfxResolution Resolution(const RenderFrontend &frontend, std::uint32_t count) {
            const auto &support = frontend.Capabilities().support;
            REQUIRE(support.features.Supports(RenderCapability::VfxGpuSimulation));
            REQUIRE(support.features.Supports(RenderCapability::VfxGpuDraw));
            std::array<VfxCapabilityFact, VfxCapabilityCount> facts;
            for (std::size_t i = 0; i < facts.size(); ++i)
                facts[i] = {static_cast<VfxCapability>(i), VfxCapabilitySupport::Unsupported};
            facts[static_cast<std::size_t>(VfxCapability::CpuSimulation)].support = VfxCapabilitySupport::Available;
            facts[static_cast<std::size_t>(VfxCapability::GpuSimulation)].support = VfxCapabilitySupport::Available;
            const VfxResourceLimits limits{count, 0, 0, 0, 128U * 1024U * 1024U, 10, 10};
            const auto caps =
                Value(VfxCapabilities::Create(Value(VfxCapabilityRevision::Create(support.deviceIncarnation)), facts, limits));
            // Product policy requires positive ceilings for every category. Actual capability counts stay zero and
            // unsupported above, so a policy ceiling never invents usable decals, lights or volumes.
            const VfxResourceLimits policyLimits{count, 1, 1, 1, 128U * 1024U * 1024U, 10, 10};
            const auto policy =
                Value(VfxQualityPolicy::Create({Value(VfxQualityPolicyRevision::Create(1)), VfxQualityProfile::Standard, policyLimits, 1}));
            VfxResolutionRequest request{.hostMode = VfxHostMode::Interactive,
                                         .requestedProfile = VfxQualityProfile::Standard,
                                         .expectedCapabilityRevision = caps.Revision(),
                                         .expectedPolicyRevision = policy.Revision(),
                                         .requirements = {.category = VfxEffectCategory::Particle,
                                                          .preference = SimulationPreference::RequireGPU,
                                                          .requirementClass = VfxRequirementClass::Cosmetic,
                                                          .requestedCount = count,
                                                          .minimumAuthoredCount = count,
                                                          .bytesPerElement = (128U * 1024U * 1024U) / count,
                                                          .cpuWorkMilliseconds = 1,
                                                          .gpuWorkMilliseconds = 1,
                                                          .hasCpuKernel = true,
                                                          .hasGpuKernel = true}};
            request.requirements.requiredGpuCapabilities[static_cast<std::size_t>(VfxCapability::GpuSimulation)] = true;
            return Value(ResolveSimulationDomain(caps, policy, request));
        }

        /** @brief Owns one separate unfocused SDL/native fixture and every real frontend/world lifetime. */
        class Fixture final {
        public:
            explicit Fixture(const std::filesystem::path &root, std::span<const CpuParticleForceModule> forces = Forces,
                             bool variedSeed = false, std::uint32_t count = Count)
                : family_(LoadFamily(root)), forces_(forces), variedSeed_(variedSeed), count_(count),
                  oracle_(Cpu(forces, variedSeed, count)) {}

            ~Fixture() {
                ReleaseGate();
                if (!Terminal(commands_) || !Terminal(pixelCommands_))
                    std::terminate();
                if (world_)
                    (void)world_->Shutdown();
                if (frontend_ && !RetireColor())
                    std::terminate();
                if (frontend_ && !Drain(*frontend_))
                    std::terminate();
                world_.reset();
                frontend_.reset();
                presentation_.reset();
                if (window_)
                    SDL_DestroyWindow(window_);
                SDL_Quit();
            }

            /** @brief Creates an isolated unfocused fixture surface and selects the real production Metal backend. */
            void Initialize() {
                REQUIRE(SDL_Init(SDL_INIT_VIDEO));
                REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "0"));
                REQUIRE(SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0"));
                window_ = SDL_CreateWindow("Horo GPU VFX native qualification", 64, 64, SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN);
                REQUIRE(window_);
                REQUIRE(SDL_SetWindowFocusable(window_, false));
                presentation_ = std::make_unique<Editor::SdlMetalPresentationPort>(*window_);
                Accepted(RegisterMetalRenderBackend(registry_, *presentation_, bridge_));
                Accepted(registry_.Seal());
                frontend_ = Value(RenderFrontend::Create(registry_, RenderBackendId{"metal"},
                                                         {.requirePresentation = true,
                                                          .enableValidation = true,
                                                          .maxFramesInFlight = 2,
                                                          .presentMode = PresentMode::Immediate}));
                REQUIRE(SDL_ShowWindow(window_));
                const auto topology = Value(Detail::RenderFrontendResourceAccess::QueueAssignments(*frontend_));
                queues_.assign(topology.begin(), topology.end());
                PrepareColor();
            }

            /** @brief Copies genuine CPU initialization, obtains native admission and transfers logical ownership into the real world. */
            void Prepare(bool observe, std::chrono::nanoseconds timeout = 5s) {
                if (observe)
                    PrepareReadback(timeout);
                auto seed = Cpu(forces_, variedSeed_, count_);
                Value(seed.Advance({.deltaSeconds = 0, .burstCount = count_, .tick = 0}));
                const auto view = Value(seed.Extract());
                std::vector<ParticleSimulationId> ids(count_);
                for (std::uint32_t i = 0; i < count_; ++i)
                    ids[i] = Value(seed.HandleAtDenseIndex(i)).particle;
                GpuParticleSimulatorCreateInfo info;
                info.scene = Effect().scope;
                info.activation = Effect();
                info.buffer = Buffer();
                info.effectSeed = 0x123456789abcdef0ULL;
                info.resolution = Resolution(*frontend_, count_);
                REQUIRE(info.resolution.domain == ResolvedSimulationDomain::GPU);
                info.initialization = GpuParticleInitialization::PreparedParticles;
                info.requiredFeatures = GpuParticleRequiredFeature::None;
                info.initial = {SeedView(view), ids};
                info.forces = forces_;
                info.layoutLimits = {128U * 1024U * 1024U, frontend_->Capabilities().support.limits.maxBufferBytes};
                info.maximumRetainedBytes = 64U * 1024U * 1024U;
                info.maximumPendingSteps = 2;
                info.maximumSteps = 128;
                info.maximumDeltaSeconds = Delta;
                VfxGpuReadbackDescriptor policy;
                if (observe)
                    policy = {true, 1,      VfxGpuReadbackPurpose::DiagnosticsCapture, VfxGpuReadbackRequirement::RequiredForVisualEffect,
                              1,    count_, count_ * VfxGpuObservationWords::Bytes,    1};
                const auto source = Source(SimulationPreference::RequireGPU, policy, variedSeed_, count_);
                CHECK(source.Data().gpuReadback.enabled == observe);
                INFO("Cold GPU logical admission: actual selected domain, finite PreparedParticles state, shared policy and payload "
                     "ledger.");
                const auto admission = CookAdmission(source);
                auto logical = Value(GpuParticleSimulator::Create(admission, info));
                CheckReadbackPolicy(logical->Preparation().descriptor.readback, source.Data().gpuReadback);
                receipt_ = PrepareNative(*logical);
                CheckReadbackPolicy(receipt_.Descriptor().readback, source.Data().gpuReadback);
                CHECK(receipt_.Readback() == (observe ? VfxGpuReadbackAdmission::Available : VfxGpuReadbackAdmission::Disabled));
                Accepted(logical->Activate(receipt_));
                std::array<VfxGpuEmitterPreparation, 1> gpu;
                gpu[0] = {Effect(), source.Data().emitter, std::move(logical)};
                INFO("Transfer actual activated GPU owner into canonical VfxWorld: one emitter, authored particle count, four bounded "
                     "frame slots.");
                world_ = Value(VfxWorld::Prepare(Effect().scope, {}, gpu, {4, count_, 4, 4, 128U * 1024U * 1024U}));
                REQUIRE_FALSE(gpu[0].simulator);
                Accepted(world_->Play(Effect()));
                CHECK(Capture().GpuBatches()[0].generation == 1);
            }

            /** @brief Extracts immutable values without advancing simulation or dispatching commands. */
            RenderWorldSnapshot Capture() {
                return world_->Extract(++snapshot_, {}, 1).snapshot;
            }

            RenderFrontend &Frontend() {
                return *frontend_;
            }

            /** @brief Reports the actual selected native device outside the measured batch. */
            std::string DeviceName() const {
                id<MTLDevice> device = (__bridge id<MTLDevice>)bridge_.Device();
                const char *name = device.name.UTF8String;
                REQUIRE(name);
                return name;
            }

            const VfxGpuPreparationReceipt &Receipt() const {
                return receipt_;
            }

            const VfxGpuStepIdentity &Step() const {
                return step_;
            }

            /** @brief Uses the scene owner exact-next-tick producer without dispatching GPU work. */
            void PrepareStep(float deltaSeconds = Delta) {
                const auto report = world_->PrepareGpuSteps(deltaSeconds);
                REQUIRE(report.status == GpuParticleStepStatus::Complete);
            }

            /** @brief Schedules and submits a real same-graph compute/direct-draw contribution using issued queue points. */
            void Submit(bool advance = true, bool gated = false) {
                auto snapshot = Capture();
                REQUIRE(snapshot.GpuBatches().size() == 1);
                const auto &batch = snapshot.GpuBatches()[0];
                const auto before = batch.generation;
                auto frame = BeginFixtureFrame();
                auto builder =
                    Value(RenderGraphBuilder::Create({.maxPasses = 8, .maxResources = 8, .maxUsages = 16, .maxDependencies = 16}));
                const auto color = Value(builder.ImportTexture(color_, RenderGraphResourceClass::Persistent));
                const auto anchor = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                const auto output = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                Accepted(builder.AddUsage({anchor, color, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}));
                VfxGpuGraphRequest request{.preparation = batch.preparation,
                                           .generation = batch.generation,
                                           .pending = advance ? batch.pending : std::nullopt,
                                           .lifetimeCancellation = batch.lifetimeCancellation,
                                           .view = {.originDelta = batch.originDelta, .extent = Extent},
                                           .color = color,
                                           .colorOperations = {.loadOperation = AttachmentLoadOperation::Load},
                                           .lightingComplete = anchor,
                                           .outputTransform = output,
                                           .remainingDrawCalls = 4,
                                           .remainingPasses = 6,
                                           .remainingResources = 7,
                                           .remainingUsages = 15};
                request.view.viewProjection.values[14] = .5F;
                auto contribution = frame.ScheduleVfxGpu(builder, request);
                INFO((contribution.HasError() ? contribution.ErrorValue().message : "Same-graph contribution admitted."));
                REQUIRE(contribution.HasValue());
                if (advance)
                    CheckCandidate(contribution.Value(), batch);
                CHECK(Capture().GpuBatches()[0].generation == before);
                Execute(frame, builder, contribution.Value(), color, anchor, output);
                commands_ = (__bridge id<MTLCommandBuffer>)bridge_.CurrentCommandBuffer();
                REQUIRE(commands_);
                if (gated)
                    HoldCompletion();
                Accepted(frame.Present());
                if (advance)
                    step_ = batch.pending->step;
            }

            /** @brief Waits only at the explicit fixture boundary before pumping genuine completion facts. */
            void WaitComplete() {
                ReleaseGate();
                REQUIRE(Terminal(commands_));
                Pump();
            }

            /** @brief Requires actual native accepted-submission evidence before observing logical advancement. */
            void AwaitAccepted() {
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                while (std::chrono::steady_clock::now() < deadline) {
                    Pump();
                    if (Capture().GpuBatches()[0].generation == step_.targetGeneration)
                        return;
                    std::this_thread::sleep_for(1ms);
                }
                FAIL("Actual native accepted-submission receipt did not arrive.");
            }

            /** @brief Drains authentic acknowledgements once and checks duplicate replay cannot advance the world. */
            void Pump() {
                std::array<GpuVfxStepAcknowledgement, 64> acknowledgements;
                const auto count = Value(frontend_->DrainVfxGpuAcknowledgements(acknowledgements));
                if (count) {
                    const auto report = world_->ApplyGpuAcknowledgements(std::span{acknowledgements}.first(count));
                    REQUIRE(report.rejected == 0);
                    const auto repeated = world_->ApplyGpuAcknowledgements(std::span{acknowledgements}.first(count));
                    CHECK(repeated.processed == 0);
                }
                Value(frontend_->ProcessVfxGpuReadbacks());
            }

            /** @brief Compares actual normalized GPU observations against a separately executed canonical CPU oracle. */
            void CheckOracle(RenderReadbackResult &result) {
                Value(oracle_.Advance({.deltaSeconds = Delta, .burstCount = step_.tick == 0 ? count_ : 0, .tick = step_.tick}));
                const auto cpu = Value(oracle_.Extract());
                REQUIRE(result.Bytes().size() == count_ * VfxGpuObservationWords::Bytes);
                for (std::uint32_t i = 0; i < count_; ++i)
                    CheckParticle(result.Bytes().subspan(i * VfxGpuObservationWords::Bytes, VfxGpuObservationWords::Bytes), cpu, i);
            }

            /** @brief Acquires an actual prepared-queue result lease without synthesizing completion. */
            RenderReadbackResult Acquire() {
                RenderReadbackResult result;
                REQUIRE(frontend_->AcquireVfxGpuObservation(receipt_, step_, result) == RenderReadbackPreparedStatus::Complete);
                return result;
            }

            /** @brief Requires a retained consumer lease to block a new required observation before native dispatch. */
            void CheckBackpressure() {
                PrepareStep();
                auto snapshot = Capture();
                auto frame = BeginFixtureFrame();
                auto builder =
                    Value(RenderGraphBuilder::Create({.maxPasses = 8, .maxResources = 8, .maxUsages = 16, .maxDependencies = 16}));
                const auto color = Value(builder.ImportTexture(color_, RenderGraphResourceClass::Persistent));
                const auto anchor = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                const auto output = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                const auto &batch = snapshot.GpuBatches()[0];
                const auto denied = frame.ScheduleVfxGpu(builder, {.preparation = receipt_,
                                                                   .generation = batch.generation,
                                                                   .pending = batch.pending,
                                                                   .lifetimeCancellation = batch.lifetimeCancellation,
                                                                   .view = {.extent = Extent},
                                                                   .color = color,
                                                                   .lightingComplete = anchor,
                                                                   .outputTransform = output,
                                                                   .remainingDrawCalls = 4,
                                                                   .remainingPasses = 6,
                                                                   .remainingResources = 7,
                                                                   .remainingUsages = 15});
                REQUIRE(denied.HasError());
                REQUIRE(denied.FailureDescriptor());
                CHECK(denied.FailureDescriptor()->code.Value() == VfxFrameErrors::Pending.code.Value());
            }

            /** @brief Copies real rendered sprite pixels only in this bounded fixture, separately from particle observation diagnostics. */
            void CheckPixels() {
                const auto identity = Value(Detail::RenderFrontendResourceAccess::BackendInstance(*frontend_, colorView_));
                auto *resident = reinterpret_cast<Detail::MetalTextureViewInstance *>(static_cast<std::uintptr_t>(identity));
                id<MTLDevice> device = (__bridge id<MTLDevice>)bridge_.Device();
                id<MTLCommandQueue> queue = [device newCommandQueue];
                id<MTLBuffer> bytes = [device newBufferWithLength:512 options:MTLResourceStorageModeShared];
                REQUIRE(queue);
                REQUIRE(bytes);
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                CopyPixel(blit, resident->texture, bytes, 32, 32, 0);
                CopyPixel(blit, resident->texture, bytes, 0, 0, 256);
                [blit endEncoding];
                pixelCommands_ = commands;
                [commands commit];
                REQUIRE(Terminal(commands));
                const auto *pixel = static_cast<const std::uint8_t *>(bytes.contents);
                CHECK(pixel[0] > 32);
                CHECK(pixel[1] > 32);
                CHECK(pixel[2] > 32);
                CHECK(pixel[256] < 3);
                CHECK(pixel[257] < 3);
                CHECK(pixel[258] < 3);
            }

            /** @brief Distinguishes immediate Stop revocation from the next frame's completed registry retirement. */
            void StopRejectsSnapshot() {
                auto retained = Capture();
                const auto &batch = retained.GpuBatches()[0];
                const auto generation = batch.generation;
                {
                    INFO("Stop phase: acquire live frame before immediate unit revocation.");
                    auto frame = BeginFixtureFrame();
                    Value(world_->Stop(Effect()));
                    CHECK(batch.lifetimeCancellation.IsCancellationRequested());
                    CHECK(batch.generation == generation);
                    CHECK(world_->Extract(++snapshot_, {}, 1).snapshot.GpuBatches().empty());
                    CheckStoppedSnapshot(frame, batch, VfxFrameErrors::Cancelled);
                }
                {
                    INFO("Stop phase: acquire next frame after aborted frame and cold registry retirement.");
                    auto frame = BeginFixtureFrame();
                    CheckStoppedSnapshot(frame, batch, VfxFrameErrors::InvalidInput);
                }
                world_.reset();
                CHECK(batch.generation == generation);
                CHECK(batch.lifetimeCancellation.IsCancellationRequested());
                CHECK(batch.preparation.Id() == receipt_.Id());
            }

        private:
            /** @brief Checks one exact lifecycle rejection without executing any compute or draw workload. */
            void CheckStoppedSnapshot(RenderFrameScope &frame, const GpuVfxRenderBatch &batch, const ErrorCodeDescriptor &expected) {
                auto builder =
                    Value(RenderGraphBuilder::Create({.maxPasses = 8, .maxResources = 8, .maxUsages = 16, .maxDependencies = 16}));
                const auto color = Value(builder.ImportTexture(color_, RenderGraphResourceClass::Persistent));
                const auto anchor = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                const auto output = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
                const auto rejected = frame.ScheduleVfxGpu(builder, {.preparation = batch.preparation,
                                                                     .generation = batch.generation,
                                                                     .lifetimeCancellation = batch.lifetimeCancellation,
                                                                     .view = {.originDelta = batch.originDelta, .extent = Extent},
                                                                     .color = color,
                                                                     .lightingComplete = anchor,
                                                                     .outputTransform = output,
                                                                     .remainingDrawCalls = 4,
                                                                     .remainingPasses = 6,
                                                                     .remainingResources = 7,
                                                                     .remainingUsages = 15});
                REQUIRE(rejected.HasError());
                REQUIRE(rejected.FailureDescriptor());
                CHECK(rejected.FailureDescriptor()->domain.Value() == expected.domain.Value());
                CHECK(rejected.FailureDescriptor()->code.Value() == expected.code.Value());
            }

            /** @brief Creates an ordinary real frontend attachment and proves readiness before frame admission. */
            void PrepareColor() {
                // Public copy-texture workloads are not admitted here. The explicit fixture-only native pixel blit below
                // follows the existing viewport smoke path and is separate from particle observation diagnostics.
                color_ = Value(frontend_->CreateTexture({.extent = Extent,
                                                         .format = RenderTextureFormat::Rgba8Unorm,
                                                         .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled}))
                             .handle;
                Value(frontend_->ProcessResourceRequests());
                REQUIRE(Value(frontend_->ResourceState(color_)) == RenderResourceState::Ready);
                colorView_ = Value(frontend_->CreateTextureView({.texture = color_, .format = RenderTextureFormat::Rgba8Unorm})).handle;
                Value(frontend_->ProcessResourceRequests());
                REQUIRE(Value(frontend_->ResourceState(colorView_)) == RenderResourceState::Ready);
            }

            /** @brief Retires the real view before its source attachment while the selected owner is still open. */
            bool RetireColor() noexcept {
                if (colorView_.IsValid() && frontend_->ReleaseTextureView(colorView_).HasError())
                    return false;
                if (color_.IsValid() && frontend_->ReleaseTexture(color_).HasError())
                    return false;
                return true;
            }

            /** @brief Reserves the existing queue only for explicit actual supported observation admission. */
            void PrepareReadback(std::chrono::nanoseconds timeout) {
                REQUIRE(frontend_->Capabilities().support.features.Supports(RenderCapability::VfxGpuReadback));
                const auto bytes = count_ * VfxGpuObservationWords::Bytes;
                Value(frontend_->PrepareVfxGpuReadbackStorage({bytes, bytes, bytes, 16, 1}, {1, bytes, 16}, 1024U * 1024U, timeout));
            }

            /** @brief Retries only the exact cold pending upload transaction under a finite fixture deadline. */
            VfxGpuPreparationReceipt PrepareNative(GpuParticleSimulator &logical) {
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                for (;;) {
                    auto prepared = frontend_->PrepareVfxGpuSimulation(logical.Preparation(), family_, logical.LifetimeCancellation());
                    if (prepared.HasValue())
                        return std::move(prepared).Value();
                    INFO(prepared.ErrorValue().code.Value() + ": " + prepared.ErrorValue().message);
                    REQUIRE(prepared.ErrorValue().code.Value() == VfxFrameErrors::Pending.code.Value());
                    Value(frontend_->ProcessResourceRequests());
                    REQUIRE(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::sleep_for(1ms);
                }
            }

            /** @brief Retries only genuine pending acquisition within one explicit fixture deadline. */
            RenderFrameScope BeginFixtureFrame() {
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                const auto number = ++frame_;
                for (;;) {
                    AwaitPresentation(deadline);
                    auto begun = frontend_->BeginFrame({.frameNumber = number, .outputExtent = Extent});
                    if (begun.HasValue())
                        return std::move(begun).Value();
                    const auto &error = begun.ErrorValue();
                    if (error.domain.Value() != RenderPresentationErrors::AcquisitionPending.domain.Value() ||
                        error.code.Value() != RenderPresentationErrors::AcquisitionPending.code.Value())
                        return Value(std::move(begun));
                    INFO("Actual Metal BeginFrame acquisition is pending for fixture frame " << number);
                    REQUIRE(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::sleep_for(1ms);
                }
            }

            /** @brief Polls only this isolated fixture drawable within the acquisition attempt's shared deadline. */
            void AwaitPresentation(std::chrono::steady_clock::time_point deadline) {
                while (std::chrono::steady_clock::now() < deadline) {
                    SDL_PumpEvents();
                    const auto available = frontend_->PollPresentationAvailability(Extent);
                    if (available.HasValue() && available.Value())
                        return;
                    if (available.HasError()) {
                        INFO(available.ErrorValue().message);
                        REQUIRE(available.ErrorValue().code.Value() == RenderPresentationErrors::DrawableUnavailable.code.Value());
                    }
                    std::this_thread::sleep_for(1ms);
                }
                FAIL("Actual Metal drawable unavailable within explicit fixture deadline.");
            }

            /** @brief Checks exact compute-output/direct-draw resource identity before native execution. */
            void CheckCandidate(const VfxGpuGraphSchedule &schedule, const GpuVfxRenderBatch &batch) {
                REQUIRE(batch.pending);
                REQUIRE(schedule.dispatched);
                REQUIRE(schedule.Workloads().size() == 2);
                const auto &compute = std::get<RenderGraphVfxGpuCompute>(schedule.records[0].workload);
                const auto &draw = std::get<RenderGraphVfxGpuDraw>(schedule.records[1].workload);
                CHECK(draw.particles == compute.destination);
                CHECK(draw.generation == batch.pending->step.targetGeneration);
                CHECK(compute.work.step == batch.pending->step);
            }

            /** @brief Compiles exact contributed resource states and uses one real frontend-issued graph submission. */
            void Execute(RenderFrameScope &frame, RenderGraphBuilder &builder, const VfxGpuGraphSchedule &contribution,
                         RenderGraphResourceId color, RenderGraphPassRef anchor, RenderGraphPassRef output) {
                const auto graph = Value(builder.Finalize());
                const auto schedule = Value(CompileRenderGraph(graph));
                std::vector<RenderGraphImportedState> initial{{color, {}, RenderGraphInitialContents::Discarded}};
                initial.insert(initial.end(), contribution.InitialStates().begin(), contribution.InitialStates().end());
                const auto sync = Value(SynthesizeRenderGraphSynchronization(graph, schedule, queues_, initial));
                const auto compiled = Value(CompileRenderGraphExecution(graph, schedule, sync, queues_));
                std::vector<RenderGraphPassWorkload> workloads;
                for (const auto &pass : compiled.Passes()) {
                    if (pass.pass == anchor)
                        workloads.push_back({anchor, RenderGraphColorAttachment{color, {.clearColor = {0, 0, 0, 0}}}});
                    else if (pass.pass == output)
                        workloads.push_back({output, std::monostate{}});
                    else {
                        const auto found = std::ranges::find(contribution.Workloads(), pass.pass, &RenderGraphPassWorkload::pass);
                        REQUIRE(found != contribution.Workloads().end());
                        workloads.push_back(*found);
                    }
                }
                const auto issued = Value(frame.ReserveSubmission(RenderQueueRole::Graphics, contribution.RequiredWaits()));
                const std::array submissions{RenderGraphQueueSubmission{issued, {0, compiled.Passes().size()}}};
                Accepted(frame.ExecuteSubmittedGraph(compiled, workloads, submissions));
            }

            /** @brief Adds a real native event dependency to test submitted cancellation without invented timeline progress. */
            void HoldCompletion() {
                id<MTLDevice> device = (__bridge id<MTLDevice>)bridge_.Device();
                gate_ = [device newSharedEvent];
                REQUIRE(gate_);
                [commands_ encodeWaitForEvent:gate_ value:1];
            }

            /** @brief Releases only this fixture explicit completion gate. */
            void ReleaseGate() noexcept {
                if (gate_) {
                    gate_.signaledValue = 1;
                    gate_ = nil;
                }
            }

            static void CopyPixel(id<MTLBlitCommandEncoder> blit, id<MTLTexture> texture, id<MTLBuffer> bytes, NSUInteger x, NSUInteger y,
                                  NSUInteger offset) {
                [blit copyFromTexture:texture
                                 sourceSlice:0
                                 sourceLevel:0
                                sourceOrigin:MTLOriginMake(x, y, 0)
                                  sourceSize:MTLSizeMake(1, 1, 1)
                                    toBuffer:bytes
                           destinationOffset:offset
                      destinationBytesPerRow:256
                    destinationBytesPerImage:256];
            }

            /** @brief Compares by stable identity with declared float tolerances and exact discrete fields. */
            void CheckParticle(std::span<const std::byte> bytes, const CpuParticleExtractView &cpu, std::uint32_t index) {
                std::array<std::uint32_t, VfxGpuObservationWords::Count> words;
                std::memcpy(words.data(), bytes.data(), bytes.size());
                const auto id = std::uint64_t{words[0]} | (std::uint64_t{words[1]} << 32);
                REQUIRE(id == Value(oracle_.HandleAtDenseIndex(index)).particle.Value());
                const std::array expected{cpu.positionX[index], cpu.positionY[index],       cpu.positionZ[index], cpu.velocityX[index],
                                          cpu.velocityY[index], cpu.velocityZ[index],       cpu.sizeX[index],     cpu.sizeY[index],
                                          cpu.rotation[index],  cpu.angularVelocity[index], cpu.age[index],       cpu.maximumAge[index]};
                for (std::size_t i = 0; i < expected.size(); ++i) {
                    const auto tolerance = Vfx::Tests::GpuPropertyTolerances[i];
                    INFO("policy=" << Vfx::Tests::GpuQualificationPolicyVersion << " property=" << tolerance.name << " particle=" << index
                                   << " tick=" << step_.tick);
                    CHECK(Vfx::Tests::GpuPropertyMatches(std::bit_cast<float>(words[i + 2]), expected[i], tolerance));
                }
                CHECK(words[VfxGpuObservationWords::PackedColor] == cpu.packedColor[index]);
                CHECK(words[VfxGpuObservationWords::CustomFlags] == cpu.customFlags[index]);
                CHECK(words[VfxGpuObservationWords::Alive] == 1);
                CHECK(words[17] == 0);
                CHECK(words[18] == 0);
                CHECK(words[19] == 0);
            }

            CookedVfxGpuPrograms family_;
            std::span<const CpuParticleForceModule> forces_;
            bool variedSeed_;
            std::uint32_t count_;
            CpuParticleSimulator oracle_;
            SDL_Window *window_{};
            std::unique_ptr<Editor::SdlMetalPresentationPort> presentation_;
            MetalEditorGraphicsBridge bridge_;
            RenderBackendRegistry registry_;
            std::unique_ptr<RenderFrontend> frontend_;
            std::unique_ptr<VfxWorld> world_;
            std::vector<RenderQueueAssignment> queues_;
            RenderTextureHandle color_;
            RenderTextureViewHandle colorView_;
            VfxGpuPreparationReceipt receipt_;
            VfxGpuStepIdentity step_;
            std::uint64_t snapshot_{}, frame_{};
            __strong id<MTLCommandBuffer> commands_{nil}, pixelCommands_{nil};
            __strong id<MTLSharedEvent> gate_{nil};
        };
    }  // namespace

    void RunEquivalentMetalVfxGpu(const std::filesystem::path &root) {
        if (MTLCreateSystemDefaultDevice() == nil)
            SKIP(VfxErrors::UnsupportedCapability.code.Value() << ": no native Metal compute device is available");
        for (std::size_t scenario = 0; scenario < 4; ++scenario) {
            INFO("force scenario=" << scenario);
            const auto forces = scenario < 3 ? std::span{Forces}.subspan(scenario, 1) : std::span{Forces};
            Fixture fixture(root, forces, true);
            fixture.Initialize();
            REQUIRE_FALSE(fixture.Frontend().Capabilities().support.synthetic);
            if (!fixture.Frontend().Capabilities().support.features.Supports(RenderCapability::VfxGpuSimulation))
                SKIP(VfxErrors::UnsupportedCapability.code.Value() << ": selected native device has no qualified VFX compute support");
            fixture.Prepare(true);
            for (unsigned step = 0; step < 12; ++step) {
                fixture.Pump();
                fixture.PrepareStep();
                fixture.Submit();
                fixture.AwaitAccepted();
                fixture.WaitComplete();
                auto result = fixture.Acquire();
                fixture.CheckOracle(result);
            }
        }
    }

    void RunBudgetMetalVfxGpu(const std::filesystem::path &root) {
        if (MTLCreateSystemDefaultDevice() == nil)
            SKIP(VfxErrors::UnsupportedCapability.code.Value() << ": no native Metal compute device is available");
        constexpr std::uint32_t particles = 65'536;
        constexpr unsigned warmup = 8;
        constexpr unsigned samples = 64;
        constexpr double budgetMilliseconds = 16.667;
        constexpr float deltaSeconds = 1.0F / 60.0F;
        Fixture fixture(root, std::span{Forces}.first(3), true, particles);
        fixture.Initialize();
        REQUIRE_FALSE(fixture.Frontend().Capabilities().support.synthetic);
        if (!fixture.Frontend().Capabilities().support.features.Supports(RenderCapability::VfxGpuSimulation))
            SKIP(VfxErrors::UnsupportedCapability.code.Value() << ": selected native device has no qualified VFX compute support");
        fixture.Prepare(false);
        const auto device = fixture.DeviceName();
        std::array<double, samples> milliseconds{};
        for (unsigned step = 0; step < warmup + samples; ++step) {
            const auto started = std::chrono::steady_clock::now();
            fixture.PrepareStep(deltaSeconds);
            fixture.Submit();
            fixture.AwaitAccepted();
            fixture.WaitComplete();
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            if (step >= warmup)
                milliseconds[step - warmup] = elapsed;
        }
        const auto diagnostics = Value(fixture.Frontend().GpuReadbackDiagnostics());
        CHECK(diagnostics.preparedCpuBytes == 0);
        CHECK(diagnostics.reservedSlots == 0);
        CHECK(diagnostics.observationCopyCalls == 0);
        CHECK(diagnostics.copiedBytes == 0);
        std::ranges::sort(milliseconds);
        const auto p95 = milliseconds[60];
        std::cout << "VFX budgetPolicy=1 backend=metal device=" << device << " particles=" << particles
                  << " forces=gravity,wind,attraction extent=64x64 deltaSeconds=" << deltaSeconds << " warmup=" << warmup
                  << " samples=" << samples << " metric=CPU-wall-presentation-compute-draw-completion p50Ms=" << milliseconds[31]
                  << " p95Ms=" << p95 << " maxMs=" << milliseconds.back() << " budgetP95Ms=" << budgetMilliseconds << '\n';
        CHECK(p95 <= budgetMilliseconds);
    }

    void RunDisabledMetalVfxGpu(const std::filesystem::path &root) {
        Fixture fixture(root);
        fixture.Initialize();
        fixture.Prepare(false);
        fixture.Submit(false);
        fixture.WaitComplete();
        fixture.PrepareStep();
        fixture.Submit();
        fixture.AwaitAccepted();
        fixture.WaitComplete();
        CHECK(fixture.Capture().GpuBatches()[0].generation == 2);
        fixture.Submit(false);
        fixture.WaitComplete();
        const auto diagnostics = Value(fixture.Frontend().GpuReadbackDiagnostics());
        CHECK(diagnostics.preparedCpuBytes == 0);
        CHECK(diagnostics.observationCopyCalls == 0);
        CHECK(diagnostics.copiedBytes == 0);
        CHECK(diagnostics.observationDiscardCalls == 0);
        RenderReadbackResult absent;
        CHECK(fixture.Frontend().AcquireVfxGpuObservation(fixture.Receipt(), fixture.Step(), absent) ==
              RenderReadbackPreparedStatus::InvalidRequest);
        fixture.CheckPixels();
        fixture.StopRejectsSnapshot();
    }

    void RunObservedMetalVfxGpu(const std::filesystem::path &root, VfxGpuObservationCase mode) {
        Fixture fixture(root);
        fixture.Initialize();
        fixture.Prepare(true, mode == VfxGpuObservationCase::TimedOut ? 1ms : 5s);
        fixture.PrepareStep();
        fixture.Submit(true, true);
        fixture.AwaitAccepted();
        RenderReadbackResult pending;
        const auto pendingStatus = fixture.Frontend().AcquireVfxGpuObservation(fixture.Receipt(), fixture.Step(), pending);
        if (mode == VfxGpuObservationCase::TimedOut)
            CHECK(
                (pendingStatus == RenderReadbackPreparedStatus::ResultPending || pendingStatus == RenderReadbackPreparedStatus::TimedOut));
        else
            CHECK(pendingStatus == RenderReadbackPreparedStatus::ResultPending);
        if (mode == VfxGpuObservationCase::Cancelled)
            CHECK(fixture.Frontend().CancelVfxGpuObservation(fixture.Receipt(), fixture.Step()) == RenderReadbackPreparedStatus::Complete);
        if (mode == VfxGpuObservationCase::TimedOut) {
            std::this_thread::sleep_for(5ms);
            fixture.Pump();
        }
        if (mode != VfxGpuObservationCase::LeaseBackpressure) {
            const auto held = Value(fixture.Frontend().GpuReadbackDiagnostics());
            CHECK(held.queue.pendingBytes == Count * VfxGpuObservationWords::Bytes);
            CHECK(held.observationCopyCalls == 0);
        }
        fixture.WaitComplete();
        if (mode != VfxGpuObservationCase::LeaseBackpressure) {
            const auto diagnostics = Value(fixture.Frontend().GpuReadbackDiagnostics());
            CHECK(diagnostics.observationCopyCalls == 0);
            CHECK(diagnostics.copiedBytes == 0);
            CHECK(diagnostics.observationDiscardCalls >= 1);
            return;
        }
        auto lease = fixture.Acquire();
        fixture.CheckOracle(lease);
        fixture.CheckBackpressure();
        lease = RenderReadbackResult{};
        fixture.Pump();
        fixture.Submit();
        fixture.AwaitAccepted();
        fixture.WaitComplete();
        auto next = fixture.Acquire();
        fixture.CheckOracle(next);
        const auto diagnostics = Value(fixture.Frontend().GpuReadbackDiagnostics());
        CHECK(diagnostics.observationCopyCalls == 2);
        CHECK(diagnostics.copiedBytes == 2 * Count * VfxGpuObservationWords::Bytes);
        fixture.CheckPixels();
    }
}  // namespace Horo::Render::Test
