#pragma once

#include "AnimationActionTestFixtures.h"
#include "AnimationGraphInstanceTestFixtures.h"

#include <algorithm>
#include <array>
#include <vector>

namespace Horo::Animation::Test::Notifies {
    /** @brief Real immutable schema with a typed foot-contact field; no event-name parsing. */
    inline AnimationEventSchema Schema() {
        return {Id<AnimationEventTypeId>(40), 1, {{Id<AnimationEventFieldId>(1), AnimationEventValueKind::UnsignedInteger, true, {0, 1}}}};
    }

    /** @brief Fixed owned payload matches the exact schema revision. */
    inline AnimationClipEvent Event(const std::uint64_t id, const std::int64_t at, const std::optional<AnimationDeltaTime> duration = {}) {
        AnimationClipEvent value;
        value.id = Id<AnimationEventMarkerId>(id);
        value.name = "FootContact";
        value.type = Id<AnimationEventTypeId>(40);
        value.time = {at};
        value.duration = duration;
        value.fireInReverse = true;
        value.payload.count = 1;
        value.payload.fields[0] = {Id<AnimationEventFieldId>(1), std::uint64_t{1}};
        return value;
    }

    /** @brief Replaces only a real clip candidate's immutable event metadata, preserving exact descriptor/revision authority. */
    template <class Fixture>
    void Events(Fixture &fixture, const std::uint32_t clip, const std::vector<AnimationClipEvent> &events,
                const AnimationWrapMode wrap = AnimationWrapMode::Loop) {
        fixture.context.evaluationDomain = AnimationEvaluationDomain::AuthoritativeSimulation;
        auto data = fixture.clips[clip]->Data();
        data.eventSchemas = {Schema()};
        data.events = events;
        data.descriptor.wrapMode = wrap;
        auto created = AnimationClipAsset::Create(std::move(data), *fixture.skeleton);
        REQUIRE(created.HasValue());
        fixture.clips[clip] = std::make_shared<const AnimationClipAsset>(std::move(created).Value());
    }

    /** @brief Cold subscription spans are consumed synchronously by Create. */
    struct Subscription final {
        std::array<AnimationEventTypeBinding, 1> types{{{Id<AnimationEventTypeId>(40), 1}}};
        std::array<AnimationEventSubscription, 1> consumers;

        explicit Subscription(GraphInstanceCreateContext &context, const std::uint32_t capacity = 256)
            : consumers{{{Id<AnimationEventConsumerId>(7), types, capacity}}} {
            context.clipEvents.subscriptions = consumers;
            context.evaluationDomain = AnimationEvaluationDomain::AuthoritativeSimulation;
        }
    };

    /** @brief Actual Evaluate/Commit and immutable value copies in bounded caller storage. */
    struct Tick final {
        GraphInstanceOutcome committed;
        std::vector<AnimationEventOccurrence> events = std::vector<AnimationEventOccurrence>(1024);
        std::uint32_t count{};

        Tick(AnimationGraphInstance &instance, const std::uint64_t tick, const std::int64_t delta, const GraphPlaybackRate rate = {}) {
            const auto evaluated = instance.Evaluate({Id<AnimationTickId>(tick), {delta}, rate});
            INFO("failure=" << static_cast<unsigned>(evaluated.failure) << " clip=" << evaluated.clipFailure.detail);
            REQUIRE(evaluated.HasValue());
            committed = instance.Commit(Id<AnimationTickId>(tick));
            REQUIRE(committed.HasValue());
            REQUIRE(instance.CopyClipEvents(committed.publication.currentPose, events, count).HasValue());
        }
    };

    /** @brief Filters by exact immutable marker identity, never by authored name. */
    inline std::uint32_t Count(const Tick &tick, const std::uint64_t marker, const AnimationNotifyPhase phase) {
        return static_cast<std::uint32_t>(std::ranges::count_if(std::span{tick.events}.first(tick.count), [&](const auto &value) {
            return value.id.marker == Id<AnimationEventMarkerId>(marker) && value.id.phase == phase;
        }));
    }
}  // namespace Horo::Animation::Test::Notifies
