#include "NativePublicationFiles.h"
#include "PrefabWorkspaceTestSupport.h"
#include "editor/document/SceneDocumentPersistence.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace Horo::Editor {
    using namespace PrefabWorkspaceTestSupport;

    /** @brief Prepares an ordinary root value edit against the real source capture authority. */
    static Prefab::PrefabValueOverrideSet RootValueIntent(Project &project) {
        const auto limits = ScenePrefabLimits();
        const std::array roots{project.asset};
        const auto source = project.Capture(project.registry.Snapshot(), roots, limits, {});
        REQUIRE(source.HasValue());
        const auto resolved = source.Value().sources.resolver.Resolve(project.asset, project.instance, limits);
        REQUIRE(resolved.HasValue());
        const auto address = Prefab::PrefabObjectAddress::Create({}, {0}).Value();
        const auto edit =
            Prefab::PreparePrefabValueEdit(resolved.Value(), {}, {Prefab::PrefabValueEditKind::Assign, address, Math::Transform{{1, 2, 3}}},
                                           nullptr, limits);
        REQUIRE(edit.HasValue());
        return edit.Value().rebased;
    }

    TEST_CASE("Workspace prefab inspector revert is fenced to the selected row and current source bytes", "[editor][prefab][native]") {
        Workspace workspace;
        workspace.project.WriteScene(RootValueIntent(workspace.project));
        workspace.Open(PrefabAsset(10).Asset());
        auto &owner = *workspace.controller;
        const auto &model = owner.ViewModel();
        const auto projection = model.prefabHierarchy;
        REQUIRE(projection);
        REQUIRE(projection->entries.size() == 3);
        const auto &root = projection->entries.front();
        const auto row = std::ranges::find_if(root.valueDiff, [](const auto &entry) {
            return entry.revertValue.has_value();
        });
        REQUIRE(row != root.valueDiff.end());
        auto command = Intent(model, root.target, PrefabHierarchyAction::RevertOverrides);
        command.prefabHierarchy->revert =
            PrefabHierarchyRevert{PrefabHierarchyRevertScope::Value, static_cast<std::uint32_t>(row - root.valueDiff.begin())};
        const auto revision = model.documentRevision;

        SECTION("the selected record reverts through normal workspace history") {
            owner.ProcessCommand(command);
            CHECK(model.documentRevision.value == revision.value + 1);
            CHECK(model.isDirty);
            CHECK_FALSE(model.prefabHierarchy->entries.front().hasOverrides);
            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
            CHECK_FALSE(model.isDirty);
            CHECK(model.prefabHierarchy->entries.front().hasOverrides);
            owner.ProcessCommand(command);
            CHECK_FALSE(model.isDirty);
            CHECK(model.prefabHierarchy->entries.front().hasOverrides);
        }
        SECTION("a narrower scope rejects an invalid row") {
            command.prefabHierarchy->revert->row = static_cast<std::uint32_t>(root.valueDiff.size());
            owner.ProcessCommand(command);
            CHECK(model.documentRevision == revision);
            CHECK_FALSE(model.isDirty);
        }
        SECTION("a concurrent source edit rejects before changing the document") {
            workspace.project.WriteSource("Changed by another owner");
            owner.ProcessCommand(command);
            CHECK(model.documentRevision == revision);
            CHECK_FALSE(model.isDirty);
            CHECK(model.prefabHierarchy->entries.front().hasOverrides);
        }
    }

    namespace {
        /** @brief Leaves both exact installed bytes in place through the real tracked publication receipts. */
        class PairedWorkspaceFiles final : public TestSupport::NativePublicationFiles {
        public:
            std::filesystem::path prefab;
            std::filesystem::path scene;
            std::size_t replacements{};
            bool armed{};

            Result<void> WritePrivateDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes, bool &created,
                                             const std::filesystem::perms mode) override {
                return native.WritePrivateDurable(path, bytes, created, mode);
            }

            Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                              AtomicFileReplacementReceipt &receipt) override {
                if (!armed || (destination != prefab && destination != scene))
                    return native.AtomicReplaceTracked(prepared, destination, receipt);
                if (++replacements > 2)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
                auto replaced = native.AtomicReplaceTracked(prepared, destination, receipt);
                if (replacements == 2 && replaced.HasValue())
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
                return replaced;
            }
        };
    }  // namespace

    TEST_CASE("Workspace startup paired recovery reviews the exact candidate before restoring either file",
              "[editor][prefab][native][recovery]") {
        PairedWorkspaceFiles faultFiles;
        ProjectMutationCoordinator faultMutations{faultFiles};
        Workspace workspace;
        const auto &root = workspace.project.directory.path;
        faultFiles.prefab = workspace.project.source;
        faultFiles.scene = root / "assets/scenes/main.horo";
        workspace.project.WriteScene(RootValueIntent(workspace.project));
        const auto read = [&](const std::filesystem::path &path) {
            const auto source = Assets::CaptureAssetSource(root, path, Prefab::PrefabHardLimits::SourceDocumentBytes);
            REQUIRE(source.HasValue());
            return source.Value().identity;
        };
        const auto initialPrefab = read(faultFiles.prefab);
        const auto initialScene = read(faultFiles.scene);

        workspace.Open(PrefabAsset(10).Asset(), &faultMutations, &faultFiles);
        workspace.OpenPrefab();
        auto &first = *workspace.controller;
        first.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::ChangeActivePanel, .targetIndex = 3, .stringPayload = "horo.viewport"});
        const auto projection = first.ViewModel().prefabHierarchy;
        REQUIRE(projection);
        const auto &placement = projection->entries.front();
        const auto row = std::ranges::find_if(placement.valueDiff, [](const auto &entry) {
            return entry.revertValue.has_value();
        });
        REQUIRE(row != placement.valueDiff.end());
        auto apply = Intent(first.ViewModel(), placement.target, PrefabHierarchyAction::ApplyOverrides);
        apply.prefabHierarchy->revert =
            PrefabHierarchyRevert{PrefabHierarchyRevertScope::Value, static_cast<std::uint32_t>(row - placement.valueDiff.begin())};
        faultFiles.armed = true;
        first.ProcessCommand(apply);
        REQUIRE(first.ViewModel().prefabApplyRecoveryNotice == PrefabApplyRecoveryNotice::Reopen);
        const auto installedPrefab = read(faultFiles.prefab);
        const auto installedScene = read(faultFiles.scene);
        REQUIRE(installedPrefab != initialPrefab);
        REQUIRE(installedScene != initialScene);
        workspace.controller.reset();

        const auto partialScene = LoadProjectScene(root, faultFiles.scene);
        REQUIRE(partialScene.HasValue());
        auto draft = partialScene.Value().objects;
        draft.front().name = "Unsaved draft";
        const SceneDocumentSnapshot checkpoint{{2}, {2}, std::move(draft), partialScene.Value().prefabInstances};
        REQUIRE(
            WriteProjectSceneRecovery(root, faultFiles.scene, checkpoint, {1}, {1}, workspace.project.mutations, workspace.project.files)
                .HasValue());
        workspace.Open(PrefabAsset(11).Asset());
        auto &reopened = *workspace.controller;
        const auto &model = reopened.ViewModel();
        const auto evidence = model.prefabApplyRecovery;
        REQUIRE(evidence);
        REQUIRE(model.recoveryAvailable);
        CHECK_FALSE(model.prefabApplyAvailable);
        CHECK(evidence->prefab.path == faultFiles.prefab);
        CHECK(evidence->scene.path == faultFiles.scene);
        CHECK(evidence->prefab.original == initialPrefab);
        CHECK(evidence->scene.original == initialScene);

        using enum EditorWorkspaceViewCommand;
        reopened.ProcessCommand({.command = ReviewPrefabApplyRecovery, .prefabApplyRecovery = evidence});
        reopened.ProcessCommand({.command = RestorePrefabApplyRecovery});
        auto unreviewed = std::make_shared<const PrefabApplyRecoveryCandidate>(*evidence);
        reopened.ProcessCommand({.command = RestorePrefabApplyRecovery, .prefabApplyRecovery = unreviewed});
        REQUIRE(read(faultFiles.prefab) == installedPrefab);
        REQUIRE(read(faultFiles.scene) == installedScene);
        const EditorWorkspaceViewCommandData confirmed{.command = RestorePrefabApplyRecovery, .prefabApplyRecovery = evidence};

        SECTION("confirmation restores both canonical files and retains the independent unsaved Scene draft") {
            reopened.ProcessCommand(confirmed);
            CHECK_FALSE(model.prefabApplyRecovery);
            CHECK(model.prefabApplyRecoveryNotice == PrefabApplyRecoveryNotice::None);
            CHECK(read(faultFiles.prefab) == initialPrefab);
            CHECK(read(faultFiles.scene) == initialScene);
            REQUIRE(model.prefabHierarchy);
            CHECK(model.prefabHierarchy->entries.front().hasOverrides);
            CHECK_FALSE(model.isDirty);
            CHECK(model.recoveryAvailable);
            const auto draftAfter = InspectProjectSceneRecovery(root, faultFiles.scene);
            REQUIRE(draftAfter.HasValue());
            REQUIRE(draftAfter.Value());
            CHECK(draftAfter.Value()->objects.front().name == "Unsaved draft");
        }
        SECTION("confirmation preserves the record and an externally changed source") {
            workspace.project.WriteSource("Foreign source");
            const auto foreign = read(faultFiles.prefab);
            reopened.ProcessCommand(confirmed);
            REQUIRE(model.prefabApplyRecovery);
            CHECK(model.prefabApplyRecovery->capsule == evidence->capsule);
            CHECK(model.prefabApplyRecovery->prefab.observed == foreign);
            CHECK(model.prefabApplyRecoveryNotice == PrefabApplyRecoveryNotice::Failed);
            reopened.ProcessCommand(confirmed);
            CHECK(read(faultFiles.prefab) == foreign);
            CHECK(read(faultFiles.scene) == installedScene);
        }
        SECTION("an open affected Prefab owner must close before confirmation can write") {
            workspace.OpenPrefab();
            reopened.ProcessCommand(confirmed);
            CHECK(model.prefabApplyRecovery == evidence);
            CHECK(model.prefabApplyRecoveryNotice == PrefabApplyRecoveryNotice::Blocked);
            CHECK(read(faultFiles.prefab) == installedPrefab);
            CHECK(read(faultFiles.scene) == installedScene);
        }
    }

    TEST_CASE("Workspace prefab inspector applies the fenced value and replays from both documents", "[editor][prefab][native]") {
        Workspace workspace;
        workspace.project.WriteScene(RootValueIntent(workspace.project));
        workspace.Open(PrefabAsset(10).Asset());
        auto &owner = *workspace.controller;
        const auto &model = owner.ViewModel();
        workspace.OpenPrefab();
        const auto prefabTab = model.prefabDocuments.front().identity.instance;
        REQUIRE(model.workspacePanelHost.ActiveDocument() == prefabTab);
        owner.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::ChangeActivePanel, .targetIndex = 3, .stringPayload = "horo.viewport"});
        REQUIRE_FALSE(model.workspacePanelHost.ActiveDocument());
        REQUIRE(model.prefabApplyAvailable);
        const auto projection = model.prefabHierarchy;
        REQUIRE(projection);
        const auto &root = projection->entries.front();
        const auto row = std::ranges::find_if(root.valueDiff, [](const auto &entry) {
            return entry.revertValue.has_value();
        });
        REQUIRE(row != root.valueDiff.end());
        auto command = Intent(model, root.target, PrefabHierarchyAction::ApplyOverrides);
        command.prefabHierarchy->revert =
            PrefabHierarchyRevert{PrefabHierarchyRevertScope::Value, static_cast<std::uint32_t>(row - root.valueDiff.begin())};
        const auto sourceTransform = [&] {
            return model.prefabDocuments.front().source->Data().objects.front().localTransform;
        };
        const auto original = sourceTransform();
        const auto revision = model.documentRevision;
        owner.ProcessCommand(command);
        INFO((model.prefabOverrideError ? model.prefabOverrideError->message : std::string{}));
        REQUIRE_FALSE(model.prefabOverrideError);
        REQUIRE(model.documentRevision.value == revision.value + 1);
        REQUIRE_FALSE(model.prefabHierarchy->entries.front().hasOverrides);
        CHECK(sourceTransform() == Math::Transform{{1, 2, 3}});
        CHECK_FALSE(model.isDirty);
        CHECK_FALSE(model.prefabDocuments.front().dirty);

        const auto focusScene = [&] {
            owner.ProcessCommand(
                {.command = EditorWorkspaceViewCommand::ChangeActivePanel, .targetIndex = 3, .stringPayload = "horo.viewport"});
        };
        const auto closeSource = [&] {
            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = prefabTab});
            REQUIRE(model.prefabDocuments.empty());
        };
        const auto renameSceneObject = [&] {
            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UpdateObjectName,
                                  .objectPayload = SceneObjectId{11},
                                  .stringPayload = "Independent Scene edit"});
            REQUIRE(model.objects.front().name == "Independent Scene edit");
        };

        SECTION("either presented owner can replay the same publication") {
            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
            INFO((model.prefabOverrideError ? model.prefabOverrideError->message : std::string{}));
            REQUIRE_FALSE(model.prefabOverrideError);
            CHECK(model.prefabHierarchy->entries.front().hasOverrides);
            CHECK(sourceTransform() == original);
            CHECK_FALSE(model.isDirty);

            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::FocusWorkspaceDocument, .documentInstance = prefabTab});
            workspace.Action(PrefabDocumentAction::Redo);
            INFO((model.prefabDocumentError ? model.prefabDocumentError->message : std::string{}));
            REQUIRE_FALSE(model.prefabDocumentError);
            CHECK(sourceTransform() == Math::Transform{{1, 2, 3}});
            CHECK_FALSE(model.prefabHierarchy->entries.front().hasOverrides);
            CHECK_FALSE(model.isDirty);
            CHECK_FALSE(model.prefabDocuments.front().dirty);
        }

        SECTION("Scene history can still replay with the source tab closed") {
            const auto oldFence = model.prefabDocuments.front().fence;
            closeSource();
            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
            REQUIRE_FALSE(model.prefabOverrideError);
            CHECK(model.prefabHierarchy->entries.front().hasOverrides);
            CHECK(model.canRedo);

            workspace.OpenPrefab();
            const auto reopened = model.prefabDocuments.front().fence;
            CHECK(reopened.instance != oldFence.instance);
            CHECK(sourceTransform() == original);
            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::PrefabDocument,
                                  .prefabFence = oldFence,
                                  .prefabAction = PrefabDocumentAction::Redo});
            CHECK(model.prefabHierarchy->entries.front().hasOverrides);
            workspace.Action(PrefabDocumentAction::Redo);
            REQUIRE_FALSE(model.prefabDocumentError);
            CHECK(sourceTransform() == Math::Transform{{1, 2, 3}});
            CHECK_FALSE(model.prefabHierarchy->entries.front().hasOverrides);
        }

        SECTION("external replacement retires both halves without discarding later Scene edits") {
            renameSceneObject();
            closeSource();
            workspace.project.WriteSource("External Prefab edit");
            workspace.OpenPrefab();
            CHECK(model.prefabDocuments.front().source->Data().objects.front().name == "External Prefab edit");
            CHECK_FALSE(model.prefabDocuments.front().canUndo);
            focusScene();
            CHECK(model.canUndo);
            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
            CHECK(model.objects.front().name == "Authored parent");
            CHECK_FALSE(model.canUndo);
            CHECK(model.canRedo);
            CHECK(model.prefabDocuments.front().source->Data().objects.front().name == "External Prefab edit");
        }

        SECTION("Scene undo cannot overwrite a closed source that changed externally") {
            closeSource();
            workspace.project.WriteSource("External Prefab edit");
            const auto revisionBeforeUndo = model.documentRevision;
            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
            REQUIRE(model.prefabOverrideError);
            CHECK(model.prefabOverrideError->code.Value() == Assets::AssetSourceErrors::Conflict.code.Value());
            CHECK(model.documentRevision == revisionBeforeUndo);
            CHECK_FALSE(model.canUndo);
            workspace.OpenPrefab();
            CHECK(model.prefabDocuments.front().source->Data().objects.front().name == "External Prefab edit");
        }

        SECTION("explicit source reload retires the group at the original state boundary") {
            renameSceneObject();
            workspace.Action(PrefabDocumentAction::Reload);
            REQUIRE_FALSE(model.prefabDocumentError);
            CHECK_FALSE(model.prefabDocuments.front().canUndo);
            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
            CHECK(model.objects.front().name == "Authored parent");
            CHECK_FALSE(model.canUndo);
        }

        SECTION("shutdown releases a suspended owner while preserving newer Scene history") {
            renameSceneObject();
            closeSource();
            REQUIRE(owner.ShutdownPrefabDocuments(true).HasValue());
            REQUIRE(model.canUndo);
            owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
            CHECK(model.objects.front().name == "Authored parent");
            CHECK_FALSE(model.canUndo);
        }
    }

    TEST_CASE("Workspace prefab selection and source navigation retain exact ownership without ordinary mutation",
              "[editor][prefab][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        const auto &model = owner.ViewModel();
        REQUIRE(model.prefabHierarchy != nullptr);
        REQUIRE(model.prefabHierarchy->entries.size() == 3);
        const auto member = model.prefabHierarchy->entries.back().target;
        const auto revision = model.documentRevision;
        const auto cache = model.prefabHierarchy;
        owner.ProcessCommand(Intent(model, member, PrefabHierarchyAction::Select));
        CHECK(model.prefabSelection == member);
        CHECK_FALSE(model.primarySelection.has_value());
        CHECK(model.selectedObjects.empty());
        CHECK(model.documentRevision == revision);
        CHECK(model.prefabHierarchy == cache);
        CHECK(workspace.captures == 1);
        owner.ProcessCommand(Intent(model, member, PrefabHierarchyAction::DeleteInstance));
        CHECK(model.documentRevision == revision);
        CHECK_FALSE(model.isDirty);
        owner.ProcessCommand(Intent(model, member, PrefabHierarchyAction::OpenSource));
        REQUIRE(model.prefabDocuments.size() == 1);
        CHECK(model.prefabDocuments.front().identity.key.source.Value() == workspace.project.record.sourcePath.String());
        CHECK(model.prefabDocuments.front().stageActive);
        CHECK_FALSE(workspace.navigated.has_value());
        owner.ProcessCommand(Intent(model, member, PrefabHierarchyAction::LocateInstance));
        CHECK(model.prefabSelection == member);
        CHECK(model.documentRevision == revision);
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument,
                              .documentInstance = model.prefabDocuments.front().identity.instance});
        REQUIRE(model.prefabDocuments.empty());
        CHECK_FALSE(model.workspacePanelHost.ActiveDocument().has_value());
        CHECK(model.prefabSelection == member);
        CHECK(model.documentRevision == revision);
        owner.ProcessCommand(Intent(model, member, PrefabHierarchyAction::LocateInstance));
        REQUIRE(model.prefabSelection.has_value());
        CHECK_FALSE(model.prefabSelection->member.has_value());
        CHECK(model.prefabSelection->instance == workspace.project.instance);
    }

    TEST_CASE("Workspace prefab source opening preserves native Unicode paths and UTF-8 identity", "[editor][prefab][native]") {
        const std::u8string_view name = u8"köprü_日本_ş";
        Workspace workspace{std::string{name.begin(), name.end()}};
        auto &owner = *workspace.controller;
        const auto &model = owner.ViewModel();
        REQUIRE(model.prefabHierarchy != nullptr);
        const auto projection = model.prefabHierarchy;
        const auto revision = model.documentRevision;
        owner.ProcessCommand(Intent(model, projection->entries.back().target, PrefabHierarchyAction::OpenSource));
        REQUIRE(model.prefabDocuments.size() == 1);
        CHECK(model.prefabDocuments.front().identity.key.source.Value() == workspace.project.record.sourcePath.String());
        CHECK(model.prefabDocuments.front().stageActive);
        CHECK_FALSE(workspace.navigated.has_value());
        CHECK(model.prefabDocuments.front().identity.key.source.Value() == workspace.project.record.sourcePath.String());
        CHECK(model.prefabHierarchy == projection);
        CHECK(model.documentRevision == revision);
        CHECK_FALSE(model.isDirty);
    }

    TEST_CASE("Workspace prefab diagnostic handoff preserves the actual native Unicode path", "[editor][prefab][native]") {
        const std::u8string_view name = u8"köprü_日本_ş";
        Workspace workspace{std::string{name.begin(), name.end()}, true};
        auto &owner = *workspace.controller;
        const auto &model = owner.ViewModel();
        REQUIRE(model.prefabHierarchy != nullptr);
        const auto revision = model.documentRevision;
        owner.ProcessCommand(Intent(model, model.prefabHierarchy->entries.back().target, PrefabHierarchyAction::OpenSource));
        REQUIRE(model.prefabDocuments.size() == 1);
        CHECK(model.prefabDocuments.front().identity.key.source.Value() == workspace.project.record.sourcePath.String());
        CHECK(model.prefabDocuments.front().stageActive);
        CHECK_FALSE(workspace.diagnosticPath.has_value());
        CHECK_FALSE(workspace.navigated.has_value());
        CHECK(model.documentRevision == revision);
        CHECK_FALSE(model.isDirty);
    }

    TEST_CASE("Workspace diagnostic navigation rejects malformed bytes before opening a document", "[editor][prefab][native]") {
        Workspace workspace{"hierarchy", true};
        auto &owner = *workspace.controller;
        const auto revision = owner.ViewModel().documentRevision;
        const auto projection = owner.ViewModel().prefabHierarchy;
        std::filesystem::path native;
        SECTION("malformed native encoding") {
#ifdef _WIN32
            std::wstring text = L"source/Bad";
            text.push_back(static_cast<wchar_t>(0xd800));
            native = std::filesystem::path{text + L".cpp"};
#else
            native = std::filesystem::path{std::string{"source/Bad"} + "\xc0\xaf" + ".cpp"};
            std::filesystem::create_directories(workspace.project.directory.path / "source");
            std::ofstream(workspace.project.directory.path / native) << "existing invalid native path";
#endif
        }
        SECTION("embedded NUL") {
            native = std::filesystem::path{u8"source/Existing.cpp"};
            std::filesystem::create_directories(workspace.project.directory.path / "source");
            std::ofstream(workspace.project.directory.path / native) << "must not alias";
            native += std::filesystem::path::value_type{};
            native += ".cpp";
        }
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenDiagnosticSource,
                              .diagnosticSource = DiagnosticSourceRequest{.absolutePath = native}});
        CHECK_FALSE(workspace.diagnosticPath.has_value());
        CHECK_FALSE(workspace.navigated.has_value());
        CHECK(owner.ViewModel().contentBrowserOperationError == "workspace.global_dock.build_output.source.invalid");
        CHECK(owner.ViewModel().prefabHierarchy == projection);
        CHECK(owner.ViewModel().documentRevision == revision);
        CHECK_FALSE(owner.ViewModel().isDirty);
    }

    TEST_CASE("Rejected malformed prefab records preserve workspace source authority", "[editor][prefab][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        const auto publication = workspace.project.registry.Snapshot();
        const auto revision = owner.ViewModel().documentRevision;
        const auto projection = owner.ViewModel().prefabHierarchy;
        REQUIRE(projection != nullptr);
        REQUIRE_FALSE(projection->entries.empty());
        std::string name = "broken";
        SECTION("malformed UTF-8") {
            name += "\xc0\xaf";
        }
        SECTION("embedded NUL") {
            name.push_back('\0');
        }
        const auto record = ScenePrefabRecord(workspace.project.asset, name);
        const auto rejected = workspace.project.registry.Publish({record});
        REQUIRE(rejected.status == Assets::AssetRegistryBuildStatus::Failed);
        REQUIRE(rejected.diagnostics.size() == 1);
        CHECK(rejected.diagnostics.front().error.code.Value() == "asset.registry.root_invalid");
        CHECK(rejected.publishedRevision == publication.Revision());
        CHECK(workspace.project.registry.Snapshot().Records().data() == publication.Records().data());
        owner.RefreshAssets(workspace.project.registry.Snapshot());
        const auto &model = owner.ViewModel();
        CHECK(model.prefabHierarchy == projection);
        CHECK_FALSE(workspace.navigated.has_value());
        CHECK_FALSE(workspace.navigatedIdentity.has_value());
        owner.ProcessCommand(Intent(model, projection->entries.front().target, PrefabHierarchyAction::OpenSource));
        REQUIRE(model.prefabDocuments.size() == 1);
        CHECK(model.prefabDocuments.front().identity.key.source.Value() == workspace.project.record.sourcePath.String());
        CHECK(model.prefabDocuments.front().stageActive);
        CHECK_FALSE(workspace.navigated.has_value());
        CHECK(model.prefabDocuments.front().identity.key.source.Value() == workspace.project.record.sourcePath.String());
        CHECK(model.documentRevision == revision);
        CHECK_FALSE(model.isDirty);
    }

    TEST_CASE("Workspace prefab root deletion uses canonical undo redo save and reload", "[editor][prefab][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        const PrefabHierarchyTarget root{workspace.project.instance, std::nullopt};
        auto stale = Intent(owner.ViewModel(), root, PrefabHierarchyAction::DeleteInstance);
        owner.ProcessCommand(stale);
        CHECK(owner.ViewModel().isDirty);
        REQUIRE(owner.ViewModel().prefabHierarchy->entries.empty());
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
        REQUIRE(owner.ViewModel().prefabHierarchy->entries.size() == 3);
        const auto revision = owner.ViewModel().documentRevision;
        owner.ProcessCommand(stale);
        CHECK(owner.ViewModel().documentRevision == revision);
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::RedoScene});
        REQUIRE(owner.ViewModel().prefabHierarchy->entries.empty());
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::SaveScene});
        CHECK_FALSE(owner.ViewModel().isDirty);
        workspace.controller.reset();
        workspace.Open(PrefabAsset(10).Asset());
        REQUIRE(workspace.controller->ViewModel().prefabHierarchy->entries.empty());
        workspace.controller->ProcessCommand(stale);
        CHECK_FALSE(workspace.controller->ViewModel().isDirty);
    }

    TEST_CASE("Workspace prefab source retirement preserves repairable roots on actual registry refresh", "[editor][prefab][native]") {
        Workspace workspace;
        const auto prior = workspace.controller->ViewModel().prefabHierarchy;
        const auto stale = Intent(workspace.controller->ViewModel(), prior->entries.back().target, PrefabHierarchyAction::OpenSource);
        const auto revision = workspace.controller->ViewModel().documentRevision;
        REQUIRE(std::filesystem::remove(workspace.project.source));
        REQUIRE(workspace.project.registry.Publish({workspace.project.record}).status == Assets::AssetRegistryBuildStatus::Complete);
        workspace.controller->RefreshAssets(workspace.project.registry.Snapshot());
        const auto current = workspace.controller->ViewModel().prefabHierarchy;
        REQUIRE(current != nullptr);
        REQUIRE(current->entries.size() == 3);
        CHECK(current->entries.front().target.instance == workspace.project.instance);
        REQUIRE(current->entries.front().failure.has_value());
        CHECK(prior->entries.size() == 3);
        CHECK(workspace.controller->ViewModel().documentRevision == revision);
        CHECK(current->publication != prior->publication);
        workspace.controller->ProcessCommand(stale);
        CHECK_FALSE(workspace.navigated.has_value());
        CHECK_FALSE(workspace.controller->ViewModel().isDirty);
    }

    TEST_CASE("Workspace prefab root movement and duplication retain one canonical history and reject member parents",
              "[editor][prefab][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        const PrefabHierarchyTarget root{workspace.project.instance, std::nullopt};
        owner.ProcessCommand(Intent(owner.ViewModel(), root, PrefabHierarchyAction::DuplicateInstance));
        REQUIRE(owner.ViewModel().prefabHierarchy->entries.size() == 6);
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
        REQUIRE(owner.ViewModel().prefabHierarchy->entries.size() == 3);
        auto rejected = Intent(owner.ViewModel(), root, PrefabHierarchyAction::ReparentInstance);
        rejected.prefabHierarchy->destinationParent = SceneObjectId{999};
        const auto prior = owner.ViewModel().documentRevision;
        owner.ProcessCommand(rejected);
        CHECK(owner.ViewModel().documentRevision == prior);
        auto moved = Intent(owner.ViewModel(), root, PrefabHierarchyAction::ReparentInstance);
        owner.ProcessCommand(moved);
        CHECK_FALSE(owner.ViewModel().prefabHierarchy->sources->instances.front().authored.parent.has_value());
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
        CHECK(owner.ViewModel().prefabHierarchy->sources->instances.front().authored.parent == SceneObjectId{11});
    }

    TEST_CASE("Closed workspace prefab gestures cannot resolve a fresh owner with the same saved scene", "[editor][prefab][native]") {
        Workspace workspace;
        const PrefabHierarchyTarget root{workspace.project.instance, std::nullopt};
        auto stale = Intent(workspace.controller->ViewModel(), root, PrefabHierarchyAction::DeleteInstance);
        const auto retained = workspace.controller->ViewModel().prefabHierarchy;
        workspace.controller.reset();
        REQUIRE(retained->entries.size() == 3);
        workspace.Open(PrefabAsset(10).Asset());
        const auto before = workspace.controller->ViewModel().documentRevision;
        REQUIRE(workspace.controller->ViewModel().prefabHierarchy->entries.size() == 3);
        workspace.controller->ProcessCommand(stale);
        CHECK(workspace.controller->ViewModel().documentRevision == before);
        CHECK_FALSE(workspace.controller->ViewModel().isDirty);
        CHECK(workspace.controller->ViewModel().prefabHierarchy->entries.size() == 3);
    }

    TEST_CASE("Prefab gesture publication survives SaveCopy and rejected SaveAs but retires on successful SaveAs",
              "[editor][prefab][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        const PrefabHierarchyTarget root{workspace.project.instance, std::nullopt};
        auto stale = Intent(owner.ViewModel(), root, PrefabHierarchyAction::Select);
        const auto publication = owner.ViewModel().prefabHierarchy;
        EditorWorkspaceViewCommandData save;
        save.command = EditorWorkspaceViewCommand::SaveSceneCopyAs;
        save.stringPayload = (workspace.project.directory.path / "assets/scenes/copy.horo").string();
        owner.ProcessCommand(save);
        CHECK(owner.ViewModel().prefabHierarchy == publication);
        save.command = EditorWorkspaceViewCommand::SaveSceneAs;
        save.stringPayload = (workspace.project.directory.path.parent_path() / "outside.horo").string();
        owner.ProcessCommand(save);
        CHECK(owner.ViewModel().prefabHierarchy == publication);
        save.stringPayload = (workspace.project.directory.path / "assets/scenes/moved.horo").string();
        owner.ProcessCommand(save);
        REQUIRE(owner.ViewModel().prefabHierarchy != nullptr);
        CHECK(owner.ViewModel().prefabHierarchy->publication != publication->publication);
        owner.ProcessCommand(stale);
        CHECK_FALSE(owner.ViewModel().prefabSelection.has_value());
        CHECK_FALSE(owner.ViewModel().isDirty);
    }

}  // namespace Horo::Editor
