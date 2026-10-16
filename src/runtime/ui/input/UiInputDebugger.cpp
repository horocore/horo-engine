#include "Horo/Runtime/Ui/UiInputDebugger.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <thread>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Preserves canonical UI error identity at debugger control/inspection failure boundaries. */
        template <class T = void> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Fences rebind to the original attachment; only publication revisions may advance. */
        bool SameScope(const UiFocusOwnerContext &a, const UiFocusOwnerContext &b) noexcept {
            return a.instance == b.instance && a.canvas == b.canvas && a.document == b.document && a.scope == b.scope;
        }

        /** @brief Matches actual target-source lineage without inferring player authority from shared canvas data. */
        bool SameHit(const UiFocusOwnerContext &a, const UiHitTestSnapshotDescriptor &b) noexcept {
            return a.instance == b.instance && a.canvas == b.canvas && a.document == b.document && a.treeRevision == b.tree &&
                   a.interaction == b.interaction;
        }

        /** @brief Restores observation admission on every return and owner failure/exception. */
        struct RecordingGuard final {
            bool &busy;

            explicit RecordingGuard(bool &value) noexcept : busy(value) {
                value = true;
            }

            ~RecordingGuard() {
                busy = false;
            }

            RecordingGuard(const RecordingGuard &) = delete;
            RecordingGuard &operator=(const RecordingGuard &) = delete;
        };
    }  // namespace

    struct UiInputDebugSnapshot::Storage final {
        UiInputDebugRecord record;
        std::vector<UiHitTestCandidate> candidates;
        std::vector<UiEventPhaseObservation> phases;
        std::vector<UiPointerCaptureInspection> captures;
        std::size_t candidateCount{};
        std::size_t phaseCount{};
        std::size_t captureCount{};

        explicit Storage(const UiInputDebuggerDescriptor &d)
            : candidates(d.candidateCapacity), phases(d.phaseCapacity), captures(d.captureCapacity) {}
    };

    struct UiInputDebugger::Storage final {
        UiInputDebuggerDescriptor descriptor;
        const std::thread::id thread{std::this_thread::get_id()};
        std::vector<std::shared_ptr<UiInputDebugSnapshot::Storage>> slots;
        std::vector<UiPointerCaptureInspection> captureScratch;
        std::uint64_t nextSerial{1};
        std::uint64_t dropped{};
        bool enabled{};
        bool stopped{};
        bool busy{};

        explicit Storage(const UiInputDebuggerDescriptor &d) : descriptor(d), captureScratch(d.captureCapacity) {
            slots.reserve(d.historyCapacity);
            for (std::uint32_t i = 0; i < d.historyCapacity; ++i)
                slots.push_back(std::make_shared<UiInputDebugSnapshot::Storage>(d));
        }

        bool OnThread() const noexcept {
            return thread == std::this_thread::get_id();
        }

        /** @brief Reclaims only an unleased oldest record; pinned history can never block or affect input. */
        std::shared_ptr<UiInputDebugSnapshot::Storage> Acquire(UiInputDebugRecordKind kind) {
            if (!OnThread() || !enabled || stopped)
                return {};
            const std::shared_ptr<UiInputDebugSnapshot::Storage> *oldest{};
            if (!busy)
                for (auto &slot : slots)
                    if (slot.use_count() == 1 && (!oldest || slot->record.serial < (*oldest)->record.serial))
                        oldest = &slot;
            if (!oldest) {
                if (dropped != std::numeric_limits<std::uint64_t>::max())
                    ++dropped;
                return {};
            }
            auto entry = *oldest;
            entry->record = {};
            entry->record.kind = kind;
            entry->record.owner = descriptor.owner;
            entry->candidateCount = 0;
            entry->phaseCount = 0;
            entry->captureCount = 0;
            return entry;
        }

        void Finish(UiInputDebugSnapshot::Storage &entry) noexcept {
            entry.record.serial = nextSerial;
            if (nextSerial == std::numeric_limits<std::uint64_t>::max()) {
                stopped = true;
                enabled = false;
            } else
                ++nextSerial;
        }
    };

    /** @copydoc UiInputDebuggerDescriptor::IsValid */
    bool UiInputDebuggerDescriptor::IsValid() const noexcept {
        if (!owner.IsValid() || historyCapacity == 0 || historyCapacity > MaximumUiInputDebugHistory || candidateCapacity == 0 ||
            candidateCapacity > MaximumUiTreeElements || phaseCapacity == 0 || phaseCapacity > 2 * MaximumUiTreeDepth ||
            captureCapacity == 0 || captureCapacity > MaximumUiPointerCaptures)
            return false;
        const std::size_t per = sizeof(UiInputDebugRecord) + 512 + sizeof(UiHitTestCandidate) * candidateCapacity +
                                sizeof(UiEventPhaseObservation) * phaseCapacity + sizeof(UiPointerCaptureInspection) * captureCapacity;
        return per * historyCapacity + sizeof(UiPointerCaptureInspection) * captureCapacity <= MaximumUiInputDebugStorageBytes;
    }

    /** @copydoc UiInputDebugSnapshot::UiInputDebugSnapshot */
    UiInputDebugSnapshot::UiInputDebugSnapshot(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiInputDebugSnapshot::Record */
    const UiInputDebugRecord &UiInputDebugSnapshot::Record() const noexcept {
        return storage_->record;
    }

    /** @copydoc UiInputDebugSnapshot::Candidates */
    std::span<const UiHitTestCandidate> UiInputDebugSnapshot::Candidates() const noexcept {
        return std::span{storage_->candidates}.first(storage_->candidateCount);
    }

    /** @copydoc UiInputDebugSnapshot::Phases */
    std::span<const UiEventPhaseObservation> UiInputDebugSnapshot::Phases() const noexcept {
        return std::span{storage_->phases}.first(storage_->phaseCount);
    }

    /** @copydoc UiInputDebugSnapshot::Captures */
    std::span<const UiPointerCaptureInspection> UiInputDebugSnapshot::Captures() const noexcept {
        return std::span{storage_->captures}.first(storage_->captureCount);
    }

    /** @copydoc UiInputDebugger::Create */
    Result<UiInputDebugger> UiInputDebugger::Create(const UiInputDebuggerDescriptor &descriptor, const UiFocusGraph &focus) {
        if (!descriptor.IsValid())
            return Failure<UiInputDebugger>(UiErrors::DiagnosticInvalid);
        if (focus.Owner() != descriptor.owner)
            return Failure<UiInputDebugger>(UiErrors::HandleOwnerMismatch);
        if (const auto state = focus.Snapshot(); state.HasError())
            return Result<UiInputDebugger>::Failure(state.ErrorValue());
        try {
            return Result<UiInputDebugger>::Success(UiInputDebugger{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiInputDebugger>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiInputDebugger::UiInputDebugger */
    UiInputDebugger::UiInputDebugger(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    UiInputDebugger::~UiInputDebugger() = default;
    UiInputDebugger::UiInputDebugger(UiInputDebugger &&) noexcept = default;
    UiInputDebugger &UiInputDebugger::operator=(UiInputDebugger &&) noexcept = default;

    /** @copydoc UiInputDebugger::SetEnabled */
    Result<void> UiInputDebugger::SetEnabled(bool enabled) {
        if (!storage_ || !storage_->OnThread() || storage_->stopped || storage_->busy)
            return Failure(UiErrors::InstanceStateInvalid);
        storage_->enabled = enabled;
        return Result<void>::Success();
    }

    /** @copydoc UiInputDebugger::Rebind */
    Result<void> UiInputDebugger::Rebind(const UiFocusGraph &focus) {
        if (!storage_ || !storage_->OnThread() || storage_->stopped || storage_->busy)
            return Failure(UiErrors::InstanceStateInvalid);
        if (const auto state = focus.Snapshot(); state.HasError())
            return Result<void>::Failure(state.ErrorValue());
        const auto &owner = focus.Owner();
        const auto &before = storage_->descriptor.owner;
        if (!SameScope(before, owner))
            return Failure(UiErrors::HandleOwnerMismatch);
        if (owner.documentRevision < before.documentRevision || owner.treeRevision < before.treeRevision ||
            owner.interaction < before.interaction)
            return Failure(UiErrors::RevisionStale);
        storage_->descriptor.owner = owner;
        return Result<void>::Success();
    }

    /** @copydoc UiInputDebugger::HitTestScreen */
    Result<std::optional<UiHitTestResult>> UiInputDebugger::HitTestScreen(const UiHitTestSnapshot &hit, const UiScreenPointerQuery &query,
                                                                          const UiPresentedInteractionState &presented) {
        const auto *source = hit.TryDescriptor();
        auto entry = storage_ && source && SameHit(storage_->descriptor.owner, *source)
                         ? storage_->Acquire(UiInputDebugRecordKind::ScreenHitTest)
                         : nullptr;
        if (!entry)
            return hit.HitTestScreen(query, presented);
        RecordingGuard guard{storage_->busy};
        UiHitTestObservation observation{entry->candidates};
        entry->record.hitSource = *source;
        entry->record.screenQuery = query;
        auto result = hit.HitTestScreen(query, presented, observation);
        entry->candidateCount = observation.written;
        entry->record.omittedCandidates = observation.omitted;
        entry->record.failed = result.HasError();
        if (result.HasValue())
            entry->record.hit = result.Value();
        storage_->Finish(*entry);
        return result;
    }

    /** @copydoc UiInputDebugger::HitTestWorld */
    Result<std::optional<UiHitTestResult>> UiInputDebugger::HitTestWorld(const UiHitTestSnapshot &hit, const UiWorldRayQuery &query,
                                                                         const UiPresentedInteractionState &presented) {
        const auto *source = hit.TryDescriptor();
        auto entry = storage_ && source && SameHit(storage_->descriptor.owner, *source)
                         ? storage_->Acquire(UiInputDebugRecordKind::WorldHitTest)
                         : nullptr;
        if (!entry)
            return hit.HitTestWorld(query, presented);
        RecordingGuard guard{storage_->busy};
        UiHitTestObservation observation{entry->candidates};
        entry->record.hitSource = *source;
        entry->record.worldQuery = query;
        auto result = hit.HitTestWorld(query, presented, observation);
        entry->candidateCount = observation.written;
        entry->record.omittedCandidates = observation.omitted;
        entry->record.failed = result.HasError();
        if (result.HasValue())
            entry->record.hit = result.Value();
        storage_->Finish(*entry);
        return result;
    }

    /** @copydoc UiInputDebugger::Dispatch */
    Result<UiEventDispatchResult> UiInputDebugger::Dispatch(UiEventDispatcher &dispatcher, const UiElementTree &tree,
                                                            const UiEventRoute &route, const UiRoutedEvent &event,
                                                            UiEventHandler &handler) {
        const bool matches = storage_ && route.instance == storage_->descriptor.owner.instance &&
                             route.canvas == storage_->descriptor.owner.canvas && route.document == storage_->descriptor.owner.document &&
                             route.tree == storage_->descriptor.owner.treeRevision &&
                             route.interaction == storage_->descriptor.owner.interaction;
        auto entry = matches ? storage_->Acquire(UiInputDebugRecordKind::Dispatch) : nullptr;
        if (!entry)
            return dispatcher.Dispatch(tree, route, event, handler);
        RecordingGuard guard{storage_->busy};
        UiEventDispatchObservation observation{entry->phases};
        entry->record.route = route;
        entry->record.event = event;
        auto result = dispatcher.Dispatch(tree, route, event, handler, observation);
        entry->phaseCount = observation.written;
        entry->record.omittedPhases = observation.omitted;
        entry->record.defaultAttempted = observation.defaultAttempted;
        entry->record.defaultFailed = observation.defaultFailed;
        entry->record.failed = result.HasError();
        entry->record.dispatch = observation.partial;
        if (result.HasValue())
            entry->record.dispatch = result.Value();
        storage_->Finish(*entry);
        return result;
    }

    /** @copydoc UiInputDebugger::CaptureState */
    Result<UiInputDebugSnapshot> UiInputDebugger::CaptureState(const UiFocusGraph &focus, const UiPointerCaptureStore &captures,
                                                               const UiPointerCaptureContext &context,
                                                               const UiNavigationInput &navigation) {
        if (!storage_ || !storage_->OnThread() || !storage_->enabled || storage_->stopped || storage_->busy || navigation.IsStopped())
            return Failure<UiInputDebugSnapshot>(UiErrors::InstanceStateInvalid);
        if (const auto &owner = storage_->descriptor.owner;
            focus.Owner() != owner || navigation.Owner() != owner || context.instance != owner.instance || context.canvas != owner.canvas ||
            context.document != owner.document || context.tree != owner.treeRevision || context.interaction != owner.interaction)
            return Failure<UiInputDebugSnapshot>(UiErrors::RevisionStale);
        auto state = focus.Snapshot();
        if (state.HasError())
            return Result<UiInputDebugSnapshot>::Failure(state.ErrorValue());
        std::array<UiFocusNavigationPreview, UiFocusDirectionCount> neighbors{};
        constexpr std::array directions{UiNavigationDirection::Next, UiNavigationDirection::Previous, UiNavigationDirection::Up,
                                        UiNavigationDirection::Down, UiNavigationDirection::Left,     UiNavigationDirection::Right};
        for (std::size_t i = 0; i < directions.size(); ++i) {
            auto next = focus.PreviewMove(directions[i]);
            if (next.HasError())
                return Result<UiInputDebugSnapshot>::Failure(next.ErrorValue());
            neighbors[i] = next.Value();
        }
        auto count = captures.Inspect(context, storage_->captureScratch);
        if (count.HasError())
            return Result<UiInputDebugSnapshot>::Failure(count.ErrorValue());
        auto entry = storage_->Acquire(UiInputDebugRecordKind::State);
        if (!entry)
            return Failure<UiInputDebugSnapshot>(UiErrors::CapacityExceeded);
        entry->record.focus = state.Value();
        entry->record.neighbors = neighbors;
        const auto presentation = navigation.Presentation();
        entry->record.modality = presentation.modality;
        entry->record.modalityDevice = presentation.device;
        entry->record.modalityRevision = presentation.revision;
        entry->record.inputPlayer = navigation.Player();
        entry->captureCount = count.Value();
        std::copy_n(storage_->captureScratch.begin(), entry->captureCount, entry->captures.begin());
        storage_->Finish(*entry);
        return Result<UiInputDebugSnapshot>::Success(UiInputDebugSnapshot{std::move(entry)});
    }

    /** @copydoc UiInputDebugger::Recent */
    Result<UiInputDebugSnapshot> UiInputDebugger::Recent(std::size_t age) const {
        if (!storage_ || !storage_->OnThread())
            return Failure<UiInputDebugSnapshot>(UiErrors::InstanceStateInvalid);
        if (age >= storage_->slots.size())
            return Failure<UiInputDebugSnapshot>(UiErrors::CapacityExceeded);
        std::optional<std::uint64_t> limit;
        std::shared_ptr<const UiInputDebugSnapshot::Storage> found;
        for (std::size_t i = 0; i <= age; ++i) {
            found.reset();
            for (const auto &slot : storage_->slots)
                if (slot->record.serial && (!limit || slot->record.serial < *limit) &&
                    (!found || slot->record.serial > found->record.serial))
                    found = slot;
            if (!found)
                return Failure<UiInputDebugSnapshot>(UiErrors::HandleStale);
            limit = found->record.serial;
        }
        return Result<UiInputDebugSnapshot>::Success(UiInputDebugSnapshot{std::move(found)});
    }

    /** @copydoc UiInputDebugger::Dropped */
    std::uint64_t UiInputDebugger::Dropped() const noexcept {
        return storage_ ? storage_->dropped : 0;
    }

    /** @copydoc UiInputDebugger::Shutdown */
    void UiInputDebugger::Shutdown() noexcept {
        if (storage_ && storage_->OnThread() && !storage_->busy) {
            storage_->enabled = false;
            storage_->stopped = true;
        }
    }
}  // namespace Horo::Runtime::Ui
