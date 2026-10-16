#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeSceneErrors.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using Matrices = std::vector<std::optional<Math::Mat4>>;

        /** @brief Carries a stable Scene failure rather than accepting malformed pose evidence. */
        template <typename T> [[nodiscard]] Result<T> InvalidPlacement(const char *message) {
            return Result<T>::Failure(MakeError(SceneErrors::InvalidCandidate, message));
        }

        /** @brief Resolves candidate hierarchy iteratively; storage and work are bounded by the Scene's slot count. */
        [[nodiscard]] Result<Matrices> WorldMatrices(RuntimeSceneView scene, const Matrices &overrides) {
            Matrices matrices(scene.SlotCount());
            std::vector<std::uint8_t> states(scene.SlotCount());
            std::vector<std::size_t> path;
            path.reserve(scene.SlotCount());
            for (std::size_t start = 0; start < scene.SlotCount(); ++start) {
                if (!scene.EntityAt(start) || states[start] == 2)
                    continue;
                std::optional<std::size_t> current = start;
                while (current && states[*current] != 2) {
                    if (states[*current] == 1)
                        return InvalidPlacement<Matrices>("Initial placement encountered a parent cycle.");
                    states[*current] = 1;
                    path.push_back(*current);
                    const auto entity = scene.EntityAt(*current);
                    if (!entity || !entity->localTransform)
                        return InvalidPlacement<Matrices>("Initial placement parent is absent.");
                    if (entity->parent && scene.Get(*entity->parent).HasError())
                        return InvalidPlacement<Matrices>("Initial placement parent belongs to a stale Scene generation.");
                    current = entity->parent ? std::optional<std::size_t>{entity->parent->entity.index} : std::nullopt;
                }
                while (!path.empty()) {
                    const std::size_t index = path.back();
                    path.pop_back();
                    const auto entity = scene.EntityAt(index);
                    if (overrides[index])
                        matrices[index] = overrides[index];
                    else {
                        auto local = entity->localTransform->TryToMatrix();
                        if (local.HasError())
                            return Result<Matrices>::Failure(local.ErrorValue());
                        matrices[index] =
                            entity->parent ? Math::Multiply(*matrices[entity->parent->entity.index], local.Value()) : local.Value();
                    }
                    states[index] = 2;
                }
            }
            return Result<Matrices>::Success(std::move(matrices));
        }

        /** @brief Rejects shear locally: the general math decomposer intentionally projects onto the closest TRS basis. */
        [[nodiscard]] Result<Math::Transform> ExactPlacementTransform(const Math::Mat4 &matrix) {
            auto transform = Math::TryDecomposeAffineTRS(matrix);
            if (transform.HasError())
                return Result<Math::Transform>::Failure(transform.ErrorValue());
            auto rebuilt = transform.Value().TryToMatrix();
            if (rebuilt.HasError())
                return Result<Math::Transform>::Failure(rebuilt.ErrorValue());
            for (std::size_t index = 0; index < matrix.values.size(); ++index) {
                const float scale = std::max({1.0F, std::abs(matrix.values[index]), std::abs(rebuilt.Value().values[index])});
                if (!Math::NearlyEqual(matrix.values[index], rebuilt.Value().values[index], Math::DefaultEpsilon * scale))
                    return InvalidPlacement<Math::Transform>("Initial placement cannot represent the candidate pose without shear.");
            }
            return transform;
        }

        /** @brief Replaces the world pose while preserving the original representable visual scale. */
        [[nodiscard]] Result<Math::Mat4> PlacedWorld(const SceneInitialPlacement &placement, const Math::Mat4 &original) {
            const auto &q = placement.heading;
            const double norm = static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y + static_cast<double>(q.z) * q.z +
                                static_cast<double>(q.w) * q.w;
            if (!Math::IsFinite(placement.position) || !Math::IsFinite(q) || std::abs(norm - 1.0) > 1.0e-6)
                return InvalidPlacement<Math::Mat4>("Initial placement pose must be finite with a unit heading.");
            auto visual = ExactPlacementTransform(original);
            if (visual.HasError())
                return Result<Math::Mat4>::Failure(visual.ErrorValue());
            Math::Transform result = visual.Value();
            result.translation = placement.position;
            result.rotation = placement.heading;
            return result.TryToMatrix();
        }
    }  // namespace

    /** @copydoc RuntimeScene::ApplyInitialPlacements */
    Result<void> RuntimeScene::ApplyInitialPlacements(const std::span<const SceneInitialPlacement> placements) {
        const auto scene = View();
        if (placements.empty())
            return Result<void>::Success();
        Matrices overrides(scene.SlotCount());
        auto original = WorldMatrices(scene, overrides);
        if (original.HasError())
            return Result<void>::Failure(original.ErrorValue());
        for (const auto &placement : placements) {
            if (!IsValid(storage_, placement.owner) || overrides[placement.owner.entity.index])
                return InvalidPlacement<void>("Initial placement owner is stale, foreign, or claimed more than once.");
            auto matrix = PlacedWorld(placement, *original.Value()[placement.owner.entity.index]);
            if (matrix.HasError())
                return Result<void>::Failure(matrix.ErrorValue());
            overrides[placement.owner.entity.index] = matrix.Value();
        }
        auto adjusted = WorldMatrices(scene, overrides);
        if (adjusted.HasError())
            return Result<void>::Failure(adjusted.ErrorValue());
        std::vector<std::pair<std::size_t, Math::Transform>> updates;
        updates.reserve(placements.size());
        for (const auto &placement : placements) {
            const auto index = placement.owner.entity.index;
            Math::Mat4 local = *adjusted.Value()[index];
            if (const auto parent = scene.EntityAt(index)->parent) {
                auto inverse = Math::TryInverseAffine(*adjusted.Value()[parent->entity.index]);
                if (inverse.HasError())
                    return Result<void>::Failure(inverse.ErrorValue());
                local = Math::Multiply(inverse.Value(), local);
            }
            auto transform = ExactPlacementTransform(local);
            if (transform.HasError())
                return Result<void>::Failure(transform.ErrorValue());
            updates.emplace_back(index, transform.Value());
        }
        // Only private preparation reaches here. No topology or entity generation changes, and existing
        // producer views stay current. Every allocation/validation precedes the no-fail value assignments.
        for (const auto &[index, transform] : updates)
            storage_.slots[index].localTransform = transform;
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
