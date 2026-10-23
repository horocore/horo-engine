#pragma once

#include "Horo/Runtime/Ui/UiAnimationOwner.h"
#include "UiAnimationLayoutProjection.h"
#include "UiAnimationPlayback.h"
#include "UiAnimationPublisher.h"

#include <thread>

namespace Horo::Runtime::Ui {
    struct UiAnimationFrameLease::Storage final {
        explicit Storage(const UiAnimationLimits &limits, const std::size_t controlCapacity, const std::size_t elementCapacity) {
            timelines.reserve(limits.timelines);
            controls.reserve(controlCapacity);
            routes.reserve(MaximumUiScreenStackRoutes);
            markers.reserve(limits.markerCrossingsPerUpdate);
            attachments.reserve(MaximumUiAnimationAttachments - 1);
            focusGeometry.reserve((MaximumUiAnimationAttachments - 1) * elementCapacity);
        }

        UiClockSnapshot clocks;
        std::optional<UiComputedStyleSnapshot> styles;
        std::optional<UiLayoutSnapshot> layout;
        std::optional<UiLayoutClipSnapshot> clipped;
        UiReloadLease generation;
        std::optional<UiPresentationProfileSelection> selection;
        std::vector<UiAnimationAttachmentFrame> attachments;
        std::vector<UiAnimationFocusGeometry> focusGeometry;
        std::vector<UiAnimationTimelineRecord> timelines;
        std::vector<UiAnimationControlRecord> controls;
        std::vector<UiRouteInstance> routes;
        std::optional<UiAnimationRouteRecord> route;
        std::vector<UiAnimationMarkerCrossing> markers;
    };

    struct UiAnimationOwner::Storage final {
        /** @brief Release unobserved frame snapshots only at the explicit quiescent collection boundary. */
        void ReleaseUnobservedFrames();
        /** @brief Validate each active attachment's prepared geometry before the shared publication cutoff. */
        [[nodiscard]] Result<void> CanPublishAttachments(const UiElementTree &tree) const;

        struct Timeline final {
            std::uint32_t generation{};
            std::uint32_t definition{};
            AnimationInternal::PlaybackCursor cursor;
            UiAnimationClockId clock;
            UiDuration origin;
            bool pendingStart{};
            bool terminalIssued{};
            UiAnimationCancellation cancellation{UiAnimationCancellation::None};
            bool occupied{};
            bool required{};
            bool waiting{};
        };

        struct RouteStage final {
            std::uint32_t definition{};
            std::uint32_t timeline{};
            UiAnimationClockId clock;
            UiRouteInstanceId scope;
            UiElementHandle root;
            UiDuration maximumWait;
            UiAnimationRoutePhase phase{UiAnimationRoutePhase::Waiting};
        };

        struct Route final {
            explicit Route(std::uint32_t capacity) {
                stages.reserve(capacity);
            }

            std::optional<UiScreenStack::AnimationGate> gate;
            std::vector<RouteStage> stages;
            std::size_t stage{};
            bool published{};
            UiDuration elapsed;
            UiAnimationCancellation cancellation{UiAnimationCancellation::None};
            std::optional<UiAnimationRouteRecord> record;
        };

        struct Attachment final {
            UiPresentationAttachmentId id;
            UiRenderViewId view;
            UiAnimationViewport viewport;
            UiLayoutEngine layout;
            std::optional<UiLayoutClipEngine> clipping;
            std::optional<UiLayoutEngine::PreparedUpdate> preparedLayout;
            std::optional<UiLayoutClipEngine::PreparedUpdate> preparedClipping;
            std::optional<UiPresentationProfileSelection> selection;
            std::vector<UiAnimationFocusGeometry> focusGeometry;
            bool active{true};
        };

        struct Candidate final {
            UiReloadLease source;
            UiLayoutPresentationRevision presentation;
            std::optional<UiPresentationProfileSelection> selection;
            std::optional<UiAnimationViewport> viewport;
            std::optional<UiStyleResolver::PreparedUpdate> styles;
            std::optional<UiLayoutEngine::PreparedUpdate> layout;
            std::optional<UiLayoutClipEngine::PreparedUpdate> clipping;
            std::vector<Timeline> timelines;
            std::vector<UiControlStateMachine *> controls;
            UiScreenStack *actionSources{};
            UiActionRouter *canvasActions{};
            std::uint32_t frameSlot{};
            std::uint64_t commandRevision{};
            std::uint64_t sourceFrame{};
            std::uint32_t remainingCrossings{};
            bool admitted{};
            std::size_t routeStage{};
            UiDuration routeElapsed;
            UiAnimationCancellation routeCancellation{UiAnimationCancellation::None};
            bool routeTerminal{};
            bool routeChanged{};
        };

