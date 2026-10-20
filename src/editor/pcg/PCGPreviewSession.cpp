#include "Horo/Editor/PCGPreviewSession.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <thread>

namespace Horo::Editor {
    namespace {
        /** @brief Uses the document's closed failure vocabulary without erasing worker causes. */
        template <typename T> Result<T> Reject(PCGDocumentCommandFailureReason reason) {
            return Result<T>::Failure(MakeError(PCGDocumentError(reason)));
        }

        /** @brief Captures logical currentness independently from provider memory ownership. */
        PCGPreviewFence Fence(const PCGDocumentSnapshot &document, const PCG::PCGSpatialSnapshot &spatial) {
            const auto &provider = spatial.Provenance();
            return {document.session, document.revision, document.catalogRevision,         document.state, spatial.Id(), provider.provider,
                    provider.source,  provider.revision, spatial.Coordinates().originEpoch};
        }

        /** @brief Composes an operation-private registry from the captured executable catalog and exact source. */
        Result<PCG::PCGRegistrySnapshot> Registry(const PCGPreviewRequest &request) {
            using namespace PCG;
            auto created = PCGRegistry::Create(PCGRegistryInstanceId::Create(request.evaluation.world.Value()).Value(),
                                               request.catalog.Capabilities());
            if (created.HasError())
                return Result<PCGRegistrySnapshot>::Failure(created.ErrorValue());
            auto registry = std::move(created).Value();
            for (const auto &entry : request.catalog.Nodes()) {
                auto registered =
                    registry.RegisterNodeRuntime({entry.type, entry.runtimeContractVersion, entry.determinism, entry.requiredCapabilities});
                if (registered.HasError())
                    return Result<PCGRegistrySnapshot>::Failure(registered.ErrorValue());
            }
            PCGGraphDescriptor descriptor{.generation = request.document.source->Data().generation};
            for (const auto &node : request.document.source->Data().nodes)
                descriptor.nodes.push_back({node.id, node.type});
            auto registered = registry.RegisterGraph(std::move(descriptor));
            if (registered.HasError())
                return Result<PCGRegistrySnapshot>::Failure(registered.ErrorValue());
            return registry.Snapshot();
        }

        /** @brief Performs ordinary transient validation/cook against the captured source; no plan is durably published. */
        Result<PCG::PCGCookedPlan> Cook(const PCGPreviewRequest &request) {
            using namespace PCG;
            const auto snapshot = Registry(request);
            if (snapshot.HasError())
                return Result<PCGCookedPlan>::Failure(snapshot.ErrorValue());
            const std::array capabilities{PCGCapability::EditorPreview};
            const auto required = PCGCapabilitySet::Create(capabilities).Value();
            const auto validated = ValidatePCGGraph(*request.document.source, snapshot.Value(), required);
            if (validated.HasError())
                return Result<PCGCookedPlan>::Failure(validated.ErrorValue());
            return CompilePCGGraph(*request.document.source, validated.Value(), snapshot.Value(), request.catalog, required);
        }

        /** @brief Builds finite shapes for every ordinary cooked point output in the plan's canonical order. */
        std::vector<PCG::PCGPointOutputBound> Bounds(const PCG::PCGCookedPlan &plan, const PCGPreviewRequest &request) {
            std::vector<PCG::PCGPointOutputBound> bounds;
            for (std::size_t index = 0; index < plan.Nodes().size(); ++index)
                for (const auto &pin : plan.Nodes()[index].pins)
                    if (pin.direction == PCG::PCGPinDirection::Output && pin.type == PCG::PCGPinType::PointSet)
                        bounds.push_back({static_cast<std::uint32_t>(index), pin.id, request.schema, request.maximumPointsPerOutput});
            return bounds;
        }

