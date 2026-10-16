#pragma once
#include "EditorInspectorTestSupport.h"
#include "OwnedTestDirectory.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <fstream>

namespace Horo::Editor::Tests::InspectorControllerSupport {
    /** @brief Uses actual Lua project discovery, workspace selection and canonical document/history owners. */
    struct Fixture final {
        Extensions::Tests::InspectorSupport::Fixture provider;
        Horo::Tests::OwnedTestDirectory project{"horo110 project owner"};
        Runtime::RuntimeSceneService runtime;
        CancellationSource cancellation;
        std::unique_ptr<EditorWorkspaceController> controller;
        SceneObjectId object;
        Gameplay::BehaviorInstanceId attachment;
        std::uint64_t tick{};

        /** @brief Publishes actual authored behavior metadata before real controller discovery and attachment. */
        Fixture() {
            std::filesystem::create_directories(project.Path() / "assets/scripts");
            const auto source = project.Path() / "assets/scripts/Counter.horo_script";
            {
                std::ofstream out{source};
                out << "return horo.behavior { type_id='project.counter', display_name='Counter', schema_version=3, "
                       "fields={{name='count', default=9007199254740993}, {name='future', default='preserve'}} }";
                REQUIRE(out.good());
            }
            {
                std::ofstream out{source.string() + ".meta"};
                out << R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"project.counter"})";
                REQUIRE(out.good());
            }
            REQUIRE(runtime.Startup(cancellation.Token()).HasValue());
            controller = std::make_unique<EditorWorkspaceController>(project.Path(), runtime, Assets::AssetRegistrySnapshot{},
                                                                     EditorWorkspaceDependencies{.documentSessionOwner =
                                                                                                     Assets::AssetId::FromBytes({1}),
                                                                                                 .inspectorHost = &provider.host});
            REQUIRE(!controller->ViewModel().objects.empty());
            REQUIRE(controller->ViewModel().availableBehaviors.size() == 1);
            object = controller->ViewModel().objects.front().id;
            EditorWorkspaceViewCommandData attach;
            attach.command = EditorWorkspaceViewCommand::AttachBehaviorToObject;
            attach.objectPayload = object;
            attach.behaviorTypePayload = Gameplay::BehaviorTypeId::Parse("project.counter").Value();
            Process(attach);
            REQUIRE(controller->ViewModel().objects.front().components.behaviors.size() == 1);
            attachment = controller->ViewModel().objects.front().components.behaviors.front().instanceId;
            EditorWorkspaceViewCommandData select;
            select.command = EditorWorkspaceViewCommand::SelectObject;
            select.objectPayload = object;
            Process(select);
        }

        /** @brief Sends actual user intent and pumps only the existing lifecycle commit owner. */
        void Process(const EditorWorkspaceViewCommandData &command) {
            controller->ProcessCommand(command);
            const Runtime::FrameContext frame{++tick, {}, 0.0, 0, {}, false, cancellation.Token()};
            REQUIRE(runtime.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
            controller->SynchronizeRuntimeScenePreview();
        }

        /** @brief Captures current exact user intent; this is not an approval token and cannot bypass controller admission. */
        InspectorExtensionEdit Request(std::int64_t value = INT64_MIN) const {
            const auto &view = controller->ViewModel();
            return {provider.provider,
                    "fixture.inspector",
                    Gameplay::BehaviorTypeId::Parse("project.counter").Value(),
                    3,
                    view.inspectorDocumentGeneration,
                    view.inspectorBehaviorRegistryRevision,
                    view.documentRevision.value,
                    view.selectionRevision,
                    {{object, attachment}},
                    "count",
                    value};
        }

        /** @brief Reads the actual authored integer from the live workspace projection. */
        std::int64_t Value() const {
            return std::get<std::int64_t>(controller->ViewModel().objects.front().components.behaviors.front().fields.front().value);
        }

        /** @brief Routes the real explicit approval command; only the controller can publish authority. */
        void Approve() {
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::ApproveInspectorContribution;
            command.inspectorEdit = Request();
            Process(command);
        }
    };
}  // namespace Horo::Editor::Tests::InspectorControllerSupport
