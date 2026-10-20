#pragma once

#include "Horo/Application/NavigationProjectBakeInput.h"
#include "Horo/Navigation/NavigationDefinitionSerialization.h"
#include "navigation/NavigationProjectBakeSourceFixture.h"
#include "navigation/NavigationSceneSourceFixture.h"

namespace Horo::Application::NavigationInputTestSupport {
    /** @brief Real two-Scene project with canonical material bytes and repeated authored object/surface IDs. */
    struct Project final {
        NavigationSourceTestSupport::Project source;

        Project() {
            Material(1);
            Scenes(Object());
        }

        /** @brief Creates a walkable box footprint exactly inside one authored eight-metre tile. */
        static SceneSource::SceneObjectSnapshot Object() {
            auto object = Tests::NavigationSources::Object();
            auto &collider = object.components.colliders.front();
            collider.source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsBoxCollider{{4, 0.5F, 4}}};
            collider.localPose.translation = {4, -0.5F, 4};
            object.components.navigationSources.front().area = Navigation::TestSupport::Id<Navigation::NavigationAreaId>(1);
            object.components.navigationSources.front().filter = Navigation::TestSupport::Id<Navigation::NavigationFilterId>(1);
            return object;
        }

        /** @brief Writes both actual Scene sources without changing stable sidecars. */
        void Scenes(const SceneSource::SceneObjectSnapshot &object) const {
            const std::array objects{object};
            const auto bytes = SceneSource::EncodeSceneSource({objects, {}});
            source.Write("Assets/first.horo", bytes);
            source.Write("Assets/second.horo", bytes);
        }

        /** @brief Writes the sole canonical Physics material representation with an exact stable registry identity. */
        void Material(std::uint64_t revision) const {
            const auto material = Physics::PhysicsMaterialAsset::Create(
                {.id = Physics::PhysicsMaterialAssetId::FromAssetId(NavigationSourceTestSupport::Asset(4)), .sourceRevision = revision});
            REQUIRE(material.HasValue());
            const auto bytes = Physics::EncodePhysicsMaterialAsset(material.Value());
            REQUIRE(bytes.HasValue());
            source.WriteBytes("Assets/floor.horoasset", bytes.Value());
            source.Sidecar("Assets/floor.horoasset", NavigationSourceTestSupport::Asset(4), Physics::PhysicsMaterialAssetTypeName);
        }

        /** @brief Uses the production source owner and actual project compatibility inspection. */
        auto Capture() const {
            return source.Capture();
        }

        /** @brief Explicit host-owned test revisions; source contents are independently byte-fenced. */
        static Navigation::NavigationBakeInputRevisions Revisions() {
            return Navigation::TestSupport::IncrementalBakeFixture{}.revisions;
        }

        /** @brief Writes one exact definition through the production envelope and record codecs. */
        void Definition(const Navigation::NavigationDefinition &definition) const {
            const auto record =
                Navigation::EncodeNavigationDefinitionRecord(definition,
                                                             Navigation::TestSupport::Id<Navigation::NavigationAuthoredRecordId>(1));
            REQUIRE(record.HasValue());
            const std::array support{Navigation::NavigationDefinitionRecordSupport()};
            const auto records = Navigation::NavigationSourceRecords::Create(Navigation::CurrentNavigationSourceSchemaVersion,
                                                                             {record.Value()}, {.supportedRecords = support});
            REQUIRE(records.HasValue());
            const auto bytes = Navigation::SerializeNavigationSourceRecords(records.Value());
            REQUIRE(bytes.HasValue());
            source.WriteBytes("Assets/navigation.horoasset", bytes.Value());
        }
    };
}  // namespace Horo::Application::NavigationInputTestSupport
