#include "Horo/Runtime/Render/MotionHistory.h"

#include "Horo/Runtime/Render/MotionHistoryErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Render {
    struct RenderMotionHistoryTracker::PublishedObject {
        RenderMotionObjectId object;
        Math::Mat4 localToWorld{Math::Mat4::Identity()};

        [[nodiscard]] constexpr auto operator<=>(const PublishedObject &) const noexcept = default;
    };

    namespace {
        /** @brief Selects the first explicit invalidation signal in deterministic diagnostic priority. */
        [[nodiscard]] std::optional<TemporalHistoryResetCause> ExplicitReset(const RenderMotionInvalidationSignals &signals) noexcept {
            if (signals.invalidInput)
                return TemporalHistoryResetCause::InvalidInput;
            if (signals.suspended)
                return TemporalHistoryResetCause::Suspension;
            if (signals.cameraCut)
                return TemporalHistoryResetCause::CameraCut;
            if (signals.explicitRequest)
                return TemporalHistoryResetCause::ExplicitRequest;
            if (signals.skippedFrame)
                return TemporalHistoryResetCause::SkippedFrame;
            if (signals.providerRequested)
                return TemporalHistoryResetCause::ProviderRequested;
            return std::nullopt;
        }

        /** @brief Maps view, surface, device, provider, and mode identity changes. */
        [[nodiscard]] std::optional<TemporalHistoryResetCause> IdentityReset(const TemporalHistoryCompatibility &previous,
                                                                             const TemporalHistoryCompatibility &current) noexcept {
            if (previous.view != current.view)
                return TemporalHistoryResetCause::ViewReplacement;
            if (previous.surfaceGeneration != current.surfaceGeneration)
                return TemporalHistoryResetCause::SurfaceReplacement;
            if (previous.deviceGeneration != current.deviceGeneration)
                return TemporalHistoryResetCause::DeviceReplacement;
            if (previous.provider != current.provider || previous.providerGeneration != current.providerGeneration)
                return TemporalHistoryResetCause::ProviderReplacement;
            if (previous.mode != current.mode || previous.modeGeneration != current.modeGeneration)
                return TemporalHistoryResetCause::ModeReplacement;
            return std::nullopt;
        }

        /** @brief Maps extent, schema, projection, and color compatibility changes. */
        [[nodiscard]] std::optional<TemporalHistoryResetCause> ImageReset(const TemporalHistoryCompatibility &previous,
                                                                          const TemporalHistoryCompatibility &current) noexcept {
            if (previous.renderExtent != current.renderExtent || previous.targetExtent != current.targetExtent)
                return TemporalHistoryResetCause::Resize;
            if (previous.inputSchemaGeneration != current.inputSchemaGeneration || previous.jitterGeneration != current.jitterGeneration)
                return TemporalHistoryResetCause::SchemaReplacement;
            if (previous.projectionGeneration != current.projectionGeneration)
                return TemporalHistoryResetCause::ProjectionChange;
            if (previous.colorGeneration != current.colorGeneration || previous.exposureGeneration != current.exposureGeneration)
                return TemporalHistoryResetCause::ColorPlanChange;
            return std::nullopt;
        }

        /** @brief Maps scene, raster-profile, and recipe generation changes. */
        [[nodiscard]] std::optional<TemporalHistoryResetCause> SceneReset(const TemporalHistoryCompatibility &previous,
                                                                          const TemporalHistoryCompatibility &current) noexcept {
            if (previous.sceneOriginGeneration != current.sceneOriginGeneration || previous.motionGeneration != current.motionGeneration)
                return TemporalHistoryResetCause::SceneDiscontinuity;
            if (previous.rasterGeneration != current.rasterGeneration)
                return TemporalHistoryResetCause::ProfileChange;
            if (previous.recipeGeneration != current.recipeGeneration)
                return TemporalHistoryResetCause::RecipeChange;
            return std::nullopt;
        }

        /** @brief Maps every changed compatibility dimension to a stable reset cause. */
        [[nodiscard]] std::optional<TemporalHistoryResetCause> CompatibilityReset(const TemporalHistoryCompatibility &previous,
                                                                                  const TemporalHistoryCompatibility &current) noexcept {
            if (auto reset = IdentityReset(previous, current); reset.has_value())
                return reset;
            if (auto reset = ImageReset(previous, current); reset.has_value())
                return reset;
            return SceneReset(previous, current);
        }

        /** @brief Validates all non-allocating frame request invariants. */
        [[nodiscard]] bool IsRequestValid(const RenderMotionFrameRequest &request, const RenderMotionHistoryLimits &limits,
                                          const std::uint64_t lastPublishedFrame, const std::uint64_t nextAttempt) noexcept {
            const bool identities = request.compatibility.IsValid() && request.frameId != 0 && request.frameId > lastPublishedFrame;
            const bool inputs = request.camera.IsValid() && request.disocclusion.IsValid();
            const bool objects =
                request.objects.size() <= limits.maxObjects && std::ranges::all_of(request.objects, &RenderMotionObjectSample::IsValid);
            return identities && inputs && objects && nextAttempt != std::numeric_limits<std::uint64_t>::max();
        }

        /** @brief Resolves first-frame, explicit, compatibility, and predecessor invalidation in priority order. */
        [[nodiscard]] std::optional<TemporalHistoryResetCause> ResolveReset(const std::optional<TemporalHistoryCompatibility> &published,
                                                                            const RenderMotionFrameRequest &request,
                                                                            const std::uint64_t lastPublishedFrame) noexcept {
            if (!published.has_value())
                return TemporalHistoryResetCause::FirstFrame;
            if (auto reset = ExplicitReset(request.invalidation); reset.has_value())
                return reset;
            if (auto reset = CompatibilityReset(*published, request.compatibility); reset.has_value())
                return reset;
            if (request.predecessorFrameId != lastPublishedFrame)
                return TemporalHistoryResetCause::MissingPredecessor;
            return std::nullopt;
        }

        /** @brief Compares a caller frame with the immutable transaction snapshot retained by the tracker. */
        [[nodiscard]] bool SameFrame(const RenderMotionFrame &left, const RenderMotionFrame &right) noexcept {
            return left.compatibility == right.compatibility && left.frameId == right.frameId && left.attempt == right.attempt &&
                   left.convention == right.convention && left.camera == right.camera && left.disocclusion == right.disocclusion &&
                   left.resetCause == right.resetCause && left.objects == right.objects;
        }
    }  // namespace

    /** @copydoc RenderMotionHistoryTracker::RenderMotionHistoryTracker */
    RenderMotionHistoryTracker::RenderMotionHistoryTracker(RenderMotionHistoryLimits limits, std::vector<PublishedObject> objects) noexcept
        : m_limits(limits), m_ownerThread(std::this_thread::get_id()), m_objects(std::move(objects)) {}

    /** @copydoc RenderMotionHistoryTracker::RenderMotionHistoryTracker */
    RenderMotionHistoryTracker::RenderMotionHistoryTracker(RenderMotionHistoryTracker &&other) noexcept
        : m_limits(other.m_limits), m_ownerThread(other.m_ownerThread), m_objects(std::move(other.m_objects)),
          m_compatibility(std::move(other.m_compatibility)), m_camera(other.m_camera),
          m_lastPublishedFrame(std::exchange(other.m_lastPublishedFrame, 0)), m_nextAttempt(std::exchange(other.m_nextAttempt, 1)),
          m_pending(std::move(other.m_pending)), m_stopped(std::exchange(other.m_stopped, true)) {}

    /** @copydoc RenderMotionHistoryTracker::operator= */
    RenderMotionHistoryTracker &RenderMotionHistoryTracker::operator=(RenderMotionHistoryTracker &&other) noexcept {
        if (this == &other)
            return *this;
        ReleaseAll();
        m_limits = other.m_limits;
        m_ownerThread = other.m_ownerThread;
        m_objects = std::move(other.m_objects);
        m_compatibility = std::move(other.m_compatibility);
        m_camera = other.m_camera;
        m_lastPublishedFrame = std::exchange(other.m_lastPublishedFrame, 0);
        m_nextAttempt = std::exchange(other.m_nextAttempt, 1);
        m_pending = std::move(other.m_pending);
        m_stopped = std::exchange(other.m_stopped, true);
        return *this;
    }

    /** @copydoc RenderMotionHistoryTracker::~RenderMotionHistoryTracker */
    RenderMotionHistoryTracker::~RenderMotionHistoryTracker() {
        ReleaseAll();
    }

    /** @copydoc RenderMotionHistoryTracker::Create */
    Result<RenderMotionHistoryTracker> RenderMotionHistoryTracker::Create(const RenderMotionHistoryLimits &limits) {
        if (!limits.IsValid())
            return Result<RenderMotionHistoryTracker>::Failure(MakeError(MotionHistoryErrors::InvalidLimits));
        try {
            std::vector<PublishedObject> objects;
            objects.reserve(limits.maxObjects);
            return Result<RenderMotionHistoryTracker>::Success(RenderMotionHistoryTracker(limits, std::move(objects)));
        } catch (const std::bad_alloc &) {
            return Result<RenderMotionHistoryTracker>::Failure(MakeError(MotionHistoryErrors::AllocationFailed));
        }
    }

    /** @copydoc RenderMotionHistoryTracker::ValidateThreadAndState */
    Result<void> RenderMotionHistoryTracker::ValidateThreadAndState() const {
        if (std::this_thread::get_id() != m_ownerThread)
            return Result<void>::Failure(MakeError(MotionHistoryErrors::WrongThread));
        if (m_stopped)
            return Result<void>::Failure(MakeError(MotionHistoryErrors::TrackerStopped));
        return Result<void>::Success();
    }

    /** @copydoc RenderMotionHistoryTracker::BuildFrame */
    RenderMotionFrame RenderMotionHistoryTracker::BuildFrame(const RenderMotionFrameRequest &request,
                                                             const std::span<const RenderMotionObjectSample> current,
                                                             std::optional<TemporalHistoryResetCause> reset) {
        const bool historyValid = !reset.has_value();
        RenderMotionFrame frame;
        frame.compatibility = request.compatibility;
        frame.frameId = request.frameId;
        frame.attempt = m_nextAttempt++;
        frame.camera.currentUnjitteredViewProjection = request.camera.unjitteredViewProjection;
        frame.camera.previousUnjitteredViewProjection =
            historyValid ? m_camera.unjitteredViewProjection : request.camera.unjitteredViewProjection;
        frame.camera.currentJitterUv = request.camera.jitterUv;
        frame.camera.previousJitterUv = historyValid ? m_camera.jitterUv : request.camera.jitterUv;
        frame.camera.hasPrevious = historyValid;
        frame.disocclusion = request.disocclusion;
        frame.resetCause = reset;
        frame.objects.reserve(current.size());

        for (const RenderMotionObjectSample &sample : current) {
            const auto previous = std::ranges::lower_bound(m_objects, sample.object, {}, &PublishedObject::object);
            const bool hasPrevious = historyValid && previous != m_objects.end() && previous->object == sample.object;
            frame.objects.push_back(
                {sample.object, sample.localToWorld, hasPrevious ? previous->localToWorld : sample.localToWorld, hasPrevious});
        }
        return frame;
    }

    /** @copydoc RenderMotionHistoryTracker::BeginFrame */
    Result<RenderMotionFrame> RenderMotionHistoryTracker::BeginFrame(const RenderMotionFrameRequest &request) {
        if (auto state = ValidateThreadAndState(); state.HasError())
            return Result<RenderMotionFrame>::Failure(state.ErrorValue());
        if (m_pending.has_value())
            return Result<RenderMotionFrame>::Failure(MakeError(MotionHistoryErrors::FrameAlreadyPending));
        if (!IsRequestValid(request, m_limits, m_lastPublishedFrame, m_nextAttempt))
            return Result<RenderMotionFrame>::Failure(MakeError(MotionHistoryErrors::InvalidRequest));

        try {
            std::vector<RenderMotionObjectSample> current(request.objects.begin(), request.objects.end());
            std::ranges::sort(current, {}, &RenderMotionObjectSample::object);
            if (std::ranges::adjacent_find(current, {}, &RenderMotionObjectSample::object) != current.end())
                return Result<RenderMotionFrame>::Failure(MakeError(MotionHistoryErrors::InvalidRequest));

            RenderMotionFrame frame = BuildFrame(request, current, ResolveReset(m_compatibility, request, m_lastPublishedFrame));
            m_pending = PendingState{frame};
            return Result<RenderMotionFrame>::Success(std::move(frame));
        } catch (const std::bad_alloc &) {
            return Result<RenderMotionFrame>::Failure(MakeError(MotionHistoryErrors::AllocationFailed));
        }
    }

    /** @copydoc RenderMotionHistoryTracker::Complete */
    Result<void> RenderMotionHistoryTracker::Complete(const RenderMotionFrame &frame, const bool publish) {
        if (auto state = ValidateThreadAndState(); state.HasError())
            return state;
        if (!m_pending.has_value() || !SameFrame(frame, m_pending->frame))
            return Result<void>::Failure(MakeError(MotionHistoryErrors::InvalidFrame));
        if (publish) {
            try {
                std::vector<PublishedObject> replacement;
                replacement.reserve(frame.objects.size());
                for (const RenderMotionObjectPair &object : frame.objects)
                    replacement.push_back({object.object, object.currentLocalToWorld});
                m_objects = std::move(replacement);
                m_compatibility = frame.compatibility;
                m_camera = {frame.camera.currentUnjitteredViewProjection, frame.camera.currentJitterUv};
                m_lastPublishedFrame = frame.frameId;
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(MakeError(MotionHistoryErrors::AllocationFailed));
            }
        }
        m_pending.reset();
        return Result<void>::Success();
    }

    /** @copydoc RenderMotionHistoryTracker::Publish */
    Result<void> RenderMotionHistoryTracker::Publish(const RenderMotionFrame &frame) {
        return Complete(frame, true);
    }

    /** @copydoc RenderMotionHistoryTracker::Abandon */
    Result<void> RenderMotionHistoryTracker::Abandon(const RenderMotionFrame &frame) {
        return Complete(frame, false);
    }

    /** @copydoc RenderMotionHistoryTracker::Shutdown */
    Result<void> RenderMotionHistoryTracker::Shutdown() {
        if (std::this_thread::get_id() != m_ownerThread)
            return Result<void>::Failure(MakeError(MotionHistoryErrors::WrongThread));
        ReleaseAll();
        return Result<void>::Success();
    }

    /** @brief Releases all CPU history without touching backend resources. */
    void RenderMotionHistoryTracker::ReleaseAll() noexcept {
        if (m_stopped)
            return;
        m_pending.reset();
        m_objects.clear();
        m_compatibility.reset();
        m_lastPublishedFrame = 0;
        m_stopped = true;
    }
}  // namespace Horo::Render
