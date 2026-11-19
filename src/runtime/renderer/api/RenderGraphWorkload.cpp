#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace Horo::Render {
    namespace {
        [[nodiscard]] bool HasSubmissionDependency(const RenderGraphExecutionRequest &request, RenderGraphPassRef before,
                                                   RenderGraphPassRef after);

        /** @brief Bounded native identity and use interval evidence for independent alias validation. */
        struct AliasUse {
            std::uint64_t instance;
            std::size_t first;
            std::size_t last;
            RenderGraphResourceKind kind;
            RenderGraphResourceClass resourceClass;
        };

        /** @brief Matches exactly four distinct whole-buffer storage uses and finite dispatch bounds. */
        [[nodiscard]] bool MatchesLightCulling(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                               const RenderGraphLightCulling &culling) {
            const auto &dispatch = culling.dispatch;
            const LightCullingBudget budget{.maximumLights = std::max(dispatch.referencesPerCluster, std::max(1U, dispatch.lightCount)),
                                            .maximumClusters = dispatch.clusterCount,
                                            .referencesPerCluster = dispatch.referencesPerCluster};
            const auto uses = request.graph.Usages().subspan(pass.usages.offset, pass.usages.count);
            if (pass.kind != RenderPassKind::Compute || !culling.kernel || culling.tableRevision == 0 || !budget.IsValid() ||
                dispatch.reserved != 0 || uses.size() != 4)
                return false;
            const std::array ids{culling.lights, culling.clusters, culling.membership, culling.references};
            for (std::size_t index = 0; index < ids.size(); ++index) {
                if (ids[index].owner != request.graph.Owner() || ids[index].value == 0 ||
                    ids[index].value > request.graph.Resources().size() ||
                    request.graph.Resources()[ids[index].value - 1].kind != RenderGraphResourceKind::Buffer)
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (ids[previous] == ids[index])
                        return false;
                const auto found = std::ranges::find(uses, ids[index], &RenderGraphResourceUsage::resource);
                if (found == uses.end() || found->kind != RenderGraphUsageKind::Storage ||
                    found->access != (index < 2 ? RenderGraphAccess::Read : RenderGraphAccess::Write))
                    return false;
            }
            return true;
        }

        bool MatchesDepth(const RenderGraphExecutionPass &pass, std::span<const RenderGraphResourceUsage> uses,
                          const RenderGraphDepthAttachment &value) {
            using enum RenderGraphAccess;

            return pass.kind == RenderPassKind::Graphics && std::isfinite(value.clearDepth) && value.clearDepth >= 0.0F &&
                   value.clearDepth <= 1.0F && uses.size() == 1 && uses[0].resource == value.texture &&
                   uses[0].kind == RenderGraphUsageKind::DepthStencilAttachment && uses[0].access == Write;
        }

        bool ColorEnvelope(const RenderGraphExecutionPass &pass, std::span<const RenderGraphResourceUsage> uses,
                           const RenderGraphColorAttachment &value) {
            return pass.kind == RenderPassKind::Graphics && value.operations.IsValid() && !value.operations.outputColor &&
                   value.operations.loadOperation != AttachmentLoadOperation::DontCare &&
                   value.operations.storeOperation == AttachmentStoreOperation::Store && uses.size() == 1;
        }

        bool MatchesColor(const RenderGraphExecutionPass &pass, std::span<const RenderGraphResourceUsage> uses,
                          const RenderGraphColorAttachment &value) {
            using enum RenderGraphAccess;

            if (!ColorEnvelope(pass, uses, value))
                return false;
            return uses[0].resource == value.texture && uses[0].kind == RenderGraphUsageKind::ColorAttachment && uses[0].access != Read &&
                   (value.operations.loadOperation != AttachmentLoadOperation::Load || uses[0].access == ReadWrite);
        }

        bool MatchesCopyUse(const RenderGraphResourceUsage &use, RenderGraphResourceId resource, RenderGraphUsageKind kind,
                            RenderGraphAccess access) {
            return use.resource == resource && use.kind == kind && use.access == access;
        }

        bool MatchesCopy(const RenderGraphExecutionPass &pass, std::span<const RenderGraphResourceUsage> uses,
                         const RenderGraphBufferCopy &value) {
            using enum RenderGraphAccess;

            if (pass.kind != RenderPassKind::Copy || uses.size() != 2 || value.byteCount == 0 || value.source == value.destination)
                return false;
            bool source = false;
            bool destination = false;
            for (const auto &use : uses) {
                source |= MatchesCopyUse(use, value.source, RenderGraphUsageKind::CopySource, Read);
                destination |= MatchesCopyUse(use, value.destination, RenderGraphUsageKind::CopyDestination, Write);
            }
            return source && destination;
        }

        /** @brief Resolves only a compiled graph's own persistent resource of the expected kind. */
        bool IsPersistentResource(const CompiledRenderGraphExecution &graph, const RenderGraphResourceId id,
                                  const RenderGraphResourceKind kind) {
            if (id.value == 0 || id.value > graph.Resources().size())
                return false;
            const auto &resource = graph.Resources()[id.value - 1];
            return resource.id == id && resource.kind == kind && resource.resourceClass == RenderGraphResourceClass::Persistent;
        }

        /** @brief Only retained renderer storage can participate in the closed coverage readback workload. */
        bool MatchesStereoCoverageCopy(const CompiledRenderGraphExecution &graph, const RenderGraphExecutionPass &pass,
                                       std::span<const RenderGraphResourceUsage> uses, const RenderGraphStereoCoverageCopy &copy) {
            if (pass.kind != RenderPassKind::Copy || uses.size() != 2 || copy.PackedBytes() == 0)
                return false;
            if (!IsPersistentResource(graph, copy.source, RenderGraphResourceKind::Texture) ||
                !IsPersistentResource(graph, copy.destination, RenderGraphResourceKind::Buffer))
                return false;
            const auto source = std::ranges::find(uses, copy.source, &RenderGraphResourceUsage::resource);
            const auto destination = std::ranges::find(uses, copy.destination, &RenderGraphResourceUsage::resource);
            return source != uses.end() && destination != uses.end() &&
                   MatchesCopyUse(*source, copy.source, RenderGraphUsageKind::CopySource, RenderGraphAccess::Read) &&
                   MatchesCopyUse(*destination, copy.destination, RenderGraphUsageKind::CopyDestination, RenderGraphAccess::Write);
        }

        bool ExternallyValidated(const RenderGraphWorkload &workload) {
            return std::holds_alternative<RenderGraphMaterialRaster>(workload) || std::holds_alternative<RenderGraphMultiview>(workload) ||
                   std::holds_alternative<RenderGraphVfx>(workload) || std::holds_alternative<RenderGraphExposureGpu>(workload) ||
                   std::holds_alternative<RenderGraphTextureUpload>(workload) ||
                   std::holds_alternative<RenderGraphNativeReconstruction>(workload);
        }

        bool MatchesBasic(const RenderGraphExecutionPass &pass, std::span<const RenderGraphResourceUsage> uses,
                          const RenderGraphWorkload &workload) {
            if (const auto *primary = std::get_if<PrimaryOutputAttachment>(&workload))
                return pass.kind == RenderPassKind::Graphics && uses.empty() && primary->IsValid();
            return std::holds_alternative<std::monostate>(workload) && pass.kind == RenderPassKind::Graphics && uses.empty();
        }

        /** @brief Requires each operation to account for exactly its retained semantic uses. */
        [[nodiscard]] bool MatchesWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                           const RenderGraphWorkload &workload) {
            using enum RenderGraphAccess;
            const auto uses = request.graph.Usages().subspan(pass.usages.offset, pass.usages.count);
            if (const auto *paint = std::get_if<RenderGraphUiPaint>(&workload))
                return ValidateUiPaintWorkload(request, pass, *paint).HasValue();
            if (const auto *particles = std::get_if<RenderGraphVfxGpuCompute>(&workload))
                return ValidateVfxGpuComputeGraphWorkload(request, pass, *particles).HasValue();
            if (const auto *particles = std::get_if<RenderGraphVfxGpuDraw>(&workload))
                return ValidateVfxGpuDrawGraphWorkload(request, pass, *particles).HasValue();
            if (ExternallyValidated(workload))
                return true;
            if (const auto *culling = std::get_if<RenderGraphLightCulling>(&workload))
                return MatchesLightCulling(request, pass, *culling);
            if (const auto *value = std::get_if<RenderGraphDepthAttachment>(&workload))
                return MatchesDepth(pass, uses, *value);
            if (const auto *value = std::get_if<RenderGraphColorAttachment>(&workload))
                return MatchesColor(pass, uses, *value);
            if (const auto *value = std::get_if<RenderGraphBufferCopy>(&workload))
                return MatchesCopy(pass, uses, *value);
            if (const auto *value = std::get_if<RenderGraphStereoCoverageCopy>(&workload))
                return MatchesStereoCoverageCopy(request.graph, pass, uses, *value);
            return MatchesBasic(pass, uses, workload);
        }

        /** @brief Validates exact pass semantics and records retained first/last uses without allocation. */
        [[nodiscard]] bool RecordUses(const RenderGraphExecutionRequest &request, const std::span<AliasUse> aliases) {
            const auto &graph = request.graph;
            for (std::size_t index = 0; index < graph.Passes().size(); ++index) {
                const auto &pass = graph.Passes()[index];
                if (!pass.usages.IsValidFor(graph.Usages().size()) || !pass.queue.IsValid() ||
                    (request.submissions.empty() && pass.queue != graph.Passes().front().queue) ||
                    request.workloads[index].pass != pass.pass || !MatchesWorkload(request, pass, request.workloads[index].workload))
                    return false;
                for (const auto &use : graph.Usages().subspan(pass.usages.offset, pass.usages.count)) {
                    if (use.resource.owner != graph.Owner() || use.resource.value == 0 || use.resource.value > graph.Resources().size())
                        return false;
                    auto &alias = aliases[use.resource.value - 1U];
                    alias.first = std::min(alias.first, index);
                    alias.last = index;
                }
            }
            return true;
        }

        /** @brief Rejects overlapping transient native objects and reuse of imported physical storage. */
        [[nodiscard]] bool CompatibleAliases(const RenderGraphExecutionRequest &request, const std::span<AliasUse> aliases) {
            std::ranges::sort(aliases, [](const AliasUse &left, const AliasUse &right) {
                return std::tie(left.kind, left.instance, left.first) < std::tie(right.kind, right.instance, right.first);
            });
            for (std::size_t index = 1; index < aliases.size(); ++index) {
                const auto &previous = aliases[index - 1];
                const auto &current = aliases[index];
                if (previous.kind != current.kind || previous.instance != current.instance)
                    continue;
                const bool previousTransient = previous.resourceClass == RenderGraphResourceClass::Transient;
                const bool currentTransient = current.resourceClass == RenderGraphResourceClass::Transient;
                if ((previousTransient || currentTransient) && (!previousTransient || !currentTransient || previous.last >= current.first))
                    return false;
                if (previousTransient && currentTransient && !request.submissions.empty() &&
                    !HasSubmissionDependency(request, request.graph.Passes()[previous.last].pass,
                                             request.graph.Passes()[current.first].pass))
                    return false;
            }
            return true;
        }

        /** @brief Checks native binding coverage and independently validates the actual reused object intervals. */
        [[nodiscard]] Result<void> ValidateInstances(const RenderGraphExecutionRequest &request, const std::span<AliasUse> aliases) {
            const auto &graph = request.graph;
            std::size_t usedCount{};
            bool hasTransient{};
            for (std::size_t index = 0; index < graph.Resources().size(); ++index) {
                const auto &resource = graph.Resources()[index];
                if (request.resources[index].resource != resource.id)
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
                if (resource.resourceClass == RenderGraphResourceClass::Transient) {
                    if (!request.transientResourcesAdmitted)
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
                    if (aliases[index].first == std::numeric_limits<std::size_t>::max())
                        continue;
                    hasTransient = true;
                }
                if (request.resources[index].instance == 0)
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
                if (aliases[index].first != std::numeric_limits<std::size_t>::max()) {
                    auto &alias = aliases[index];
                    alias.instance = request.resources[index].instance;
                    alias.kind = resource.kind;
                    alias.resourceClass = resource.resourceClass;
                    aliases[usedCount++] = alias;
                }
            }
            if (hasTransient && !CompatibleAliases(request, aliases.first(usedCount)))
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            return Result<void>::Success();
        }

        /** @brief Finds the canonical submission covering one compiled pass identity. */
        [[nodiscard]] std::size_t SubmissionForPass(const RenderGraphExecutionRequest &request, const RenderGraphPassRef pass) {
            const auto passes = request.graph.Passes();
            const auto found = std::ranges::find(passes, pass, &RenderGraphExecutionPass::pass);
            if (found == passes.end())
                return request.submissions.size();
            const auto index = static_cast<std::size_t>(found - passes.begin());
            for (std::size_t submission = 0; submission < request.submissions.size(); ++submission) {
                const auto range = request.submissions[submission].passes;
                if (index >= range.offset && index - range.offset < range.count)
                    return submission;
            }
            return request.submissions.size();
        }

        /** @brief Requires a cross-queue dependency to wait on its exact producer signal. */
        [[nodiscard]] bool HasSubmissionDependency(const RenderGraphExecutionRequest &request, const RenderGraphPassRef before,
                                                   const RenderGraphPassRef after) {
            const auto source = SubmissionForPass(request, before);
            const auto destination = SubmissionForPass(request, after);
            if (source >= request.submissions.size() || destination >= request.submissions.size() || source > destination)
                return false;
            const auto &producer = request.submissions[source].submission;
            const auto &consumer = request.submissions[destination].submission;
            return producer.queue == consumer.queue ||
                   (source < destination && std::ranges::find(consumer.waits, producer.signal) != consumer.waits.end());
        }

        /** @brief Rejects repeated queue waits and any current/future graph signal used before it is issued. */
        [[nodiscard]] bool ValidSubmissionWaits(const RenderGraphExecutionRequest &request, const std::size_t index) {
            const auto waits = request.submissions[index].submission.waits;
            for (std::size_t wait = 0; wait < waits.size(); ++wait) {
                for (std::size_t prior = 0; prior < wait; ++prior)
                    if (waits[prior].queue == waits[wait].queue)
                        return false;
                for (std::size_t future = index; future < request.submissions.size(); ++future)
                    if (request.submissions[future].submission.signal == waits[wait])
                        return false;
            }
            return true;
        }

        /** @brief Preserves strictly increasing signal values independently on each logical queue. */
        [[nodiscard]] bool MonotonicSubmissionSignal(const RenderGraphExecutionRequest &request, const std::size_t index) {
            const auto &submission = request.submissions[index].submission;
            return std::ranges::none_of(request.submissions.first(index), [&submission](const RenderGraphQueueSubmission &previous) {
                return previous.submission.queue == submission.queue && previous.submission.signal.value >= submission.signal.value;
            });
        }

        /** @brief Validates one bounded exhaustive partition and monotonic submission authority. */
        [[nodiscard]] bool ValidSubmissionPartition(const RenderGraphExecutionRequest &request) {
            if (request.submissions.size() > RenderGraphSubmissionLimits::MaximumSubmissions)
                return false;
            std::size_t offset = 0;
            std::array<RenderQueueId, RenderGraphSubmissionLimits::MaximumQueues> queues{};
            std::size_t queueCount = 0;
            std::uint64_t previousOrder = 0;
            for (std::size_t index = 0; index < request.submissions.size(); ++index) {
                const auto &record = request.submissions[index];
                const auto &submission = record.submission;
                if (!submission.IsValid() || submission.order.value <= previousOrder ||
                    submission.waits.size() > RenderGraphSubmissionLimits::MaximumQueues || record.passes.count == 0 ||
                    record.passes.offset != offset || !record.passes.IsValidFor(request.graph.Passes().size()))
                    return false;
                for (const auto &pass : request.graph.Passes().subspan(offset, record.passes.count))
                    if (pass.queue != submission.queue)
                        return false;
                if (const auto issuedQueues = std::span{queues}.first(queueCount);
                    std::ranges::find(issuedQueues, submission.queue) == issuedQueues.end()) {
                    if (queueCount == queues.size())
                        return false;
                    queues[queueCount++] = submission.queue;
                }
                if (!MonotonicSubmissionSignal(request, index) || !ValidSubmissionWaits(request, index))
                    return false;
                previousOrder = submission.order.value;
                offset += record.passes.count;
            }
            return offset == request.graph.Passes().size();
        }

        /** @brief Matches all compiled ownership pairs and authored execution dependencies to submission evidence. */
        [[nodiscard]] bool ValidSubmissionEdges(const RenderGraphExecutionRequest &request) {
            const auto releases = request.graph.ReleaseTransfers();
            const auto acquires = request.graph.AcquireTransfers();
            if (releases.size() != acquires.size())
                return false;
            for (const auto &release : releases) {
                if (const auto matches =
                        [&release](const RenderGraphOwnershipTransfer &acquire) {
                    return acquire.resource == release.resource && acquire.releaseAfter == release.releaseAfter &&
                           acquire.acquireBefore == release.acquireBefore && acquire.sourceQueue == release.sourceQueue &&
                           acquire.destinationQueue == release.destinationQueue;
                };
                    std::ranges::count_if(acquires, matches) != 1 || !release.sourceQueue.IsValid() ||
                    !release.destinationQueue.IsValid() || release.sourceQueue == release.destinationQueue ||
                    !HasSubmissionDependency(request, release.releaseAfter, release.acquireBefore))
                    return false;
                const auto source = SubmissionForPass(request, release.releaseAfter);
                const auto destination = SubmissionForPass(request, release.acquireBefore);
                if (request.submissions[source].submission.queue != release.sourceQueue ||
                    request.submissions[destination].submission.queue != release.destinationQueue)
                    return false;
            }
            return std::ranges::all_of(request.graph.Dependencies(), [&](const auto &dependency) {
                return HasSubmissionDependency(request, dependency.before, dependency.after);
            });
        }

        /** @brief Preserves semantic validator failures instead of collapsing them into a generic boolean graph error. */
        Result<void> ValidateTypedPass(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                       const RenderGraphWorkload &workload) {
            if (const auto *reconstruction = std::get_if<RenderGraphNativeReconstruction>(&workload))
                return ValidateNativeReconstructionGraphWorkload(request, pass, *reconstruction);
            if (const auto *upload = std::get_if<RenderGraphTextureUpload>(&workload))
                return ValidateTextureUploadGraphWorkload(request, pass, *upload);
            if (const auto *raster = std::get_if<RenderGraphMaterialRaster>(&workload))
                return ValidateMaterialRasterWorkload(request, pass, *raster);
            if (const auto *vfx = std::get_if<RenderGraphVfx>(&workload))
                return ValidateVfxGraphWorkload(request, pass, *vfx);
            if (const auto *exposure = std::get_if<RenderGraphExposureGpu>(&workload))
                return ValidateExposureGpuGraphWorkload(request, pass, *exposure);
            return Result<void>::Success();
        }

        /** @brief Compares the producer's exact ordered views while allowing distinct output material generations. */
        bool SameMultiviewProjection(const RenderGraphMultiview &color, const RenderGraphMultiview &motion) {
            return color.active == motion.active &&
                   std::ranges::equal(color.scene->Views(), motion.scene->Views(), [](const auto &a, const auto &b) {
                return a.index == b.index && a.depthRange == b.depthRange && a.worldToClip == b.worldToClip;
            });
        }

        /** @brief Keeps image ownership disjoint across compositor projection and auxiliary outputs. */
        bool DisjointMultiviewTargets(const RenderGraphMultiview &color, const RenderGraphMultiview &motion) {
            for (const auto &target : color.targets) {
                if (std::ranges::any_of(motion.targets, [&](const auto &auxiliary) {
                    return target.color == auxiliary.color || target.color == auxiliary.depth ||
                           (target.depth.IsValid() && (target.depth == auxiliary.color || target.depth == auxiliary.depth)) ||
                           (auxiliary.predecessorDepth.IsValid() &&
                            (target.color == auxiliary.predecessorDepth || target.depth == auxiliary.predecessorDepth));
                }))
                    return false;
            }
            return true;
        }

        /** @brief Requires the coverage replay to use the projection's exact visible geometry, without its material. */
        bool SameCoverageGeometry(const RenderGraphMultiview &projection, const RenderGraphMultiview &coverage) {
            const auto equivalent = [&](const auto &a, const auto &b) {
                const auto &first = projection.scene->Instances()[a.instanceIndex];
                const auto &second = coverage.scene->Instances()[b.instanceIndex];
                return a.visibleViews == b.visibleViews && first.mesh == second.mesh && first.localToWorld == second.localToWorld;
            };
            return std::ranges::equal(projection.scene->Draws(), coverage.scene->Draws(), equivalent);
        }

        /** @brief Enforces producer and consumer order within one validated submission range. */
        bool SharesOrderedSubmission(const RenderGraphExecutionRequest &request, const std::size_t producer, const std::size_t consumer) {
            if (producer >= consumer)
                return false;
            return std::ranges::any_of(request.submissions, [&](const auto &submission) {
                const auto [offset, count] = submission.passes;
                return submission.passes.IsValidFor(request.workloads.size()) && offset <= producer && consumer < offset + count;
            });
        }

        /** @brief Every requested tile must be inside its exact written eye view. */
        bool CoverageTilesWrittenBy(const RenderGraphStereoCoverageCopy &copy, const RenderGraphMultiview &coverage) {
            for (std::size_t eye = 0; eye < copy.tiles.size(); ++eye) {
                const auto &target = coverage.targets[eye];
                const auto &tile = copy.tiles[eye];
                const auto &rect = target.rectangle;
                if (target.color != copy.source || target.arrayLayer != eye || tile.x < rect.x || tile.y < rect.y ||
                    !MultiviewRenderRectangle{tile.x - rect.x, tile.y - rect.y, tile.width, tile.height}.Fits({rect.width, rect.height}))
                    return false;
            }
            return true;
        }

        /** @brief The observed content and staging receipt have no second writer. */
        bool CoverageCopyProven(const RenderGraphExecutionRequest &request, const RenderGraphMultiview &coverage,
                                const RenderGraphStereoCoverageCopy &copy, const std::size_t producer, const std::size_t consumer) {
            if (!SharesOrderedSubmission(request, producer, consumer) || !CoverageTilesWrittenBy(copy, coverage))
                return false;
            const auto coveragePass = request.workloads[producer].pass;
            const auto readbackPass = request.workloads[consumer].pass;
            return std::ranges::none_of(request.graph.Usages(), [&](const auto &use) {
                if (use.resource == copy.destination)
                    return use.pass != readbackPass;
                return use.resource == copy.source && use.access != RenderGraphAccess::Read && use.pass != coveragePass;
            });
        }

        Result<void> ValidateTypedWorkloads(const RenderGraphExecutionRequest &request) {
            std::array<const RenderGraphMultiview *, RenderGraphMultiview::MaximumPassesPerFrame> multiview{};
            std::size_t reconstructionCount = 0;
            const RenderGraphStereoCoverageCopy *readback{};
            std::size_t coverageIndex{}, readbackIndex{};
            for (std::size_t index = 0; index < request.workloads.size(); ++index) {
                const auto &binding = request.workloads[index];
                if (std::holds_alternative<RenderGraphNativeReconstruction>(binding.workload) &&
                    ++reconstructionCount > RenderGraphNativeReconstruction::MaximumPassesPerFrame)
                    return Result<void>::Failure(MakeError(NativeReconstructionErrors::InvalidDispatch));
                if (auto valid = ValidateTypedPass(request, request.graph.Passes()[index], binding.workload); valid.HasError())
                    return valid;
                if (const auto *copy = std::get_if<RenderGraphStereoCoverageCopy>(&binding.workload)) {
                    if (readback)
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
                    readback = copy;
                    readbackIndex = index;
                }
                if (const auto *scene = std::get_if<RenderGraphMultiview>(&binding.workload)) {
                    if (const auto valid = ValidateMultiviewGraphWorkload(request, request.graph.Passes()[index], *scene); valid.HasError())
                        return valid;
                    auto &output = multiview[static_cast<std::size_t>(scene->output)];
                    if (output)
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload,
                                                               "One graph admits at most one multiview pass for each output."));
                    output = scene;
                    if (scene->output == MultiviewOutput::ReprojectionCoverage)
                        coverageIndex = index;
                }
            }
            const auto *color = multiview[static_cast<std::size_t>(MultiviewOutput::ProjectionColor)];
            const auto *motion = multiview[static_cast<std::size_t>(MultiviewOutput::MotionDepth)];
            const auto *coverage = multiview[static_cast<std::size_t>(MultiviewOutput::ReprojectionCoverage)];
            if (color && motion && (!SameMultiviewProjection(*color, *motion) || !DisjointMultiviewTargets(*color, *motion)))
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph,
                                                       "Paired multiview outputs require the same exact views and disjoint attachments."));
            if (coverage && (!color || !SameMultiviewProjection(*color, *coverage) || !SameCoverageGeometry(*color, *coverage) ||
                             !DisjointMultiviewTargets(*color, *coverage) || (motion && !DisjointMultiviewTargets(*motion, *coverage))))
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph,
                                                       "Coverage requires the exact current projection geometry and disjoint outputs."));
            if (readback && (!coverage || !CoverageCopyProven(request, *coverage, *readback, coverageIndex, readbackIndex)))
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph,
                                                       "Coverage readback requires exact owned eye tiles and its producer submission."));
            return Result<void>::Success();
        }

        Result<void> ValidateRequestEnvelope(const RenderGraphExecutionRequest &request) {
            if (request.role > RenderGraphExecutionRole::UiComposition ||
                (request.role == RenderGraphExecutionRole::UiComposition &&
                 (request.workloads.empty() || !std::ranges::all_of(request.workloads, [](const auto &binding) {
                return std::holds_alternative<RenderGraphUiPaint>(binding.workload);
            }))))
                return Result<void>::Failure(
                    MakeError(RenderGraphExecutionErrors::InvalidGraph, "UI composition is one explicit paint-only frame stage."));
            if (const auto &graph = request.graph;
                !graph.Owner().IsValid() || graph.Resources().size() > RenderGraphLimits::HardMaxResources ||
                graph.Passes().size() > RenderGraphLimits::HardMaxPasses || request.workloads.size() != graph.Passes().size() ||
                request.resources.size() != graph.Resources().size())
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateRenderGraphExecutionRequest */
    Result<void> ValidateRenderGraphExecutionRequest(const RenderGraphExecutionRequest &request) {
        if (auto envelope = ValidateRequestEnvelope(request); envelope.HasError())
            return envelope;
        const auto &graph = request.graph;
        if (auto typed = ValidateTypedWorkloads(request); typed.HasError())
            return typed;
        if (request.submissions.empty() && (!graph.ReleaseTransfers().empty() || !graph.AcquireTransfers().empty()))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload,
                                                   "Single-queue workload admission does not implement ownership transfers."));
        if (!request.submissions.empty() && (!ValidSubmissionPartition(request) || !ValidSubmissionEdges(request)))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidSynchronization,
                                                   "Graph submissions require exact ordered partitions and producer signal waits."));
        std::array<AliasUse, RenderGraphLimits::HardMaxResources> storage;
        const auto aliases = std::span{storage}.first(graph.Resources().size());
        std::ranges::fill(aliases, AliasUse{0, std::numeric_limits<std::size_t>::max(), 0, RenderGraphResourceKind::Buffer,
                                            RenderGraphResourceClass::Persistent});
        if (!RecordUses(request, aliases))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        return ValidateInstances(request, aliases);
    }
}  // namespace Horo::Render
