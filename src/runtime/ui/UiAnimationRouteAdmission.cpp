#include "UiAnimationAdmission.h"

#include <algorithm>

namespace Horo::Runtime::Ui::AnimationInternal {
    namespace {
        /** @brief Walks only the actual bounded retained parent lineage; authored IDs alone cannot assert subtree ownership. */
        [[nodiscard]] bool DescendsFrom(const UiElementTree &tree, UiElementHandle target, const UiElementHandle root) {
            auto remaining = tree.Size();
            while (remaining != 0 && target.IsValid()) {
                --remaining;
                if (target == root)
                    return true;
                const auto record = tree.Get(target);
                if (record.HasError())
                    return false;
                target = record.Value().parent;
            }
            return false;
        }

        /** @brief Requires one exact admitted required definition and every actual track target within its bound route subtree. */
        [[nodiscard]] Result<void> ValidateMotion(const UiAnimationCanvasDefinition &definition, const UiElementTree &tree,
                                                  const UiElementHandle root, const std::optional<UiAnimationId> id,
                                                  const UiAnimationLifecycle lifecycle) {
            if (!id)
                return Result<void>::Success();
            const auto animation = std::ranges::find(definition.animations, *id, &UiAnimationDefinition::id);
            if (animation == definition.animations.end() || animation->time.domain != UiTimeDomain::ScreenTransition ||
                animation->time.lifecycle != lifecycle || animation->time.loop.kind != UiLoopKind::Finite)
                return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            for (const auto &track : animation->tracks) {
                const auto target = tree.Find(track.target);
                if (target.HasError() || !DescendsFrom(tree, target.Value(), root))
                    return Result<void>::Failure(MakeError(UiErrors::AnimationTargetStale));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateRouteBindings */
    Result<void> ValidateRouteBindings(const UiAnimationCanvasDefinition &definition, const UiReloadCanvas &canvas) {
        if (definition.routes.empty())
            return Result<void>::Success();
        if (!canvas.routes || definition.routes.size() > canvas.routes->Definitions().size())
            return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        const auto catalog = canvas.routes->Definitions();
        for (std::size_t index = 0; index < definition.routes.size(); ++index) {
            const auto &binding = definition.routes[index];
            if (const auto prior = std::span(definition.routes).first(index);
                !binding.route.IsValid() || !binding.root.IsValid() || (!binding.enter && !binding.exit) ||
                binding.maximumWait.nanoseconds <= 0 || std::ranges::find(catalog, binding.route, &UiRouteMetadata::id) == catalog.end() ||
                std::ranges::find(prior, binding.route, &UiAnimationRouteBinding::route) != prior.end())
                return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            const auto root = canvas.tree.Find(binding.root);
            if (root.HasError())
                return Result<void>::Failure(root.ErrorValue());
            if (auto enter = ValidateMotion(definition, canvas.tree, root.Value(), binding.enter, UiAnimationLifecycle::RequiredEnter);
                enter.HasError())
                return enter;
            if (auto exit = ValidateMotion(definition, canvas.tree, root.Value(), binding.exit, UiAnimationLifecycle::RequiredExit);
                exit.HasError())
                return exit;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui::AnimationInternal
