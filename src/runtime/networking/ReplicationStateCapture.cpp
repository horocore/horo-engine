#include "ReplicationStateCaptureInternal.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Network {
    namespace {
        template <typename T> Result<T> Fail(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Closes a successful owner read exactly once on every candidate exit. */
        struct OwnerReadGuard final {
            const ICommittedReplicationSource &source;
            ReplicationCommittedRead admittedRead;

            OwnerReadGuard(const ICommittedReplicationSource &owner, const ReplicationCommittedRead &admitted) noexcept
                : source(owner), admittedRead(admitted) {}

            OwnerReadGuard(const OwnerReadGuard &) = delete;
            OwnerReadGuard &operator=(const OwnerReadGuard &) = delete;
            OwnerReadGuard(OwnerReadGuard &&) = delete;
            OwnerReadGuard &operator=(OwnerReadGuard &&) = delete;

            ~OwnerReadGuard() {
                source.EndRead(admittedRead);
            }
        };

        /** @brief Prevents a callback from recursively opening the same coordinator. */
        struct CaptureGuard final {
            bool &capturing;
            bool previous;

            explicit CaptureGuard(bool &value) noexcept : capturing(value), previous(std::exchange(value, true)) {}

            CaptureGuard(const CaptureGuard &) = delete;
            CaptureGuard &operator=(const CaptureGuard &) = delete;
            CaptureGuard(CaptureGuard &&) = delete;
            CaptureGuard &operator=(CaptureGuard &&) = delete;

            ~CaptureGuard() {
                capturing = previous;
            }
        };
    }  // namespace

    /** @brief Resolves exact sorted target identity without looking at owner storage. */
    ReplicationStateCapture::Impl::Target *ReplicationStateCapture::Impl::Find(const NetworkObjectId object) noexcept {
        const auto found = std::ranges::lower_bound(targets, object, {}, [](const Target &target) {
            return target.binding.object.object;
        });
        return found != targets.end() && found->binding.object.object == object ? std::to_address(found) : nullptr;
    }

    /** @brief Resolves immutable target metadata on the coordinator owner thread. */
    const ReplicationStateCapture::Impl::Target *ReplicationStateCapture::Impl::Find(const NetworkObjectId object) const noexcept {
        const auto found = std::ranges::lower_bound(targets, object, {}, [](const Target &target) {
            return target.binding.object.object;
        });
        return found != targets.end() && found->binding.object.object == object ? std::to_address(found) : nullptr;
    }

    /** @brief Checks callback completion fences; only identity retirement invalidates a prior snapshot. */
    Result<void> ReplicationStateCapture::Impl::ValidateRead(Target &target, const ReplicationWorldCaptureRead &worldRead,
                                                             const ICommittedReplicationSource &source,
                                                             const ReplicationCommittedRead &read,
                                                             const CancellationToken &cancellation) const {
        if (!admission->load())
            return Fail<void>(ReplicationCaptureErrors::Closed);
        if (cancellation.IsCancellationRequested())
            return Fail<void>(NetworkErrors::ReplicationWorldCancelled);
        if (!worldRead.IsCurrent()) {
            target.latest.reset();
            return Fail<void>(ReplicationCaptureErrors::Stale);
        }
        if (!source.IsCurrent(read))
            return Fail<void>(ReplicationCaptureErrors::Uncommitted);
        return Result<void>::Success();
    }

    /** @brief Validates and compares typed canonical fields without encoded buffers or source extraction. */
    Result<bool> ReplicationStateCapture::Impl::CompareCandidate(const Target &target, const ReplicationCapturedState &candidate,
                                                                 const ReplicationCapturedStatePin &prior) const {
        bool equal = prior && prior->IsCurrent();
        for (std::size_t index{}; index < candidate.fields_.size(); ++index) {
            const auto &field = candidate.fields_[index];
            const auto valid =
                serializers->CanonicallyEqual(target.binding.object.provenance.schema, field.field, field.value, field.value);
            if (valid.HasError())
                return Result<bool>::Failure(valid.ErrorValue());
            if (!valid.Value())
                return Fail<bool>(ReplicationCaptureErrors::Invalid);
            if (equal) {
                const auto compared = serializers->CanonicallyEqual(target.binding.object.provenance.schema, field.field, field.value,
                                                                    prior->fields_[index].value);
                if (compared.HasError())
                    return Result<bool>::Failure(compared.ErrorValue());
                equal = compared.Value();
            }
        }
        return Result<bool>::Success(equal);
    }

    /** @brief Captures one candidate with owner/world fences before atomic immutable publication. */
    Result<bool> ReplicationStateCapture::Impl::CaptureTarget(Target &target, const ReplicationWorldCaptureRead &worldRead,
                                                              const std::uint64_t tick, const CancellationToken &cancellation) {
        if (const auto mapping = worldRead.Resolve(target.binding.object.object);
            mapping.HasError() || mapping.Value() != target.binding.object) {
            target.latest.reset();
            return Fail<bool>(ReplicationCaptureErrors::Stale);
        }
        const auto available = std::ranges::find_if(target.pool, [](const auto &slot) {
            return slot.use_count() == 1;
        });
        if (available == target.pool.end())
            return Fail<bool>(ReplicationCaptureErrors::Capacity);
        const auto source = target.binding.source;  // Keeps a reentrant Shutdown from destroying the active owner adapter.
        auto begun = source->BeginRead(target.binding.object, tick);
        if (begun.HasError())
            return Result<bool>::Failure(begun.ErrorValue());
        OwnerReadGuard ownerRead{*source, begun.Value()};
        const auto &read = ownerRead.admittedRead;
        if (read.simulationTick != tick || read.sourceRevision == 0 || read.commitRevision == 0)
            return Fail<bool>(ReplicationCaptureErrors::Uncommitted);
        auto &candidate = **available;
        ReplicationCaptureWriter writer{candidate, target.fields};
        if (const auto captured = source->Capture(read, writer); captured.HasError())
            return Result<bool>::Failure(captured.ErrorValue());
        if (const auto valid = ValidateRead(target, worldRead, *source, read, cancellation); valid.HasError())
            return Result<bool>::Failure(valid.ErrorValue());
        if (const auto complete = writer.Complete(); complete.HasError())
            return Result<bool>::Failure(complete.ErrorValue());
        const auto prior = target.latest;
        const auto compared = CompareCandidate(target, candidate, prior);
        if (compared.HasError())
            return Result<bool>::Failure(compared.ErrorValue());
        if (const auto valid = ValidateRead(target, worldRead, *source, read, cancellation); valid.HasError())
            return Result<bool>::Failure(valid.ErrorValue());
        if (compared.Value()) {
            return Result<bool>::Success(false);
        }
        return Publish(target, candidate, *available, worldRead, read);
    }

    /** @brief Publishes one complete validated slot without reclaiming pool ownership. */
    Result<bool> ReplicationStateCapture::Impl::Publish(Target &target, ReplicationCapturedState &candidate,
                                                        const std::shared_ptr<ReplicationCapturedState> &slot,
                                                        const ReplicationWorldCaptureRead &worldRead,
                                                        const ReplicationCommittedRead &read) const {
        if (target.publication == std::numeric_limits<std::uint64_t>::max())
            return Fail<bool>(ReplicationCaptureErrors::Capacity);
        candidate.object_ = target.binding.object;
        candidate.world_ = worldRead;
        candidate.simulationTick_ = read.simulationTick;
        candidate.sourceRevision_ = read.sourceRevision;
        candidate.commitRevision_ = read.commitRevision;
        candidate.publicationRevision_ = ++target.publication;
        target.latest = slot;
        return Result<bool>::Success(true);
    }

    /** @copydoc ReplicationStateCapture::ReplicationStateCapture */
    ReplicationStateCapture::ReplicationStateCapture(ConstructionKey, std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc ReplicationStateCapture::~ReplicationStateCapture */
    ReplicationStateCapture::~ReplicationStateCapture() {
        Shutdown();
    }

    /** @copydoc ReplicationStateCapture::MarkDirty */
    Result<void> ReplicationStateCapture::MarkDirty(const NetworkObjectId object) {
        if (impl_->owner != std::this_thread::get_id())
            return Fail<void>(ReplicationCaptureErrors::Invalid);
        if (!impl_->admission->load())
            return Fail<void>(ReplicationCaptureErrors::Closed);
        auto *target = impl_->Find(object);
        if (!target)
            return Fail<void>(ReplicationCaptureErrors::Stale);
        if (!target->dirty) {
            target->dirty = true;
            impl_->hints[(impl_->hintHead + impl_->hintCount) % impl_->hints.size()] =
                static_cast<std::size_t>(target - impl_->targets.data());
            ++impl_->hintCount;
        }
        return Result<void>::Success();
    }

    /** @copydoc ReplicationStateCapture::CaptureAtCommit */
    Result<ReplicationCaptureReport> ReplicationStateCapture::CaptureAtCommit(const ReplicationWorldCaptureRead &worldRead,
                                                                              const std::uint64_t committedTick,
                                                                              const CancellationToken &cancellation) {
        if (impl_->owner != std::this_thread::get_id() || impl_->capturing || committedTick == 0 || committedTick <= impl_->lastTick ||
            worldRead.SimulationTick() != committedTick)
            return Fail<ReplicationCaptureReport>(ReplicationCaptureErrors::Invalid);
        if (!impl_->admission->load())
            return Fail<ReplicationCaptureReport>(ReplicationCaptureErrors::Closed);
        if (!worldRead.IsCurrent() || worldRead.Descriptor() != impl_->world)
            return Fail<ReplicationCaptureReport>(ReplicationCaptureErrors::Stale);
        if (cancellation.IsCancellationRequested())
            return Fail<ReplicationCaptureReport>(NetworkErrors::ReplicationWorldCancelled);
        CaptureGuard guard{impl_->capturing};
        impl_->lastTick = committedTick;
        Impl::TickWork work{worldRead, committedTick, cancellation, {.simulationTick = committedTick}};
        impl_->Schedule(work);
        work.report.deferred = impl_->targets.size() - work.report.considered;
        return Result<ReplicationCaptureReport>::Success(std::move(work.report));
    }

    /** @brief Keeps plugin exceptions inside one candidate's atomic failure boundary. */
    Result<bool> ReplicationStateCapture::Impl::CaptureSafely(Target &target, const ReplicationWorldCaptureRead &worldRead,
                                                              const std::uint64_t tick, const CancellationToken &cancellation) {
        try {
            return CaptureTarget(target, worldRead, tick, cancellation);
        } catch (const std::bad_alloc &) {
            return Fail<bool>(ReplicationCaptureErrors::Capacity);
        } catch (...) {
            // Foreign owner/codec callbacks may throw non-std types. This boundary preserves the last complete snapshot.
            return Fail<bool>(ReplicationCaptureErrors::CallbackFault);
        }
    }

    /** @brief Bounds scheduling after every owner or codec callback. */
    bool ReplicationStateCapture::Impl::Continue(const TickWork &work) const noexcept {
        return admission->load() && work.world.IsCurrent() && !work.cancellation.IsCancellationRequested() &&
               work.report.considered < std::min(targets.size(), limits.maximumTargetsPerTick);
    }

    /** @brief Charges one complete target once and records a candidate's typed outcome. */
    bool ReplicationStateCapture::Impl::Consider(const std::size_t index, TickWork &work) {
        auto &target = targets[index];
        if (target.lastConsideredTick == work.tick)
            return true;
        if (target.fields.size() > limits.maximumFieldsPerTick - work.fields || target.byteBound > limits.maximumBytesPerTick - work.bytes)
            return false;
        work.fields += target.fields.size();
        work.bytes += target.byteBound;
        target.lastConsideredTick = work.tick;
        ++work.report.considered;
        if (const auto result = CaptureSafely(target, work.world, work.tick, work.cancellation); result.HasError()) {
            ++work.report.failed;
            if (!work.report.firstError)
                work.report.firstError = result.ErrorValue();
        } else if (result.Value()) {
            ++work.report.published;
        } else {
            ++work.report.unchanged;
        }
        return true;
    }

    /** @brief Advances at most one fair reconciliation cursor step without a population scan. */
    void ReplicationStateCapture::Impl::Reconcile(TickWork &work) {
        if (Consider(cursor, work))
            cursor = (cursor + 1) % targets.size();
    }

    /** @brief Services a bounded coalesced hint ring while preserving per-tick target uniqueness. */
    void ReplicationStateCapture::Impl::ServeHints(TickWork &work) {
        const std::size_t allowance = std::min(targets.size(), limits.maximumTargetsPerTick);
        for (std::size_t attempts{}; attempts < allowance - 1; ++attempts) {
            if (hintCount == 0 || !Continue(work))
                break;
            const auto index = hints[hintHead];
            hintHead = (hintHead + 1) % hints.size();
            --hintCount;
            targets[index].dirty = false;
            if (!Consider(index, work))
                break;
        }
    }

    /** @brief Reserves reconciliation before hint service, then fills spare work with bounded rotation. */
    void ReplicationStateCapture::Impl::Schedule(TickWork &work) {
        Reconcile(work);
        ServeHints(work);
        const std::size_t allowance = std::min(targets.size(), limits.maximumTargetsPerTick);
        for (std::size_t steps = 1; steps < allowance && Continue(work); ++steps)
            Reconcile(work);
    }

    /** @copydoc ReplicationStateCapture::Latest */
    Result<ReplicationCapturedStatePin> ReplicationStateCapture::Latest(const NetworkObjectId object) const {
        if (impl_->owner != std::this_thread::get_id())
            return Fail<ReplicationCapturedStatePin>(ReplicationCaptureErrors::Invalid);
        if (!impl_->admission->load())
            return Fail<ReplicationCapturedStatePin>(ReplicationCaptureErrors::Closed);
        const auto *target = impl_->Find(object);
        if (!target || !target->latest || !target->latest->IsCurrent())
            return Fail<ReplicationCapturedStatePin>(ReplicationCaptureErrors::Stale);
        return Result<ReplicationCapturedStatePin>::Success(target->latest);
    }

    /** @copydoc ReplicationStateCapture::CanReclaim */
    bool ReplicationStateCapture::CanReclaim() const noexcept {
        if (impl_->owner != std::this_thread::get_id() || impl_->capturing || impl_->admission->load())
            return false;
        return std::ranges::all_of(impl_->targets, [](const Impl::Target &target) {
            return std::ranges::all_of(target.pool, [](const auto &slot) {
                return slot.use_count() == 1;
            });
        });
    }

    /** @copydoc ReplicationStateCapture::Shutdown */
    void ReplicationStateCapture::Shutdown() noexcept {
        if (impl_->owner != std::this_thread::get_id() || !impl_->admission->exchange(false))
            return;
        // Pools remain owned until coordinator destruction outside the hot path; old external pins never own the last pool reference.
        for (auto &target : impl_->targets) {
            target.latest.reset();
            // Owner/module pins remain until explicit coordinator destruction at the host quiescent boundary.
        }
    }
}  // namespace Horo::Network