        /** @brief Worker-only ordinary cook/evaluation with cooperative cancellation between bounded phases. */
        Result<PCG::PCGCpuCandidate> Evaluate(const PCGPreviewRequest &request, const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Reject<PCG::PCGCpuCandidate>(PCGDocumentCommandFailureReason::Cancelled);
            const auto plan = Cook(request);
            if (plan.HasError())
                return Result<PCG::PCGCpuCandidate>::Failure(plan.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Reject<PCG::PCGCpuCandidate>(PCGDocumentCommandFailureReason::Cancelled);
            const auto bounds = Bounds(plan.Value(), request);
            return PCG::EvaluatePCGCpu(plan.Value(), request.spatial, bounds, request.inputs, request.evaluation,
                                       PCG::PCGCpuAdmission::Accepting, cancellation);
        }

        /** @brief Checks the exact immutable document owner independently from executable/provider memory. */
        bool ValidDocument(const PCGDocumentSnapshot &document) {
            return document.source && document.session.IsValid() && document.revision != 0 && document.catalogRevision != 0;
        }

        /** @brief Checks the closed worker envelope before an older accepted intent is superseded. */
        bool ValidCapture(const PCGPreviewRequest &request) {
            return request.catalog.IsValid() && request.schema && request.maximumPointsPerOutput > 0 &&
                   request.maximumPointsPerOutput <= 65'536 && request.inputs.size() <= 1024 &&
                   request.catalog.Capabilities().granted.Contains(PCG::PCGCapability::EditorPreview);
        }

        /** @brief Checks immutable host session admission separately from per-request source validation. */
        bool ValidSession(PCG::PCGWorldId world, std::size_t maximumDrawPoints) {
            return world.IsValid() && maximumDrawPoints > 0 && maximumDrawPoints <= 65'536;
        }

        /** @brief Tests currentness without treating a retained lease as publication authority. */
        bool Matches(const PCGPreviewFence &fence, const PCGPreviewFence *current) {
            return current && fence == *current;
        }
    }  // namespace

    struct PCGPreviewSession::State final {
        /** @brief One cross-thread mailbox. Only result is shared; its mutex is never held during work, wait or callbacks. */
        struct Mailbox final {
            std::mutex mutex;
            std::optional<PCG::PCGCpuCandidate> candidate;
        };

        struct Pending final {
            std::uint64_t id;
            PCGPreviewFence fence;
            PCGPreviewRequest request;
        };

        struct Active final {
            std::uint64_t id;
            PCGPreviewFence fence;
            JobHandle job;
            std::shared_ptr<Mailbox> mailbox;
        };

        JobSystem &jobs;
        PCG::PCGWorldId world;
        std::size_t maximumDrawPoints;
        std::thread::id owner{std::this_thread::get_id()};
        std::optional<Pending> pending;
        std::optional<Active> active;
        PCGPreviewSnapshot view;
        std::vector<std::weak_ptr<const PCGPreviewOverlay>> retained;
        CancellationToken currentCancellation;
        bool closed{};
    };

    /** @copydoc PCGPreviewSession::PCGPreviewSession */
    PCGPreviewSession::PCGPreviewSession(JobSystem &jobs, PCG::PCGWorldId world, std::size_t maximumDrawPoints)
        : state_(std::make_unique<State>(State{.jobs = jobs, .world = world, .maximumDrawPoints = maximumDrawPoints})) {
        state_->retained.reserve(16);
    }

    PCGPreviewSession::~PCGPreviewSession() {
        if (state_->active) {
            static_cast<void>(state_->active->job.RequestCancel());
            // Lifecycle fallback only: never detach a worker or release its immutable capture before termination.
            static_cast<void>(state_->active->job.Wait());
        }
    }

    /** @copydoc PCGPreviewSession::CheckAccess */
    Result<void> PCGPreviewSession::CheckAccess() const {
        if (state_->owner != std::this_thread::get_id())
            return Reject<void>(PCGDocumentCommandFailureReason::WrongThread);
        return Result<void>::Success();
    }

