#include "WorldBakeInternal.h"

namespace Horo::Application::WorldBakeDetail {
    namespace S = Runtime;

    namespace {
        /** @brief Bounds copied diagnostic paths before reference records allocate their owned origins. */
        struct ReferenceCapture final {
            const S::WorldBuildValidationLimits &limits;
            std::vector<S::WorldBuildReference> references;
            std::size_t sourceBytes{};

            Result<void> Charge(const std::optional<DiagnosticSourceLocation> &source) {
                if (!source)
                    return Result<void>::Success();
                if (source->absolutePath.size() > limits.maximumSourceBytes - sourceBytes)
                    return Result<void>::Failure(MakeError(WorldBakeErrors::Capacity));
                sourceBytes += source->absolutePath.size();
                return Result<void>::Success();
            }

            Result<void> Add(const S::WorldBuildCell &cell, const S::SceneObjectId owner, const S::SceneObjectId target) {
                if (references.size() >= limits.maximumReferences)
                    return Result<void>::Failure(MakeError(WorldBakeErrors::Capacity));
                if (auto charged = Charge(cell.source); charged.HasError())
                    return charged;
                references.push_back({{cell.expected, owner}, {cell.expected, target}, cell.source});
                return Result<void>::Success();
            }
        };

        /** @brief Captures actual component-owned local endpoints without parsing opaque provider payloads. */
        Result<void> CaptureEntity(const S::RuntimeEntityDefinition &entity, const S::WorldBuildCell &cell, ReferenceCapture &capture) {
            if (entity.parent)
                if (auto reference = capture.Add(cell, entity.object, *entity.parent); reference.HasError())
                    return reference;
            for (const auto &collider : entity.components.colliders)
                if (auto reference = capture.Add(cell, entity.object, collider.body.object); reference.HasError())
                    return reference;
            for (const auto &constraint : entity.components.physicsConstraints) {
                if (auto reference = capture.Add(cell, entity.object, constraint.first.body.object); reference.HasError())
                    return reference;
                if (const auto *body = std::get_if<S::PhysicsConstraintBodyEndpoint>(&constraint.second))
                    if (auto reference = capture.Add(cell, entity.object, body->body.object); reference.HasError())
                        return reference;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Captures complete actual typed references alongside exact per-cell source navigation. */
    Result<std::vector<S::WorldBuildReference>> CaptureReferences(
        std::span<const S::WorldBuildCell> expected, std::span<const std::shared_ptr<const S::RuntimeSceneCellPayload>> payloads,
        const S::WorldBuildValidationLimits &limits) {
        ReferenceCapture capture{limits};
        for (const auto &cell : expected)
            if (auto charged = capture.Charge(cell.source); charged.HasError())
                return Result<std::vector<S::WorldBuildReference>>::Failure(charged.ErrorValue());
        for (std::size_t index = 0; index < payloads.size(); ++index)
            for (const auto &entity : payloads[index]->Definition().Entities())
                if (auto reference = CaptureEntity(entity, expected[index], capture); reference.HasError())
                    return Result<std::vector<S::WorldBuildReference>>::Failure(reference.ErrorValue());
        return Result<std::vector<S::WorldBuildReference>>::Success(std::move(capture.references));
    }
}  // namespace Horo::Application::WorldBakeDetail
