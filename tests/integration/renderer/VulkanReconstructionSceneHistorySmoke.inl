/** @file VulkanReconstructionSceneHistorySmoke.inl @brief Exact scene history through the installed Vulkan graph path. */
namespace ReconstructionSmoke {
    /** @brief Cold-provisions one whole native image identity for the actual scene history owner. */
    NativeReconstructionSceneImage SceneImage(RenderFrontend &frontend, Image role) {
        const auto [texture, view] = PrepareImage(frontend, ImageDescriptor(role));
        return {texture, view};
    }

    /** @brief Uses the normal world origin transaction and scene capture for view-history cache identity. */
    class SceneOrigin final {
    public:
        explicit SceneOrigin(RenderViewId view)
            : initial_(
                  Take(WorldStreaming::OriginFrame::Create({Id<WorldStreaming::OriginFrameId>(), Id<WorldStreaming::OriginFrameRevision>(),
                                                            Id<WorldStreaming::OriginGeneration>()},
                                                           {}))),
              origin_(Take(WorldStreaming::OriginFrameOwner::Create(initial_))),
              adapter_(Take(RenderOriginSceneAdapter::Create(*origin_, view, requirement_, {2, 1, 1}))) {}

        TemporalHistoryCompatibility Stamp(TemporalHistoryCompatibility current) {
            const RenderOriginSceneSource source{{{}, Math::WorldCoordinate64::FromMillimeters(0, 0, -1000)}, {}, {}, 1};
            return Take(Take(adapter_->Capture(source)).StampHistory(current));
        }

        void Rebase() {
            using namespace WorldStreaming;
            const auto policy = Take(OriginShiftPolicy::Create({.id = Id<OriginShiftPolicyId>(),
                                                                .revision = Id<OriginShiftPolicyRevision>(),
                                                                .frame = initial_.Binding().identity,
                                                                .mode = OriginShiftRuntimeMode::EditorPreview}));
            const auto marker = Math::WorldCoordinate64::FromMillimeters(2'000'000, 0, 0);
            const auto decision =
                Take(EvaluateOriginShift(policy, {policy.Id(), policy.Revision(), initial_, OriginShiftPolicyState::Active},
                                         {Id<OriginShiftRequestId>(), OriginShiftRequester::Editor, marker}));
            REQUIRE(decision.kind == OriginShiftDecisionKind::RequestShift);
            const std::array required{requirement_};
            const std::array participants{static_cast<IOriginRebaseParticipant *>(adapter_.get())};
            auto transaction = Take(
                OriginRebaseTransaction::Prepare({Id<OriginRebaseTransactionId>(), decision, initial_, 1, OriginRebaseLifecycle::Active},
                                                 required, participants));
            REQUIRE(transaction.Commit(*origin_, OriginRebaseCommitPoint::PostSimulation, OriginRebaseLifecycle::Active).HasValue());
        }

    private:
        template <typename Identity> static Identity Id() {
            return Take(Identity::Create(1));
        }

        WorldStreaming::OriginFrame initial_;
        std::unique_ptr<WorldStreaming::OriginFrameOwner> origin_;
        const WorldStreaming::OriginRebaseParticipantRequirement requirement_{Id<WorldStreaming::OriginRebaseParticipantId>(),
                                                                              Id<WorldStreaming::OriginRebaseParticipantRevision>()};
        std::unique_ptr<RenderOriginSceneAdapter> adapter_;
    };

    /** @brief Supplies view history identities that the owning scene cache completes with the actual origin generation. */
    TemporalHistoryCompatibility SceneCompatibility(const RenderFrontend &frontend, RenderViewId view) {
        return {.view = view,
                .provider = {1},
                .mode = {1},
                .renderExtent = Source,
                .targetExtent = Target,
                .providerGeneration = 1,
                .modeGeneration = 1,
                .surfaceGeneration = 1,
                .rasterGeneration = 1,
                .colorGeneration = 1,
                .exposureGeneration = 1,
                .inputSchemaGeneration = 1,
                .deviceGeneration = frontend.Capabilities().support.deviceIncarnation,
                .projectionGeneration = 1,
                .jitterGeneration = 1,
                .motionGeneration = 1,
                .recipeGeneration = 1};
    }

