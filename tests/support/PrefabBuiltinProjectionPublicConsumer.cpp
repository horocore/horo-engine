#include "Horo/Prefab/PrefabSpawnService.h"
#include "Horo/Scene/PrefabComponentProjection.h"

#include <type_traits>

namespace {
    /** @brief Compiles the supported host-owned codec boundary without any source-private include root. */
    class Projector final : public Horo::Prefab::PrefabComponentProjector {
    public:
        Horo::Result<bool> Project(const Horo::Prefab::RawComponentPayload &source, Horo::Runtime::RuntimeComponentSet &destination,
                                   std::optional<Horo::Runtime::PrimitiveMeshDescriptor> &primitive) const override {
            return Horo::SceneSource::ProjectPrefabBuiltinComponent(source, destination, primitive);
        }
    };

    static_assert(std::is_same_v<decltype(Horo::SceneSource::EncodePrefabNavigationComponent(
                                     Horo::Runtime::BuiltinSceneComponentKind::NavigationAgent, Horo::Prefab::PrefabComponentInstanceId{},
                                     Horo::SceneSource::SceneObjectComponentSet{})),
                                 Horo::Result<Horo::Prefab::RawComponentPayload>>);
    static_assert(Horo::Runtime::SceneStructuralParticipantContractVersion == 4);
    static_assert(std::is_constructible_v<Horo::Prefab::PrefabSpawnService, Horo::Prefab::PrefabTemplateProvider &,
                                          Horo::Runtime::RuntimeSceneService &, const Horo::Gameplay::ComponentRegistry *,
                                          const Horo::Prefab::PrefabComponentProjector *>);
}  // namespace

int main() {
    Horo::Runtime::RuntimeComponentSet values;
    return Horo::Runtime::BuiltinSceneComponentSchemas().empty() || Horo::Runtime::BuiltinSceneComponentCount(values) != 0 ? 1 : 0;
}
