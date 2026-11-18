#include "Horo/Runtime/Render/RenderOriginScene.h"

#include <algorithm>
#include <limits>
#include <new>
#include <thread>
#include <vector>

namespace Horo::Render {
    namespace WS = WorldStreaming;

    namespace Detail {
        /** @brief Storage remains immutable while any frame pins the slot, independent of origin lifetime. */
        struct RenderOriginFrameStorage final {
            WS::OriginFrameBinding binding;
            RenderCameraView camera;
            std::vector<RenderStaticMeshInstance> instances;
            std::size_t instanceCount{};
            std::vector<char> materialBytes;
            std::vector<RenderLight> lights;
            std::uint64_t sceneDomain{};
            std::uint64_t revision{};
        };

        /** @brief Owner-lane state shared only by the adapter and its exact prepared rebase receipts. */
        struct RenderOriginSceneState final {
            const WS::OriginFrameOwner *source;
            WS::OriginFrame frame;
            RenderViewId view;
            WS::OriginRebaseParticipantRequirement requirement;
            RenderOriginSceneLimits limits;
            const std::thread::id lane{std::this_thread::get_id()};
            std::vector<std::shared_ptr<RenderOriginFrameStorage>> slots;
            std::uint64_t revision{};
            bool active{true};

            [[nodiscard]] bool UsesOwnerLane() const noexcept {
                return std::this_thread::get_id() == lane;
            }
        };
    }  // namespace Detail

    namespace {
        /** @brief Denies cross-owner and stale operations through the existing origin error contract. */
        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Applies no work except publishing one already validated successor on the canonical safe point. */
        class PreparedRenderOrigin final : public WS::IOriginRebasePreparedParticipant {
        public:
            PreparedRenderOrigin(std::weak_ptr<Detail::RenderOriginSceneState> owner, WS::OriginRebasePreparedBinding binding,
                                 WS::OriginFrame successor) noexcept
                : owner_(std::move(owner)), binding_(binding), successor_(successor) {}

            WS::OriginRebasePreparedBinding Binding() const noexcept override {
                return binding_;
            }

            void ApplyPrepared() noexcept override {
                const auto state = owner_.lock();
                owner_.reset();
                if (state && state->UsesOwnerLane() && state->active && state->frame.Binding() == binding_.previousFrame &&
                    state->source->Publishes(successor_))
                    state->frame = successor_;
            }

            void RollbackPrepared() noexcept override {
                owner_.reset();
            }

        private:
            std::weak_ptr<Detail::RenderOriginSceneState> owner_;
            WS::OriginRebasePreparedBinding binding_;
            WS::OriginFrame successor_;
        };

        /** @brief Preserves only pure affine source rotation and scale. Translation has exactly one canonical authority. */
        bool HasNoTranslation(const Math::Mat4 &matrix) noexcept {
            return matrix.At(0, 3) == 0 && matrix.At(1, 3) == 0 && matrix.At(2, 3) == 0 && matrix.At(3, 0) == 0 && matrix.At(3, 1) == 0 &&
                   matrix.At(3, 2) == 0 && matrix.At(3, 3) == 1;
        }

        /** @brief Subtracts canonical integer positions before one narrowing. Both points are already inside the exact frame. */
        Result<Math::Vec3> Relative(const Math::WorldCoordinate64 &point, const std::array<std::int64_t, 3> &camera) {
            const auto mm = point.Millimeters();
            const std::array delta{mm[0] - camera[0], mm[1] - camera[1], mm[2] - camera[2]};
            const auto limit = WS::OriginFrame::MaximumLocalHalfExtentMillimeters;
            if (std::ranges::any_of(delta, [limit](std::int64_t value) {
                return value < -limit || value > limit;
            }))
                return Reject<Math::Vec3>(WS::WorldStreamingErrors::OriginFrameRangeExceeded);
            return Result<Math::Vec3>::Success({static_cast<float>(static_cast<double>(delta[0]) / 1000.0),
                                                static_cast<float>(static_cast<double>(delta[1]) / 1000.0),
                                                static_cast<float>(static_cast<double>(delta[2]) / 1000.0)});
        }