    /** @brief Describes the native outputs and actual view-local source images for one deterministic test frame. */
    ReconstructionFrameRequest SceneFrame(TemporalHistoryCompatibility compatibility, std::uint64_t frame, std::uint64_t predecessor,
                                          NativeReconstructionSceneImage color, NativeReconstructionSceneImage depth,
                                          NativeReconstructionSceneImage motion) {
        const auto input = [&](NativeReconstructionSceneImage image, Image role) {
            return ReconstructionTextureInput{image.texture, ImageDescriptor(role), compatibility.view, frame};
        };
        return {.compatibility = compatibility,
                .resourceOwner = color.texture.owner,
                .timing = {frame, predecessor, frame, std::chrono::milliseconds{16}, 0},
                .color = input(color, Image::CurrentColor),
                .positiveLinearDepth = input(depth, Image::CurrentDepth),
                .motion = input(motion, Image::Motion),
                .output = {.descriptor = ImageDescriptor(Image::OutputColor), .writeUsage = RenderGraphUsageKind::Storage}};
    }

    /** @brief Records owner-bound Vulkan work on the real frontend path using published preserved image state. */
    RenderTimelinePoint RecordSceneFrame(RenderFrameScope &frame, std::span<const RenderQueueAssignment> queues,
                                         const NativeReconstructionScenePlan &plan,
                                         const std::shared_ptr<IResidentNativeReconstructionKernel> &kernel,
                                         const std::shared_ptr<NativeReconstructionViewHistory> &history, bool checkWrongView = false) {
        constexpr auto images = RenderGraphNativeReconstruction::ImageCount;
        auto builder = Take(RenderGraphBuilder::Create(
            {.maxPasses = images + 1, .maxResources = images, .maxUsages = images * 2, .maxDependencies = images * 2}));
        std::vector<RenderGraphPassWorkload> workloads;
        std::vector<RenderGraphImportedState> initial;
        RenderGraphNativeReconstruction workload{.dispatch = plan.dispatch, .kernel = kernel};
        for (std::size_t slot = 0; slot < images; ++slot) {
            const auto role = static_cast<Image>(slot);
            if (!NativeReconstructionUsesImage(plan.dispatch.entry, role))
                continue;
            const auto previous = role == Image::PreviousColor || role == Image::PreviousDepth;
            const auto written = NativeReconstructionWritesImage(role);
            const auto image =
                Take(builder.ImportTexture(plan.images[slot].texture,
                                           previous || written ? RenderGraphResourceClass::History : RenderGraphResourceClass::Persistent));
            workload.images[slot] = {image, plan.images[slot].view};
            if (previous) {
                REQUIRE(plan.previousStates.has_value());
                const auto &state = role == Image::PreviousColor ? plan.previousStates->color : plan.previousStates->depth;
                initial.push_back({image, state, RenderGraphInitialContents::Preserved});
            } else {
                initial.push_back({image, {}, RenderGraphInitialContents::Discarded});
            }
            if (previous || written)
                continue;
            const auto clear = Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
            REQUIRE(builder.AddUsage({clear, image, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}).HasValue());
            workloads.push_back({clear, RenderGraphColorAttachment{image, {.clearColor = ImageContents(role)}}});
        }
        const auto reconstruct = Take(builder.AddPass(RenderPassKind::Compute, RenderQueueRole::Compute));
        for (std::size_t slot = 0; slot < images; ++slot) {
            const auto image = workload.images[slot].resource;
            if (!image.IsValid())
                continue;
            const auto written = NativeReconstructionWritesImage(static_cast<Image>(slot));
            REQUIRE(builder
                        .AddUsage({reconstruct, image, written ? RenderGraphAccess::Write : RenderGraphAccess::Read,
                                   written ? RenderGraphUsageKind::Storage : RenderGraphUsageKind::Sampled})
                        .HasValue());
        }
        workloads.push_back({reconstruct, workload});
        const auto graph = Take(builder.Finalize());
        const auto schedule = Take(CompileRenderGraph(graph));
        const auto synchronization = Take(SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial));
        const auto compiled = Take(CompileRenderGraphExecution(graph, schedule, synchronization, queues));
        const auto submit = Take(frame.ReserveSubmission(RenderQueueRole::Graphics, {}));
        const std::array submissions{RenderGraphQueueSubmission{submit, {0, workloads.size()}}};
        const std::array histories{history};
        if (checkWrongView) {
            auto &bad = std::get<RenderGraphNativeReconstruction>(workloads.back().workload);
            bad.images[static_cast<std::size_t>(Image::OutputColor)].view = plan.images[static_cast<std::size_t>(Image::CurrentColor)].view;
            const auto rejected = frame.ExecuteNativeReconstructionScene(compiled, workloads, submissions, histories);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == TemporalHistoryErrors::InvalidFrame.code.Value());
            workloads.back().workload = workload;
        }
        const auto recorded = frame.ExecuteNativeReconstructionScene(compiled, workloads, submissions, histories);
        INFO((recorded.HasError() ? recorded.ErrorValue().message : "scene reconstruction recorded"));
        REQUIRE(recorded.HasValue());
        return submit.signal;
    }

}  // namespace ReconstructionSmoke

