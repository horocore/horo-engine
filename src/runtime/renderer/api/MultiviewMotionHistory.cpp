#include "Horo/Runtime/Render/MultiviewMotionHistory.h"

#include <algorithm>
#include <memory>

namespace Horo::Render {
    /** @copydoc MultiviewMotionHistory::Stage */
    bool MultiviewMotionHistory::Stage(const MultiviewMotionFrame &frame, const MultiviewSceneSnapshot &snapshot,
                                       const std::span<const RenderMotionObjectId> objects) noexcept {
        staged_ = false;
        if (!frame.publication.IsValid() || frame.publication != snapshot.Publication() || frame.projectionSequence == 0 ||
            frame.predictedDisplayTimeNanoseconds <= 0 || objects.size() != snapshot.Instances().size() ||
            (published_ && frame.projectionSequence <= slots_[1 - writable_].frame.projectionSequence))
            return false;
        auto &candidate = slots_[writable_];
        candidate.frame = frame;
        candidate.viewCount = snapshot.Views().size();
        candidate.objectCount = objects.size();
        for (const auto &view : snapshot.Views()) {
            if (view.depthRange != Math::ClipDepthRange::ZeroToOne)
                return false;
            candidate.views[view.index] = view.worldToClip;
        }
        for (std::size_t index = 0; index < objects.size(); ++index) {
            if (!objects[index].IsValid())
                return false;
            candidate.instanceIds[index] = objects[index];
            const auto &instance = snapshot.Instances()[index];
            candidate.objects[index] = {objects[index], instance.mesh, instance.localToWorld, 0};
        }
        for (const auto &draw : snapshot.Draws())
            candidate.objects[draw.instanceIndex].visibleViews = draw.visibleViews;
        auto table = std::span{candidate.objects}.first(candidate.objectCount);
        std::ranges::sort(table, {}, &Object::id);
        staged_ = std::ranges::adjacent_find(table, {}, &Object::id) == table.end();
        return staged_;
    }

    /** @copydoc MultiviewMotionHistory::Find */
    const MultiviewMotionHistory::Object *MultiviewMotionHistory::Find(const Projection &projection,
                                                                       const RenderMotionObjectId object) noexcept {
        const auto table = std::span{projection.objects}.first(projection.objectCount);
        const auto found = std::ranges::lower_bound(table, object, {}, &Object::id);
        return found != table.end() && found->id == object ? std::to_address(found) : nullptr;
    }

    /** @copydoc MultiviewMotionHistory::Adjacent */
    bool MultiviewMotionHistory::Adjacent() const noexcept {
        const auto &current = slots_[writable_];
        const auto &previous = slots_[1 - writable_];
        const auto &a = current.frame.publication;
        const auto &b = previous.frame.publication;
        return current.frame.projectionSequence - 1 == previous.frame.projectionSequence &&
               current.frame.predictedDisplayTimeNanoseconds > previous.frame.predictedDisplayTimeNanoseconds && a.frame > b.frame &&
               a.sceneIncarnation == b.sceneIncarnation && a.originRevision == b.originRevision && a.viewRevision == b.viewRevision &&
               a.deviceIncarnation == b.deviceIncarnation && a.capabilityRevision == b.capabilityRevision &&
               current.viewCount == previous.viewCount;
    }

    /** @copydoc MultiviewMotionHistory::Previous */
    std::optional<MultiviewMotionPredecessor> MultiviewMotionHistory::Previous(const MultiviewMotionFrame &frame,
                                                                               const RenderMotionObjectId object,
                                                                               const std::uint32_t view) const noexcept {
        const auto &current = slots_[writable_];
        if (!staged_ || !published_ || current.frame != frame || view >= current.viewCount || !Adjacent())
            return std::nullopt;
        const auto &previous = slots_[1 - writable_];
        const auto *currentObject = Find(current, object);
        const auto *previousObject = Find(previous, object);
        if (!currentObject || !previousObject || currentObject->mesh != previousObject->mesh)
            return std::nullopt;
        const auto visible = std::uint32_t{1} << view;
        if ((currentObject->visibleViews & previousObject->visibleViews & visible) == 0)
            return std::nullopt;
        return MultiviewMotionPredecessor{previous.views[view], previousObject->localToWorld};
    }

    /** @copydoc MultiviewMotionHistory::ReadDraw */
    std::optional<MultiviewMotionDraw> MultiviewMotionHistory::ReadDraw(const MultiviewMotionFrame &frame,
                                                                        const std::uint32_t instanceIndex) const noexcept {
        const auto &current = slots_[writable_];
        if (!staged_ || !published_ || current.frame != frame || instanceIndex >= current.objectCount || !Adjacent())
            return std::nullopt;
        const auto id = current.instanceIds[instanceIndex];
        const auto *object = Find(current, id);
        if (!object || object->visibleViews == 0)
            return std::nullopt;
        MultiviewMotionDraw draw{.viewCount = current.viewCount};
        for (std::uint32_t view = 0; view < current.viewCount; ++view) {
            if ((object->visibleViews & (std::uint32_t{1} << view)) == 0)
                continue;
            const auto previous = Previous(frame, id, view);
            if (!previous)
                return std::nullopt;
            draw.currentLocalToClip[view] = Math::Multiply(current.views[view], object->localToWorld);
            draw.previousLocalToClip[view] = Math::Multiply(previous->worldToClip, previous->localToWorld);
            if (!Math::IsFinite(draw.currentLocalToClip[view]) || !Math::IsFinite(draw.previousLocalToClip[view]))
                return std::nullopt;
        }
        return draw;
    }

    /** @copydoc MultiviewMotionHistory::Publish */
    bool MultiviewMotionHistory::Publish(const MultiviewMotionFrame &frame) noexcept {
        if (!staged_ || slots_[writable_].frame != frame)
            return false;
        writable_ = 1 - writable_;
        staged_ = false;
        published_ = true;
        return true;
    }

    /** @copydoc MultiviewMotionHistory::Reset */
    void MultiviewMotionHistory::Reset() noexcept {
        staged_ = false;
        published_ = false;
    }
}  // namespace Horo::Render