        /** @brief Stages a complete converted packet. The caller reserves the slot and publishes only on success. */
        Result<void> Project(Detail::RenderOriginFrameStorage &slot, const WS::OriginFrame &frame, const RenderOriginSceneSource &source,
                             std::size_t materialIdBytes) {
            if (const auto camera = frame.ToLocal(source.camera.position); camera.HasError())
                return Result<void>::Failure(camera.ErrorValue());
            if (const auto target = frame.ToLocal(source.camera.target); target.HasError())
                return Result<void>::Failure(target.ErrorValue());
            const auto camera = source.camera.position.Millimeters();
            const auto target = Relative(source.camera.target, camera);
            if (target.HasError())
                return Result<void>::Failure(target.ErrorValue());
            slot.camera = {{}, target.Value(), source.camera.up, source.camera.projection};
            if (!slot.camera.IsValid() || Math::Length(Math::Cross(slot.camera.target, slot.camera.up)) <= Math::DefaultEpsilon)
                return Reject<void>(WS::WorldStreamingErrors::OriginFrameInvalid);

            slot.instanceCount = 0;
            for (const auto &sourceInstance : source.instances) {
                const auto &sourceDraw = sourceInstance.draw;
                if (!sourceDraw.IsValid() || !HasNoTranslation(sourceDraw.localToWorld))
                    return Reject<void>(WS::WorldStreamingErrors::OriginFrameInvalid);
                if (sourceDraw.material.value.size() > materialIdBytes)
                    return Reject<void>(WS::WorldStreamingErrors::OriginRebaseCapacityExceeded);
                if (const auto point = frame.ToLocal(sourceInstance.position); point.HasError())
                    return Result<void>::Failure(point.ErrorValue());
                const auto relative = Relative(sourceInstance.position, camera);
                if (relative.HasError())
                    return Result<void>::Failure(relative.ErrorValue());
                auto *material = slot.materialBytes.data() + slot.instanceCount * materialIdBytes;
                std::ranges::copy(sourceDraw.material.value, material);
                auto &draw = slot.instances[slot.instanceCount++];
                draw = sourceDraw;
                draw.material.value = {material, sourceDraw.material.value.size()};
                draw.localToWorld.values[12] = relative.Value().x;
                draw.localToWorld.values[13] = relative.Value().y;
                draw.localToWorld.values[14] = relative.Value().z;
            }
            slot.lights.clear();
            for (const auto &sourceLight : source.lights) {
                if (!sourceLight.light.IsValid() || sourceLight.light.position != Math::Vec3{})
                    return Reject<void>(WS::WorldStreamingErrors::OriginFrameInvalid);
                if (const auto point = frame.ToLocal(sourceLight.position); point.HasError())
                    return Result<void>::Failure(point.ErrorValue());
                const auto relative = Relative(sourceLight.position, camera);
                if (relative.HasError())
                    return Result<void>::Failure(relative.ErrorValue());
                auto light = sourceLight.light;
                light.position = relative.Value();
                slot.lights.push_back(light);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RenderOriginSceneAdapter::Create */
    Result<std::unique_ptr<RenderOriginSceneAdapter>> RenderOriginSceneAdapter::Create(const WS::OriginFrameOwner &origin,
                                                                                       RenderViewId view,
                                                                                       WS::OriginRebaseParticipantRequirement requirement,
                                                                                       RenderOriginSceneLimits limits) {
        using Output = std::unique_ptr<RenderOriginSceneAdapter>;
        if (!view.IsValid() || !requirement.participant.IsValid() || !requirement.revision.IsValid())
            return Reject<Output>(WS::WorldStreamingErrors::OriginRebaseInvalid);
        if (limits.frameSlots == 0 || limits.frameSlots > limits.MaximumFrameSlots || limits.instances > limits.MaximumInstances ||
            limits.lights > MaximumSceneLights || limits.materialIdBytes == 0 || limits.materialIdBytes > limits.MaximumMaterialIdBytes)
            return Reject<Output>(WS::WorldStreamingErrors::OriginRebaseCapacityExceeded);
        const auto lease = origin.Lease();
        if (lease.HasError())
            return Result<Output>::Failure(lease.ErrorValue());
        const auto frame = lease.Value().Get();
        if (frame.HasError())
            return Result<Output>::Failure(frame.ErrorValue());
        try {
            auto state = std::make_shared<Detail::RenderOriginSceneState>(&origin, frame.Value(), view, requirement, limits);
            state->slots.reserve(limits.frameSlots);
            for (std::size_t index = 0; index < limits.frameSlots; ++index) {
                auto slot = std::make_shared<Detail::RenderOriginFrameStorage>();
                slot->instances.resize(limits.instances);
                slot->materialBytes.resize(limits.instances * limits.materialIdBytes);
                slot->lights.reserve(limits.lights);
                state->slots.push_back(std::move(slot));
            }
            return Result<Output>::Success(Output{new RenderOriginSceneAdapter{std::move(state)}});
        } catch (const std::bad_alloc &) {
            return Reject<Output>(WS::WorldStreamingErrors::OriginRebaseStorageUnavailable);
        }
    }

    /** @copydoc RenderOriginSceneAdapter::~RenderOriginSceneAdapter */
    RenderOriginSceneAdapter::~RenderOriginSceneAdapter() {
        Shutdown();
    }

    /** @copydoc RenderOriginSceneAdapter::Requirement */
    WS::OriginRebaseParticipantRequirement RenderOriginSceneAdapter::Requirement() const noexcept {
        return state_->requirement;
    }

    /** @copydoc RenderOriginSceneAdapter::Prepare */
    Result<std::unique_ptr<WS::IOriginRebasePreparedParticipant>> RenderOriginSceneAdapter::Prepare(
        const WS::OriginRebaseEvent &event) noexcept {
        using Output = std::unique_ptr<WS::IOriginRebasePreparedParticipant>;
        if (!state_->UsesOwnerLane() || !state_->active)
            return Reject<Output>(WS::WorldStreamingErrors::OriginRebaseLifecycleUnavailable);
        const auto &old = event.previousFrame.Binding();
        const auto &next = event.replacementFrame.Binding();
        if (!event.transaction.IsValid() || !event.request.IsValid() || old.identity != next.identity ||
            old.revision.Value() == std::numeric_limits<std::uint64_t>::max() ||
            old.generation.Value() == std::numeric_limits<std::uint64_t>::max() || next.revision.Value() != old.revision.Value() + 1 ||
            next.generation.Value() != old.generation.Value() + 1)
            return Reject<Output>(WS::WorldStreamingErrors::OriginRebaseInvalid);
        if (state_->frame != event.previousFrame || !state_->source->Publishes(event.previousFrame))
            return Reject<Output>(WS::WorldStreamingErrors::OriginRebaseStale);
        try {
            const WS::OriginRebasePreparedBinding binding{state_->requirement, event.transaction, old, next};
            return Result<Output>::Success(std::make_unique<PreparedRenderOrigin>(state_, binding, event.replacementFrame));
        } catch (const std::bad_alloc &) {
            return Reject<Output>(WS::WorldStreamingErrors::OriginRebaseStorageUnavailable);
        }
    }

    /** @copydoc RenderOriginSceneAdapter::UsesOrigin */
    bool RenderOriginSceneAdapter::UsesOrigin(const WS::OriginFrameOwner &origin) const noexcept {
        return state_->UsesOwnerLane() && state_->active && state_->source == &origin && origin.IsActive();
    }

    /** @copydoc RenderOriginSceneAdapter::Capture */
    Result<RenderOriginFrame> RenderOriginSceneAdapter::Capture(const RenderOriginSceneSource &source) {
        if (!state_->UsesOwnerLane() || !state_->active)
            return Reject<RenderOriginFrame>(WS::WorldStreamingErrors::OriginFrameLifecycleUnavailable);
        if (!state_->source->Publishes(state_->frame))
            return Reject<RenderOriginFrame>(WS::WorldStreamingErrors::OriginFrameStale);
        if (source.sceneDomain == 0)
            return Reject<RenderOriginFrame>(WS::WorldStreamingErrors::OriginFrameInvalid);
        if (source.instances.size() > state_->limits.instances || source.lights.size() > state_->limits.lights ||
            state_->revision == std::numeric_limits<std::uint64_t>::max())
            return Reject<RenderOriginFrame>(WS::WorldStreamingErrors::OriginRebaseCapacityExceeded);
        const auto free = std::ranges::find_if(state_->slots, [](const auto &slot) {
            return slot.use_count() == 1;
        });
        if (free == state_->slots.end())
            return Reject<RenderOriginFrame>(WS::WorldStreamingErrors::OriginRebaseCapacityExceeded);
        auto &slot = **free;
        if (auto projected = Project(slot, state_->frame, source, state_->limits.materialIdBytes); projected.HasError())
            return Result<RenderOriginFrame>::Failure(projected.ErrorValue());
        slot.binding = state_->frame.Binding();
        slot.sceneDomain = source.sceneDomain;
        slot.revision = ++state_->revision;
        return Result<RenderOriginFrame>::Success(RenderOriginFrame{*free, state_});
    }

    /** @copydoc RenderOriginSceneAdapter::Shutdown */
    void RenderOriginSceneAdapter::Shutdown() noexcept {
        if (!state_->UsesOwnerLane())
            return;
        state_->active = false;
        state_->slots.clear();
    }

    /** @copydoc RenderOriginFrame::Current */
    Result<RenderViewId> RenderOriginFrame::Current() const {
        const auto state = owner_.lock();
        if (!state || !state->UsesOwnerLane() || !state->active)
            return Reject<RenderViewId>(WS::WorldStreamingErrors::OriginFrameLifecycleUnavailable);
        if (!storage_ || state->frame.Binding() != storage_->binding || !state->source->Publishes(state->frame))
            return Reject<RenderViewId>(WS::WorldStreamingErrors::OriginFrameStale);
        return Result<RenderViewId>::Success(state->view);
    }

    /** @copydoc RenderOriginFrame::Scene */
    Result<RenderSceneView> RenderOriginFrame::Scene(std::span<const RenderMeshResourceView> meshes) const {
        if (const auto current = Current(); current.HasError())
            return Result<RenderSceneView>::Failure(current.ErrorValue());
        return Result<RenderSceneView>::Success({storage_->camera, meshes, std::span{storage_->instances}.first(storage_->instanceCount),
                                                 storage_->lights, storage_->sceneDomain, storage_->revision});
    }

    /** @copydoc RenderOriginFrame::StampHistory */
    Result<TemporalHistoryCompatibility> RenderOriginFrame::StampHistory(TemporalHistoryCompatibility history) const {
        const auto current = Current();
        if (current.HasError())
            return Result<TemporalHistoryCompatibility>::Failure(current.ErrorValue());
        if (history.view != current.Value())
            return Reject<TemporalHistoryCompatibility>(WS::WorldStreamingErrors::OriginFrameStale);
        history.sceneOriginGeneration = storage_->binding.generation.Value();
        if (!history.IsValid())
            return Reject<TemporalHistoryCompatibility>(WS::WorldStreamingErrors::OriginFrameInvalid);
        return Result<TemporalHistoryCompatibility>::Success(history);
    }
}  // namespace Horo::Render
