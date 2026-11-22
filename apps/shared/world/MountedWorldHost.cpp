#include "MountedWorldHost.h"

#include "Horo/Assets/AssetPayloadCache.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Assets/NavMeshAssetType.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/RuntimeDispatchEvidence.h"
#include "Horo/Runtime/Scene/SceneCellPayloadCodec.h"
#include "Horo/WorldStreaming/SharedAssetResidency.h"
#include "Horo/WorldStreaming/StreamingCellInspection.h"
#include "Horo/WorldStreaming/StreamingCellState.h"
#include "Horo/WorldStreaming/StreamingOwnerFrameBudget.h"
#include "Horo/WorldStreaming/StreamingSourcePlanner.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "Horo/WorldStreaming/WorldDependencyArtifact.h"
#include "ScenePhysicsHost.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <thread>
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
        struct MountedNavigationMembership final {
            WS::StreamingCellId cell;
            Navigation::NavigationCellTileSelection selection;
        };

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

        /** @brief Retains verified membership only when its complete base envelope exists in the selected inventory. */
        Result<void> CaptureNavigation(const Assets::AssetCookGenerationContents &contents, const WS::StreamingCellHeaderView &cell,
                                       std::span<const std::byte> bytes, std::vector<MountedNavigationMembership> &out) {
            const auto row =
                std::ranges::find(cell.payloads, WS::StreamingCellProvider::NavigationMesh, &WS::StreamingCellPayloadHeader::provider);
            if (row == cell.payloads.end())
                return Result<void>::Success();
            if (cell.compression != WS::StreamingCellCompression::None ||
                row->requirement != WS::StreamingCellPayloadRequirement::Required ||
                row->version != Navigation::NavigationCellTileSelection::WireVersion)
                return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Unsupported));
            auto selected = Navigation::DecodeNavigationCellTileSelection(bytes.subspan(static_cast<std::size_t>(row->offset),
                                                                                        static_cast<std::size_t>(row->compressedSize)),
                                                                          MaximumCellBytes);
            if (selected.HasError())
                return Result<void>::Failure(selected.ErrorValue());
            const auto entry = std::ranges::find(contents.entries, selected.Value().definition, &Assets::AssetCookManifestEntry::assetId);
            if (entry == contents.entries.end() || entry->assetType.Value() != Assets::NavMeshAssetTypeName ||
                entry->artifactHash != selected.Value().cookedEnvelope)
                return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
            out.emplace_back(cell.cell, std::move(selected).Value());
            return Result<void>::Success();
        }

        /** @brief Verifies actual cell bytes against every corresponding index row before publishing a provider. */
        Result<void> LoadCells(Assets::MemoryAssetProvider &provider, const Assets::AssetCookGenerationContents &contents,
                               const Assets::AssetCookTargetId &target, const WS::DecodedWorldIndex &index,
                               const CancellationToken &cancellation, std::vector<MountedNavigationMembership> &navigation) {
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
                if (auto admitted = CaptureNavigation(contents, actual, std::as_bytes(std::span{artifact.Value().payload}), navigation);
                    admitted.HasError())
                    return admitted;
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
                                 const WS::StreamingSchedulerAdmissionLedger &scheduler, WS::StreamingSchedulerReservation reservation,
                                 MountedWorldStartup boundary) noexcept
                : identity_(identity), residency_(residency), scheduler_(scheduler), reservation_(reservation),
                  boundary_(std::move(boundary)) {}

            Result<void> ValidatePublication(const Runtime::SceneCellPayloadIdentity &identity,
                                             const WS::StreamingFence &fence) const override {
                if (const auto ready = CheckBoundary(boundary_); ready.HasError())
                    return ready;
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
            MountedWorldStartup boundary_;
        };

        /** @brief Verifies the exact retiring reservation still owns the attached cell at Scene commit. */
        class LiveCellRetirementAuthority final : public Runtime::SceneCellPayloadAuthority {
        public:
            LiveCellRetirementAuthority(const Runtime::SceneCellPayloadIdentity &identity, const WS::StreamingCellStateLedger &residency,
                                        const WS::StreamingSchedulerAdmissionLedger &scheduler,
                                        WS::StreamingSchedulerReservation reservation)
                : identity_(identity), residency_(residency), scheduler_(scheduler), reservation_(reservation) {}

            Result<void> ValidatePublication(const Runtime::SceneCellPayloadIdentity &identity,
                                             const WS::StreamingFence &fence) const override {
                const auto operation = scheduler_.Inspect(reservation_);
                const auto state = residency_.Resolve(residency_.Owner(), fence);
                if (identity != identity_ || fence != reservation_.operation.fence || operation.HasError() || state.HasError() ||
                    operation.Value().State() != WS::StreamingCellOperationState::Retiring ||
                    state.Value().operation != reservation_.operation || state.Value().state != WS::StreamingCellState::Evicting)
                    return Result<void>::Failure(MakeError(Runtime::SceneCellPayloadErrors::Stale));
                return Result<void>::Success();
            }

        private:
            Runtime::SceneCellPayloadIdentity identity_;
            const WS::StreamingCellStateLedger &residency_;
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

        /** @brief Retires the canonical reservation and reports the original rejected work if cleanup succeeded. */
        Result<void> RejectCellOperation(WS::StreamingSchedulerAdmissionLedger &scheduler, WS::StreamingCellStateLedger &residency,
                                         const WS::StreamingSchedulerReservation &reservation, const Error &error) {
            using enum WS::StreamingCellOperationTransition;
            const auto current = scheduler.Inspect(reservation);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            const auto state = residency.Resolve(residency.Owner(), reservation.operation.fence);
            const bool recorded = state.HasValue() && state.Value().operation == reservation.operation;
            const auto advance = [&](const auto step) {
                if (recorded)
                    return AdvanceCell(scheduler, residency, reservation, step);
                const auto unrecorded = scheduler.Advance(reservation, step);
                return unrecorded.HasError() ? Result<void>::Failure(unrecorded.ErrorValue()) : Result<void>::Success();
            };
            if (!current.Value().IsTerminal()) {
                if (current.Value().State() != WS::StreamingCellOperationState::Retiring) {
                    const auto code = error.code.Value();
                    const bool cancelled = code == Runtime::SceneCellPayloadErrors::Cancelled.code.Value() ||
                                           code == WS::WorldArtifactErrors::Cancelled.code.Value();
                    if (const auto interrupted = advance(cancelled ? Cancel : Fail); interrupted.HasError())
                        return interrupted;
                }
                if (const auto retired = advance(AcknowledgeRetirement); retired.HasError())
                    return retired;
            }
            if (auto released = scheduler.Release(reservation); released.HasError())
                return released;
            return Result<void>::Failure(error);
        }

        /** @brief Reserves and prepares one exact operation, keeping the reservation until its owner settles the work. */
        Result<WS::StreamingSchedulerReservation> BeginCellOperation(
            WS::StreamingSchedulerAdmissionLedger &scheduler, WS::StreamingCellStateLedger &residency,
            const WS::StreamingCellOperationHandle &handle, const WS::StreamingCellOperationKind kind,
            const std::initializer_list<WS::StreamingCellOperationTransition> setup) {
            using Output = Result<WS::StreamingSchedulerReservation>;
            const auto queued = WS::StreamingCellOperation::Create(handle, kind);
            if (queued.HasError())
                return Output::Failure(queued.ErrorValue());
            const auto admitted = scheduler.TryAdmit(queued.Value(), 1, scheduler.Limits().concurrency.revision);
            if (admitted.HasError())
                return admitted;
            const auto &reservation = admitted.Value();
            const auto fail = [&](const Error &error) {
                return Output::Failure(RejectCellOperation(scheduler, residency, reservation, error).ErrorValue());
            };
            // Retire goes directly from admission to Evicting.
            if (kind != WS::StreamingCellOperationKind::Retire) {
                const auto current = scheduler.Inspect(reservation);
                if (current.HasError())
                    return fail(current.ErrorValue());
                if (auto recorded = residency.Apply(residency.Owner(), current.Value()); recorded.HasError())
                    return fail(recorded.ErrorValue());
            }
            for (const auto step : setup)
                if (const auto advanced = AdvanceCell(scheduler, residency, reservation, step); advanced.HasError())
                    return fail(advanced.ErrorValue());
            return admitted;
        }

        /** @brief Run one synchronous, bounded cold operation; work returns only after its borrows and deferred work settle. */
        template <typename Work>
        Result<void> RunCellOperation(WS::StreamingSchedulerAdmissionLedger &scheduler, WS::StreamingCellStateLedger &residency,
                                      const WS::StreamingCellOperationHandle &handle, const WS::StreamingCellOperationKind kind,
                                      const std::initializer_list<WS::StreamingCellOperationTransition> setup,
                                      const WS::StreamingCellOperationTransition terminal, Work work) {
            const auto admitted = BeginCellOperation(scheduler, residency, handle, kind, setup);
            if (admitted.HasError())
                return Result<void>::Failure(admitted.ErrorValue());
            const auto &reservation = admitted.Value();
            const auto result = work(reservation);
            if (result.HasError())
                return kind == WS::StreamingCellOperationKind::Retire
                           ? result
                           : RejectCellOperation(scheduler, residency, reservation, result.ErrorValue());
            if (const auto finished = AdvanceCell(scheduler, residency, reservation, terminal); finished.HasError())
                return RejectCellOperation(scheduler, residency, reservation, finished.ErrorValue());
            return scheduler.Release(reservation);
        }

        struct CellDecodeOptions final {
            std::size_t maximumEntities{65536};
            std::size_t maximumAssets{MaximumCells};
            bool hasNavigation{};
        };

        /** @brief Admit the committed cell's providers against its independent authored identity. */
        Result<Runtime::RuntimeSceneCellPayload> DecodeCell(const Assets::MemoryAssetProvider &provider,
                                                            const WS::WorldPartitionDescriptor &partition,
                                                            const WS::WorldPartitionCellDescriptor &cell,
                                                            const WS::CookedWorldCellManifestEntry &metadata,
                                                            const Runtime::SceneCellPayloadIdentity &identity,
                                                            const CancellationToken &cancellation, const CellDecodeOptions &options = {}) {
            using Output = Result<Runtime::RuntimeSceneCellPayload>;
            const auto bytes = provider.Load(cell.package.chunkAsset, cancellation);
            if (bytes.HasError())
                return Output::Failure(bytes.ErrorValue());
            const auto artifact = std::as_bytes(std::span{bytes.Value()});
            auto inspected = WS::InspectStreamingCellArtifact(artifact, metadata.artifactHash, CellLimits, cancellation);
            if (inspected.HasError())
                return Output::Failure(inspected.ErrorValue());
            const auto header = inspected.Value().Header();
            const auto expectedProviders = options.hasNavigation ? 2U : 1U;
            if (header.compression != WS::StreamingCellCompression::None || header.payloads.size() != expectedProviders ||
                header.payloads.front().provider != WS::StreamingCellProvider::CoreEcs ||
                header.payloads.front().requirement != WS::StreamingCellPayloadRequirement::Required ||
                (options.hasNavigation && header.payloads.back().provider != WS::StreamingCellProvider::NavigationMesh))
                return Output::Failure(MakeError(Runtime::SceneCellPayloadErrors::Unsupported));
            const auto &core = header.payloads.front();
            const auto body = artifact.subspan(static_cast<std::size_t>(core.offset), static_cast<std::size_t>(core.compressedSize));
            if (!Runtime::MatchesSceneCellPayloadWireVersion(body, core.version))
                return Output::Failure(MakeError(Runtime::SceneCellPayloadErrors::Unsupported));
            return Runtime::DecodeSceneCellPayload(partition, body, identity, {}, {},
                                                   {MaximumCellBytes,
                                                    MaximumCellBytes,
                                                    {options.maximumEntities, options.maximumAssets, MaximumCellBytes}},
                                                   cancellation);
        }

        struct MountedCellAsset final {
            Assets::AssetId id;
            std::vector<std::uint8_t> envelope;
        };

        /** @brief Transfer already verified immutable envelopes from the selected commit to the worker-visible inventory. */
        std::vector<MountedCellAsset> CaptureCellAssets(Assets::AssetCookGenerationContents &contents,
                                                        std::span<const MountedNavigationMembership> navigation, bool includeCellAssets) {
            std::vector<MountedCellAsset> assets;
            for (std::size_t index = 0; index < contents.entries.size(); ++index) {
                const auto &entry = contents.entries[index];
                const auto type = entry.assetType.Value();
                if (type == "world.index" || type == "world.dependencies" || type == "world.cell")
                    continue;
                if (!includeCellAssets && std::ranges::none_of(navigation, [&](const auto &row) {
                    return row.selection.definition == entry.assetId;
                }))
                    continue;
                assets.emplace_back(entry.assetId, std::move(contents.artifacts[index]));
            }
            return assets;
        }

        struct DetachedCellAsset final {
            Assets::AssetId id;
            Assets::AssetPayloadRetainedBytes bytes;
        };

        struct PreparedCellAssets final {
            std::vector<Runtime::RuntimeGroupAssetLease> resources;
            std::vector<DetachedCellAsset> newAllocations;
            std::vector<WS::SharedAssetLease> claims;
        };

        struct PreparedLiveCell final {
            Runtime::RuntimeSceneCellPayload payload;
            PreparedCellAssets assets;
        };

        /** @brief Projects an exact retained CPU allocation into the complete streaming budget vector. */
        WS::StreamingBudgetAmounts CpuPayloadCharge(const std::size_t bytes) {
            std::array<WS::StreamingBudgetAmount, WS::StreamingBudgetDimensionCount> amounts{};
            for (std::size_t index = 0; index < amounts.size(); ++index)
                amounts[index].dimension = static_cast<WS::StreamingBudgetDimension>(index);
            amounts[static_cast<std::size_t>(WS::StreamingBudgetDimension::CpuResidentBytes)].value = bytes;
            return WS::StreamingBudgetAmounts::Create(amounts).Value();
        }

        /** @brief Mount-owned payload pins and authority tickets, immutable to the worker during its single in-flight load. */
        class MountedLiveAssets final {
        public:
            struct Entry final {
                Assets::AssetId id;
                Assets::AssetPayloadLease pin;
                Assets::AssetPayloadRetainedBytes bytes;
                std::optional<WS::SharedAssetRetirement> retiring;
            };

            MountedLiveAssets(WS::SharedAssetResidencyLedger ledger, const std::size_t capacity, const std::size_t entries)
                : ledger_(std::move(ledger)), capacity_(capacity) {
                entries_.reserve(entries);
            }

            /** @brief Look up the exact retained allocation in this immutable cook generation. */
            [[nodiscard]] const Entry *Find(const Assets::AssetId id) const {
                const auto entry = std::ranges::lower_bound(entries_, id, {}, &Entry::id);
                return entry != entries_.end() && entry->id == id ? std::to_address(entry) : nullptr;
            }

            /** @brief Project the physically retained CPU payload budget, including retiring allocations. */
            [[nodiscard]] std::size_t ChargedBytes() const {
                return static_cast<std::size_t>(ledger_.Charged(WS::StreamingBudgetDimension::CpuResidentBytes).Value());
            }

            /** @brief Only this uncharged capacity may be used to prepare new worker allocations. */
            [[nodiscard]] std::size_t AvailableBytes() const {
                return capacity_ - ChargedBytes();
            }

            /** @brief Transfer new allocations to the mount and acquire one authority lease per Scene consumer. */
            [[nodiscard]] Result<void> Track(const PreparedCellAssets &prepared, const WS::StreamingFence &fence,
                                             std::vector<WS::SharedAssetLease> &claims) {
                for (const auto &resource : prepared.resources) {
                    const auto id = resource.metadata.id;
                    const auto entry = std::ranges::lower_bound(entries_, id, {}, &Entry::id);
                    const bool existing = entry != entries_.end() && entry->id == id;
                    const Assets::AssetPayloadRetainedBytes *bytes = nullptr;
                    if (existing) {
                        if (!entry->pin.SharesAllocationWith(resource.artifact))
                            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
                        bytes = &entry->bytes;
                    } else {
                        const auto detached = std::ranges::lower_bound(prepared.newAllocations, id, {}, &DetachedCellAsset::id);
                        if (detached == prepared.newAllocations.end() || detached->id != id)
                            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
                        bytes = &detached->bytes;
                    }
                    // The mount pins one exact verified manifest. Its epoch is the immutable content revision in this ledger.
                    const WS::SharedAssetKey key{id, Identity<WS::SharedAssetRevision>(fence.epoch.Value())};
                    auto claimed = ledger_.Acquire(key, CpuPayloadCharge(bytes->Bytes()), {.fence = fence}, fence);
                    if (claimed.HasError())
                        return Result<void>::Failure(claimed.ErrorValue());
                    if (!existing)
                        entries_.emplace(entry, id, resource.artifact, *bytes);
                    claims.push_back(std::move(claimed).Value());
                }
                return Result<void>::Success();
            }

            /** @brief Release consumers after Scene detach, leaving charges until final physical release. */
            [[nodiscard]] Result<void> Release(std::vector<WS::SharedAssetLease> &claims) {
                while (!claims.empty()) {
                    const auto claim = claims.back();
                    if (const auto released = ledger_.Release(claim); released.HasError())
                        return released;
                    claims.pop_back();
                    auto retiring = ledger_.BeginRetire(claim.key, claim.charge);
                    if (retiring.HasError()) {
                        if (retiring.ErrorValue().code.Value() == WS::WorldStreamingErrors::SharedAssetLifecycleUnavailable.code.Value())
                            continue;  // A sibling cell still owns an exact consumer lease.
                        return Result<void>::Failure(retiring.ErrorValue());
                    }
                    auto entry = std::ranges::lower_bound(entries_, claim.key.asset, {}, &Entry::id);
                    entry->retiring = std::move(retiring).Value();
                    entry->pin = {};
                    ++retiring_;
                }
                return ReconcileRetired();
            }

            /** @brief Reports whether cache retirement still needs an ordinary structural cutoff. */
            [[nodiscard]] bool HasRetiring() const noexcept {
                return retiring_ != 0;
            }

            /** @brief Reclaim capacity only when the last pin has physically freed its vector capacity. */
            [[nodiscard]] Result<void> ReconcileRetired() {
                if (!HasRetiring())
                    return Result<void>::Success();
                for (auto entry = entries_.begin(); entry != entries_.end();) {
                    if (!entry->retiring || entry->bytes.Bytes() != 0) {
                        ++entry;
                        continue;
                    }
                    if (const auto retired = ledger_.AcknowledgeRetired(*entry->retiring); retired.HasError())
                        return retired;
                    entry = entries_.erase(entry);
                    --retiring_;
                }
                return Result<void>::Success();
            }

        private:
            WS::SharedAssetResidencyLedger ledger_;
            std::size_t capacity_;
            std::vector<Entry> entries_;
            std::size_t retiring_{};
        };

        /** @brief Validates an exact cooked envelope and reserves its retained bytes in the uncharged pool. */
        Result<void> PrepareDetachedCellAsset(const MountedCellAsset &asset, const Runtime::SceneAssetDependency &dependency,
                                              const Assets::AssetCookTargetId &target, PreparedCellAssets &prepared,
                                              std::size_t &remainingPool) {
            if (asset.envelope.size() > remainingPool)
                return Result<void>::Failure(MakeError(Runtime::SceneCellPayloadErrors::CapacityExceeded));
            const auto decoded = Assets::DecodeCookedArtifact(asset.envelope, CookLimits);
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            const auto &header = decoded.Value();
            if (header.target != target || header.type != dependency.expectedType || header.id != dependency.id)
                return Result<void>::Failure(
                    MakeError(WS::WorldArtifactErrors::Invalid, "A mounted cell asset does not match the World target."));
            // An exact one-allocation cache provides a retirement observation without pinning its payload.
            const auto cache = Assets::AssetPayloadCache::Create(1, remainingPool);
            if (cache.HasError())
                return Result<void>::Failure(cache.ErrorValue());
            auto lease = cache.Value()->Admit(std::as_bytes(std::span{asset.envelope}));
            if (lease.HasError())
                return Result<void>::Failure(lease.ErrorValue());
            auto bytes = cache.Value()->ObserveRetainedPayloadBytes();
            remainingPool -= bytes.Bytes();
            prepared.resources.emplace_back(dependency, std::move(lease).Value());
            prepared.newAllocations.emplace_back(dependency.id, std::move(bytes));
            return Result<void>::Success();
        }

        /** @brief Resolves every declared dependency against the pinned registry and the exact committed World envelopes. */
        Result<PreparedCellAssets> PrepareCellAssets(std::span<const Runtime::SceneAssetDependency> dependencies,
                                                     std::span<const MountedCellAsset> inventory,
                                                     const Assets::AssetRegistrySnapshot &registry, const Assets::AssetCookTargetId &target,
                                                     const MountedWorldLiveLimits &limits, const MountedLiveAssets *shared,
                                                     const CancellationToken &cancellation) {
            using Output = Result<PreparedCellAssets>;
            if (dependencies.empty())
                return Output::Success({});
            if (!shared)
                return Output::Failure(MakeError(Runtime::SceneCellPayloadErrors::Unsupported));
            if (dependencies.size() > limits.maximumCellAssets)
                return Output::Failure(MakeError(Runtime::SceneCellPayloadErrors::CapacityExceeded));
            PreparedCellAssets prepared;
            prepared.resources.reserve(dependencies.size());
            prepared.newAllocations.reserve(dependencies.size());
            prepared.claims.reserve(dependencies.size());
            std::size_t remainingCell = limits.maximumCellAssetBytes;
            std::size_t remainingPool = shared->AvailableBytes();
            for (const auto &dependency : dependencies) {
                if (cancellation.IsCancellationRequested())
                    return Output::Failure(MakeError(WS::WorldArtifactErrors::Cancelled));
                const auto *record = registry.Find(dependency.id);
                const auto found = std::ranges::lower_bound(inventory, dependency.id, {}, &MountedCellAsset::id);
                if (!record || record->type != dependency.expectedType || found == inventory.end() || found->id != dependency.id)
                    return Output::Failure(
                        MakeError(WS::WorldArtifactErrors::Invalid, "A mounted cell asset is absent or has the wrong type."));
                if (found->envelope.size() > remainingCell)
                    return Output::Failure(MakeError(Runtime::SceneCellPayloadErrors::CapacityExceeded));
                remainingCell -= found->envelope.size();
                if (const auto *existing = shared->Find(dependency.id)) {
                    if (existing->retiring)
                        return Output::Failure(MakeError(Runtime::SceneCellPayloadErrors::CapacityExceeded));
                    prepared.resources.emplace_back(dependency, existing->pin);
                    continue;
                }
                if (const auto detached = PrepareDetachedCellAsset(*found, dependency, target, prepared, remainingPool);
                    detached.HasError())
                    return Output::Failure(detached.ErrorValue());
            }
            return Output::Success(std::move(prepared));
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

    /** @copydoc ParseMountedWorldLiveLimits */
    Result<MountedWorldLiveLimits> ParseMountedWorldLiveLimits(const std::span<char *const> arguments) {
        using Output = Result<MountedWorldLiveLimits>;
        const auto parse = [](const std::string_view value, auto &limit) {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), limit);
            return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
        };
        MountedWorldLiveLimits limits;
        if (arguments.size() != 3 || !parse(arguments[0], limits.maximumFrameNanoseconds) ||
            !parse(arguments[1], limits.maximumUnitNanoseconds) || !parse(arguments[2], limits.maximumCellEntities) || !limits.IsValid())
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Invalid, "Invalid live World frame or entity limits."));
        return Output::Success(limits);
    }

    struct MountedWorld::State final {
        explicit State(Runtime::RuntimeSceneService &scene) : scene(scene) {}

        Runtime::RuntimeSceneService &scene;
        const std::thread::id ownerThread{std::this_thread::get_id()};
        std::shared_ptr<Assets::MemoryAssetProvider> cells = std::make_shared<Assets::MemoryAssetProvider>();
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
            bool navigationHasCoverage{};
        };

        struct PreparedCell final {
            WS::StreamingSourceRegistration source;
            Runtime::RuntimeSceneCellPayload payload;
            ActiveCell cell;
        };

        struct PendingCell final {
            WS::StreamingSourceRegistration source;
            ActiveCell cell;
            WS::StreamingSchedulerReservation reservation;
            std::optional<Runtime::ScenePublicationReceipt> publication;
            std::shared_ptr<const InitialCellAuthority> authority;
        };

        /** @brief Returns the exact startup cell only while the Scene service and scheduler still admit this candidate. */
        [[nodiscard]] Result<const ActiveCell *> InitialCellFor(const Runtime::RuntimeSceneView &candidate) const {
            using Output = Result<const ActiveCell *>;
            if (!pendingCell || !scene.IsInitialStreamedCandidate(candidate) || candidate.RuntimeId() != pendingCell->cell.address.parent)
                return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
            const auto &cell = pendingCell->cell;
            if (const auto ready = pendingCell->authority->ValidatePublication(cell.identity, cell.fence); ready.HasError())
                return Output::Failure(ready.ErrorValue());
            return Output::Success(&cell);
        }

        /** @brief Loads verified Core ECS content through the actual mounted planner and residency ledgers. */
        [[nodiscard]] Result<PreparedCell> PrepareInitialCell(const CommittedWorldInitialCell &selected,
                                                              const MountedWorldStartup &boundary, Runtime::SceneRuntimeId parent);

        /** @brief Finds the currently published parent of the exact mounted cell occurrence. */
        [[nodiscard]] std::optional<Runtime::RuntimeSceneView> AttachedParent(const ActiveCell &cell) const noexcept {
            const auto current = scene.ActiveScene();
            if (current && Runtime::FindStreamedSceneInstance(*current, cell.address, cell.identity, cell.fence.epoch))
                return current;
            return {};
        }

        std::optional<ActiveCell> activeCell;
        std::optional<PendingCell> pendingCell;
        std::optional<CancellationSource> pendingLifetime;
        bool initialAttempted{};

        struct LiveDemand final {
            CommittedWorldInitialCell selected;
            CancellationToken cancellation;
            CancellationSource lifetime;
            bool withdrawn{};
            bool forcedEviction{};

            /** @brief Returns whether this exact occurrence must stop work. */
            [[nodiscard]] bool IsWithdrawn() const noexcept {
                return withdrawn || cancellation.IsCancellationRequested();
            }

            /** @brief Closes the occurrence and its worker or Scene owner admission. */
            void Withdraw() {
                withdrawn = true;
                lifetime.RequestCancellation();
            }
        };

        struct LiveLoad final {
            JobHandle job;
            // Only the worker writes the slot. The owner reads it after the job record publishes terminal status.
            std::shared_ptr<std::optional<PreparedLiveCell>> payload;
        };

        struct LiveResident final {
            ActiveCell cell;
            LiveDemand demand;
            Runtime::SceneInstanceOperation attachment;
            std::vector<WS::SharedAssetLease> claims;
        };

        struct LiveStreaming final {
            Runtime::RuntimeDispatchSource dispatch;
            const JobSystem &jobs;
            MountedWorldLiveLimits limits;
            Assets::AssetRegistrySnapshot assets;
            MountedWorldNavigationPublication navigation;
            MountedWorldNavigationRetirement navigationRetired;
            std::uint64_t lastFrame{};
            std::uint64_t nextIdentity{4}; /**< Distinct from the three cold-start operations. */
            std::optional<LiveDemand> demand;
            std::optional<ActiveCell> candidate;
            std::optional<LiveLoad> load;
            std::shared_ptr<std::optional<PreparedLiveCell>> prepared;
            std::optional<WS::StreamingSchedulerReservation> reservation;
            std::optional<Runtime::SceneInstanceOperation> attachment;
            std::optional<MountedLiveAssets> sharedAssets;
            std::vector<WS::SharedAssetLease> claims;
            std::vector<LiveResident> residents;
            std::optional<std::size_t> retiring;
            std::optional<Runtime::SceneInstanceOperation> removal;

            /** @brief Looks up the exact resident occurrence without matching logical cell coordinates alone. */
            [[nodiscard]] auto FindResident(Runtime::SceneInstanceId occurrence) noexcept {
                return std::ranges::find_if(residents, [occurrence](const LiveResident &resident) {
                    return resident.demand.selected.occurrence == occurrence;
                });
            }

            /** @brief Returns the in-flight retirement or the next withdrawn live occurrence. */
            [[nodiscard]] std::optional<std::size_t> RetirementCandidate() const noexcept {
                if (retiring.has_value())
                    return retiring;
                const auto found = std::ranges::find_if(residents, [](const LiveResident &resident) {
                    return resident.demand.IsWithdrawn();
                });
                if (found == residents.end())
                    return std::nullopt;
                return static_cast<std::size_t>(found - residents.begin());
            }
        };

        Assets::AssetCookTargetId target;
        std::shared_ptr<const std::vector<MountedCellAsset>> committedAssets;
        std::vector<MountedNavigationMembership> navigationCells;
        std::optional<LiveStreaming> live;

        /** @brief Looks up admitted membership for this exact committed cell, if required. */
        [[nodiscard]] const MountedNavigationMembership *NavigationCell(WS::StreamingCellId id) const noexcept {
            const auto found = std::ranges::find(navigationCells, id, &MountedNavigationMembership::cell);
            return found == navigationCells.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] Result<MountedWorldLiveCellState> StartLiveLoad(const Runtime::RuntimeSceneView &parent);
        [[nodiscard]] Result<MountedWorldLiveCellState> PollLiveLoad();
        [[nodiscard]] Result<MountedWorldLiveCellState> QueueLiveAttachment(const Runtime::RuntimeSceneView &parent);
        [[nodiscard]] Result<MountedWorldLiveCellState> PollLiveAttachment();
        [[nodiscard]] Result<MountedWorldLiveCellState> RetireLiveCell(const Runtime::RuntimeSceneView &parent, std::size_t index);
        /** @brief Builds the exact aggregate command owner for an added or removed cell with Navigation content. */
        [[nodiscard]] Result<MountedWorldNavigationPreparation> PrepareNavigationChange(const Runtime::RuntimeSceneView &parent,
                                                                                        WS::StreamingCellId cell,
                                                                                        MountedWorldNavigationChange change) const;
        [[nodiscard]] Result<MountedWorldLiveCellState> RejectLive(const Error &error);
        /** @brief Reserves retirement of one exact Resident or Active cell until its resources have settled. */
        [[nodiscard]] Result<void> BeginLiveRetirement(const ActiveCell &cell);
        /** @brief Releases a private Resident candidate after its payload is discarded. */
        [[nodiscard]] Result<void> DiscardPreparedCell();
        [[nodiscard]] Result<void> SettleLiveReservation(WS::StreamingCellOperationTransition terminal);
        [[nodiscard]] Result<WS::StreamingCellOperationId> IssueLiveOperation();
        [[nodiscard]] MountedWorldLiveCellState LiveStage() const noexcept;

        /** @brief Returns the single occurrence available to geometry callers with an implicit selection. */
        [[nodiscard]] const ActiveCell *ImplicitActiveCell() const noexcept {
            if (activeCell)
                return &*activeCell;
            return live && live->residents.size() == 1 ? &live->residents.front().cell : nullptr;
        }

        /** @brief Drains outstanding live work at the cold owner shutdown boundary. */
        void CloseLiveCell() noexcept;
    };

    /** @copydoc MountedWorld::Prepare */
    Result<std::unique_ptr<MountedWorld>> MountedWorld::Prepare(const DurableFileSystem &files, const CommittedWorldSelection &selection,
                                                                const std::uint64_t generation, Runtime::RuntimeSceneService &scene,
                                                                const CancellationToken &cancellation) {
        using Output = Result<std::unique_ptr<MountedWorld>>;
        if (generation == 0 || !selection.targetRoot.is_absolute() || !selection.target.IsValid() || !selection.index.IsValid() ||
            !selection.dependencies.IsValid() || selection.index == selection.dependencies ||
            (selection.liveCell && !selection.liveCell->limits.IsValid()))
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
        auto contents = Assets::ReadCookGenerationContents(files, published.Value(), MaximumBytes, CookLimits);
        if (contents.HasError())
            return Output::Failure(contents.ErrorValue());
        auto inventory = std::move(contents).Value();

        const auto indexArtifact = ReadArtifact(inventory, selection.target, selection.index, "world.index");
        if (indexArtifact.HasError())
            return Output::Failure(indexArtifact.ErrorValue());
        auto index = WS::DecodeWorldIndexArtifact(std::as_bytes(std::span{indexArtifact.Value().payload}), IndexLimits, cancellation);
        if (index.HasError())
            return Output::Failure(index.ErrorValue());
        if (index.Value().coordinates != WS::WorldIndexCoordinateSystem::RightHandedYUp)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Unsupported, "Standalone World mounting requires Horo world axes."));
        const auto bundle = ReadArtifact(inventory, selection.target, selection.dependencies, "world.dependencies");
        if (bundle.HasError())
            return Output::Failure(bundle.ErrorValue());
        auto dependencies = WS::DecodeWorldDependencyArtifact(std::as_bytes(std::span{bundle.Value().payload}), index.Value(),
                                                              DependencyLimits, cancellation);
        if (dependencies.HasError())
            return Output::Failure(dependencies.ErrorValue());

        auto state = std::make_unique<State>(scene);
        if (auto loaded = LoadCells(*state->cells, inventory, selection.target, index.Value(), cancellation, state->navigationCells);
            loaded.HasError())
            return Output::Failure(loaded.ErrorValue());
        state->target = selection.target;
        const bool captureCellAssets = selection.liveCell && selection.liveCell->limits.maximumCellAssets != 0;
        if (captureCellAssets || !state->navigationCells.empty())
            state->committedAssets = std::make_shared<const std::vector<MountedCellAsset>>(
                CaptureCellAssets(inventory, state->navigationCells, captureCellAssets));
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
        const std::array services{binding(Planner, state->planner.get()), binding(AssetProvider, state->cells.get()),
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

    /** @copydoc MountedWorld::State::PrepareInitialCell */
    Result<MountedWorld::State::PreparedCell> MountedWorld::State::PrepareInitialCell(const CommittedWorldInitialCell &selected,
                                                                                      const MountedWorldStartup &boundary,
                                                                                      const Runtime::SceneRuntimeId parent) {
        using Output = Result<PreparedCell>;
        if (auto ready = CheckBoundary(boundary); ready.HasError())
            return Output::Failure(ready.ErrorValue());
        if (!parent.IsValid() || initialAttempted || live || !selected.cell.IsValid() || !selected.occurrence.IsValid() ||
            !selected.definition.IsValid() || selected.revision.value == 0)
            return Output::Failure(MakeError(Runtime::SceneCellPayloadErrors::Invalid));
        const auto partition = registry->Snapshot();
        if (partition.HasError())
            return Output::Failure(partition.ErrorValue());
        const auto handle = partition.Value().Find(selected.cell);
        if (handle.HasError())
            return Output::Failure(handle.ErrorValue());
        const auto record = partition.Value().Resolve(handle.Value());
        if (record.HasError())
            return Output::Failure(record.ErrorValue());
        const auto metadata =
            std::ranges::lower_bound(integrity, selected.cell, WS::StreamingCellCanonicalLess{}, &WS::CookedWorldCellManifestEntry::cell);
        const auto cellDependencies = std::ranges::lower_bound(dependencies->cells, selected.cell, WS::StreamingCellCanonicalLess{},
                                                               &WS::WorldDependencyCellProjection::cell);
        if (metadata == integrity.end() || cellDependencies == dependencies->cells.end() || metadata->cell != selected.cell ||
            cellDependencies->cell != selected.cell)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        if (!cellDependencies->hardTargets.empty())
            return Output::Failure(
                MakeError(WS::WorldArtifactErrors::Unsupported,
                          "Mounted World startup supports one cell. The selection requires a hard co-load transaction."));

        initialAttempted = true;
        auto source = PlanInitialCell(*planner, partition.Value(), handle.Value(), boundary);
        if (source.HasError())
            return Output::Failure(source.ErrorValue());
        const auto &owner = composition->Owner();
        const WS::StreamingFence fence{owner.partition, owner.epoch, selected.cell, Identity<WS::StreamingGeneration>(1)};
        const Runtime::SceneCellPayloadIdentity identity{owner.partition, selected.cell, selected.definition, selected.revision};
        auto &scheduler = composition->Scheduler();
        using enum WS::StreamingCellOperationTransition;
        std::optional<Runtime::RuntimeSceneCellPayload> payload;
        const auto loaded = RunCellOperation(scheduler, *residency, {Identity<WS::StreamingCellOperationId>(1), fence},
                                             WS::StreamingCellOperationKind::Load, {BeginPreparation}, Complete, [&](const auto &) {
            auto decoded = DecodeCell(*cells, *partition.Value().Descriptor(), *record.Value(), *metadata, identity, boundary.cancellation);
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
            return Output::Failure(loaded.ErrorValue());

        const Runtime::SceneInstanceAddress address{parent, selected.occurrence, selected.definition, Runtime::SceneInstanceMode::Streamed};
        const auto authored = payload->WorldAnchors();
        State::ActiveCell active{fence, address, identity, {authored.begin(), authored.end()}, std::vector<Math::Vec3>(authored.size())};
        for (const auto &entity : payload->Definition().Entities()) {
            const auto anchor = std::ranges::lower_bound(authored, entity.object, {}, &Runtime::SceneCellWorldAnchor::object);
            if (anchor != authored.end() && anchor->object == entity.object)
                active.initialLocalPositions[static_cast<std::size_t>(anchor - authored.begin())] = entity.localTransform.translation;
        }
        return Output::Success({std::move(source).Value(), std::move(*payload), std::move(active)});
    }

    /** @copydoc MountedWorld::ActivateInitialCell */
    Result<void> MountedWorld::ActivateInitialCell(const CommittedWorldInitialCell &selected, const MountedWorldStartup &boundary,
                                                   const Runtime::FrameContext *phase) {
        const auto parent = state_->scene.ActiveScene();
        auto prepared = state_->PrepareInitialCell(selected, boundary, parent ? parent->RuntimeId() : Runtime::SceneRuntimeId{});
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        auto cell = std::move(prepared).Value();
        const auto &fence = cell.cell.fence;
        const auto &address = cell.cell.address;
        const auto &identity = cell.cell.identity;
        auto &scheduler = state_->composition->Scheduler();
        using enum WS::StreamingCellOperationTransition;
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
            auto authority = std::make_shared<const InitialCellAuthority>(identity, *state_->residency, scheduler, reservation, boundary);
            auto queued = Runtime::QueueStreamedSceneInstance(state_->scene, address, cell.payload, request, {}, std::move(authority));
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
                !Runtime::FindStreamedSceneInstance(*current, address, identity, fence.epoch)) {
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

        state_->initialSource.emplace(std::move(cell.source));
        state_->activeCell.emplace(std::move(cell.cell));
        return Result<void>::Success();
    }

    /** @copydoc MountedWorld::QueueInitialCell */
    Result<void> MountedWorld::QueueInitialCell(const Runtime::RuntimeSceneDefinition &parent, const CommittedWorldInitialCell &selected,
                                                const MountedWorldStartup &boundary, const Assets::AssetRegistryRevision registry) {
        if (state_->pendingCell)
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        auto &scheduler = state_->composition->Scheduler();
        auto &residency = *state_->residency;
        std::optional<WS::StreamingSchedulerReservation> reservation;
        const auto reject = [&](const Error &error) {
            if (state_->pendingLifetime)
                state_->pendingLifetime->RequestCancellation();
            state_->pendingCell.reset();
            state_->pendingLifetime.reset();
            return reservation ? RejectCellOperation(scheduler, residency, *reservation, error) : Result<void>::Failure(error);
        };
        try {
            auto prepared = state_->PrepareInitialCell(selected, boundary, state_->scene.NextRuntimeId());
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            auto data = std::move(prepared).Value();
            const auto &cell = data.cell;
            state_->pendingLifetime.emplace();
            using enum WS::StreamingCellOperationTransition;
            const auto admitted = BeginCellOperation(scheduler, residency, {Identity<WS::StreamingCellOperationId>(2), cell.fence},
                                                     WS::StreamingCellOperationKind::Activate, {BeginPreparation, BeginActivation});
            if (admitted.HasError())
                return reject(admitted.ErrorValue());
            reservation = admitted.Value();
            const Runtime::SceneCellAttachmentRequest request{.runtime = cell.address.parent,
                                                              .fence = cell.fence,
                                                              .registry = registry,
                                                              .limits = {1, 65536, 1},
                                                              .cancellation = boundary.cancellation,
                                                              .ownerCancellation = state_->pendingLifetime->Token()};
            auto authority = std::make_shared<const InitialCellAuthority>(cell.identity, residency, scheduler, *reservation, boundary);
            state_->pendingCell.emplace(
                State::PendingCell{std::move(data.source), std::move(data.cell), *reservation, {}, std::move(authority)});
            auto &pending = *state_->pendingCell;
            auto queued = Runtime::QueuePreparationWithInitialStreamedScene(state_->scene, parent, pending.cell.address, data.payload,
                                                                            request, pending.authority);
            if (queued.HasError())
                return reject(queued.ErrorValue());
            pending.publication.emplace(std::move(queued).Value());
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return reject(MakeError(WS::WorldArtifactErrors::CapacityExceeded));
        }
    }

    /** @copydoc MountedWorld::ReconcileInitialCell */
    Result<bool> MountedWorld::ReconcileInitialCell() {
        using Output = Result<bool>;
        if (!state_->pendingCell) {
            if (state_->activeCell && state_->AttachedParent(*state_->activeCell))
                return Output::Success(true);
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        }
        auto &pending = *state_->pendingCell;
        if (!pending.publication)
            return Output::Success(false);
        const auto observation = pending.publication->Snapshot();
        if (observation.HasError())
            return Output::Failure(observation.ErrorValue());
        const auto &published = observation.Value();
        if (published.status == Runtime::ScenePublicationStatus::Pending)
            return Output::Success(false);

        auto &scheduler = state_->composition->Scheduler();
        auto &residency = *state_->residency;
        const auto reject = [&](const Error &error) {
            const auto retired = RejectCellOperation(scheduler, residency, pending.reservation, error);
            state_->pendingCell.reset();
            state_->pendingLifetime.reset();
            return Output::Failure(retired.ErrorValue());
        };
        if (published.status != Runtime::ScenePublicationStatus::Published) {
            if (const auto error = state_->scene.TakeOperationError())
                return reject(*error);
            return reject(MakeError(published.status == Runtime::ScenePublicationStatus::Cancelled ? WS::WorldArtifactErrors::Cancelled
                                                                                                   : WS::WorldArtifactErrors::Stale));
        }

        const auto current = state_->AttachedParent(pending.cell);
        if (!current || published.scene != current->RuntimeId())
            return reject(MakeError(WS::WorldArtifactErrors::Stale));
        if (published.structuralRevision != current->StructuralRevision()) {
            state_->scene.Shutdown();
            return reject(MakeError(WS::WorldArtifactErrors::Stale));
        }
        auto completed = AdvanceCell(scheduler, residency, pending.reservation, WS::StreamingCellOperationTransition::Complete);
        if (completed.HasValue())
            completed = scheduler.Release(pending.reservation);
        if (completed.HasError()) {
            state_->scene.Shutdown();
            return reject(completed.ErrorValue());
        }

        state_->initialSource.emplace(std::move(pending.source));
        state_->activeCell.emplace(std::move(pending.cell));
        state_->pendingCell.reset();
        state_->pendingLifetime.reset();
        return Output::Success(true);
    }

    /** @copydoc MountedWorld::State::IssueLiveOperation */
    Result<WS::StreamingCellOperationId> MountedWorld::State::IssueLiveOperation() {
        if (live->nextIdentity == std::numeric_limits<std::uint64_t>::max())
            return Result<WS::StreamingCellOperationId>::Failure(MakeError(WS::WorldArtifactErrors::CapacityExceeded));
        return WS::StreamingCellOperationId::Create(live->nextIdentity++);
    }

    /** @copydoc MountedWorld::State::SettleLiveReservation */
    Result<void> MountedWorld::State::SettleLiveReservation(WS::StreamingCellOperationTransition terminal) {
        auto &scheduler = composition->Scheduler();
        const auto reservation = *live->reservation;
        const auto current = scheduler.Inspect(reservation);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (!current.Value().IsTerminal())
            if (const auto settled = AdvanceCell(scheduler, *residency, reservation, terminal); settled.HasError())
                return settled;
        auto released = scheduler.Release(reservation);
        if (released.HasValue())
            live->reservation.reset();
        return released;
    }

    /** @copydoc MountedWorld::State::BeginLiveRetirement */
    Result<void> MountedWorld::State::BeginLiveRetirement(const ActiveCell &cell) {
        if (live->reservation)
            return Result<void>::Success();
        const auto operation = IssueLiveOperation();
        if (operation.HasError())
            return Result<void>::Failure(operation.ErrorValue());
        const auto admitted =
            BeginCellOperation(composition->Scheduler(), *residency, {operation.Value(), cell.fence},
                               WS::StreamingCellOperationKind::Retire, {WS::StreamingCellOperationTransition::BeginRetirement});
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        live->reservation = admitted.Value();
        return Result<void>::Success();
    }

    /** @copydoc MountedWorld::State::DiscardPreparedCell */
    Result<void> MountedWorld::State::DiscardPreparedCell() {
        const auto current = residency->Resolve(residency->Owner(), live->candidate->fence);
        if (current.HasValue() && current.Value().state == WS::StreamingCellState::Resident) {
            const auto retired = BeginLiveRetirement(*live->candidate);
            if (retired.HasError())
                return retired;
        }
        live->prepared->reset();
        if (live->reservation) {
            const auto settled = SettleLiveReservation(WS::StreamingCellOperationTransition::AcknowledgeRetirement);
            if (settled.HasError())
                return settled;
        }
        live->prepared.reset();
        return Result<void>::Success();
    }

    /** @copydoc MountedWorld::State::RejectLive */
    Result<MountedWorldLiveCellState> MountedWorld::State::RejectLive(const Error &error) {
        auto &scheduler = composition->Scheduler();
        if (live->prepared && (!live->reservation || !*live->prepared)) {
            if (const auto discarded = DiscardPreparedCell(); discarded.HasError())
                return Result<MountedWorldLiveCellState>::Failure(discarded.ErrorValue());
        } else {
            live->prepared.reset();
        }
        const auto rejected =
            live->reservation ? RejectCellOperation(scheduler, *residency, *live->reservation, error) : Result<void>::Failure(error);
        if (live->reservation && scheduler.Inspect(*live->reservation).HasValue())
            return Result<MountedWorldLiveCellState>::Failure(rejected.ErrorValue());
        if (live->sharedAssets) {
            const auto released = live->sharedAssets->Release(live->claims);
            if (released.HasError())
                return Result<MountedWorldLiveCellState>::Failure(released.ErrorValue());
        }
        live->demand.reset();
        live->load.reset();
        live->candidate.reset();
        live->attachment.reset();
        live->reservation.reset();
        return Result<MountedWorldLiveCellState>::Failure(rejected.ErrorValue());
    }

    /** @copydoc MountedWorld::State::StartLiveLoad */
    Result<MountedWorldLiveCellState> MountedWorld::State::StartLiveLoad(const Runtime::RuntimeSceneView &parent) {
        using Stage = MountedWorldLiveCellState;
        const auto &demand = *live->demand;
        if (demand.IsWithdrawn()) {
            live->demand.reset();
            return Result<Stage>::Success(LiveStage());
        }
        const auto partition = registry->Snapshot();
        if (partition.HasError())
            return RejectLive(partition.ErrorValue());
        const auto &selected = demand.selected;
        const auto handle = partition.Value().Find(selected.cell);
        if (handle.HasError())
            return RejectLive(handle.ErrorValue());
        const auto descriptor = partition.Value().Resolve(handle.Value());
        if (descriptor.HasError())
            return RejectLive(descriptor.ErrorValue());
        const auto metadata =
            std::ranges::lower_bound(integrity, selected.cell, WS::StreamingCellCanonicalLess{}, &WS::CookedWorldCellManifestEntry::cell);
        const auto required = std::ranges::lower_bound(dependencies->cells, selected.cell, WS::StreamingCellCanonicalLess{},
                                                       &WS::WorldDependencyCellProjection::cell);
        if (metadata == integrity.end() || metadata->cell != selected.cell || required == dependencies->cells.end() ||
            required->cell != selected.cell || !required->hardTargets.empty())
            return RejectLive(MakeError(WS::WorldArtifactErrors::Unsupported));
        const auto operation = IssueLiveOperation();
        if (operation.HasError())
            return RejectLive(operation.ErrorValue());
        const auto &owner = composition->Owner();
        const WS::StreamingFence fence{owner.partition, owner.epoch, selected.cell,
                                       Identity<WS::StreamingGeneration>(operation.Value().Value())};
        const Runtime::SceneCellPayloadIdentity identity{owner.partition, selected.cell, selected.definition, selected.revision};
        const Runtime::SceneInstanceAddress address{parent.RuntimeId(), selected.occurrence, selected.definition,
                                                    Runtime::SceneInstanceMode::Streamed};
        using enum WS::StreamingCellOperationTransition;
        const auto reserved = BeginCellOperation(composition->Scheduler(), *residency, {operation.Value(), fence},
                                                 WS::StreamingCellOperationKind::Load, {BeginPreparation});
        if (reserved.HasError())
            return RejectLive(reserved.ErrorValue());
        live->reservation = reserved.Value();
        live->candidate.emplace(fence, address, identity);
        auto loaded = std::make_shared<std::optional<PreparedLiveCell>>();
        const auto maximumDependencies = live->limits.maximumCellAssets ? live->limits.maximumCellAssets : MaximumCells;
        // The mount drains this job before destruction and serializes every owner mutation behind its terminal result.
        const auto *sharedAssets = live->sharedAssets ? &*live->sharedAssets : nullptr;
        const JobProducerScope noBlocking{JobProducerRole::MainEditor};
        auto job = live->jobs.SubmitResult({.parentCancellation = demand.lifetime.Token(), .requirement = JobRequirement::Optional},
                                           [loaded, provider = std::shared_ptr<const Assets::MemoryAssetProvider>{cells},
                                            snapshot = partition.Value(), cell = *descriptor.Value(), metadata = *metadata, identity,
                                            assets = committedAssets, registry = live->assets, target = target, limits = live->limits,
                                            maximumDependencies, sharedAssets,
                                            hasNavigation = NavigationCell(selected.cell) != nullptr](const CancellationToken &cancel) {
            auto decoded = DecodeCell(*provider, *snapshot.Descriptor(), cell, metadata, identity, cancel,
                                      {limits.maximumCellEntities, maximumDependencies, hasNavigation});
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            const auto inventory = assets ? std::span<const MountedCellAsset>{*assets} : std::span<const MountedCellAsset>{};
            auto resources = PrepareCellAssets(decoded.Value().Definition().AssetDependencies(), inventory, registry, target, limits,
                                               sharedAssets, cancel);
            if (resources.HasError())
                return Result<void>::Failure(resources.ErrorValue());
            loaded->emplace(std::move(decoded).Value(), std::move(resources).Value());
            return Result<void>::Success();
        });
        if (job.HasError())
            return RejectLive(job.ErrorValue());
        live->load.emplace(std::move(job).Value(), std::move(loaded));
        return Result<Stage>::Success(Stage::Loading);
    }

    /** @copydoc MountedWorld::State::PollLiveLoad */
    Result<MountedWorldLiveCellState> MountedWorld::State::PollLiveLoad() {
        using Stage = MountedWorldLiveCellState;
        const auto &demand = *live->demand;
        const bool cancelled = demand.IsWithdrawn();
        if (cancelled)
            static_cast<void>(live->load->job.RequestCancel());
        const auto snapshot = live->load->job.Snapshot();
        if (!snapshot || !snapshot->terminalResult)
            return Result<Stage>::Success(Stage::Loading);
        if (cancelled || snapshot->state == JobState::Cancelled) {
            live->load.reset();
            return RejectLive(MakeError(WS::WorldArtifactErrors::Cancelled));
        }
        if (snapshot->state != JobState::Succeeded || !*live->load->payload) {
            auto error = snapshot->error.value_or(MakeError(WS::WorldArtifactErrors::Invalid));
            live->load.reset();
            return RejectLive(error);
        }
        live->prepared = std::move(live->load->payload);
        live->load.reset();
        if (live->sharedAssets) {
            const auto &assets = live->prepared->value().assets;
            live->claims = std::move(live->prepared->value().assets.claims);
            const auto tracked = live->sharedAssets->Track(assets, live->candidate->fence, live->claims);
            if (tracked.HasError())
                return RejectLive(tracked.ErrorValue());
        }
        return Result<Stage>::Success(Stage::Prepared);
    }

    /** @copydoc MountedWorld::State::PrepareNavigationChange */
    Result<MountedWorldNavigationPreparation> MountedWorld::State::PrepareNavigationChange(const Runtime::RuntimeSceneView &parent,
                                                                                           WS::StreamingCellId cell,
                                                                                           MountedWorldNavigationChange change) const {
        using Output = Result<MountedWorldNavigationPreparation>;
        const auto *changed = NavigationCell(cell);
        if (!changed)
            return Output::Success({});
        if (!live->navigation || !committedAssets)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Unsupported));
        try {
            const bool creating = change == MountedWorldNavigationChange::Attach;
            std::vector<MountedWorldNavigationCell> desired;
            desired.reserve(live->residents.size() + (creating ? 1U : 0U));
            const auto append = [&](const MountedNavigationMembership &row) {
                const auto asset = std::ranges::lower_bound(*committedAssets, row.selection.definition, {}, &MountedCellAsset::id);
                if (asset == committedAssets->end() || asset->id != row.selection.definition)
                    return false;
                desired.emplace_back(row.cell, row.selection, asset->envelope);
                return true;
            };
            for (const auto &resident : live->residents) {
                const auto id = resident.cell.fence.cell;
                if (id == cell)
                    continue;
                const auto *retained = NavigationCell(id);
                if (retained && !append(*retained))
                    return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
            }
            if (creating && !append(*changed))
                return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
            auto owner = live->navigation(parent, desired, change);
            if (owner.HasValue() && !owner.Value().publication)
                return Output::Failure(MakeError(WS::WorldArtifactErrors::Unsupported));
            return owner;
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(WS::WorldArtifactErrors::CapacityExceeded));
        }
    }

    /** @copydoc MountedWorld::State::QueueLiveAttachment */
    Result<MountedWorldLiveCellState> MountedWorld::State::QueueLiveAttachment(const Runtime::RuntimeSceneView &parent) {
        using Stage = MountedWorldLiveCellState;
        const auto &demand = *live->demand;
        if (demand.IsWithdrawn() || !*live->prepared)
            return RejectLive(MakeError(WS::WorldArtifactErrors::Cancelled));
        auto &cell = *live->candidate;
        if (parent.RuntimeId() != cell.address.parent)
            return RejectLive(MakeError(Runtime::SceneCellPayloadErrors::Stale));
        if (!live->prepared->value().assets.resources.empty() && parent.AssetRegistryRevision() != live->assets.Revision())
            return RejectLive(MakeError(Runtime::SceneCellPayloadErrors::Stale));
        auto navigation = PrepareNavigationChange(parent, cell.fence.cell, MountedWorldNavigationChange::Attach);
        if (navigation.HasError())
            return RejectLive(navigation.ErrorValue());
        cell.navigationHasCoverage = navigation.Value().attachedCellHasCoverage;
        const auto operation = IssueLiveOperation();
        if (operation.HasError())
            return RejectLive(operation.ErrorValue());
        using enum WS::StreamingCellOperationTransition;
        if (live->reservation)
            if (const auto settled = SettleLiveReservation(Complete); settled.HasError())
                return RejectLive(settled.ErrorValue());
        auto &scheduler = composition->Scheduler();
        const auto activated = BeginCellOperation(scheduler, *residency, {operation.Value(), cell.fence},
                                                  WS::StreamingCellOperationKind::Activate, {BeginPreparation, BeginActivation});
        if (activated.HasError()) {
            const auto current = residency->Resolve(residency->Owner(), cell.fence);
            if (current.HasValue() && current.Value().state == WS::StreamingCellState::Resident)
                return Result<Stage>::Failure(activated.ErrorValue());
            return RejectLive(activated.ErrorValue());
        }
        live->reservation = activated.Value();
        const Runtime::SceneCellAttachmentRequest request{.runtime = cell.address.parent,
                                                          .fence = cell.fence,
                                                          .registry = parent.AssetRegistryRevision(),
                                                          .limits = {live->limits.maximumResidentCells,
                                                                     live->limits.maximumResidentCells * live->limits.maximumCellEntities,
                                                                     std::max<std::size_t>(1, live->limits.maximumResidentCells *
                                                                                                  live->limits.maximumCellAssets)},
                                                          .cancellation = demand.cancellation,
                                                          .ownerCancellation = demand.lifetime.Token(),
                                                          .publication = std::move(navigation).Value().publication};
        const auto authority =
            std::make_shared<const InitialCellAuthority>(cell.identity, *residency, scheduler, activated.Value(),
                                                         MountedWorldStartup{demand.lifetime.Token(),
                                                                             std::chrono::steady_clock::time_point::max()});
        auto &prepared = live->prepared->value();
        auto queued = Runtime::QueueStreamedSceneInstance(scene, cell.address, prepared.payload, request,
                                                          std::move(prepared.assets.resources), authority);
        if (queued.HasError())
            return RejectLive(queued.ErrorValue());
        live->attachment.emplace(std::move(queued).Value());
        live->prepared.reset();
        return Result<Stage>::Success(Stage::Attaching);
    }

    /** @copydoc MountedWorld::State::PollLiveAttachment */
    Result<MountedWorldLiveCellState> MountedWorld::State::PollLiveAttachment() {
        using Stage = MountedWorldLiveCellState;
        const auto *receipt = live->attachment->Receipt();
        if (!receipt->Complete())
            return Result<Stage>::Success(Stage::Attaching);
        if (const auto *error = receipt->Failure()) {
            if (error->code.Value() == Runtime::SceneCellPayloadErrors::Cancelled.code.Value())
                return RejectLive(MakeError(WS::WorldArtifactErrors::Cancelled));
            return RejectLive(*error);
        }
        if (receipt->ResultValue()->baselinesAttached != 1 || !AttachedParent(*live->candidate))
            return RejectLive(MakeError(Runtime::SceneCellPayloadErrors::Stale));
        if (const auto completed = SettleLiveReservation(WS::StreamingCellOperationTransition::Complete); completed.HasError())
            return Result<Stage>::Failure(completed.ErrorValue());
        live->residents.emplace_back(std::move(*live->candidate), std::move(*live->demand), std::move(*live->attachment),
                                     std::move(live->claims));
        live->candidate.reset();
        live->demand.reset();
        live->attachment.reset();
        return Result<Stage>::Success(LiveStage());
    }

    /** @copydoc MountedWorld::State::RetireLiveCell */
    Result<MountedWorldLiveCellState> MountedWorld::State::RetireLiveCell(const Runtime::RuntimeSceneView &parent,
                                                                          const std::size_t index) {
        using Stage = MountedWorldLiveCellState;
        using enum WS::StreamingCellOperationTransition;
        auto &resident = live->residents[index];
        const auto &cell = resident.cell;
        if (live->removal) {
            const auto *receipt = live->removal->Receipt();
            if (!receipt->Complete())
                return Result<Stage>::Success(Stage::Retiring);
            if (const auto *error = receipt->Failure()) {
                auto result = Result<Stage>::Failure(*error);
                live->removal.reset();
                return result;  // Retain the exact charge and attachment so a later frame can retry removal.
            }
            if (receipt->ResultValue()->baselinesDetached != 1 || AttachedParent(cell))
                return Result<Stage>::Failure(MakeError(Runtime::SceneCellPayloadErrors::Stale));
            if (NavigationCell(cell.fence.cell) && live->navigationRetired) {
                const auto drained = live->navigationRetired();
                if (drained.HasError())
                    return Result<Stage>::Failure(drained.ErrorValue());
                if (!drained.Value())
                    return Result<Stage>::Success(Stage::Retiring);
            }
            if (live->sharedAssets) {
                const auto released = live->sharedAssets->Release(resident.claims);
                if (released.HasError())
                    return Result<Stage>::Failure(released.ErrorValue());
            }
            if (const auto settled = SettleLiveReservation(AcknowledgeRetirement); settled.HasError())
                return Result<Stage>::Failure(settled.ErrorValue());
            live->residents.erase(live->residents.begin() + static_cast<std::ptrdiff_t>(index));
            live->retiring.reset();
            live->removal.reset();
            return Result<Stage>::Success(LiveStage());
        }
        if (!AttachedParent(cell) || parent.RuntimeId() != cell.address.parent)
            return Result<Stage>::Failure(MakeError(Runtime::SceneCellPayloadErrors::Stale));
        const auto change =
            resident.demand.forcedEviction ? MountedWorldNavigationChange::ForceEvict : MountedWorldNavigationChange::Retire;
        auto navigation = PrepareNavigationChange(parent, cell.fence.cell, change);
        if (navigation.HasError())
            return Result<Stage>::Failure(navigation.ErrorValue());
        if (const auto retired = BeginLiveRetirement(cell); retired.HasError())
            return Result<Stage>::Failure(retired.ErrorValue());
        live->retiring = index;
        auto &scheduler = composition->Scheduler();
        const Runtime::SceneCellAttachmentRequest request{.runtime = cell.address.parent,
                                                          .fence = cell.fence,
                                                          .registry = parent.AssetRegistryRevision(),
                                                          .limits = {1, live->limits.maximumCellEntities, 1},
                                                          .expectedRevision = cell.identity.revision,
                                                          .publication = std::move(navigation).Value().publication};
        auto authority = std::make_shared<const LiveCellRetirementAuthority>(cell.identity, *residency, scheduler, *live->reservation);
        auto queued = Runtime::QueueStreamedSceneRemoval(scene, resident.attachment, request, std::move(authority));
        if (queued.HasError())
            return Result<Stage>::Failure(queued.ErrorValue());
        live->removal.emplace(std::move(queued).Value());
        return Result<Stage>::Success(Stage::Retiring);
    }

    /** @copydoc MountedWorld::State::CloseLiveCell */
    void MountedWorld::State::CloseLiveCell() noexcept {
        if (!live)
            return;
        if (live->demand)
            live->demand->Withdraw();
        if (live->load) {
            static_cast<void>(live->load->job.RequestCancel());
            // The host's cold boundary may join the bounded, detached decoder. The frame pump only polls.
            static_cast<void>(live->load->job.Wait());
            live->load.reset();
        }
        const auto cancelPending = [this](const std::optional<Runtime::SceneInstanceOperation> &operation) {
            if (operation && !operation->Receipt()->Complete()) {
                static_cast<void>(operation->Cancel(scene));
                if (!operation->Receipt()->Complete())
                    scene.Shutdown();
            }
        };
        cancelPending(live->attachment);
        cancelPending(live->removal);
        if ((live->candidate && AttachedParent(*live->candidate)) ||
            std::ranges::any_of(live->residents, [this](const LiveResident &resident) {
            return AttachedParent(resident.cell).has_value();
        }))
            scene.Shutdown();
        if (live->demand)
            static_cast<void>(RejectLive(MakeError(WS::WorldArtifactErrors::Cancelled)));
        if (live->retiring.has_value())
            static_cast<void>(SettleLiveReservation(WS::StreamingCellOperationTransition::AcknowledgeRetirement));
        for (std::size_t index = 0; index < live->residents.size(); ++index) {
            auto &resident = live->residents[index];
            if (live->retiring != index && BeginLiveRetirement(resident.cell).HasValue())
                static_cast<void>(SettleLiveReservation(WS::StreamingCellOperationTransition::AcknowledgeRetirement));
            if (live->sharedAssets)
                static_cast<void>(live->sharedAssets->Release(resident.claims));
        }
        live->residents.clear();
    }

    /** @copydoc MountedWorldLiveLimits::IsValid */
    bool MountedWorldLiveLimits::IsValid() const noexcept {
        return maximumUnitNanoseconds > 0 && maximumUnitNanoseconds < maximumFrameNanoseconds && maximumCellEntities > 0 &&
               maximumResidentCells > 0 && maximumResidentCells <= MaximumCells && maximumCellEntities <= 65536 / maximumResidentCells &&
               maximumCellAssets <= WS::SharedAssetResidencyLimits::MaximumEntries / maximumResidentCells &&
               maximumCellAssetBytes <= MaximumCellBytes && (maximumCellAssets == 0) == (maximumCellAssetBytes == 0);
    }

    /** @copydoc MountedWorld::BindLiveCell */
    Result<void> MountedWorld::BindLiveCell(const Runtime::RuntimeDispatchSource &dispatch, const JobSystem &jobs,
                                            const MountedWorldLiveLimits &limits, Assets::AssetRegistrySnapshot assets) {
        if (std::this_thread::get_id() != state_->ownerThread || dispatch.BindingStatus() != Runtime::RuntimeDispatchStatus::Valid ||
            state_->initialAttempted || state_->live || !limits.IsValid() ||
            (limits.maximumCellAssets != 0 && (!state_->committedAssets || assets.Revision().value == 0)))
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
        try {
            State::LiveStreaming live{dispatch, jobs, limits, std::move(assets)};
            live.residents.reserve(limits.maximumResidentCells);
            if (limits.maximumCellAssets != 0) {
                const auto slots = static_cast<std::uint32_t>(limits.maximumCellAssets * limits.maximumResidentCells);
                const auto capacity = CpuPayloadCharge(limits.maximumCellAssetBytes);
                auto shared = WS::SharedAssetResidencyLedger::Create(state_->composition->Owner(), {slots, slots, capacity});
                if (shared.HasError())
                    return Result<void>::Failure(shared.ErrorValue());
                live.sharedAssets.emplace(std::move(shared).Value(), limits.maximumCellAssetBytes, slots);
            }
            state_->live.emplace(std::move(live));
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::CapacityExceeded));
        }
        return Result<void>::Success();
    }

    /** @copydoc MountedWorld::BindLiveNavigation */
    Result<void> MountedWorld::BindLiveNavigation(MountedWorldNavigationPublication publication,
                                                  MountedWorldNavigationRetirement retirement) {
        if (std::this_thread::get_id() != state_->ownerThread || !state_->live || !publication || state_->live->navigation ||
            state_->live->demand || !state_->live->residents.empty())
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
        state_->live->navigation = std::move(publication);
        state_->live->navigationRetired = std::move(retirement);
        return Result<void>::Success();
    }

    /** @copydoc MountedWorld::HasLiveNavigationContent */
    bool MountedWorld::HasLiveNavigationContent() const noexcept {
        return !state_->navigationCells.empty();
    }

    /** @copydoc MountedWorld::RequestLiveCell */
    Result<void> MountedWorld::RequestLiveCell(const CommittedWorldInitialCell &selected, const CancellationToken &cancellation) {
        if (std::this_thread::get_id() != state_->ownerThread || !state_->live || !selected.cell.IsValid() ||
            !selected.occurrence.IsValid() || !selected.definition.IsValid() || selected.revision.value == 0)
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Cancelled));
        auto &live = *state_->live;
        if (state_->NavigationCell(selected.cell) && !live.navigation)
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Unsupported));
        if (live.demand || live.RetirementCandidate().has_value() || live.residents.size() == live.limits.maximumResidentCells)
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
        if (std::ranges::any_of(live.residents, [&](const State::LiveResident &resident) {
            const auto &current = resident.demand.selected;
            return current.cell == selected.cell || current.occurrence == selected.occurrence || current.definition == selected.definition;
        }))
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
        live.demand.emplace(selected, cancellation);
        return Result<void>::Success();
    }

    /** @copydoc MountedWorld::WithdrawLiveCell */
    Result<void> MountedWorld::WithdrawLiveCell() {
        if (std::this_thread::get_id() != state_->ownerThread || !state_->live)
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        const auto &live = *state_->live;
        if (live.demand && live.residents.empty())
            return WithdrawLiveCell(live.demand->selected.occurrence);
        if (!live.demand && live.residents.size() == 1)
            return WithdrawLiveCell(live.residents.front().demand.selected.occurrence);
        return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
    }

    /** @copydoc MountedWorld::WithdrawLiveCell(Runtime::SceneInstanceId) */
    Result<void> MountedWorld::WithdrawLiveCell(const Runtime::SceneInstanceId occurrence) {
        if (std::this_thread::get_id() != state_->ownerThread || !state_->live)
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        auto &live = *state_->live;
        if (live.demand && live.demand->selected.occurrence == occurrence) {
            live.demand->Withdraw();
            return Result<void>::Success();
        }
        const auto found = live.FindResident(occurrence);
        if (found == live.residents.end())
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        found->demand.Withdraw();
        return Result<void>::Success();
    }

    /** @copydoc MountedWorld::ForceEvictLiveCell */
    Result<void> MountedWorld::ForceEvictLiveCell(Runtime::SceneInstanceId occurrence) {
        if (std::this_thread::get_id() != state_->ownerThread || !state_->live)
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        auto &live = *state_->live;
        const auto found = live.FindResident(occurrence);
        if (found == live.residents.end())
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        if (found->demand.forcedEviction)
            return Result<void>::Success();
        if (live.retiring == static_cast<std::size_t>(found - live.residents.begin()))
            return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
        found->demand.forcedEviction = true;
        found->demand.Withdraw();
        return Result<void>::Success();
    }

    /** @copydoc MountedWorld::InspectLiveCell */
    Result<MountedWorldCellReadiness> MountedWorld::InspectLiveCell(Runtime::SceneInstanceId occurrence) const {
        using Output = Result<MountedWorldCellReadiness>;
        if (std::this_thread::get_id() != state_->ownerThread || !state_->live)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        auto &live = *state_->live;
        const auto found = live.FindResident(occurrence);
        const State::ActiveCell *cell = found == live.residents.end() ? nullptr : &found->cell;
        if (!cell && live.candidate && live.candidate->address.instance == occurrence)
            cell = &*live.candidate;
        if (!cell)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        const auto parent = state_->scene.ActiveScene();
        if (!parent || parent->RuntimeId() != cell->address.parent)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        const bool visible = state_->AttachedParent(*cell).has_value();
        using enum MountedWorldCellNavigationReadiness;
        auto readiness = NotDeclared;
        if (state_->NavigationCell(cell->fence.cell))
            readiness = visible && cell->navigationHasCoverage ? Ready : Unavailable;
        return Output::Success({cell->fence, visible, readiness});
    }

    /** @copydoc MountedWorld::InspectLiveAssetBytes */
    Result<std::size_t> MountedWorld::InspectLiveAssetBytes() const {
        if (std::this_thread::get_id() != state_->ownerThread || !state_->live)
            return Result<std::size_t>::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
        const auto &assets = state_->live->sharedAssets;
        return Result<std::size_t>::Success(assets ? assets->ChargedBytes() : 0);
    }

    /** @copydoc MountedWorld::State::LiveStage */
    MountedWorldLiveCellState MountedWorld::State::LiveStage() const noexcept {
        using enum MountedWorldLiveCellState;
        if (live->attachment)
            return Attaching;
        if (live->prepared)
            return Prepared;
        if (live->load)
            return Loading;
        if (live->demand)
            return Requested;
        if (live->RetirementCandidate().has_value())
            return Retiring;
        return live->residents.empty() ? Idle : Active;
    }

    /** @copydoc MountedWorld::AdvanceLiveCell */
    Result<MountedWorldLiveCellState> MountedWorld::AdvanceLiveCell(const Runtime::RuntimeDispatchEvidence &evidence) {
        using Stage = MountedWorldLiveCellState;
        const auto begin = std::chrono::steady_clock::now();
        auto &state = *state_;
        if (std::this_thread::get_id() != state.ownerThread || !state.live)
            return Result<Stage>::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
        auto &live = *state.live;
        Runtime::RuntimeDispatchFacts facts;
        if (evidence.Read(live.dispatch, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, facts) !=
                Runtime::RuntimeDispatchStatus::Valid ||
            facts.frame <= live.lastFrame)
            return Result<Stage>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        live.lastFrame = facts.frame;
        const auto stage = state.LiveStage();
        const bool idle = stage == Stage::Idle || stage == Stage::Active;
        const bool retiringAssets = live.sharedAssets && live.sharedAssets->HasRetiring() && !live.load;
        if (idle && !retiringAssets)
            return Result<Stage>::Success(stage);
        auto budget = WS::StreamingOwnerFrameBudget::Create(
            {state.composition->Scheduler().Owner(), Identity<WS::StreamingOwnerWorkRevision>(1),
             Identity<WS::StreamingOwnerFrameId>(facts.frame), live.limits.maximumFrameNanoseconds, 1});
        if (budget.HasError())
            return Result<Stage>::Failure(budget.ErrorValue());
        auto frameBudget = std::move(budget).Value();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
        const auto admitted = frameBudget.TryConsume(state.composition->Scheduler().Owner(), live.limits.maximumUnitNanoseconds,
                                                     static_cast<std::uint64_t>(elapsed));
        if (admitted.HasError())
            return Result<Stage>::Failure(admitted.ErrorValue());
        if (!admitted.Value())
            return Result<Stage>::Success(stage);
        try {
            if (retiringAssets) {
                const auto retired = live.sharedAssets->ReconcileRetired();
                if (retired.HasError())
                    return Result<Stage>::Failure(retired.ErrorValue());
            }
            if (idle)
                return Result<Stage>::Success(stage);
            if (live.load)
                return state.PollLiveLoad();
            if (live.attachment)
                return state.PollLiveAttachment();
            const auto parent = state.scene.ActiveScene();
            if (!parent)
                return Result<Stage>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
            if (live.prepared)
                return state.QueueLiveAttachment(*parent);
            if (live.demand)
                return state.StartLiveLoad(*parent);
            return state.RetireLiveCell(*parent, *live.RetirementCandidate());
        } catch (const std::bad_alloc &) {
            auto error = MakeError(WS::WorldArtifactErrors::CapacityExceeded);
            if (live.demand && !live.load && !live.attachment)
                return state.RejectLive(error);
            return Result<Stage>::Failure(std::move(error));
        }
    }

    /** @copydoc MountedWorld::Anchors */
    std::span<const Runtime::SceneCellWorldAnchor> MountedWorld::Anchors(const Runtime::RuntimeSceneView &parent) const noexcept {
        const auto *active = state_->ImplicitActiveCell();
        if (!parent.IsCurrent() || !active)
            return {};
        if (const auto current = state_->scene.ActiveScene();
            !current || !parent.SameIncarnation(*current) ||
            !Runtime::FindStreamedSceneInstance(parent, active->address, active->identity, active->fence.epoch))
            return {};
        return active->anchors;
    }

    /** @copydoc MountedWorld::CaptureGeometry */
    Result<MountedWorldRenderPacket> MountedWorld::CaptureGeometry(const Runtime::RuntimeSceneView &parent,
                                                                   const Runtime::SceneObjectId camera,
                                                                   const std::span<const Runtime::SceneObjectId> meshes,
                                                                   Runtime::PrimitiveMeshCache &primitives,
                                                                   Render::RenderOriginSceneAdapter &renderer,
                                                                   const Render::RenderOriginCoordinateBasis basis) const {
        return CaptureScopedGeometry(parent, camera, meshes, primitives, renderer, basis, GeometryScope::Active);
    }

    /** @copydoc MountedWorld::CaptureInitialGeometry */
    Result<MountedWorldRenderPacket> MountedWorld::CaptureInitialGeometry(const Runtime::RuntimeSceneView &candidate,
                                                                          const Runtime::SceneObjectId camera,
                                                                          const std::span<const Runtime::SceneObjectId> meshes,
                                                                          Runtime::PrimitiveMeshCache &primitives,
                                                                          Render::RenderOriginSceneAdapter &renderer) const {
        return CaptureScopedGeometry(candidate, camera, meshes, primitives, renderer, Render::RenderOriginCoordinateBasis::OriginRelative,
                                     GeometryScope::PrivateStartup);
    }

    /** @copydoc MountedWorld::ResolveInitialTrackingRoot */
    Result<MountedWorldTrackingRoot> MountedWorld::ResolveInitialTrackingRoot(const Runtime::RuntimeSceneView &candidate,
                                                                              const Runtime::SceneObjectId object) const {
        using Output = Result<MountedWorldTrackingRoot>;
        const auto selected = state_->InitialCellFor(candidate);
        if (selected.HasError())
            return Output::Failure(selected.ErrorValue());
        const auto &cell = *selected.Value();
        const auto baseline = Runtime::FindStreamedSceneInstance(candidate, cell.address, cell.identity, cell.fence.epoch);
        if (!baseline)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        const auto root = FindAnchoredGeometryEntity(candidate, *baseline, cell.anchors, cell.initialLocalPositions, object);
        if (root.HasError())
            return Output::Failure(root.ErrorValue());
        constexpr Math::Vec3 unit{1.0F, 1.0F, 1.0F};
        const auto &transform = *root.Value().entity.localTransform;
        if (transform.scale != unit)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Unsupported, "A mounted tracking root must be rigid."));
        const auto orientation = transform.rotation.TryNormalized();
        if (orientation.HasError())
            return Output::Failure(orientation.ErrorValue());
        auto lease = Origin().Lease();
        if (lease.HasError())
            return Output::Failure(lease.ErrorValue());
        const auto frame = lease.Value().Get();
        if (frame.HasError())
            return Output::Failure(frame.ErrorValue());
        const auto position = frame.Value().ToLocal(Global(root.Value().anchor.positionMillimeters));
        if (position.HasError())
            return Output::Failure(position.ErrorValue());
        return Output::Success({{position.Value().Value(), orientation.Value(), unit}, std::move(lease).Value()});
    }

    /** @copydoc MountedWorld::CaptureScopedGeometry */
    Result<MountedWorldRenderPacket> MountedWorld::CaptureScopedGeometry(
        const Runtime::RuntimeSceneView &parent, const Runtime::SceneObjectId camera, const std::span<const Runtime::SceneObjectId> meshes,
        Runtime::PrimitiveMeshCache &primitives, Render::RenderOriginSceneAdapter &renderer,
        const Render::RenderOriginCoordinateBasis basis, const GeometryScope scope) const {
        using Output = Result<MountedWorldRenderPacket>;
        constexpr std::size_t MaximumSelectedMeshes = 64;
        if (meshes.size() > MaximumSelectedMeshes)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::CapacityExceeded));
        if (!camera.IsValid() || !std::ranges::is_sorted(meshes) || std::ranges::adjacent_find(meshes) != meshes.end())
            return Output::Failure(
                MakeError(WS::WorldArtifactErrors::Invalid, "World geometry requires explicit unique ordered object identities."));
        if (!parent.IsCurrent())
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        const State::ActiveCell *scoped = nullptr;
        if (scope == GeometryScope::PrivateStartup) {
            const auto initial = state_->InitialCellFor(parent);
            if (initial.HasError())
                return Output::Failure(initial.ErrorValue());
            scoped = initial.Value();
        } else {
            const auto current = state_->scene.ActiveScene();
            if (!current || !parent.SameIncarnation(*current))
                return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
            scoped = state_->ImplicitActiveCell();
        }
        if (!scoped || scoped->anchors.empty())
            return Output::Failure(
                MakeError(WS::WorldArtifactErrors::Unsupported, "No canonical World positions are published for this selected cell."));
        if (!renderer.UsesOrigin(Origin()))
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale, "The renderer belongs to another World origin."));

        const auto baseline = Runtime::FindStreamedSceneInstance(parent, scoped->address, scoped->identity, scoped->fence.epoch);
        if (!baseline)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        const auto lookup = [&](const Runtime::SceneObjectId object) {
            return FindAnchoredGeometryEntity(parent, *baseline, scoped->anchors, scoped->initialLocalPositions, object);
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
            auto matrix = transform.TryToMatrix();
            if (matrix.HasError())
                return Output::Failure(matrix.ErrorValue());
            instances.emplace_back(Global(placed.Value().anchor.positionMillimeters),
                                   Render::RenderStaticMeshInstance{handle, matrix.Value(), resource.localBounds});
        }

        auto frame = renderer.Capture({originCamera.Value(), instances, {}, parent.RuntimeId().value}, basis);
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

    /** @copydoc MountedWorldRenderPacket::OriginScene */
    Result<Render::RenderOriginLocalScene> MountedWorldRenderPacket::OriginScene() const {
        if (!parent_.IsCurrent())
            return Result<Render::RenderOriginLocalScene>::Failure(MakeError(WS::WorldArtifactErrors::Stale));
        return frame_.OriginScene(resources_);
    }

    /** @copydoc MountedWorldRenderPacket::IsSuccessorOf */
    bool MountedWorldRenderPacket::IsSuccessorOf(const MountedWorldRenderPacket &previous) const noexcept {
        return parent_.SameIncarnation(previous.parent_) && frame_.IsSuccessorOf(previous.frame_);
    }

    MountedWorld::MountedWorld(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc MountedWorld::Origin */
    const WS::OriginFrameOwner &MountedWorld::Origin() const noexcept {
        return *state_->composition->Origin();
    }

    /** @copydoc MountedWorld::~MountedWorld */
    MountedWorld::~MountedWorld() {
        state_->CloseLiveCell();
        if (state_->pendingCell) {
            // Close the Scene predicate before its borrowed scheduler and residency ledgers retire.
            state_->pendingLifetime->RequestCancellation();
            if (state_->AttachedParent(state_->pendingCell->cell))
                state_->scene.Shutdown();
            static_cast<void>(RejectCellOperation(state_->composition->Scheduler(), *state_->residency, state_->pendingCell->reservation,
                                                  MakeError(WS::WorldArtifactErrors::Cancelled)));
            state_->pendingCell.reset();
        }
        if (state_->activeCell) {
            using enum WS::StreamingCellOperationTransition;
            const auto &cell = *state_->activeCell;
            auto retired = RunCellOperation(state_->composition->Scheduler(), *state_->residency,
                                            {Identity<WS::StreamingCellOperationId>(3), cell.fence}, WS::StreamingCellOperationKind::Retire,
                                            {BeginRetirement}, AcknowledgeRetirement, [&](const auto &) {
                // Shut down only the published incarnation that owns this cell.
                if (state_->AttachedParent(cell))
                    state_->scene.Shutdown();
                return state_->AttachedParent(cell) ? Result<void>::Failure(MakeError(Runtime::SceneCellPayloadErrors::Stale))
                                                    : Result<void>::Success();
            });
            if (retired.HasError() && state_->AttachedParent(cell))
                state_->scene.Shutdown();
        }
        state_->initialSource.reset();
        static_cast<void>(state_->composition->BeginShutdown(state_->composition->Owner()));
        static_cast<void>(state_->residency->BeginShutdown(state_->composition->Owner()));
        static_cast<void>(state_->planner->Shutdown());
        state_->registry->Shutdown();
    }
}  // namespace Horo::Application::Internal
