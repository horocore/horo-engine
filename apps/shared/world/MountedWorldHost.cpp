#include "MountedWorldHost.h"

#include "Horo/Assets/AssetProvider.h"
#include "Horo/Runtime/Scene/SceneCellPayloadCodec.h"
#include "Horo/WorldStreaming/StreamingCellInspection.h"
#include "Horo/WorldStreaming/StreamingCellState.h"
#include "Horo/WorldStreaming/StreamingSourcePlanner.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "Horo/WorldStreaming/WorldDependencyArtifact.h"
#include "ScenePhysicsHost.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <initializer_list>
#include <optional>
#include <utility>

namespace Horo::Application::Internal {
    namespace WS = WorldStreaming;

    const ErrorCodeDescriptor MountedWorldErrors::Deadline{ErrorDomainId{"application.world"},
                                                           ErrorCode{"application.world.startup_deadline"},
                                                           ErrorSeverity::Error,
                                                           "Mounted World startup deadline expired.",
                                                           "Select a bounded starting cell and restart Play.",
                                                           false,
                                                           false,
                                                           {}};

    namespace {
        constexpr std::uint32_t MaximumCells = 4096;
        constexpr std::size_t MaximumBytes = 256U * 1024U * 1024U;
        constexpr std::size_t MaximumCellBytes = 32U * 1024U * 1024U;
        constexpr Assets::AssetCookLimits CookLimits{MaximumBytes, 40U * 1024U * 1024U, MaximumCells + 2U, 1};
        constexpr WS::WorldIndexArtifactLimits IndexLimits{8U * 1024U * 1024U,
                                                           65536,
                                                           1024,
                                                           {256, MaximumCells, 65536},
                                                           {MaximumCells, MaximumCells, 65536, MaximumCellBytes, MaximumCellBytes}};
        constexpr WS::WorldDependencyArtifactLimits
            DependencyLimits{32U * 1024U * 1024U, 64U * 1024U * 1024U, 65536, MaximumCells, 65536, 65536, 65536, 65536, 65536, 1'048'576};
        constexpr WS::StreamingCellInspectionLimits CellLimits{32, MaximumCellBytes, MaximumCellBytes};

        template <typename Id> [[nodiscard]] Id Identity(const std::uint64_t value) {
            return Id::Create(value).Value();
        }

        /** @brief Reads only members of the selected immutable inventory, preserving exact envelope type and target. */
        Result<Assets::AssetCookArtifact> ReadArtifact(const Assets::AssetCookGenerationContents &contents,
                                                       const Assets::AssetCookTargetId &target, const Assets::AssetId id,
                                                       const std::string_view type) {
            const auto entry = std::ranges::lower_bound(contents.entries, id, {}, &Assets::AssetCookManifestEntry::assetId);
            if (entry == contents.entries.end() || entry->assetId != id || entry->assetType.Value() != type)
                return Result<Assets::AssetCookArtifact>::Failure(
                    MakeError(WS::WorldArtifactErrors::Invalid,
                              "The selected World generation is missing an exact generated artifact role."));
            auto artifact =
                Assets::DecodeCookedArtifact(contents.artifacts[static_cast<std::size_t>(entry - contents.entries.begin())], CookLimits);
            if (artifact.HasError())
                return artifact;
            if (artifact.Value().id != id || artifact.Value().target != target || artifact.Value().type != entry->assetType)
                return Result<Assets::AssetCookArtifact>::Failure(
                    MakeError(WS::WorldArtifactErrors::Invalid, "The World artifact envelope does not match its selected manifest entry."));
            return artifact;
        }

        /** @brief Prepares the actual source-planning owner for one standalone mounted registry. */
        Result<std::unique_ptr<WS::StreamingSourcePlanner>> MakePlanner(const WS::WorldPartitionRegistrySnapshot &partition) {
            const auto priority = WS::StreamingPriorityPolicy::Create(
                {.id = Identity<WS::StreamingPriorityPolicyId>(1), .revision = Identity<WS::StreamingPriorityPolicyRevision>(1)});
            const auto stability =
                WS::StreamingCellStabilityPolicy::Create({.id = Identity<WS::StreamingCellStabilityPolicyId>(1),
                                                          .revision = Identity<WS::StreamingCellStabilityPolicyRevision>(1),
                                                          .maximumTrackedCells = MaximumCells});
            const auto prefetch = WS::StreamingPrefetchPolicy::Create(
                {.id = Identity<WS::StreamingPrefetchPolicyId>(1), .revision = Identity<WS::StreamingPrefetchPolicyRevision>(1)});
            using Output = Result<std::unique_ptr<WS::StreamingSourcePlanner>>;
            if (priority.HasError())
                return Output::Failure(priority.ErrorValue());
            if (stability.HasError())
                return Output::Failure(stability.ErrorValue());
            if (prefetch.HasError())
                return Output::Failure(prefetch.ErrorValue());
            std::vector<WS::StreamingLayerId> enabled;
            enabled.reserve(partition.Layers().size());
            for (const auto &layer : partition.Layers())
                if ((static_cast<std::uint32_t>(layer.flags) & (static_cast<std::uint32_t>(WS::WorldLayerFlags::ClientOnly) |
                                                                static_cast<std::uint32_t>(WS::WorldLayerFlags::ServerOnly))) == 0)
                    enabled.push_back(layer.id);
            return WS::StreamingSourcePlanner::Create({.limits = {1024, MaximumCells, 16384, 32, 128, 256, 256, 60000},
                                                       .shapeSupport = {true, true, true, true},
                                                       .priority = priority.Value(),
                                                       .stability = stability.Value(),
                                                       .prefetch = prefetch.Value()},
                                                      partition, {1, {1}, 1, 0, enabled});
        }

