#include "CanonicalPhysicsStateBackendInternal.h"
#include "PhysicsStateHashInternal.h"

namespace Horo::Physics::Detail {
    namespace {
        void Layer(StateDetail::StateHash &hash, const CollisionLayerDefinition &value) noexcept {
            hash.Id(value.id);
            hash.Word(value.enabled);
            hash.Word(value.admitsStatic);
            hash.Word(value.admitsKinematic);
            hash.Word(value.admitsDynamic);
            hash.Word(value.admitsOverlap);
        }

        void Profile(StateDetail::StateHash &hash, const CollisionProfileDefinition &value) noexcept {
            hash.Id(value.id);
            hash.Id(value.layer);
            hash.Word(value.simulationEnabled);
            hash.Word(value.queryEnabled);
            hash.Word(value.lifecycleEventsEnabled);
            hash.Word(value.queryResponses.size());
            for (const auto &response : value.queryResponses) {
                hash.Id(response.channel);
                hash.Word(static_cast<std::uint8_t>(response.response));
            }
        }

        void Schema(StateDetail::StateHash &hash, const NormalizedCollisionSchema &schema) noexcept {
            hash.Id(schema.DefaultProfile());
            hash.Word(schema.Layers().size());
            for (const auto &value : schema.Layers())
                Layer(hash, value);
            hash.Word(schema.Pairs().size());
            for (const auto &value : schema.Pairs()) {
                hash.Id(value.first);
                hash.Id(value.second);
                hash.Word(static_cast<std::uint8_t>(value.response));
            }
            hash.Word(schema.QueryChannels().size());
            for (const auto &value : schema.QueryChannels()) {
                hash.Id(value.id);
                hash.Word(value.enabled);
            }
            hash.Word(schema.Profiles().size());
            for (const auto &value : schema.Profiles())
                Profile(hash, value);
        }
    }  // namespace

    /** @copydoc CanonicalPhysicsStateFilterFingerprint */
    Sha256Digest CanonicalPhysicsStateFilterFingerprint(const CanonicalWorld &world) noexcept {
        StateDetail::StateHash hash{"Horo.Physics.State.Filter.v2"};
        const auto &binding = world.simulation.binding;
        hash.Word(static_cast<bool>(binding.schema));
        if (binding.schema)
            Schema(hash, *binding.schema);
        // Also bind the REAL compiled rows consumed by the solver, not just their
        // source document.
        hash.Word(world.simulation.layerCount);
        for (std::size_t index = 0; index < world.simulation.layerCount; ++index)
            Layer(hash, world.simulation.layers[index]);
        for (const auto response : world.simulation.responses)
            hash.Word(static_cast<std::uint8_t>(response));
        return hash.Finish();
    }
}  // namespace Horo::Physics::Detail
