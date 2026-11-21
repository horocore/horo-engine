#include "Horo/Runtime/Render/TemporalHistory.h"

#include "Horo/Runtime/Render/TemporalHistoryErrors.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Render {
    struct TemporalHistoryStore::Record {
        TemporalHistoryCompatibility compatibility;
        std::vector<RenderTextureHandle> resources;
        std::uint32_t generation{1};
        std::uint64_t contentGeneration{0};
        std::uint64_t lastPublishedFrame{0};
        std::uint64_t pendingFrame{0};
        std::uint64_t nextAttempt{1};
        std::uint64_t pendingAttempt{0};
        bool pendingCanReadPrevious{false};
        std::optional<TemporalHistoryResetCause> pendingFrameReset;
        std::optional<TemporalHistoryResetCause> pendingReset{TemporalHistoryResetCause::FirstFrame};
        bool active{false};
        bool valid{false};
    };

    namespace {
        /** @brief Acquires one non-zero process-local store identity without wrapping. */
        [[nodiscard]] Result<TemporalHistoryOwnerId> AcquireOwner() {
            static std::atomic<std::uint64_t> nextOwner{1};
            std::uint64_t candidate = nextOwner.load();
            while (candidate != std::numeric_limits<std::uint64_t>::max()) {
                if (nextOwner.compare_exchange_weak(candidate, candidate + 1))
                    return Result<TemporalHistoryOwnerId>::Success(TemporalHistoryOwnerId{candidate});
            }
            return Result<TemporalHistoryOwnerId>::Failure(MakeError(TemporalHistoryErrors::OwnerExhausted));
        }

        /** @brief Validates the exact bounded resident texture generations adopted by a history. */
        [[nodiscard]] bool AreResourcesValid(const std::span<const RenderTextureHandle> resources,
                                             const TemporalHistoryLimits &limits) noexcept {
            return !resources.empty() && resources.size() <= limits.maxResourcesPerHistory &&
                   std::ranges::all_of(resources, &RenderTextureHandle::IsValid);
        }
    }  // namespace

    /** @copydoc TemporalHistoryStore::TemporalHistoryStore */
    TemporalHistoryStore::TemporalHistoryStore(TemporalHistoryOwnerId owner, TemporalHistoryLimits limits,
                                               std::vector<Record> records) noexcept
        : m_owner(owner), m_limits(limits), m_ownerThread(std::this_thread::get_id()), m_records(std::move(records)) {}

    /** @copydoc TemporalHistoryStore::TemporalHistoryStore */
    TemporalHistoryStore::TemporalHistoryStore(TemporalHistoryStore &&other) noexcept
        : m_owner(std::exchange(other.m_owner, {})), m_limits(other.m_limits), m_ownerThread(other.m_ownerThread),
          m_records(std::move(other.m_records)), m_stopped(std::exchange(other.m_stopped, true)), m_size(std::exchange(other.m_size, 0)) {}

    /** @copydoc TemporalHistoryStore::operator= */
    TemporalHistoryStore &TemporalHistoryStore::operator=(TemporalHistoryStore &&other) noexcept {
        if (this == &other)
            return *this;
        ReleaseAll();
        m_owner = std::exchange(other.m_owner, {});
        m_limits = other.m_limits;
        m_ownerThread = other.m_ownerThread;
        m_records = std::move(other.m_records);
        m_stopped = std::exchange(other.m_stopped, true);
        m_size = std::exchange(other.m_size, 0);
        return *this;
    }

    /** @copydoc TemporalHistoryStore::~TemporalHistoryStore */
    TemporalHistoryStore::~TemporalHistoryStore() {
        ReleaseAll();
    }

    /** @copydoc TemporalHistoryStore::Create */
    Result<TemporalHistoryStore> TemporalHistoryStore::Create(const TemporalHistoryLimits &limits) {
        if (!limits.IsValid())
            return Result<TemporalHistoryStore>::Failure(MakeError(TemporalHistoryErrors::InvalidLimits));
        auto owner = AcquireOwner();
        if (owner.HasError())
            return Result<TemporalHistoryStore>::Failure(owner.ErrorValue());
        try {
            std::vector<Record> records;
            records.reserve(limits.maxHistories);
            return Result<TemporalHistoryStore>::Success(TemporalHistoryStore(owner.Value(), limits, std::move(records)));
        } catch (const std::bad_alloc &) {
            return Result<TemporalHistoryStore>::Failure(MakeError(TemporalHistoryErrors::AllocationFailed));
        }
    }

    /** @copydoc TemporalHistoryStore::ValidateThreadAndState */
    Result<void> TemporalHistoryStore::ValidateThreadAndState() const {
        if (std::this_thread::get_id() != m_ownerThread)
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::WrongThread));
        if (m_stopped)
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::StoreStopped));
        return Result<void>::Success();
    }

    /** @copydoc TemporalHistoryStore::Resolve */
    Result<TemporalHistoryStore::Record *> TemporalHistoryStore::Resolve(const TemporalHistoryHandle history) {
        if (!history.IsValid())
            return Result<Record *>::Failure(MakeError(TemporalHistoryErrors::InvalidHandle));
        if (history.owner != m_owner)
            return Result<Record *>::Failure(MakeError(TemporalHistoryErrors::WrongOwner));
        if (history.slot > m_records.size())
            return Result<Record *>::Failure(MakeError(TemporalHistoryErrors::InvalidHandle));
        Record &record = m_records[history.slot - 1];
        if (!record.active || record.generation != history.generation)
            return Result<Record *>::Failure(MakeError(TemporalHistoryErrors::InvalidHandle));
        return Result<Record *>::Success(&record);
    }

    /** @copydoc TemporalHistoryStore::ResolveIdle */
    Result<TemporalHistoryStore::Record *> TemporalHistoryStore::ResolveIdle(const TemporalHistoryHandle history) {
        auto resolved = Resolve(history);
        if (resolved.HasError())
            return resolved;
        if (resolved.Value()->pendingFrame != 0)
            return Result<Record *>::Failure(MakeError(TemporalHistoryErrors::FrameAlreadyPending));
        return resolved;
    }

    /** @copydoc TemporalHistoryStore::ResolvePending */
    Result<TemporalHistoryStore::Record *> TemporalHistoryStore::ResolvePending(const TemporalHistoryFrame &frame) {
        if (auto state = ValidateThreadAndState(); state.HasError())
            return Result<Record *>::Failure(state.ErrorValue());
        auto resolved = Resolve(frame.history);
        if (resolved.HasError())
            return resolved;
        if (auto valid = ValidatePendingFrame(frame, *resolved.Value()); valid.HasError())
            return Result<Record *>::Failure(valid.ErrorValue());
        return resolved;
    }

    /** @copydoc TemporalHistoryStore::ValidateFrameRequest */
    Result<void> TemporalHistoryStore::ValidateFrameRequest(const Record &record, const std::uint64_t frameId) const {
        if (record.pendingFrame != 0)
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::FrameAlreadyPending));
        if (frameId == 0 || frameId <= record.lastPublishedFrame)
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::InvalidFrame));
        return Result<void>::Success();
    }

    /** @copydoc TemporalHistoryStore::Add */
    Result<TemporalHistoryHandle> TemporalHistoryStore::Add(const TemporalHistoryCompatibility &compatibility,
                                                            const std::span<const RenderTextureHandle> resources) {
        if (auto state = ValidateThreadAndState(); state.HasError())
            return Result<TemporalHistoryHandle>::Failure(state.ErrorValue());
        if (!compatibility.IsValid() || !AreResourcesValid(resources, m_limits))
            return Result<TemporalHistoryHandle>::Failure(MakeError(TemporalHistoryErrors::InvalidDescriptor));

        const auto reusable = std::ranges::find_if(m_records, [](const Record &record) {
            return !record.active && record.generation != std::numeric_limits<std::uint32_t>::max();
        });
        if (reusable == m_records.end() && m_records.size() == m_limits.maxHistories)
            return Result<TemporalHistoryHandle>::Failure(MakeError(TemporalHistoryErrors::CapacityExceeded));

        try {
            const std::size_t index =
                reusable == m_records.end() ? m_records.size() : static_cast<std::size_t>(reusable - m_records.begin());
            if (reusable == m_records.end())
                m_records.emplace_back();
            Record &record = m_records[index];
            record.compatibility = compatibility;
            record.resources.assign(resources.begin(), resources.end());
            record.contentGeneration = 0;
            record.lastPublishedFrame = 0;
            record.pendingFrame = 0;
            record.nextAttempt = 1;
            record.pendingAttempt = 0;
            record.pendingCanReadPrevious = false;
            record.pendingFrameReset.reset();
            record.pendingReset = TemporalHistoryResetCause::FirstFrame;
            record.active = true;
            record.valid = false;
            ++m_size;
            return Result<TemporalHistoryHandle>::Success(
                TemporalHistoryHandle{m_owner, static_cast<std::uint32_t>(index + 1), record.generation});
        } catch (const std::bad_alloc &) {
            return Result<TemporalHistoryHandle>::Failure(MakeError(TemporalHistoryErrors::AllocationFailed));
        }
    }

    /** @copydoc TemporalHistoryStore::BeginFrame */
    Result<TemporalHistoryFrame> TemporalHistoryStore::BeginFrame(const TemporalHistoryHandle history, const std::uint64_t frameId,
                                                                  const std::uint64_t predecessorFrameId) {
        return Begin(history, frameId, predecessorFrameId, nullptr);
    }

    /** @copydoc TemporalHistoryStore::BeginFrame(const TemporalHistoryFrameRequest &) */
    Result<TemporalHistoryFrame> TemporalHistoryStore::BeginFrame(const TemporalHistoryFrameRequest &request) {
        return Begin(request.history, request.frameId, request.predecessorFrameId, &request);
    }

    /** @copydoc TemporalHistoryStore::Begin */
    Result<TemporalHistoryFrame> TemporalHistoryStore::Begin(const TemporalHistoryHandle history, const std::uint64_t frameId,
                                                             const std::uint64_t predecessorFrameId,
                                                             const TemporalHistoryFrameRequest *expected) {
        if (auto state = ValidateThreadAndState(); state.HasError())
            return Result<TemporalHistoryFrame>::Failure(state.ErrorValue());
        auto resolved = Resolve(history);
        if (resolved.HasError())
            return Result<TemporalHistoryFrame>::Failure(resolved.ErrorValue());
        Record &record = *resolved.Value();
        if (auto valid = ValidateFrameRequest(record, frameId); valid.HasError())
            return Result<TemporalHistoryFrame>::Failure(valid.ErrorValue());
        if (expected && (!expected->compatibility.IsValid() || !expected->resourceOwner.IsValid() || predecessorFrameId >= frameId))
            return Result<TemporalHistoryFrame>::Failure(MakeError(TemporalHistoryErrors::InvalidFrame));
        if (expected && (expected->compatibility != record.compatibility ||
                         std::ranges::any_of(record.resources, [expected](const RenderTextureHandle resource) {
            return resource.owner != expected->resourceOwner;
        })))
            return Result<TemporalHistoryFrame>::Failure(MakeError(TemporalHistoryErrors::ResetRequired));
        const auto reset = expected ? expected->reset : std::nullopt;
        if (reset && !IsKnownTemporalHistoryResetCause(*reset))
            return Result<TemporalHistoryFrame>::Failure(MakeError(TemporalHistoryErrors::InvalidResetCause));
        if (record.nextAttempt == std::numeric_limits<std::uint64_t>::max())
            return Result<TemporalHistoryFrame>::Failure(MakeError(TemporalHistoryErrors::InvalidFrame));

        std::optional<TemporalHistoryResetCause> cause = reset ? reset : record.pendingReset;
        const bool predecessorMatches = record.valid && predecessorFrameId == record.lastPublishedFrame;
        if (!cause.has_value() && !predecessorMatches)
            cause = TemporalHistoryResetCause::MissingPredecessor;
        TemporalHistoryFrame frame{history,
                                   record.compatibility,
                                   frameId,
                                   record.contentGeneration + 1,
                                   record.nextAttempt,
                                   predecessorMatches && !cause.has_value(),
                                   cause,
                                   {}};
        frame.resources.size_ = record.resources.size();
        std::ranges::copy(record.resources, frame.resources.resources_.begin());
        record.pendingFrame = frameId;
        record.pendingAttempt = record.nextAttempt;
        record.pendingCanReadPrevious = frame.canReadPrevious;
        record.pendingFrameReset = frame.resetCause;
        if (reset)
            record.pendingReset = reset;
        ++record.nextAttempt;
        return Result<TemporalHistoryFrame>::Success(std::move(frame));
    }

    /** @copydoc TemporalHistoryStore::ValidatePendingFrame */
    Result<void> TemporalHistoryStore::ValidatePendingFrame(const TemporalHistoryFrame &frame, const Record &record) const {
        if (frame.compatibility != record.compatibility || frame.frameId == 0 || record.pendingFrame != frame.frameId ||
            frame.contentGeneration != record.contentGeneration + 1 || frame.attempt == 0 || frame.attempt != record.pendingAttempt ||
            frame.canReadPrevious != record.pendingCanReadPrevious || frame.resetCause != record.pendingFrameReset ||
            !std::ranges::equal(frame.resources, record.resources))
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::InvalidFrame));
        return Result<void>::Success();
    }

    /** @copydoc TemporalHistoryStore::CompletePending */
    Result<void> TemporalHistoryStore::CompletePending(const TemporalHistoryFrame &frame, const PendingCompletion completion) {
        auto resolved = ResolvePending(frame);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        Record &record = *resolved.Value();
        if (completion == PendingCompletion::Publish) {
            record.contentGeneration = frame.contentGeneration;
            record.lastPublishedFrame = frame.frameId;
            record.pendingReset.reset();
            record.valid = true;
        }
        record.pendingFrame = 0;
        record.pendingAttempt = 0;
        record.pendingCanReadPrevious = false;
        record.pendingFrameReset.reset();
        return Result<void>::Success();
    }

    /** @copydoc TemporalHistoryStore::CheckPending */
    Result<void> TemporalHistoryStore::CheckPending(const TemporalHistoryFrame &frame) {
        const auto pending = ResolvePending(frame);
        return pending.HasValue() ? Result<void>::Success() : Result<void>::Failure(pending.ErrorValue());
    }

    /** @copydoc TemporalHistoryStore::Publish */
    Result<void> TemporalHistoryStore::Publish(const TemporalHistoryFrame &frame) {
        return CompletePending(frame, PendingCompletion::Publish);
    }

    /** @copydoc TemporalHistoryStore::Abandon */
    Result<void> TemporalHistoryStore::Abandon(const TemporalHistoryFrame &frame) {
        return CompletePending(frame, PendingCompletion::Abandon);
    }

    /** @copydoc TemporalHistoryStore::Reset */
    Result<void> TemporalHistoryStore::Reset(const TemporalHistoryHandle history, const TemporalHistoryCompatibility &compatibility,
                                             const std::span<const RenderTextureHandle> resources, const TemporalHistoryResetCause cause) {
        if (auto state = ValidateThreadAndState(); state.HasError())
            return state;
        if (!IsKnownTemporalHistoryResetCause(cause))
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::InvalidResetCause));
        if (!compatibility.IsValid() || !AreResourcesValid(resources, m_limits))
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::InvalidDescriptor));
        auto resolved = ResolveIdle(history);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        Record &record = *resolved.Value();
        try {
            std::vector replacement(resources.begin(), resources.end());
            const bool replacesView = record.compatibility.view != compatibility.view;
            record.compatibility = compatibility;
            record.resources = std::move(replacement);
            if (replacesView) {
                record.contentGeneration = 0;
                record.lastPublishedFrame = 0;
            }
            record.pendingReset = cause;
            record.valid = false;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::AllocationFailed));
        }
    }

    /** @copydoc TemporalHistoryStore::Retire */
    Result<void> TemporalHistoryStore::Retire(const TemporalHistoryHandle history) {
        if (auto state = ValidateThreadAndState(); state.HasError())
            return state;
        auto resolved = ResolveIdle(history);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        Record &record = *resolved.Value();
        record.resources.clear();
        record.active = false;
        record.valid = false;
        record.pendingAttempt = 0;
        record.pendingCanReadPrevious = false;
        record.pendingFrameReset.reset();
        if (record.generation != std::numeric_limits<std::uint32_t>::max())
            ++record.generation;
        --m_size;
        return Result<void>::Success();
    }

    /** @copydoc TemporalHistoryStore::Shutdown */
    Result<void> TemporalHistoryStore::Shutdown() {
        if (std::this_thread::get_id() != m_ownerThread)
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::WrongThread));
        ReleaseAll();
        return Result<void>::Success();
    }

    /** @brief Releases bounded CPU records defensively without calling a backend. */
    void TemporalHistoryStore::ReleaseAll() noexcept {
        if (m_stopped)
            return;
        for (Record &record : m_records) {
            record.resources.clear();
            record.active = false;
            record.valid = false;
            record.pendingFrame = 0;
            record.pendingAttempt = 0;
            record.pendingCanReadPrevious = false;
            record.pendingFrameReset.reset();
        }
        m_size = 0;
        m_stopped = true;
    }

    /** @copydoc TemporalHistoryStore::Size */
    Result<std::size_t> TemporalHistoryStore::Size() const {
        if (std::this_thread::get_id() != m_ownerThread)
            return Result<std::size_t>::Failure(MakeError(TemporalHistoryErrors::WrongThread));
        return Result<std::size_t>::Success(m_size);
    }
}  // namespace Horo::Render
