#include "SceneDocumentTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {
    using namespace Horo::Editor::SceneDocumentTestSupport;

    [[nodiscard]] Horo::Editor::SceneObjectComponentSet LargeIdentityComponents() {
        using namespace Horo;
        using namespace Horo::Editor;
        std::vector<Gameplay::SerializedComponent> largeComponents;
        largeComponents.reserve(5);
        for (std::size_t index = 0; index < 5; ++index) {
            auto typeId = Gameplay::ComponentTypeId::Parse("game.audit.component_" + std::to_string(index));
            REQUIRE((typeId.HasValue()));
            largeComponents.push_back(Gameplay::SerializedComponent{
                .typeId = std::move(typeId).Value(),
                .payload = std::vector<std::byte>(Gameplay::MaximumSerializedComponentBytes),
            });
        }

        SceneObjectComponentSet largeComponentSet;
        largeComponentSet.gameplayComponents = largeComponents;
        largeComponentSet.navigationSurface = NavigationSurface(8);
        largeComponentSet.navigationRegion = NavigationRegion(11, 8);
        largeComponentSet.navigationModifier = NavigationModifier(17, 8);
        largeComponentSet.navigationLink = NavigationLink(23, 8);
        largeComponentSet.aiAgent = Horo::AI::AiAgentComponent{.agent = Horo::AI::AgentId::Create(41).Value()};
        largeComponentSet.aiController =
            Horo::AI::AiControllerComponent{.controller = Horo::AI::ControllerTypeId::Create(51).Value(),
                                            .decisionAsset = Horo::AI::DecisionGraphAssetId::Create(61).Value(),
                                            .blackboardSchema = Horo::AI::BlackboardSchemaId::Create(71).Value(),
                                            .requiredCapabilities = Horo::AI::AiCapabilitySet::Of(Horo::AI::AiCapability::Behavior)};
        const auto behaviorType = Gameplay::BehaviorTypeId::Parse("game.audit.identity_behavior");
        REQUIRE((behaviorType.HasValue()));
        largeComponentSet.behaviors.push_back(
            Gameplay::BehaviorComponent{Gameplay::BehaviorInstanceId{44}, behaviorType.Value(), 1, true, {}});

        return largeComponentSet;
    }

    [[nodiscard]] Horo::Editor::SceneObjectComponentSet SmallIdentityComponents(Horo::Editor::SceneObjectComponentSet components) {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneObjectComponentSet smallIdentityComponents = std::move(components);
        smallIdentityComponents.gameplayComponents.clear();
        const auto smallSurface = Navigation::SurfaceId::Create(1).Value();
        smallIdentityComponents.navigationSurface->id = smallSurface;
        smallIdentityComponents.navigationRegion->id = Navigation::NavigationRegionId::Create(1).Value();
        smallIdentityComponents.navigationRegion->surface = smallSurface;
        smallIdentityComponents.navigationModifier->id = Navigation::NavigationModifierId::Create(1).Value();
        smallIdentityComponents.navigationModifier->surface = smallSurface;
        smallIdentityComponents.navigationLink->id = Navigation::NavigationLinkId::Create(1).Value();
        smallIdentityComponents.navigationLink->start.surface = smallSurface;
        smallIdentityComponents.navigationLink->end.surface = smallSurface;
        smallIdentityComponents.aiAgent->agent = AI::AgentId::Create(1).Value();
        smallIdentityComponents.behaviors.front().instanceId = Gameplay::BehaviorInstanceId{1};

        return smallIdentityComponents;
    }

    TEST_CASE("Oversized scene object duplicate rejection preserves component identity counters", "[unit][editor][history]") {
        using namespace Horo;
        using namespace Horo::Editor;

        const SceneObjectComponentSet largeComponentSet = LargeIdentityComponents();

        std::vector<SceneObjectSnapshot> loadedObjects;
        loadedObjects.push_back(SceneObjectSnapshot{.id = SceneObjectId{1}, .name = "Large Source", .components = largeComponentSet});

        SceneDocument document;
        REQUIRE((document.LoadSaved(std::move(loadedObjects)).HasValue()));
        const SceneDocumentSnapshot beforeRejectedEdits = document.Snapshot();
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};

        const auto oversizedCreate = commands.Execute(CreateSceneObjectCommand{.name = "Oversized", .components = largeComponentSet});
        REQUIRE((oversizedCreate.HasError()));
        const auto oversizedDuplicate = commands.Execute(DuplicateSceneObjectCommand{SceneObjectId{1}, "Large Copy"});
        REQUIRE((oversizedDuplicate.HasError()));
        REQUIRE((document.Revision() == beforeRejectedEdits.revision));
        REQUIRE((document.State() == beforeRejectedEdits.state));
        REQUIRE((document.Objects().size() == 1));
        REQUIRE((document.Objects().front().components.gameplayComponents == largeComponentSet.gameplayComponents));
        REQUIRE_FALSE((history.CanUndo()));
        REQUIRE_FALSE((history.CanRedo()));

        const SceneObjectComponentSet smallIdentityComponents = SmallIdentityComponents(largeComponentSet);

        const auto created = commands.Execute(CreateSceneObjectCommand{.name = "Small", .components = smallIdentityComponents});
        REQUIRE((created.HasValue()));
        REQUIRE((created.Value().object == SceneObjectId{2}));
        const auto successfulDuplicate = commands.Execute(DuplicateSceneObjectCommand{created.Value().object, "Small Copy"});
        REQUIRE((successfulDuplicate.HasValue()));
        const SceneObjectComponentSet &duplicatedComponents = document.Objects().back().components;
        REQUIRE((duplicatedComponents.aiAgent->agent.Value() == 42));
        REQUIRE((duplicatedComponents.behaviors.front().instanceId.value == 45));
        REQUIRE((duplicatedComponents.navigationSurface->id.Value() == 9));
        REQUIRE((duplicatedComponents.navigationRegion->id.Value() == 12));
        REQUIRE((duplicatedComponents.navigationModifier->id.Value() == 18));
        REQUIRE((duplicatedComponents.navigationLink->id.Value() == 24));
        const DocumentRevision committedRevision = document.Revision();
        const auto noOpRename = commands.Execute(RenameSceneObjectCommand{created.Value().object, "Small"});
        REQUIRE((noOpRename.HasValue()));
        REQUIRE_FALSE((noOpRename.Value().committed));
        REQUIRE((document.Revision() == committedRevision));

        REQUIRE((commands.Undo().HasValue()));
        REQUIRE((document.Objects().size() == 2));
        REQUIRE((commands.Redo().HasValue()));
        REQUIRE((document.Objects().size() == 3));
    }

}  // namespace