    /** @copydoc PCGPreviewSession::Request */
    Result<std::uint64_t> PCGPreviewSession::Request(PCGPreviewRequest request) {
        if (auto access = CheckAccess(); access.HasError())
            return Result<std::uint64_t>::Failure(access.ErrorValue());
        if (state_->closed)
            return Reject<std::uint64_t>(PCGDocumentCommandFailureReason::Closed);
        if (!ValidSession(state_->world, state_->maximumDrawPoints) || !ValidDocument(request.document) || !ValidCapture(request))
            return Reject<std::uint64_t>(PCGDocumentCommandFailureReason::Invalid);
        if (request.cancellation.IsCancellationRequested())
            return Reject<std::uint64_t>(PCGDocumentCommandFailureReason::Cancelled);
        if (state_->view.request == std::numeric_limits<std::uint64_t>::max())
            return Reject<std::uint64_t>(PCGDocumentCommandFailureReason::Exhausted);
        std::erase_if(state_->retained, [](const auto &lease) {
            return lease.expired();
        });
        if (state_->retained.size() >= 16)
            return Reject<std::uint64_t>(PCGDocumentCommandFailureReason::Limit);
        request.evaluation.world = state_->world;
        request.evaluation.jobs = nullptr;
        request.evaluation.workers = 1;        // One background CPU job; no nested scheduler borrows or runtime time slicing.
        request.evaluation.retainedBytes = 0;  // Recomputed from outstanding immutable overlay leases at submission.
        request.evaluation.grantedCapabilities = request.catalog.Capabilities().granted;
        const auto fence = Fence(request.document, request.spatial);
        State::Pending pending{state_->view.request + 1, fence, std::move(request)};
        if (state_->active)
            static_cast<void>(state_->active->job.RequestCancel());
        state_->currentCancellation = pending.request.cancellation;
        state_->pending = std::move(pending);
        state_->view = {.phase = PCGPreviewPhase::Evaluating, .request = state_->pending->id};
        return Result<std::uint64_t>::Success(state_->view.request);
    }