        Storage(Detail::UiAnimationPublisher publisher, RuntimeStyleRegistry registry, UiStyleResolver resolver,
                UiAnimationCanvasDefinition definition, UiAnimationLimits limits, UiElementSlotRange range, std::size_t controlCapacity)
            : publisher(std::move(publisher)), registry(std::move(registry)), resolver(std::move(resolver)),
              definition(std::move(definition)), limits(limits), range(std::move(range)), timelines(limits.timelines),
              route(limits.timelines) {
            frames.reserve(limits.retainedSnapshots);
            for (std::uint32_t index = 0; index < limits.retainedSnapshots; ++index)
                frames.push_back(
                    std::make_shared<UiAnimationFrameLease::Storage>(limits, controlCapacity, this->definition.elements.size()));
            candidate.timelines.resize(limits.timelines);
            candidate.controls.reserve(controlCapacity);
            work.elementInputs.resize(this->definition.elements.size());
            work.layoutDescriptors.resize(this->definition.elements.size());
            work.focusBounds.resize(this->definition.elements.size());
            work.focusEligibility.resize(this->definition.elements.size());
            work.layoutBindings.reserve(this->definition.layoutBindings.size());
            work.samples.resize(static_cast<std::size_t>(limits.timelines) * limits.propertiesPerTimeline);
            work.sampleCounts.resize(this->definition.elements.size());
            work.sampleOffsets.resize(this->definition.elements.size() + 1);
        }

        const std::thread::id ownerThread{std::this_thread::get_id()};
        Detail::UiAnimationPublisher publisher;
        RuntimeStyleRegistry registry;
        UiStyleResolver resolver;
        UiAnimationCanvasDefinition definition;
        UiAnimationLimits limits;
        UiElementSlotRange range;
        std::vector<Timeline> timelines;
        std::vector<std::shared_ptr<UiAnimationFrameLease::Storage>> frames;

        std::optional<UiPresentationProfileTable> profiles;
        UiPresentationAttachmentId primaryAttachment;
        std::array<std::optional<Attachment>, MaximumUiAnimationAttachments - 1> attachments;
        std::array<std::uint32_t, MaximumUiAnimationAttachments - 1> attachmentGenerations{};
        UiLayoutPresentationRevision presentation;
        std::optional<UiAnimationViewport> publishedViewport;
        bool attachmentDirty{};
        std::optional<UiLayoutInspector> inspector;
        UiPresentationAttachmentId inspectedAttachment;

        struct WorkBuffers final {
            std::vector<UiStyleElementInput> elementInputs;
            std::vector<UiLayoutElementDescriptor> layoutDescriptors;
            std::vector<UiLogicalRect> focusBounds;
            std::vector<std::uint8_t> focusEligibility;
            std::vector<AnimationInternal::LayoutBinding> layoutBindings;
            std::vector<UiStyleAnimationSample> samples;
            std::vector<std::uint32_t> sampleCounts;
            std::vector<std::uint32_t> sampleOffsets;
        };

        WorkBuffers work;

        Candidate candidate;
        Route route;
        std::uint32_t issuedRouteClockGeneration{1};

        struct ClockBinding final {
            UiAnimationHostSourceId source;
            UiClockSnapshot clocks;
            bool bound{};
        };

        ClockBinding binding;

        std::optional<std::uint32_t> currentFrame;
        std::uint64_t commandRevision{1};
        std::uint32_t pendingCommands{};
        std::uint64_t lastSourceFrame{};

        /** @brief Owner-thread lifecycle admission; callback dispatch excludes reload, publication and reclamation. */
        struct LifecycleState final {
            bool stopped{};
            bool draining{};
            bool pointerDispatching{};
        };

        LifecycleState lifecycle;
    };
}  // namespace Horo::Runtime::Ui
