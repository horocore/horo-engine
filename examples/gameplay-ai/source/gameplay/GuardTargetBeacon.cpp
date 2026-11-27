#include "Horo/Gameplay/NativeBehavior.h"

#include <algorithm>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;

    /** @brief Publishes the observed target's live generation and position on a named gameplay channel. */
    class GuardTargetBeacon final : public IBehaviorInstance {
    public:
        static BehaviorDescriptor DescribeBehavior() {
            BehaviorDescriptor descriptor;
            descriptor.displayName = "Guard target beacon";
            descriptor.category = "Guard example";
            descriptor.fields = {{"channel", std::string{"guard"}}};
            descriptor.phases.push_back({BehaviorPhase::Gameplay, "game.example.guard.target_beacon", {}, {}, {}});
            return descriptor;
        }

        void OnFixedUpdate(BehaviorContext &context, FixedDeltaTime) override {
            const auto fields = context.Fields();
            const auto field = std::ranges::find(fields, std::string_view{"channel"}, &BehaviorField::name);
            const auto *channel = field != fields.end() ? std::get_if<std::string>(&field->value) : nullptr;
            if (!channel || channel->empty())
                return;
            const auto transform = context.LocalTransform();
            if (transform.HasError())
                return;
            const auto entity = context.Entity();
            const auto position = transform.Value().translation;
            static_cast<void>(context.Publish({GameplayEventTypeId{"game.example.guard.target"},
                                               1,
                                               std::nullopt,
                                               {{"channel", *channel},
                                                {"index", static_cast<std::int64_t>(entity.index)},
                                                {"generation", static_cast<std::int64_t>(entity.generation)},
                                                {"x", static_cast<double>(position.x)},
                                                {"y", static_cast<double>(position.y)},
                                                {"z", static_cast<double>(position.z)}}}));
        }
    };
}  // namespace

HORO_BEHAVIOR(GuardTargetBeacon, "game.example.guard.target_beacon")
