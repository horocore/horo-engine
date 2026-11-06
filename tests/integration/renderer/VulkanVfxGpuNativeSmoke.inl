/** @file VulkanVfxGpuNativeSmoke.inl @brief Genuine registered Vulkan PreparedParticles execution; portable fixture is actual Mac crosscook
 * evidence only. */
namespace VulkanVfxGpuSmoke {
    using namespace Horo::Vfx;
    using namespace std::chrono_literals;
    constexpr std::uint32_t Count = 8;
    constexpr float Delta = .1F;
    constexpr FramebufferExtent Extent{64, 64};

    template <class T> T Value(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Actual operation accepted."));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    void Accepted(const Result<void> &result) {
        INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Actual operation accepted."));
        REQUIRE(result.HasValue());
    }

    std::vector<std::uint8_t> Read(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        REQUIRE(input.good());
        const auto size = input.tellg();
        REQUIRE(size > 0);
        REQUIRE(size <= 16 * 1024 * 1024);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(input.good());
        return bytes;
    }

    void Hash(const std::filesystem::path &path, std::string_view expected) {
        const auto bytes = Read(path);
        CHECK(FormatSha256(ComputeSha256(std::as_bytes(std::span{bytes}))) == expected);
    }

    std::filesystem::path Package() {
        const char *configured = std::getenv("HORO_VFX_GPU_COOKED_ROOT");
        if (configured && *configured)
            return configured;
#ifdef HORO_VFX_GPU_VULKAN_FIXTURE_PATH
        return HORO_VFX_GPU_VULKAN_FIXTURE_PATH;
#else
        return {};
#endif
    }

    void GenuineFiles(const std::filesystem::path &root) {
        const std::array<std::pair<const char *, const char *>, 7> expected{
            {{"shader-host-catalog.json", "sha256:7531a5d6c5273218bfaf27a90bd0c77bc8a767f8fefd3f938ef36c450576d313"},
             {"shader-host-catalog.signature.json", "sha256:dfd6bd9e9784b434c428fa615d69c76cb6b16720201452d39658acd70266a688"},
             {"shaders/runtime/vfx-gpu-draw-vulkan.horoshdr", "sha256:5bae90d1ef542f2bb90a524fb56826e015b1093d26383ebf9752fe96f0603859"},
             {"shaders/runtime/vfx-gpu-draw-vulkan.reflection.json",
              "sha256:701902b9b66120b5d9c6a1cf018802cf5bbb28894a8a08fa3b3a2e64f1c0bea9"},
             {"shaders/runtime/vfx-simulation-vulkan.horoshdr", "sha256:a7c4dffd59c1d371bfa92c16d7700c022b0525d675662db39df35d21d0589256"},
             {"shaders/runtime/vfx-simulation-vulkan.reflection.json",
              "sha256:93e3f379b09a2d6b58acf91256f724bee6883c53342859bc6ed6de758c17851c"},
             {"vfx-gpu-vulkan-cook-provenance.json", "sha256:b9c067ebd11289c857671059ad92637ba7735f1a886c1bd46f695a2019f9e862"}}};
        for (const auto &[path, digest] : expected)
            Hash(root / path, digest);
        Hash(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/shaders/runtime/vfx_simulation.hlsl",
             "sha256:8e57a76ccfdfd633ff7157e8bc86d8f7168f4573b39dd9019e1e9c27bb98b7ba");
        Hash(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/shaders/runtime/vfx_gpu_draw.hlsl",
             "sha256:dd36b30f3d5ae49a47805b2a75dce0c7ceaa277668641f89f3872e9d97d8e6d0");
    }

