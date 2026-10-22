#include "WorldBakeInternal.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <set>

namespace Horo::Application::WorldBakeDetail {
    /** @brief Stable canonical identity hash; persistent fields are encoded explicitly, never native object layout. */
    Sha256Digest Identity(const Assets::AssetId page, const std::uint64_t object) {
        constexpr std::string_view domain = "Horo.WorldBake.Object.v1";
        std::array<std::byte, 8> number{};
        for (std::size_t i = 0; i < number.size(); ++i)
            number[i] = static_cast<std::byte>((object >> (8U * i)) & 255U);
        const std::array<std::span<const std::byte>, 3> spans{std::as_bytes(std::span{domain.data(), domain.size()}),
                                                              std::as_bytes(std::span{page.Bytes()}), std::span<const std::byte>{number}};
        return ComputeSha256Fragments(spans);
    }

    /** @brief Uses a nonzero deterministic Scene identity, checked for collisions across the complete world. */
    std::uint64_t ObjectId(const Assets::AssetId page, const std::uint64_t object) {
        const auto digest = Identity(page, object);
        std::uint64_t result{};
        for (std::size_t i = 0; i < 8; ++i)
            result |= static_cast<std::uint64_t>(digest.bytes[i]) << (8U * i);
        return result == 0 ? 1 : result;
    }

    /** @brief Computes a versioned stable chunk identity from exact canonical world and cell bytes. */
    Assets::AssetId ChunkId(const W::WorldPartitionId &world, const W::StreamingCellId cell) {
        constexpr std::string_view domain = "Horo.WorldBake.Cell.v1";
        const auto bytes = W::SerializeStreamingCellId(cell);
        const std::array<std::span<const std::byte>, 3> spans{std::as_bytes(std::span{domain.data(), domain.size()}),
                                                              std::as_bytes(std::span{world.Bytes()}), std::as_bytes(std::span{bytes})};
        const auto hash = ComputeSha256Fragments(spans);
        std::array<std::uint8_t, 16> id{};
        std::copy_n(hash.bytes.begin(), id.size(), id.begin());
        return Assets::AssetId::FromBytes(id);
    }

    /** @brief Remaps page-local typed Physics endpoints together with the containing Scene object identity. */
    void Remap(Runtime::RuntimeEntityDefinition &entity, const Assets::AssetId page) {
        entity.object.value = ObjectId(page, entity.object.value);
        if (entity.parent)
            entity.parent->value = ObjectId(page, entity.parent->value);
        for (auto &collider : entity.components.colliders)
            collider.body.object.value = ObjectId(page, collider.body.object.value);
        for (auto &constraint : entity.components.physicsConstraints) {
            constraint.first.body.object.value = ObjectId(page, constraint.first.body.object.value);
            if (auto *body = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&constraint.second))
                body->body.object.value = ObjectId(page, body->body.object.value);
        }
    }

    /** @brief Converts one actual admitted immutable page through the canonical Scene conversion owner. */
    Result<Runtime::RuntimeSceneDefinition> ConvertPage(const SceneSource::SceneSourceView &source,
                                                        const W::WorldAuthoringPageDescriptor &page, const WorldBakeRequest &request) {
        const Runtime::SceneDefinitionId id{ObjectId(page.sourceAsset, 0)};
        const Runtime::SceneDefinitionRevision revision{page.revision.Value()};
        if (!source.prefabInstances.empty()) {
            if (!request.project.prefabs || !request.project.prefabLimits)
                return Result<Runtime::RuntimeSceneDefinition>::Failure(
                    MakeError(WorldBakeErrors::Dependency, "The captured prefab source publication is required."));
            return SceneSource::ConvertSceneSourceToRuntime(source, id, revision, *request.project.prefabs, *request.project.prefabLimits,
                                                            request.project.prefabSchemas.get(), request.project.triggers);
        }
        return SceneSource::ConvertSceneSourceToRuntime(source, id, revision, request.project.triggers);
    }

    /** @brief Projects complete authored placement with the same canonical assignment and spanning-policy cores as inspection. */
    Result<std::vector<W::StreamingCellId>> Placement(const Editor::PartitionedWorldObject &object, const WorldBakeRequest &request) {
        const auto &document = request.document->document;
        if (object.descriptor.placement == W::WorldSpatialObjectPlacementClass::AlwaysPresent)
            return Result<std::vector<W::StreamingCellId>>::Success({});
        const W::WorldSpatialAssignmentCandidate candidate{object.descriptor.address, object.descriptor.revision, object.descriptor.bounds,
                                                           object.layer, object.assignmentLod};
        const W::WorldSpatialAssignmentAuthoringDomain domain{document.settings.partition, document.settings.bounds, document.settings.grid,
                                                              document.layers};
        auto assignment =
            W::WorldSpatialAssignment::CreateForAuthoring(domain, std::span{&candidate, 1},
                                                          {1, request.limits.maximumCellsPerObject, request.limits.maximumAssignments});
        if (assignment.HasError())
            return Result<std::vector<W::StreamingCellId>>::Failure(assignment.ErrorValue());
        const auto directive = object.spanning ? std::span{&*object.spanning, 1} : std::span<const W::WorldSpanningObjectDirective>{};
        auto plan =
            W::WorldSpanningObjectPlan::Create(assignment.Value(), directive,
                                               {1, document.settings.maximumDirectCellsPerObject, request.limits.maximumAssignments});
        if (plan.HasError())
            return Result<std::vector<W::StreamingCellId>>::Failure(plan.ErrorValue());
        if (plan.Value().Objects().front().placement == W::WorldSpanningObjectPlacement::SplitPerCell)
            return Result<std::vector<W::StreamingCellId>>::Failure(
                MakeError(WorldBakeErrors::Unsupported, "SplitPerCell requires actual provider-split content."));
        const auto cells = plan.Value().CellsForObject(0);
        return Result<std::vector<W::StreamingCellId>>::Success({cells.begin(), cells.end()});
    }

}  // namespace Horo::Application::WorldBakeDetail