TEST_CASE("Vulkan scene history publishes only from its retained completed frame", "[integration][renderer][vulkan][gpu][reconstruction]") {
    using namespace ReconstructionSmoke;
    const std::array shaders{CookScene(Entry::TemporalSeed).reconstruction, CookScene(Entry::TemporalHistory).reconstruction};
    Owner owner;
    REQUIRE(RegisterVulkanRenderBackend(owner.registry, owner.observed).HasValue());
    REQUIRE(owner.registry.Seal().HasValue());
    RenderBackendConfig config{.requirePresentation = false, .enableValidation = true};
    config.queueAssignments = {{RenderQueueRole::Graphics, {1}}, {RenderQueueRole::Compute, {1}}, {RenderQueueRole::Transfer, {1}}};
    owner.frontend = Take(RenderFrontend::Create(owner.registry, RenderBackendId{"vulkan"}, config));
    auto &frontend = *owner.frontend;
    auto declared = Take(DescribeNativeReconstructionImageProvider(ReconstructionKind::Temporal, shaders));
    const auto route = declared.route;
    const std::array cookKeys{shaders[0].artifact.artifactKey, shaders[1].artifact.artifactKey};
    const std::array inventory{ImageDescriptor(Image::CurrentColor), ImageDescriptor(Image::CurrentDepth), ImageDescriptor(Image::Motion),
                               ImageDescriptor(Image::OutputColor), ImageDescriptor(Image::OutputDepth)};
    ImageProviderRegistry providers;
    REQUIRE(providers.Register(std::move(declared)).HasValue());
    providers.Seal();
    const ImageProviderEvidence evidence{.hostApi = {1, 0}, .admittedCookedArtifacts = cookKeys, .textures = inventory};
    const auto available = Take(frontend.DiscoverImageProvider(providers, route, evidence));
    REQUIRE(available.installation == ImageProviderInstallStatus::Linked);
    REQUIRE(available.readiness == ImageProviderReadiness::Ready);
    const auto seed = Take(frontend.RealizeNativeReconstructionKernel(shaders[0]));
    const auto temporal = Take(frontend.RealizeNativeReconstructionKernel(shaders[1]));
    const auto color = SceneImage(frontend, Image::CurrentColor);
    const auto depth = SceneImage(frontend, Image::CurrentDepth);
    const auto motion = SceneImage(frontend, Image::Motion);
    const NativeReconstructionSceneViews views{color.view, depth.view, motion.view};
    const NativeReconstructionHistoryImages firstPair{SceneImage(frontend, Image::OutputColor), SceneImage(frontend, Image::OutputDepth)};
    const NativeReconstructionHistoryImages secondPair{SceneImage(frontend, Image::OutputColor), SceneImage(frontend, Image::OutputDepth)};
    const NativeReconstructionViewHistory::ImagePairs pairs{firstPair, secondPair};
    const auto view = Take(frontend.CreateRenderViewIdentity());
    const auto recipe = SceneCompatibility(frontend, view);
    SceneOrigin origin{view};
    const auto compatibility = origin.Stamp(recipe);
    const auto history = Take(NativeReconstructionViewHistory::Create(frontend, compatibility, pairs));
    const NativeReconstructionTemporalSettings tuning{.historyWeight = 0.75F,
                                                      .absoluteDepthTolerance = 0.01F,
                                                      .relativeDepthTolerance = 0.02F};

    const auto first = Take(history->Stage(frontend, SceneFrame(compatibility, 1, 0, color, depth, motion), views, tuning));
    REQUIRE(first.dispatch.entry == Entry::TemporalSeed);
    auto frame = Take(frontend.BeginFrame({.frameNumber = 1, .outputExtent = Target}));
    const auto firstSignal = RecordSceneFrame(frame, config.queueAssignments, first, seed, history, true);
    CHECK_FALSE(Take(history->Poll(frontend, compatibility)));
    CHECK_FALSE(history->Published(compatibility).has_value());
    auto moved = std::move(frame);
    REQUIRE(moved.Present().HasValue());
    CHECK_FALSE(history->Published(compatibility).has_value());
    RequireCompletion(frontend, firstSignal);
    REQUIRE(Take(history->Poll(frontend, compatibility)));
    const auto firstPublished = history->Published(compatibility);
    REQUIRE(firstPublished.has_value());
    CHECK(firstPublished->frameId == 1);
    CHECK(firstPublished->images.color.texture == firstPair.color.texture);
    CHECK(firstPublished->images.depth.texture == firstPair.depth.texture);

    const auto cancelled = Take(history->Stage(frontend, SceneFrame(compatibility, 2, 1, color, depth, motion), views, tuning));
    REQUIRE(cancelled.dispatch.entry == Entry::TemporalHistory);
    REQUIRE(cancelled.previousStates.has_value());
    CHECK(cancelled.images[static_cast<std::size_t>(Image::OutputColor)].texture == secondPair.color.texture);
    auto second = Take(frontend.BeginFrame({.frameNumber = 2, .outputExtent = Target}));
    static_cast<void>(RecordSceneFrame(second, config.queueAssignments, cancelled, temporal, history));
    second.Cancel();
    CHECK_FALSE(Take(history->Poll(frontend, compatibility)));
    REQUIRE(history->Published(compatibility).has_value());
    CHECK(history->Published(compatibility)->frameId == 1);

    const auto next = Take(history->Stage(frontend, SceneFrame(compatibility, 3, 1, color, depth, motion), views, tuning));
    REQUIRE(next.dispatch.entry == Entry::TemporalHistory);
    auto third = Take(frontend.BeginFrame({.frameNumber = 3, .outputExtent = Target}));
    const auto nextSignal = RecordSceneFrame(third, config.queueAssignments, next, temporal, history);
    REQUIRE(third.Present().HasValue());
    RequireCompletion(frontend, nextSignal);
    REQUIRE(Take(history->Poll(frontend, compatibility)));
    const auto secondPublished = history->Published(compatibility);
    REQUIRE(secondPublished.has_value());
    CHECK(secondPublished->frameId == 3);
    CHECK(secondPublished->images.color.texture == secondPair.color.texture);
    CHECK(secondPublished->generation > firstPublished->generation);

    const auto stale = Take(history->Stage(frontend, SceneFrame(compatibility, 4, 3, color, depth, motion), views, tuning));
    auto fourth = Take(frontend.BeginFrame({.frameNumber = 4, .outputExtent = Target}));
    const auto staleSignal = RecordSceneFrame(fourth, config.queueAssignments, stale, temporal, history);
    REQUIRE(fourth.Present().HasValue());
    origin.Rebase();
    const auto nextOrigin = origin.Stamp(recipe);
    REQUIRE(nextOrigin.sceneOriginGeneration != compatibility.sceneOriginGeneration);
    RequireCompletion(frontend, staleSignal);
    const auto rejected = history->Poll(frontend, nextOrigin);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == TemporalHistoryErrors::ResetRequired.code.Value());
    CHECK_FALSE(history->Published(nextOrigin).has_value());
    CHECK_FALSE(history->Published(compatibility).has_value());

    const auto replacement = history->Reset(frontend, nextOrigin, pairs, TemporalHistoryResetCause::SceneDiscontinuity);
    REQUIRE(replacement.HasValue());
    const auto afterReset = Take(history->Stage(frontend, SceneFrame(nextOrigin, 5, 0, color, depth, motion), views, tuning));
    CHECK(afterReset.dispatch.entry == Entry::TemporalSeed);
    REQUIRE(history->CancelStaged().HasValue());
    Test::RequireDrainedOwner(frontend);
}