    CookedVfxGpuProgram Load(const std::filesystem::path &root, VfxGpuProgramRole role, std::string_view prefix) {
        const auto target = Horo::Editor::Offline::VfxGpuVulkanCookTarget();
        auto manifest = MakeVfxGpuShaderManifest(role, target);
        auto payload = Read(root / (std::string{prefix} + ".horoshdr"));
        const auto metadata = Read(root / (std::string{prefix} + ".reflection.json"));
        auto bound = Value(DecodeReflectedShaderArtifact(manifest, target, payload, metadata));
        CompiledShaderArtifact artifact{.backend = target.backend,
                                        .payloadFormat = target.payloadFormat,
                                        .artifactKey = bound.compiledArtifactKey,
                                        .payload = std::move(payload),
                                        .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(bound))};
        const auto &r = artifact.reflection->reflection;
        ShaderReflectionCandidate candidate{r.backend,        r.interfaceSchemaVersion, r.bindings, r.parameters,
                                            r.stageInterface, r.targetBindings,         r.sourceMap};
        return {std::move(manifest), target, std::move(artifact), std::move(candidate)};
    }

    CookedVfxGpuPrograms Family(const std::filesystem::path &root) {
        GenuineFiles(root);
        const auto prefixes = Horo::Editor::Offline::VfxGpuVulkanCookPrefixes();
        CookedVfxGpuPrograms result{Load(root, VfxGpuProgramRole::Simulation, prefixes[0]),
                                    Load(root, VfxGpuProgramRole::Draw, prefixes[1])};
        Accepted(ValidateCookedVfxGpuPrograms(result, ShaderTargetBackend::Vulkan));
        return result;
    }

    /** @brief Authors source readback policy before canonical asset validation; CPU oracle calls retain disabled defaults. */
    ParticleSystemDescriptor Source(SimulationPreference preference, VfxGpuReadbackDescriptor readback = {}) {
        auto d = Vfx::Tests::ValidParticleDescriptorData();
        d.maximumParticles = Count;
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
        return Vfx::Tests::ParticleDescriptor(d);
    }

    /** @brief Requires the actual authored policy to survive both logical and real Vulkan preparation unchanged. */
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

    const std::array
        Forces{CpuParticleForceModule{.kind = CpuParticleForceKind::Gravity, .vector = {0, -.1F, 0}},
               CpuParticleForceModule{.kind = CpuParticleForceKind::Wind, .vector = {1, 0, 0}, .strength = .1F},
               CpuParticleForceModule{.kind = CpuParticleForceKind::Attraction, .center = {0, .5F, 0}, .strength = .05F, .falloff = .5F},
               CpuParticleForceModule{.kind = CpuParticleForceKind::Noise, .strength = .05F, .frequency = 2, .randomChannel = 19}};

    /** @brief Provides authored logical fixture identity, never a native submission identity. */
    EffectSystemId Effect() {
        return {VfxIdentityScope::Create(42).Value(), 2, 1};
    }

    /** @brief Provides authored logical buffer identity independently of renderer-issued resource handles. */
    ParticleBufferId Buffer() {
        return {Effect().scope, 5, 1};
    }

    /** @brief Creates the genuine canonical CPU oracle; it never produces runtime GPU output. */
    CpuParticleSimulator Cpu() {
        return Value(CpuParticleSimulator::Create(Source(SimulationPreference::RequireCPU), {.buffer = Buffer(),
                                                                                             .activation = Effect(),
                                                                                             .effectSeed = 0x123456789abcdef0ULL,
                                                                                             .maximumBurstParticles = Count,
                                                                                             .forces = Forces}));
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
    VfxResolution Resolution(const RenderFrontend &frontend) {
        const auto &support = frontend.Capabilities().support;
        REQUIRE(support.features.Supports(RenderCapability::VfxGpuSimulation));
        REQUIRE(support.features.Supports(RenderCapability::VfxGpuDraw));
        std::array<VfxCapabilityFact, VfxCapabilityCount> facts;
        for (std::size_t i = 0; i < facts.size(); ++i)
            facts[i] = {static_cast<VfxCapability>(i), VfxCapabilitySupport::Unsupported};
        facts[static_cast<std::size_t>(VfxCapability::CpuSimulation)].support = VfxCapabilitySupport::Available;
        facts[static_cast<std::size_t>(VfxCapability::GpuSimulation)].support = VfxCapabilitySupport::Available;
        const VfxResourceLimits limits{Count, 0, 0, 0, 8U * 1024U * 1024U, 10, 10};
        const auto caps = Value(VfxCapabilities::Create(Value(VfxCapabilityRevision::Create(support.deviceIncarnation)), facts, limits));
        // Product policy requires positive ceilings for every category. Actual capability counts stay zero and
        // unsupported above, so a policy ceiling never invents usable decals, lights or volumes.
        const VfxResourceLimits policyLimits{Count, 1, 1, 1, 8U * 1024U * 1024U, 10, 10};
        const auto policy =
            Value(VfxQualityPolicy::Create({Value(VfxQualityPolicyRevision::Create(1)), VfxQualityProfile::Standard, policyLimits, 1}));
        VfxResolutionRequest request{.hostMode = VfxHostMode::Interactive,
                                     .requestedProfile = VfxQualityProfile::Standard,
                                     .expectedCapabilityRevision = caps.Revision(),
                                     .expectedPolicyRevision = policy.Revision(),
                                     .requirements = {.category = VfxEffectCategory::Particle,
                                                      .preference = SimulationPreference::RequireGPU,
                                                      .requirementClass = VfxRequirementClass::Cosmetic,
                                                      .requestedCount = Count,
                                                      .minimumAuthoredCount = Count,
                                                      .bytesPerElement = 1024U * 1024U,
                                                      .cpuWorkMilliseconds = 1,
                                                      .gpuWorkMilliseconds = 1,
                                                      .hasCpuKernel = true,
                                                      .hasGpuKernel = true}};
        request.requirements.requiredGpuCapabilities[static_cast<std::size_t>(VfxCapability::GpuSimulation)] = true;
        return Value(ResolveSimulationDomain(caps, policy, request));
    }

    class Unit final {
    public:
        Unit(NativeOwner &owner, const std::filesystem::path &root) : owner_(owner), family_(Family(root)), oracle_(Cpu()) {}

        ~Unit() {
            if (world_)
                (void)world_->Shutdown();
            world_.reset();
            if (receipt_.IsLive())
                (void)owner_.frontend->ReleaseVfxGpuSimulation(receipt_);
            if (color_.IsValid())
                (void)owner_.frontend->ReleaseTexture(color_);
        }

        /** @brief Prepares real Vulkan execution exclusively from the validated asset's authored observation policy. */
        void Prepare() {
            auto &frontend = *owner_.frontend;
            const auto bytes = Count * VfxGpuObservationWords::Bytes;
            Value(frontend.PrepareVfxGpuReadbackStorage({bytes, bytes, bytes, 16, 1}, {1, bytes, 16}, 1024U * 1024U, 5s));
            auto seed = Cpu();
            Value(seed.Advance({.deltaSeconds = 0, .burstCount = Count, .tick = 0}));
            const auto view = Value(seed.Extract());
            std::array<ParticleSimulationId, Count> ids;
            for (std::uint32_t i = 0; i < Count; ++i)
                ids[i] = Value(seed.HandleAtDenseIndex(i)).particle;
            GpuParticleSimulatorCreateInfo info;
            info.scene = Effect().scope;
            info.activation = Effect();
            info.buffer = Buffer();
            info.effectSeed = 0x123456789abcdef0ULL;
            info.resolution = Resolution(frontend);
            info.initialization = GpuParticleInitialization::PreparedParticles;
            info.requiredFeatures = GpuParticleRequiredFeature::None;
            info.initial = {SeedView(view), ids};
            info.forces = Forces;
            info.layoutLimits = {1024U * 1024U, frontend.Capabilities().support.limits.maxBufferBytes};
            info.maximumRetainedBytes = 6U * 1024U * 1024U;
            info.maximumPendingSteps = 2;
            info.maximumSteps = 16;
            info.maximumDeltaSeconds = Delta;
            const VfxGpuReadbackDescriptor
                policy{true, 1,     VfxGpuReadbackPurpose::DiagnosticsCapture, VfxGpuReadbackRequirement::RequiredForVisualEffect,
                       1,    Count, Count * VfxGpuObservationWords::Bytes,     1};
            const auto source = Source(SimulationPreference::RequireGPU, policy);
            auto logical = Value(GpuParticleSimulator::Create(source, info));
            CheckReadbackPolicy(logical->Preparation().descriptor.readback, source.Data().gpuReadback);
            PrepareNative(*logical);
            CheckReadbackPolicy(receipt_.Descriptor().readback, source.Data().gpuReadback);
            CHECK(receipt_.Readback() == VfxGpuReadbackAdmission::Available);
            Accepted(logical->Activate(receipt_));
            std::array<VfxGpuEmitterPreparation, 1> gpu;
            gpu[0] = {Effect(), source.Data().emitter, std::move(logical)};
            world_ = Value(VfxWorld::Prepare(Effect().scope, {}, gpu, {4, Count, 4, 4, 1024U * 1024U}));
            Accepted(world_->Play(Effect()));
            auto color = Value(frontend.CreateTexture(
                {.extent = Extent, .format = RenderTextureFormat::Rgba8Unorm, .usage = RenderTextureUsage::RenderAttachment}));
            color_ = color.handle;
            Value(frontend.ProcessResourceRequests());
            Accepted(frontend.ResourceOperationResult(color.operation));
        }

        void PrepareNative(GpuParticleSimulator &logical) {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            for (;;) {
                auto prepared = owner_.frontend->PrepareVfxGpuSimulation(logical.Preparation(), family_, logical.LifetimeCancellation());
                if (prepared.HasValue()) {
                    receipt_ = std::move(prepared).Value();
                    return;
                }
                INFO(prepared.ErrorValue().code.Value() + ": " + prepared.ErrorValue().message);
                REQUIRE(prepared.ErrorValue().code.Value() == VfxFrameErrors::Pending.code.Value());
                Value(owner_.frontend->ProcessResourceRequests());
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(1ms);
            }
        }

        RenderWorldSnapshot Capture() {
            return world_->Extract(++snapshot_, {}, 1).snapshot;
        }

        void Step(bool partialFailure = false) {
            // Retire the prior acquired CPU result after its consumer lease was released, outside a live frame.
            Pump();
            REQUIRE(world_->PrepareGpuSteps(Delta).status == GpuParticleStepStatus::Complete);
            auto snapshot = Capture();
            REQUIRE(snapshot.GpuBatches().size() == 1);
            const auto &batch = snapshot.GpuBatches()[0];
            REQUIRE(batch.pending);
            step_ = batch.pending->step;
            INFO("Actual candidate tick=" << step_.tick << " generation=" << step_.sourceGeneration << "->" << step_.targetGeneration);
            INFO("Operation: real Vulkan BeginFrame before compute and draw encoding.");
            auto frame = Value(owner_.frontend->BeginFrame({.frameNumber = ++frame_, .outputExtent = Extent}));
            auto builder = Value(RenderGraphBuilder::Create({.maxPasses = 8, .maxResources = 8, .maxUsages = 16, .maxDependencies = 16}));
            const auto color = Value(builder.ImportTexture(color_, RenderGraphResourceClass::Persistent));
            const auto anchor = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
            const auto output = Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
            Accepted(builder.AddUsage({anchor, color, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}));
            VfxGpuGraphRequest request{.preparation = batch.preparation,
                                       .generation = batch.generation,
                                       .pending = batch.pending,
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
            INFO((contribution.HasError() ? contribution.ErrorValue().code.Value() + ": " + contribution.ErrorValue().message
                                          : "Complete canonical GPU graph contribution."));
            REQUIRE(contribution.HasValue());
            REQUIRE(contribution.Value().dispatched);
            const auto &compute = std::get<RenderGraphVfxGpuCompute>(contribution.Value().records[0].workload);
            const auto &draw = std::get<RenderGraphVfxGpuDraw>(contribution.Value().records[1].workload);
            CHECK(draw.particles == compute.destination);
            Execute(frame, builder, contribution.Value(), color, anchor, output, partialFailure);
            Present(frame, partialFailure);
        }

        void RequireFrameAdmissionClosed() {
            INFO("Phase: failed native suffix permanently closes frame admission on this owner; accepted prefix remains observable.");
            const auto next = owner_.frontend->BeginFrame({.frameNumber = ++frame_, .outputExtent = Extent});
            INFO((next.HasError() ? next.ErrorValue().code.Value() + ": " + next.ErrorValue().message
                                  : "Unexpected future frame admission after failed Submit2."));
            REQUIRE(next.HasError());
            CHECK(next.ErrorValue().code.Value() == "render.vulkan.invalid_command_state");
            CHECK(Capture().GpuBatches()[0].generation == 2);
        }

        void Present(RenderFrameScope &frame, bool partialFailure) {
            if (!partialFailure) {
                Accepted(frame.Present());
                return;
            }
            owner_.loader.ArmSecondSubmitFailure();
            const auto presented = frame.Present();
            REQUIRE(presented.HasError());
            CHECK(owner_.loader.SubmitAttempts() == 2);
            CHECK(owner_.loader.ForwardedSubmits() == 1);
            CHECK(owner_.loader.InjectedSubmits() == 1);
        }

        void Execute(RenderFrameScope &frame, RenderGraphBuilder &builder, const VfxGpuGraphSchedule &contribution,
                     RenderGraphResourceId color, RenderGraphPassRef anchor, RenderGraphPassRef output, bool partialFailure) {
            const auto graph = Value(builder.Finalize());
            const auto schedule = Value(CompileRenderGraph(graph));
            std::vector<RenderGraphImportedState> initial{{color, {}, RenderGraphInitialContents::Discarded}};
            initial.insert(initial.end(), contribution.InitialStates().begin(), contribution.InitialStates().end());
            const auto sync = Value(SynthesizeRenderGraphSynchronization(graph, schedule, owner_.selectedQueues, initial));
            const auto compiled = Value(CompileRenderGraphExecution(graph, schedule, sync, owner_.selectedQueues));
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
            Submit(frame, compiled, workloads, contribution, partialFailure);
        }

        void Submit(RenderFrameScope &frame, const CompiledRenderGraphExecution &compiled,
                    std::span<const RenderGraphPassWorkload> workloads, const VfxGpuGraphSchedule &contribution, bool partialFailure) {
            const auto first = Value(frame.ReserveSubmission(RenderQueueRole::Graphics, contribution.RequiredWaits()));
            if (!partialFailure) {
                const std::array submissions{RenderGraphQueueSubmission{first, {0, compiled.Passes().size()}}};
                Accepted(frame.ExecuteSubmittedGraph(compiled, workloads, submissions));
                return;
            }
            const auto draw = std::ranges::find_if(workloads, [](const auto &entry) {
                return std::holds_alternative<RenderGraphVfxGpuDraw>(entry.workload);
            });
            REQUIRE(draw != workloads.end());
            const auto split = static_cast<std::size_t>(draw - workloads.begin());
            REQUIRE(split > 0);
            const std::array waits{first.signal};
            const auto second = Value(frame.ReserveSubmission(RenderQueueRole::Graphics, waits));
            const std::array submissions{RenderGraphQueueSubmission{first, {0, split}},
                                         RenderGraphQueueSubmission{second, {split, compiled.Passes().size() - split}}};
            Accepted(frame.ExecuteSubmittedGraph(compiled, workloads, submissions));
        }

        void Pump() {
            std::array<GpuVfxStepAcknowledgement, 64> acknowledgements;
            const auto count = Value(owner_.frontend->DrainVfxGpuAcknowledgements(acknowledgements));
            for (const auto &ack : std::span{acknowledgements}.first(count)) {
                CHECK((ack.State() == VfxGpuSubmissionState::Accepted || ack.State() == VfxGpuSubmissionState::Completed));
                CHECK(ack.Submission().IsValid());
            }
            if (count) {
                CHECK(world_->ApplyGpuAcknowledgements(std::span{acknowledgements}.first(count)).rejected == 0);
                CHECK(world_->ApplyGpuAcknowledgements(std::span{acknowledgements}.first(count)).processed == 0);
            }
            Value(owner_.frontend->ProcessVfxGpuReadbacks());
        }

        RenderReadbackResult Observe() {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (std::chrono::steady_clock::now() < deadline) {
                Pump();
                RenderReadbackResult result;
                const auto status = owner_.frontend->AcquireVfxGpuObservation(receipt_, step_, result);
                if (status == RenderReadbackPreparedStatus::Complete)
                    return result;
                REQUIRE(status == RenderReadbackPreparedStatus::ResultPending);
                std::this_thread::sleep_for(1ms);
            }
            FAIL("Actual Vulkan GPU observation did not complete before the fixture deadline.");
            return {};
        }

        void CheckParticle(std::span<const std::byte> bytes, const CpuParticleExtractView &cpu, std::uint32_t index) {
            std::array<std::uint32_t, VfxGpuObservationWords::Count> words;
            std::memcpy(words.data(), bytes.data(), bytes.size());
            const auto id = std::uint64_t{words[0]} | (std::uint64_t{words[1]} << 32);
            REQUIRE(id == Value(oracle_.HandleAtDenseIndex(index)).particle.Value());
            const std::array expected{cpu.positionX[index], cpu.positionY[index],       cpu.positionZ[index], cpu.velocityX[index],
                                      cpu.velocityY[index], cpu.velocityZ[index],       cpu.sizeX[index],     cpu.sizeY[index],
                                      cpu.rotation[index],  cpu.angularVelocity[index], cpu.age[index],       cpu.maximumAge[index]};
            for (std::size_t i = 0; i < expected.size(); ++i)
                CHECK(std::bit_cast<float>(words[i + 2]) == Catch::Approx(expected[i]).margin(.00002F).epsilon(.0002F));
            CHECK(words[VfxGpuObservationWords::PackedColor] == cpu.packedColor[index]);
            CHECK(words[VfxGpuObservationWords::CustomFlags] == cpu.customFlags[index]);
            CHECK(words[VfxGpuObservationWords::Alive] == 1);
            CHECK(words[17] == 0);
            CHECK(words[18] == 0);
            CHECK(words[19] == 0);
        }

        void CheckOracle(RenderReadbackResult &result) {
            Value(oracle_.Advance({.deltaSeconds = Delta, .burstCount = step_.tick == 0 ? Count : 0, .tick = step_.tick}));
            const auto cpu = Value(oracle_.Extract());
            REQUIRE(result.Bytes().size() == Count * VfxGpuObservationWords::Bytes);
            for (std::uint32_t i = 0; i < Count; ++i)
                CheckParticle(result.Bytes().subspan(i * VfxGpuObservationWords::Bytes, VfxGpuObservationWords::Bytes), cpu, i);
            CHECK(Capture().GpuBatches()[0].generation == step_.targetGeneration);
        }

    private:
        NativeOwner &owner_;
        CookedVfxGpuPrograms family_;
        CpuParticleSimulator oracle_;
        std::unique_ptr<VfxWorld> world_;
        RenderTextureHandle color_;
        VfxGpuPreparationReceipt receipt_;
        VfxGpuStepIdentity step_;
        std::uint64_t snapshot_{}, frame_{};
    };

    TEST_CASE("Vulkan actual registered backend executes genuine PreparedParticles compute draw and normalized CPU parity",
              "[integration][renderer][vulkan][gpu][vfx]") {
        const auto package = Package();
        if (package.empty() || !std::filesystem::is_directory(package))
            SKIP("Genuine portable Vulkan cooked fixture is absent; no runtime cooking or native substitute.");
        NativeOwner owner;
        Accepted(owner.Initialize(false));
        REQUIRE(owner.frontend->Capabilities().support.features.Supports(RenderCapability::VfxGpuSimulation));
        REQUIRE(owner.frontend->Capabilities().support.features.Supports(RenderCapability::VfxGpuDraw));
        REQUIRE(owner.frontend->Capabilities().support.features.Supports(RenderCapability::VfxGpuReadback));
        {
            Unit unit{owner, package};
            unit.Prepare();
            for (unsigned step = 0; step < 2; ++step) {
                unit.Step();
                auto result = unit.Observe();
                unit.CheckOracle(result);
            }
        }
        Horo::Render::Test::RequireDrainedOwner(*owner.frontend);
    }

    TEST_CASE("Vulkan partial Submit2 preserves accepted compute observations and closes future frame admission",
              "[integration][renderer][vulkan][gpu][vfx][partial-submit]") {
        const auto package = Package();
        if (package.empty() || !std::filesystem::is_directory(package))
            SKIP("Genuine portable Vulkan cooked fixture is absent; no runtime cooking or native substitute.");
        NativeOwner owner;
        Accepted(owner.Initialize(false));
        {
            Unit unit{owner, package};
            unit.Prepare();
            unit.Step(true);
            auto first = unit.Observe();
            unit.CheckOracle(first);
            unit.RequireFrameAdmissionClosed();
        }
        Horo::Render::Test::RequireDrainedOwner(*owner.frontend);
    }
}  // namespace VulkanVfxGpuSmoke
