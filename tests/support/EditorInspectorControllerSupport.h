#pragma once
#include "../helpers/ProjectReplicationTestSupport.h"
#include "EditorInspectorTestSupport.h"
#include "Horo/Foundation/Paths.h"
#include "OwnedTestDirectory.h"
#include "editor/gameplay/ProjectGameplayRegistry.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <fstream>
#include <string_view>
#include <utility>

namespace Horo::Editor::Tests::InspectorControllerSupport {
    /** @brief Admits the matching primitive without giving the provider authored-value authority. */
    inline Extensions::EditorInspectorDefinition Presentation(Extensions::EditorInspectorFieldKind kind) {
        using namespace Extensions;
        using enum EditorInspectorFieldKind;
        auto value = Extensions::Tests::InspectorSupport::Definition();
        const auto &base = EditorUiNodeBaseOf(value.form.nodes.front());
        auto &field = value.fields.front();
        field = {.field = "count", .kind = kind, .node = {"fixture.count"}};
        switch (kind) {
            case Boolean:
                value.form.nodes.front().payload = EditorUiBooleanNode{base, {"count"}, false};
                break;
            case Integer:
                break;
            case Number:
                value.form.nodes.front().payload = EditorUiNumberNode{.base = base, .binding = {"count"}, .value = 0.0};
                break;
            case String:
                field.maximumStringBytes = 64;
                value.form.nodes.front().payload = EditorUiTextFieldNode{.base = base, .binding = {"count"}, .maximumBytes = 64};
                break;
        }
        return value;
    }

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

        /** @brief Publishes actual authored behavior metadata before real controller discovery and attachment.
         * @param definition Provider presentation admitted through the real host.
         * @param defaultValue Trusted Lua scalar expression authored by the test, then parsed by the production loader.
         */
        explicit Fixture(Extensions::EditorInspectorDefinition definition = Extensions::Tests::InspectorSupport::Definition(),
                         std::string_view defaultValue = "9007199254740993")
            : provider(std::move(definition)) {
            Horo::Tests::WriteReplicationProject(project.Path());
            std::filesystem::create_directories(project.Path() / "assets/scripts");
            const auto source = project.Path() / "assets/scripts/Counter.horo_script";
            {
                std::ofstream out{source};
                out << "return horo.behavior { type_id='game.fixture.counter', display_name='Counter', schema_version=3, "
                       "fields={{name='count', default="
                    << defaultValue << "}, {name='future', default='preserve'}} }";
                REQUIRE(out.good());
            }
            {
                std::ofstream out{source.string() + ".meta"};
                out << R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.fixture.counter"})";
                REQUIRE(out.good());
            }
            ValidateAuthoredBehavior(source);
            REQUIRE(runtime.Startup(cancellation.Token()).HasValue());
            controller =
                std::make_unique<EditorWorkspaceController>(project.Path(), runtime, Assets::AssetRegistrySnapshot{},
                                                            EditorWorkspaceDependencies{.world = {.documentSessionOwner =
                                                                                                      Assets::AssetId::FromBytes({1})},
                                                                                        .inspectorHost = &provider.host});
            EditorWorkspaceViewCommandData create;
            create.command = EditorWorkspaceViewCommand::CreatePrimitive;
            create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
            Process(create);
            REQUIRE(!controller->ViewModel().objects.empty());
            REQUIRE(controller->ViewModel().availableBehaviors.size() == 1);
            object = controller->ViewModel().objects.front().id;
            EditorWorkspaceViewCommandData attach;
            attach.command = EditorWorkspaceViewCommand::AttachBehaviorToObject;
            attach.objectPayload = object;
            attach.behaviorTypePayload = Gameplay::BehaviorTypeId::Parse("game.fixture.counter").Value();
            Process(attach);
            REQUIRE(controller->ViewModel().objects.front().components.behaviors.size() == 1);
            attachment = controller->ViewModel().objects.front().components.behaviors.front().instanceId;
            EditorWorkspaceViewCommandData select;
            select.command = EditorWorkspaceViewCommand::SelectObject;
            select.objectPayload = object;
            Process(select);
        }

        /** @brief Proves the real source and catalog accept the canonical identity before controller admission. */
        void ValidateAuthoredBehavior(const std::filesystem::path &source) const {
            REQUIRE(ProjectLayout::ScriptsRoot(project.Path()) == source.parent_path());
            const auto loaded = Gameplay::LuaBehaviorProgram::LoadFiles(source, source.string() + ".meta");
            if (loaded.HasError())
                UNSCOPED_INFO(loaded.ErrorValue().domain.Value()
                              << ":" << loaded.ErrorValue().code.Value() << ": " << loaded.ErrorValue().message);
            REQUIRE(loaded.HasValue());
            REQUIRE(loaded.Value()->Descriptor().typeId.Value() == "game.fixture.counter");
            const auto discovered = ProjectGameplayRegistry::Discover(project.Path());
            for (const auto &diagnostic : discovered->Diagnostics())
                UNSCOPED_INFO(diagnostic.source.string() << ": " << diagnostic.error.domain.Value() << ":" << diagnostic.error.code.Value()
                                                         << ": " << diagnostic.error.message);
            REQUIRE(discovered->Diagnostics().empty());
            REQUIRE(discovered->Registry().Registrations().size() == 1);
            REQUIRE(discovered->Registry().Registrations().front().descriptor.typeId == loaded.Value()->Descriptor().typeId);
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
                    Gameplay::BehaviorTypeId::Parse("game.fixture.counter").Value(),
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