    /** @copydoc PCGPreviewSession::StartPending */
    Result<void> PCGPreviewSession::StartPending() {
        auto &pending = *state_->pending;
        std::size_t retainedBytes = 0;
        for (const auto &lease : state_->retained) {
            if (const auto overlay = lease.lock()) {
                const auto bytes = overlay->candidate.ReservedBytes();
                if (bytes > std::numeric_limits<std::size_t>::max() - retainedBytes) {
                    state_->pending.reset();
                    state_->view.phase = PCGPreviewPhase::Failed;
                    state_->view.error = MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Limit));
                    return Result<void>::Failure(*state_->view.error);
                }
                retainedBytes += bytes;
            }
        }
        pending.request.evaluation.retainedBytes = retainedBytes;
        auto mailbox = std::make_shared<State::Mailbox>();
        const auto parent = pending.request.cancellation;
        // Move the immutable capture into a shared const root before submission; no worker retains the session.
        auto capture = std::make_shared<const PCGPreviewRequest>(std::move(pending.request));
        auto submitted = state_->jobs.SubmitResult({.parentCancellation = parent, .priority = JobPriority::Background},
                                                   [capture, mailbox](const CancellationToken &token) -> Result<void> {
            auto evaluated = Evaluate(*capture, token);
            if (token.IsCancellationRequested())
                return JobCancelled();
            if (evaluated.HasError())
                return Result<void>::Failure(evaluated.ErrorValue());
            std::lock_guard lock(mailbox->mutex);
            mailbox->candidate = std::move(evaluated).Value();
            return Result<void>::Success();
        });
        if (submitted.HasError()) {
            state_->pending.reset();
            state_->view.phase = PCGPreviewPhase::Failed;
            state_->view.error = submitted.ErrorValue();
            return Result<void>::Failure(submitted.ErrorValue());
        }
        state_->active = State::Active{pending.id, pending.fence, std::move(submitted).Value(), std::move(mailbox)};
        state_->pending.reset();
        return Result<void>::Success();
    }

    /** @copydoc PCGPreviewSession::Obsolete */
    bool PCGPreviewSession::Obsolete(const PCGPreviewFence *current) const {
        if (state_->currentCancellation.IsCancellationRequested())
            return true;
        if (state_->pending && !Matches(state_->pending->fence, current))
            return true;
        if (state_->active && state_->active->id == state_->view.request && !Matches(state_->active->fence, current))
            return true;
        return state_->view.overlay && !Matches(state_->view.overlay->fence, current);
    }

    /** @copydoc PCGPreviewSession::CanPublish */
    bool PCGPreviewSession::CanPublish(const PCGPreviewFence *current) const {
        return !state_->closed && !state_->pending && state_->active->id == state_->view.request &&
               state_->view.phase == PCGPreviewPhase::Evaluating && Matches(state_->active->fence, current);
    }

    /** @copydoc PCGPreviewSession::CompleteActive */
    void PCGPreviewSession::CompleteActive(const PCGPreviewFence *current) {
        if (!state_->active)
            return;
        auto &active = *state_->active;
        const auto job = active.job.Snapshot();
        if (!job || !job->terminalResult)
            return;
        if (CanPublish(current)) {
            state_->view.error = job->terminalResult->error;
            state_->view.phase = job->state == JobState::Succeeded   ? PCGPreviewPhase::Ready
                                 : job->state == JobState::Cancelled ? PCGPreviewPhase::Cancelled
                                                                     : PCGPreviewPhase::Failed;
            std::lock_guard lock(active.mailbox->mutex);
            if (state_->view.phase == PCGPreviewPhase::Ready && active.mailbox->candidate) {
                state_->view.overlay = std::make_shared<const PCGPreviewOverlay>(
                    PCGPreviewOverlay{active.id, active.fence, std::move(*active.mailbox->candidate)});
                state_->retained.push_back(state_->view.overlay);
            }
        }
        state_->active.reset();
    }

    /** @copydoc PCGPreviewSession::Advance */
    Result<PCGPreviewSnapshot> PCGPreviewSession::Advance(const PCGDocumentSnapshot *document, const PCG::PCGSpatialSnapshot *spatial) {
        if (auto access = CheckAccess(); access.HasError())
            return Result<PCGPreviewSnapshot>::Failure(access.ErrorValue());
        const auto current = document && spatial ? std::optional{Fence(*document, *spatial)} : std::nullopt;
        const auto *fence = current ? &*current : nullptr;
        if (Obsolete(fence))
            static_cast<void>(Cancel());
        CompleteActive(fence);
        if (!state_->active && state_->pending && !state_->closed)
            static_cast<void>(StartPending());
        return Result<PCGPreviewSnapshot>::Success(state_->view);
    }

    /** @copydoc PCGPreviewSession::Cancel */
    Result<void> PCGPreviewSession::Cancel() {
        if (auto access = CheckAccess(); access.HasError())
            return access;
        if (state_->active)
            static_cast<void>(state_->active->job.RequestCancel());
        state_->pending.reset();
        state_->view.overlay.reset();
        state_->view.error.reset();
        state_->view.phase = state_->closed ? PCGPreviewPhase::Closed : PCGPreviewPhase::Cancelled;
        return Result<void>::Success();
    }

    /** @copydoc PCGPreviewSession::Shutdown */
    Result<void> PCGPreviewSession::Shutdown(Duration timeout) {
        if (auto access = CheckAccess(); access.HasError())
            return access;
        if (timeout <= Duration{})
            return Reject<void>(PCGDocumentCommandFailureReason::Invalid);
        state_->closed = true;
        static_cast<void>(Cancel());
        if (state_->active) {
            auto waited = state_->active->job.Wait({WaitPolicy::OwnerThreadBlockAllowed, timeout});
            const auto job = state_->active->job.Snapshot();
            if (!job || !job->terminalResult)
                return waited;
            state_->active.reset();
        }
        return Result<void>::Success();
    }

    /** @copydoc PCGPreviewSession::MaximumDrawPoints */
    std::size_t PCGPreviewSession::MaximumDrawPoints() const noexcept {
        return state_->maximumDrawPoints;
    }
}  // namespace Horo::Editor