        /** @brief Verifies actual cell bytes against every corresponding index row before publishing a provider. */
        Result<void> LoadCells(Assets::MemoryAssetProvider &provider, const Assets::AssetCookGenerationContents &contents,
                               const Assets::AssetCookTargetId &target, const WS::DecodedWorldIndex &index,
                               const CancellationToken &cancellation) {
            for (const auto &cell : index.descriptor.Cells()) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Cancelled));
                const auto expected = std::ranges::lower_bound(index.cells, cell.id, WS::StreamingCellCanonicalLess{},
                                                               &WS::CookedWorldCellManifestEntry::cell);
                if (expected == index.cells.end() || expected->cell != cell.id)
                    return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
                auto artifact = ReadArtifact(contents, target, cell.package.chunkAsset, "world.cell");
                if (artifact.HasError())
                    return Result<void>::Failure(artifact.ErrorValue());
                auto inspected = WS::InspectStreamingCellArtifact(std::as_bytes(std::span{artifact.Value().payload}),
                                                                  expected->artifactHash, CellLimits, cancellation);
                if (inspected.HasError())
                    return Result<void>::Failure(inspected.ErrorValue());
                const auto actual = inspected.Value().Header();
                if (actual.cell != cell.id || actual.compressedSize != expected->compressedSize ||
                    actual.uncompressedSize != expected->uncompressedSize || actual.payloadCrc32 != expected->payloadCrc32)
                    return Result<void>::Failure(
                        MakeError(WS::WorldArtifactErrors::Invalid,
                                  "A World cell does not match the exact identity and integrity advertised by its index."));
                provider.Insert(cell.package.chunkAsset, std::move(artifact).Value().payload);
            }
            return Result<void>::Success();
        }

        /** @brief Shared cold load cancellation/deadline check before touching the private Scene. */
        Result<void> CheckBoundary(const MountedWorldStartup &boundary) {
            if (boundary.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Cancelled));
            if (std::chrono::steady_clock::now() >= boundary.deadline)
                return Result<void>::Failure(MakeError(MountedWorldErrors::Deadline));
            return Result<void>::Success();
        }

        /** @brief A startup pin must appear as the only exact demand in the actual mounted planner publication. */
        Result<WS::StreamingSourceRegistration> PlanInitialCell(WS::StreamingSourcePlanner &planner,
                                                                const WS::WorldPartitionRegistrySnapshot &partition,
                                                                const WS::WorldPartitionCellHandle &cell,
                                                                const MountedWorldStartup &boundary) {
            using Output = Result<WS::StreamingSourceRegistration>;
            const auto bounds = partition.CellBounds(cell);
            if (bounds.HasError())
                return Output::Failure(bounds.ErrorValue());
            const auto priority = WS::StreamingSourcePriority::Create(1.0F);
            if (priority.HasError())
                return Output::Failure(priority.ErrorValue());
            auto source =
                planner.Register({.id = Identity<WS::StreamingSourceId>(1),
                                  .revision = Identity<WS::StreamingSourceRevision>(1),
                                  .intent = WS::StreamingSourceIntent::Preload,
                                  .priority = priority.Value(),
                                  .shape = WS::StreamingSourceBox{bounds.Value().minimum, bounds.Value().maximum},
                                  .residency = WS::StreamingDesiredResidency::Activated,
                                  .retention = WS::StreamingRetention::Pinned,
                                  .selection = {.grid = partition.Binding().registry, .layer = cell.cell.layer, .lod = cell.cell.lod}});
            if (source.HasError())
                return source;
            for (;;) {
                if (auto ready = CheckBoundary(boundary); ready.HasError())
                    return Output::Failure(ready.ErrorValue());
                const auto serviceTime =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                auto progress = planner.Service(static_cast<std::uint64_t>(serviceTime));
                if (progress.HasError())
                    return Output::Failure(progress.ErrorValue());
                if (progress.Value().published)
                    break;
            }
            const auto plan = planner.Plan();
            if (plan.HasError())
                return Output::Failure(plan.ErrorValue());
            if (plan.Value().cells.size() != 1 || plan.Value().cells.front().cell != cell ||
                plan.Value().cells.front().pinnedFloor != WS::StreamingDesiredResidency::Activated)
                return Output::Failure(
                    MakeError(WS::WorldArtifactErrors::Unsupported,
                              "The exact startup cell is outside standalone planner admission or requires multiple demands."));
            return source;
        }

        /** @brief Check actual scheduler and residency evidence at the exact Scene structural cutoff. */
        class InitialCellAuthority final : public Runtime::SceneCellPayloadAuthority {
        public:
            InitialCellAuthority(const Runtime::SceneCellPayloadIdentity &identity, WS::StreamingCellStateLedger &residency,
                                 const WS::StreamingSchedulerAdmissionLedger &scheduler,
                                 WS::StreamingSchedulerReservation reservation) noexcept
                : identity_(identity), residency_(residency), scheduler_(scheduler), reservation_(reservation) {}

            Result<void> ValidatePublication(const Runtime::SceneCellPayloadIdentity &identity,
                                             const WS::StreamingFence &fence) const override {
                const auto operation = scheduler_.Inspect(reservation_);
                const auto resident = residency_.Resolve(residency_.Owner(), fence);
                if (identity != identity_ || operation.HasError() || resident.HasError() || operation.Value().Handle().fence != fence ||
                    operation.Value().State() != WS::StreamingCellOperationState::Activating ||
                    resident.Value().operation != operation.Value().Handle() || resident.Value().state != WS::StreamingCellState::Resident)
                    return Result<void>::Failure(MakeError(Runtime::SceneCellPayloadErrors::Stale));
                return Result<void>::Success();
            }

        private:
            Runtime::SceneCellPayloadIdentity identity_;
            WS::StreamingCellStateLedger &residency_;
            const WS::StreamingSchedulerAdmissionLedger &scheduler_;
            WS::StreamingSchedulerReservation reservation_;
        };

        /** @brief Reconcile the actual scheduler successor through the mounted residency ledger. */
        Result<void> AdvanceCell(WS::StreamingSchedulerAdmissionLedger &scheduler, WS::StreamingCellStateLedger &residency,
                                 const WS::StreamingSchedulerReservation &reservation, const WS::StreamingCellOperationTransition step) {
            auto advanced = scheduler.Advance(reservation, step);
            if (advanced.HasError())
                return Result<void>::Failure(advanced.ErrorValue());
            auto recorded = residency.Apply(residency.Owner(), advanced.Value());
            return recorded.HasError() ? Result<void>::Failure(recorded.ErrorValue()) : Result<void>::Success();
        }

        /** @brief Run one synchronous, bounded cold operation; work returns only after its borrows and deferred work settle. */
        template <typename Work>
        Result<void> RunCellOperation(WS::StreamingSchedulerAdmissionLedger &scheduler, WS::StreamingCellStateLedger &residency,
                                      const WS::StreamingCellOperationHandle &handle, const WS::StreamingCellOperationKind kind,
                                      const std::initializer_list<WS::StreamingCellOperationTransition> setup,
                                      const WS::StreamingCellOperationTransition terminal, Work work) {
            using enum WS::StreamingCellOperationTransition;
            const auto queued = WS::StreamingCellOperation::Create(handle, kind);
            if (queued.HasError())
                return Result<void>::Failure(queued.ErrorValue());
            const auto admitted = scheduler.TryAdmit(queued.Value(), 1, scheduler.Limits().concurrency.revision);
            if (admitted.HasError())
                return Result<void>::Failure(admitted.ErrorValue());
            const auto &reservation = admitted.Value();
            const auto advance = [&](const auto step) {
                return AdvanceCell(scheduler, residency, reservation, step);
            };

            const auto fail = [&](const Error &error) -> Result<void> {
                const auto code = error.code.Value();
                const bool cancelled = code == Runtime::SceneCellPayloadErrors::Cancelled.code.Value() ||
                                       code == WS::WorldArtifactErrors::Cancelled.code.Value();
                auto current = scheduler.Inspect(reservation);
                if (current.HasError())
                    return Result<void>::Failure(current.ErrorValue());
                const auto state = residency.Resolve(residency.Owner(), handle.fence);
                const bool recorded = state.HasValue() && state.Value().operation == handle;
                if (!current.Value().IsTerminal()) {
                    if (current.Value().State() != WS::StreamingCellOperationState::Retiring) {
                        const auto disposition = cancelled ? Cancel : Fail;
                        const auto interrupted = recorded ? advance(disposition) : Result<void>::Success();
                        if (interrupted.HasError())
                            return interrupted;
                        if (!recorded) {
                            const auto unrecorded = scheduler.Advance(reservation, disposition);
                            if (unrecorded.HasError())
                                return Result<void>::Failure(unrecorded.ErrorValue());
                        }
                    }
                    if (recorded) {
                        if (const auto retired = advance(AcknowledgeRetirement); retired.HasError())
                            return retired;
                    } else {
                        const auto unrecorded = scheduler.Advance(reservation, AcknowledgeRetirement);
                        if (unrecorded.HasError())
                            return Result<void>::Failure(unrecorded.ErrorValue());
                    }
                }
                if (auto released = scheduler.Release(reservation); released.HasError())
                    return released;
                return Result<void>::Failure(error);
            };

            // Retire goes directly from admission to Evicting.
            if (kind != WS::StreamingCellOperationKind::Retire) {
                auto current = scheduler.Inspect(reservation);
                if (current.HasError())
                    return Result<void>::Failure(current.ErrorValue());
                if (auto recorded = residency.Apply(residency.Owner(), current.Value()); recorded.HasError())
                    return fail(recorded.ErrorValue());
            }
            for (const auto step : setup)
                if (const auto advanced = advance(step); advanced.HasError())
                    return fail(advanced.ErrorValue());

            const auto result = work(reservation);
            if (result.HasError())
                return kind == WS::StreamingCellOperationKind::Retire ? result : fail(result.ErrorValue());
            if (const auto finished = advance(terminal); finished.HasError())
                return fail(finished.ErrorValue());
            return scheduler.Release(reservation);
        }

        /** @brief Admit the committed cell's single supported provider against its independent authored identity. */
        Result<Runtime::RuntimeSceneCellPayload> DecodeInitialCell(const Assets::MemoryAssetProvider &provider,
                                                                   const WS::WorldPartitionDescriptor &partition,
                                                                   const WS::WorldPartitionCellDescriptor &cell,
                                                                   const WS::CookedWorldCellManifestEntry &metadata,
                                                                   const Runtime::SceneCellPayloadIdentity &identity,
                                                                   const CancellationToken &cancellation) {
            using Output = Result<Runtime::RuntimeSceneCellPayload>;
            const auto bytes = provider.Load(cell.package.chunkAsset, cancellation);
            if (bytes.HasError())
                return Output::Failure(bytes.ErrorValue());
            const auto artifact = std::as_bytes(std::span{bytes.Value()});
            auto inspected = WS::InspectStreamingCellArtifact(artifact, metadata.artifactHash, CellLimits, cancellation);
            if (inspected.HasError())
                return Output::Failure(inspected.ErrorValue());
            const auto header = inspected.Value().Header();
            if (header.compression != WS::StreamingCellCompression::None || header.payloads.size() != 1 ||
                header.payloads.front().provider != WS::StreamingCellProvider::CoreEcs ||
                header.payloads.front().requirement != WS::StreamingCellPayloadRequirement::Required)
                return Output::Failure(MakeError(Runtime::SceneCellPayloadErrors::Unsupported));
            const auto &core = header.payloads.front();
            const auto body = artifact.subspan(static_cast<std::size_t>(core.offset), static_cast<std::size_t>(core.compressedSize));
            if (!Runtime::MatchesSceneCellPayloadWireVersion(body, core.version))
                return Output::Failure(MakeError(Runtime::SceneCellPayloadErrors::Unsupported));
            return Runtime::DecodeSceneCellPayload(partition, body, identity, {}, {},
                                                   {MaximumCellBytes, MaximumCellBytes, {65536, MaximumCells, MaximumCellBytes}},
                                                   cancellation);
        }

        struct AnchoredGeometryEntity final {
            Runtime::RuntimeEntityView entity;
            const Runtime::SceneCellWorldAnchor &anchor;
        };

        /** @brief Matches an actual baseline entity generation with its immutable, independently authored global position. */
        Result<AnchoredGeometryEntity> FindAnchoredGeometryEntity(const Runtime::RuntimeSceneView &scene,
                                                                  const Runtime::SceneBaselineAttachmentView &baseline,
                                                                  std::span<const Runtime::SceneCellWorldAnchor> anchors,
                                                                  std::span<const Math::Vec3> initialLocalPositions,
                                                                  const Runtime::SceneObjectId object) {
            using Output = Result<AnchoredGeometryEntity>;
            const auto anchor = std::ranges::lower_bound(anchors, object, {}, &Runtime::SceneCellWorldAnchor::object);
            const auto entity = scene.Find(object);
            if (anchor == anchors.end() || anchor->object != object || !entity ||
                std::ranges::find(baseline.entities, entity->entity) == baseline.entities.end())
                return Output::Failure(
                    MakeError(WS::WorldArtifactErrors::Unsupported, "The selected cell object has no exact mounted World anchor."));
            const auto live = scene.Get(*entity);
            if (live.HasError())
                return Output::Failure(live.ErrorValue());
            if (live.Value().parent)
                return Output::Failure(MakeError(WS::WorldArtifactErrors::Unsupported, "Mounted geometry requires root transforms."));
            if (live.Value().localTransform->translation != initialLocalPositions[static_cast<std::size_t>(anchor - anchors.begin())])
                return Output::Failure(
                    MakeError(WS::WorldArtifactErrors::Stale, "The live object moved without a new canonical World position."));
            return Output::Success(AnchoredGeometryEntity{live.Value(), *anchor});
        }

        /** @brief Keeps conversion from wire representation to the canonical position explicit and exact. */
        Math::WorldCoordinate64 Global(const std::array<std::int64_t, 3> &position) noexcept {
            return Math::WorldCoordinate64::FromMillimeters(position[0], position[1], position[2]);
        }

        /** @brief Joins live camera optics and up orientation to independent exact global look endpoints. */
        Result<Render::RenderOriginCamera> CameraFor(const AnchoredGeometryEntity &source) {
            using Output = Result<Render::RenderOriginCamera>;
            const auto &camera = source.entity.components->camera;
            if (!camera || !camera->enabled || !source.anchor.cameraTargetMillimeters)
                return Output::Failure(
                    MakeError(WS::WorldArtifactErrors::Unsupported, "An enabled camera and exact World look target are required."));
            auto up = source.entity.localTransform->rotation.TryRotate({0.0F, 1.0F, 0.0F});
            if (up.HasError())
                return Output::Failure(up.ErrorValue());
            const auto kind = camera->projection == Runtime::CameraProjection::Perspective ? Render::RenderProjectionKind::Perspective
                                                                                           : Render::RenderProjectionKind::Orthographic;
            return Output::Success(
                {Global(source.anchor.positionMillimeters),
                 Global(*source.anchor.cameraTargetMillimeters),
                 up.Value(),
                 {kind, camera->verticalFieldOfViewRadians, camera->orthographicHeight, camera->nearPlane, camera->farPlane}});
        }
    }  // namespace

    /** @copydoc ParseCommittedWorldSelection */
    Result<CommittedWorldSelection> ParseCommittedWorldSelection(const std::span<char *const> arguments) {
        using Output = Result<CommittedWorldSelection>;
        if (arguments.size() != CommittedWorldSelectionArgumentCount)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Invalid,
                                             "Select a World cook root, target, manifest digest, index and dependency "
                                             "artifact IDs, and three origin coordinates."));
        auto root = ParseSceneHostAbsolutePath(arguments[0]);
        auto target = Assets::AssetCookTargetId::Parse(arguments[1]);
        auto manifest = ParseSha256(arguments[2]);
        auto index = Assets::AssetId::Parse(arguments[3]);
        auto dependencies = Assets::AssetId::Parse(arguments[4]);
        if (root.HasError() || target.HasError() || manifest.HasError() || index.HasError() || dependencies.HasError() ||
            index.Value() == dependencies.Value() || !index.Value().IsValid() || !dependencies.Value().IsValid())
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Invalid,
                                             "World mount root, target, manifest digest or artifact identities are invalid."));
        std::array<std::int64_t, 3> mm{};
        for (std::size_t axis = 0; axis < mm.size(); ++axis) {
            const std::string_view value{arguments[axis + 5]};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), mm[axis]);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                return Output::Failure(MakeError(WS::WorldArtifactErrors::Invalid, "World origin must use signed integer millimeters."));
        }
        return Output::Success({std::move(root).Value(), std::move(target).Value(), manifest.Value(), index.Value(), dependencies.Value(),
                                Math::WorldCoordinate64::FromMillimeters(mm[0], mm[1], mm[2])});
    }

    /** @copydoc ParseCommittedWorldInitialCell */
    Result<CommittedWorldInitialCell> ParseCommittedWorldInitialCell(const std::span<char *const> arguments) {
        using Output = Result<CommittedWorldInitialCell>;
        if (arguments.size() != CommittedWorldCellArgumentCount)
            return Output::Failure(
                MakeError(WS::WorldArtifactErrors::Invalid, "Select exactly the World startup cell, occurrence, definition and revision."));
        const auto parse = [](const std::string_view value, auto &number) {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
        };
        CommittedWorldInitialCell source;
        std::uint16_t layer{};
        if (!parse(arguments[0], source.cell.x) || !parse(arguments[1], source.cell.y) || !parse(arguments[2], source.cell.z) ||
            !parse(arguments[3], source.cell.lod) || !parse(arguments[4], layer) || !parse(arguments[5], source.occurrence.value) ||
            !parse(arguments[6], source.definition.value) || !parse(arguments[7], source.revision.value) || !source.occurrence.IsValid() ||
            !source.definition.IsValid() || source.revision.value == 0)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Invalid, "Invalid World startup cell or Scene identity."));
        const auto typedLayer = WS::StreamingLayerId::Create(layer);
        if (typedLayer.HasError())
            return Output::Failure(typedLayer.ErrorValue());
        source.cell.layer = typedLayer.Value();
        return Output::Success(source);
    }

    struct MountedWorld::State final {
        explicit State(Runtime::RuntimeSceneService &scene) noexcept : scene(scene) {}

        Runtime::RuntimeSceneService &scene;
        Assets::MemoryAssetProvider cells;
        std::vector<WS::CookedWorldCellManifestEntry> integrity;
        std::unique_ptr<WS::WorldPartitionRegistry> registry;
        std::unique_ptr<WS::StreamingSourcePlanner> planner;
        std::unique_ptr<WS::StreamingCellStateLedger> residency;
        std::optional<WS::DecodedWorldDependencies> dependencies;
        std::optional<WS::WorldStreamingRuntimeComposition> composition;
        std::optional<WS::StreamingSourceRegistration> initialSource;

        struct ActiveCell final {
            WS::StreamingFence fence;
            Runtime::SceneInstanceAddress address;
            Runtime::SceneCellPayloadIdentity identity;
            std::vector<Runtime::SceneCellWorldAnchor> anchors;
            std::vector<Math::Vec3> initialLocalPositions;
        };

        std::optional<ActiveCell> activeCell;
        bool initialAttempted{};
    };

    /** @copydoc MountedWorld::Prepare */
    Result<std::unique_ptr<MountedWorld>> MountedWorld::Prepare(const DurableFileSystem &files, const CommittedWorldSelection &selection,
                                                                const std::uint64_t generation, Runtime::RuntimeSceneService &scene,
                                                                const CancellationToken &cancellation) {
        using Output = Result<std::unique_ptr<MountedWorld>>;
        if (generation == 0 || !selection.targetRoot.is_absolute() || !selection.target.IsValid() || !selection.index.IsValid() ||
            !selection.dependencies.IsValid() || selection.index == selection.dependencies)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
        if (cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Cancelled));

        const auto published = Assets::ResolveCurrentCookGeneration(files, selection.targetRoot, CookLimits);
        if (published.HasError())
            return Output::Failure(published.ErrorValue());
        if (published.Value().target != selection.target || published.Value().manifestDigest != selection.manifest)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale, "The selected committed World manifest or target changed."));
        if (cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Cancelled));
        const auto contents = Assets::ReadCookGenerationContents(files, published.Value(), MaximumBytes, CookLimits);
        if (contents.HasError())
            return Output::Failure(contents.ErrorValue());

        const auto indexArtifact = ReadArtifact(contents.Value(), selection.target, selection.index, "world.index");
        if (indexArtifact.HasError())
            return Output::Failure(indexArtifact.ErrorValue());
        auto index = WS::DecodeWorldIndexArtifact(std::as_bytes(std::span{indexArtifact.Value().payload}), IndexLimits, cancellation);
        if (index.HasError())
            return Output::Failure(index.ErrorValue());
        if (index.Value().coordinates != WS::WorldIndexCoordinateSystem::RightHandedYUp)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Unsupported, "Standalone World mounting requires Horo world axes."));
        const auto bundle = ReadArtifact(contents.Value(), selection.target, selection.dependencies, "world.dependencies");
        if (bundle.HasError())
            return Output::Failure(bundle.ErrorValue());
        auto dependencies = WS::DecodeWorldDependencyArtifact(std::as_bytes(std::span{bundle.Value().payload}), index.Value(),
                                                              DependencyLimits, cancellation);
        if (dependencies.HasError())
            return Output::Failure(dependencies.ErrorValue());

        auto state = std::make_unique<State>(scene);
        if (auto loaded = LoadCells(state->cells, contents.Value(), selection.target, index.Value(), cancellation); loaded.HasError())
            return Output::Failure(loaded.ErrorValue());
        state->dependencies.emplace(std::move(dependencies).Value());
        state->integrity = std::move(index).Value().cells;
        const WS::StreamingRuntimeOwnerToken owner{index.Value().descriptor.Partition(), Identity<WS::PartitionEpoch>(generation),
                                                   Identity<WS::StreamingRuntimeOwnerId>(generation)};
        auto registry =
            WS::WorldPartitionRegistry::Create(Identity<WS::WorldPartitionRegistryId>(generation), owner, {MaximumCells, MaximumCells});
        if (registry.HasError())
            return Output::Failure(registry.ErrorValue());
        state->registry = std::move(registry).Value();
        auto described = state->registry->Publish(std::move(index).Value().descriptor, Identity<WS::WorldPartitionRegistryRevision>(1));
        if (described.HasError())
            return Output::Failure(described.ErrorValue());
        auto partition = state->registry->Snapshot();
        if (partition.HasError())
            return Output::Failure(partition.ErrorValue());
        auto planner = MakePlanner(partition.Value());
        if (planner.HasError())
            return Output::Failure(planner.ErrorValue());
        state->planner = std::move(planner).Value();
        auto residency = WS::StreamingCellStateLedger::Create({owner, WS::StreamingCellStateLedgerConfig::MaximumTrackedAttempts});
        if (residency.HasError())
            return Output::Failure(residency.ErrorValue());
        state->residency = std::make_unique<WS::StreamingCellStateLedger>(std::move(residency).Value());

        constexpr auto standalone = WS::WorldPartitionProjectProfile::Standalone;
        const WS::WorldStreamingRuntimeCompositionConfig
            config{.owner = owner,
                   .revision = Identity<WS::StreamingRuntimeCompositionRevision>(1),
                   .schedulerOwner = Identity<WS::StreamingSchedulerLedgerId>(generation),
                   .schedulerLimits = {.concurrentOperations = 4,
                                       .capacityUnits = 4,
                                       .concurrency = {standalone, Identity<WS::StreamingConcurrencyRevision>(1), 2, 2, 2}},
                   .maximumFeatureAdapters = 1};
        const auto binding = [](const WS::StreamingRuntimeServiceRole role, const void *service) {
            return WS::StreamingRuntimeServiceBinding{Identity<WS::StreamingRuntimeServiceId>(static_cast<std::uint64_t>(role) + 1),
                                                      Identity<WS::StreamingRuntimeServiceRevision>(1), role, service};
        };
        using enum WS::StreamingRuntimeServiceRole;
        const std::array services{binding(Planner, state->planner.get()), binding(AssetProvider, &state->cells),
                                  binding(SceneRuntime, &scene), binding(FeatureAdapter, state->residency.get())};
        auto origin = WS::OriginFrame::Create({Identity<WS::OriginFrameId>(generation), Identity<WS::OriginFrameRevision>(1),
                                               Identity<WS::OriginGeneration>(1)},
                                              selection.origin);
        if (origin.HasError())
            return Output::Failure(origin.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Cancelled));
        auto composition =
            WS::WorldStreamingRuntimeComposition::CreateWithOrigin(config, services, *state->registry, std::move(origin).Value());
        if (composition.HasError())
            return Output::Failure(composition.ErrorValue());
        state->composition.emplace(std::move(composition).Value());
        return Output::Success(std::unique_ptr<MountedWorld>{new MountedWorld{std::move(state)}});
    }

    /** @copydoc MountedWorld::ActivateInitialCell */
    Result<void> MountedWorld::ActivateInitialCell(const CommittedWorldInitialCell &selected, const MountedWorldStartup &boundary,
                                                   const Runtime::FrameContext *phase) {
        if (auto ready = CheckBoundary(boundary); ready.HasError())
            return ready;
        const auto parent = state_->scene.ActiveScene();
        if (!parent || state_->initialAttempted || !selected.cell.IsValid() || !selected.occurrence.IsValid() ||
            !selected.definition.IsValid() || selected.revision.value == 0)
            return Result<void>::Failure(MakeError(Runtime::SceneCellPayloadErrors::Invalid));
        const auto partition = state_->registry->Snapshot();
        if (partition.HasError())
            return Result<void>::Failure(partition.ErrorValue());
        const auto handle = partition.Value().Find(selected.cell);
        if (handle.HasError())
            return Result<void>::Failure(handle.ErrorValue());
        const auto record = partition.Value().Resolve(handle.Value());
        if (record.HasError())
            return Result<void>::Failure(record.ErrorValue());
        const auto metadata = std::ranges::lower_bound(state_->integrity, selected.cell, WS::StreamingCellCanonicalLess{},
                                                       &WS::CookedWorldCellManifestEntry::cell);
        const auto dependencies = std::ranges::lower_bound(state_->dependencies->cells, selected.cell, WS::StreamingCellCanonicalLess{},
                                                           &WS::WorldDependencyCellProjection::cell);
        if (metadata == state_->integrity.end() || dependencies == state_->dependencies->cells.end() || metadata->cell != selected.cell ||
            dependencies->cell != selected.cell)
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        if (!dependencies->hardTargets.empty())
            return Result<void>::Failure(
                MakeError(WS::WorldArtifactErrors::Unsupported,
                          "Standalone startup supports one cell. The selection requires a hard co-load transaction."));

        state_->initialAttempted = true;
        auto source = PlanInitialCell(*state_->planner, partition.Value(), handle.Value(), boundary);
        if (source.HasError())
            return Result<void>::Failure(source.ErrorValue());
        const auto &owner = state_->composition->Owner();
        const WS::StreamingFence fence{owner.partition, owner.epoch, selected.cell, Identity<WS::StreamingGeneration>(1)};
        const Runtime::SceneCellPayloadIdentity identity{owner.partition, selected.cell, selected.definition, selected.revision};
        auto &scheduler = state_->composition->Scheduler();
        using enum WS::StreamingCellOperationTransition;
        std::optional<Runtime::RuntimeSceneCellPayload> payload;
        const auto loaded = RunCellOperation(scheduler, *state_->residency, {Identity<WS::StreamingCellOperationId>(1), fence},
                                             WS::StreamingCellOperationKind::Load, {BeginPreparation}, Complete, [&](const auto &) {
            auto decoded = DecodeInitialCell(state_->cells, *partition.Value().Descriptor(), *record.Value(), *metadata, identity,
                                             boundary.cancellation);
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            if (!decoded.Value().Definition().AssetDependencies().empty())
                return Result<void>::Failure(
                    MakeError(Runtime::SceneCellPayloadErrors::Unsupported,
                              "The initial cell requires external assets whose runtime admission is not composed."));
            if (auto ready = CheckBoundary(boundary); ready.HasError())
                return ready;
            payload.emplace(std::move(decoded).Value());
            return Result<void>::Success();
        });
        if (loaded.HasError())
            return loaded;

        const Runtime::SceneInstanceAddress address{parent->RuntimeId(), selected.occurrence, selected.definition,
                                                    Runtime::SceneInstanceMode::Streamed};
        const auto authored = payload->WorldAnchors();
        State::ActiveCell active{fence, address, identity, {authored.begin(), authored.end()}, std::vector<Math::Vec3>(authored.size())};
        for (const auto &entity : payload->Definition().Entities()) {
            const auto anchor = std::ranges::lower_bound(authored, entity.object, {}, &Runtime::SceneCellWorldAnchor::object);
            if (anchor != authored.end() && anchor->object == entity.object)
                active.initialLocalPositions[static_cast<std::size_t>(anchor - authored.begin())] = entity.localTransform.translation;
        }
        const Runtime::SceneCellAttachmentRequest request{.runtime = address.parent,
                                                          .fence = fence,
                                                          .registry = parent->AssetRegistryRevision(),
                                                          .limits = {1, 65536, 1},
                                                          .cancellation = boundary.cancellation,
                                                          .ownerCancellation = boundary.cancellation};
        const auto activated = RunCellOperation(scheduler, *state_->residency, {Identity<WS::StreamingCellOperationId>(2), fence},
                                                WS::StreamingCellOperationKind::Activate, {BeginPreparation, BeginActivation}, Complete,
                                                [&](const auto &reservation) {
            if (auto ready = CheckBoundary(boundary); ready.HasError())
                return ready;
            auto authority = std::make_shared<const InitialCellAuthority>(identity, *state_->residency, scheduler, reservation);
            auto queued = Runtime::QueueStreamedSceneInstance(state_->scene, address, *payload, request, {}, std::move(authority));
            if (queued.HasError())
                return Result<void>::Failure(queued.ErrorValue());

            const Runtime::FrameContext frame{.frameNumber = 1, .cancellation = boundary.cancellation};
            const auto advanced = state_->scene.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, phase ? *phase : frame);
            if (advanced.HasError()) {
                state_->scene.Shutdown();
                return advanced;
            }
            if (const auto *error = queued.Value().Receipt()->Failure()) {
                state_->scene.Shutdown();
                return Result<void>::Failure(*error);
            }
            const auto current = state_->scene.ActiveScene();
            const auto *receipt = queued.Value().Receipt()->ResultValue();
            if (!receipt || receipt->baselinesAttached != 1 || !current ||
                !Runtime::FindStreamedSceneInstance(*current, address, identity, owner.epoch)) {
                state_->scene.Shutdown();
                return Result<void>::Failure(MakeError(Runtime::SceneCellPayloadErrors::Stale));
            }
            if (auto ready = CheckBoundary(boundary); ready.HasError()) {
                state_->scene.Shutdown();
                return ready;
            }
            return Result<void>::Success();
        });
        if (activated.HasError()) {
            state_->scene.Shutdown();
            return activated;
        }

        state_->initialSource.emplace(std::move(source).Value());
        state_->activeCell.emplace(std::move(active));
        return Result<void>::Success();
    }

    /** @copydoc MountedWorld::Anchors */
    std::span<const Runtime::SceneCellWorldAnchor> MountedWorld::Anchors(const Runtime::RuntimeSceneView &parent) const noexcept {
        if (!parent.IsCurrent() || !state_->activeCell)
            return {};
        const auto current = state_->scene.ActiveScene();
        const auto &active = *state_->activeCell;
        if (!current || !parent.SameIncarnation(*current) ||
            !Runtime::FindStreamedSceneInstance(parent, active.address, active.identity, active.fence.epoch))
            return {};
        return active.anchors;
    }

    /** @copydoc MountedWorld::CaptureGeometry */
    Result<MountedWorldRenderPacket> MountedWorld::CaptureGeometry(const Runtime::RuntimeSceneView &parent,
                                                                   const Runtime::SceneObjectId camera,
                                                                   const std::span<const Runtime::SceneObjectId> meshes,
                                                                   Runtime::PrimitiveMeshCache &primitives,
                                                                   Render::RenderOriginSceneAdapter &renderer) const {
        using Output = Result<MountedWorldRenderPacket>;
        constexpr std::size_t MaximumSelectedMeshes = 64;
        if (meshes.size() > MaximumSelectedMeshes)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::CapacityExceeded));
        if (!camera.IsValid() || !std::ranges::is_sorted(meshes) || std::ranges::adjacent_find(meshes) != meshes.end())
            return Output::Failure(
                MakeError(WS::WorldArtifactErrors::Invalid, "World geometry requires explicit unique ordered object identities."));
        if (!parent.IsCurrent())
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        const auto current = state_->scene.ActiveScene();
        if (!current || !parent.SameIncarnation(*current))
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        const auto anchors = Anchors(parent);
        if (anchors.empty())
            return Output::Failure(
                MakeError(WS::WorldArtifactErrors::Unsupported, "No canonical World positions are published for this selected cell."));
        if (!renderer.UsesOrigin(Origin()))
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale, "The renderer belongs to another World origin."));

        const auto &active = *state_->activeCell;
        const auto baseline = Runtime::FindStreamedSceneInstance(parent, active.address, active.identity, active.fence.epoch);
        if (!baseline)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        const auto lookup = [&](const Runtime::SceneObjectId object) {
            return FindAnchoredGeometryEntity(parent, *baseline, anchors, active.initialLocalPositions, object);
        };
        const auto cameraEntity = lookup(camera);
        if (cameraEntity.HasError())
            return Output::Failure(cameraEntity.ErrorValue());
        const auto originCamera = CameraFor(cameraEntity.Value());
        if (originCamera.HasError())
            return Output::Failure(originCamera.ErrorValue());

        std::vector<Runtime::PrimitiveMeshLease> leases;
        std::vector<Render::RenderMeshResourceView> resources;
        std::vector<Render::RenderOriginInstance> instances;
        leases.reserve(meshes.size());
        resources.reserve(meshes.size());
        instances.reserve(meshes.size());
        for (const auto object : meshes) {
            const auto placed = lookup(object);
            if (placed.HasError())
                return Output::Failure(placed.ErrorValue());
            const auto &entity = placed.Value().entity;
            if (!*entity.primitiveMesh)
                return Output::Failure(
                    MakeError(WS::WorldArtifactErrors::Unsupported, "The selected World object has no admitted procedural mesh."));
            auto acquired = primitives.Acquire(**entity.primitiveMesh);
            if (acquired.HasError())
                return Output::Failure(acquired.ErrorValue());
            auto lease = std::move(acquired).Value();
            const Render::RenderMeshSourceHandle handle{lease.Id(), 1};
            const auto &resource = lease.Data();
            if (std::ranges::find(resources, handle, &Render::RenderMeshResourceView::handle) == resources.end()) {
                resources.emplace_back(handle, resource.vertices, resource.indices, resource.localBounds);
                leases.push_back(std::move(lease));
            }
            auto transform = *entity.localTransform;
            transform.translation = {};
            auto basis = transform.TryToMatrix();
            if (basis.HasError())
                return Output::Failure(basis.ErrorValue());
            instances.emplace_back(Global(placed.Value().anchor.positionMillimeters),
                                   Render::RenderStaticMeshInstance{handle, basis.Value(), resource.localBounds});
        }

        auto frame = renderer.Capture({originCamera.Value(), instances, {}, parent.RuntimeId().value});
        if (frame.HasError())
            return Output::Failure(frame.ErrorValue());
        return Output::Success(MountedWorldRenderPacket{parent, std::move(frame).Value(), std::move(leases), std::move(resources)});
    }

    MountedWorldRenderPacket::MountedWorldRenderPacket(Runtime::RuntimeSceneView parent, Render::RenderOriginFrame frame,
                                                       std::vector<Runtime::PrimitiveMeshLease> leases,
                                                       std::vector<Render::RenderMeshResourceView> resources) noexcept
        : parent_(std::move(parent)), frame_(std::move(frame)), leases_(std::move(leases)), resources_(std::move(resources)) {}

    /** @copydoc MountedWorldRenderPacket::Scene */
    Result<Render::RenderSceneView> MountedWorldRenderPacket::Scene() const {
        if (!parent_.IsCurrent())
            return Result<Render::RenderSceneView>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        return frame_.Scene(resources_);
    }

    MountedWorld::MountedWorld(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc MountedWorld::Origin */
    const WS::OriginFrameOwner &MountedWorld::Origin() const noexcept {
        return *state_->composition->Origin();
    }

    /** @copydoc MountedWorld::~MountedWorld */
    MountedWorld::~MountedWorld() {
        if (state_->activeCell) {
            using enum WS::StreamingCellOperationTransition;
            const auto &fence = state_->activeCell->fence;
            auto retired =
                RunCellOperation(state_->composition->Scheduler(), *state_->residency, {Identity<WS::StreamingCellOperationId>(3), fence},
                                 WS::StreamingCellOperationKind::Retire, {BeginRetirement}, AcknowledgeRetirement, [&](const auto &) {
                // Parent shutdown performs actual synchronous removal of all owned baselines.
                state_->scene.Shutdown();
                return state_->scene.ActiveScene() ? Result<void>::Failure(MakeError(Runtime::SceneCellPayloadErrors::Stale))
                                                   : Result<void>::Success();
            });
            if (retired.HasError())
                state_->scene.Shutdown();
        }
        state_->initialSource.reset();
        static_cast<void>(state_->composition->BeginShutdown(state_->composition->Owner()));
        static_cast<void>(state_->residency->BeginShutdown(state_->composition->Owner()));
        static_cast<void>(state_->planner->Shutdown());
        state_->registry->Shutdown();
    }
}  // namespace Horo::Application::Internal
