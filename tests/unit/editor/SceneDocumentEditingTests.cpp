#include "SceneDocumentTestSupport.h"

namespace {
    using namespace Horo::Editor::SceneDocumentTestSupport;

    TEST_CASE("Camera Component Editing Is Validated And Survives History", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        using namespace Horo::Runtime;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        CreateSceneObjectUseCase create{document, commands};

        const auto created = create.Execute({PrimitiveId{"primitive.object.camera"}, std::nullopt});
        REQUIRE((created.HasValue()));
        const CameraComponent original = *document.Objects().front().components.camera;
        const DocumentRevision initialRevision = document.Revision();

        CameraComponent edited = original;
        edited.projection = CameraProjection::Orthographic;
        edited.orthographicHeight = 18.0F;
        edited.nearPlane = 0.25F;
        edited.farPlane = 2400.0F;
        const auto committed = commands.Execute(SetSceneObjectCameraCommand{created.Value().object, edited});
        REQUIRE((committed.HasValue()));
        REQUIRE((committed.Value().committed));
        REQUIRE((committed.Value().kind == DocumentChangeKind::ComponentChanged));
        REQUIRE((document.Revision().value == initialRevision.value + 1));
        REQUIRE((*document.Objects().front().components.camera == edited));

        const auto noOp = commands.Execute(SetSceneObjectCameraCommand{created.Value().object, edited});
        REQUIRE((noOp.HasValue()));
        REQUIRE((!noOp.Value().committed));
        REQUIRE((document.Revision().value == initialRevision.value + 1));

        CameraComponent invalid = edited;
        invalid.farPlane = invalid.nearPlane;
        const auto rejected = commands.Execute(SetSceneObjectCameraCommand{created.Value().object, invalid});
        REQUIRE((rejected.HasError()));
        REQUIRE((document.Revision().value == initialRevision.value + 1));
        REQUIRE((*document.Objects().front().components.camera == edited));

        REQUIRE((commands.Undo().HasValue()));
        REQUIRE((*document.Objects().front().components.camera == original));
        REQUIRE((commands.Redo().HasValue()));
        REQUIRE((*document.Objects().front().components.camera == edited));
    }

    TEST_CASE("Light Component Editing Is Validated And Survives History", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        using namespace Horo::Runtime;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        CreateSceneObjectUseCase create{document, commands};

        const auto created = create.Execute({PrimitiveId{"primitive.object.light_point"}, std::nullopt});
        REQUIRE((created.HasValue()));
        const LightComponent original = *document.Objects().front().components.light;
        const DocumentRevision initialRevision = document.Revision();

        LightComponent edited = original;
        edited.kind = LightKind::Spot;
        edited.color = {0.25F, 0.5F, 0.75F};
        edited.intensity = 3.5F;
        edited.range = 42.0F;
        edited.innerConeRadians = 0.3F;
        edited.outerConeRadians = 0.8F;
        const auto committed = commands.Execute(SetSceneObjectLightCommand{created.Value().object, edited});
        REQUIRE((committed.HasValue()));
        REQUIRE((committed.Value().committed));
        REQUIRE((committed.Value().kind == DocumentChangeKind::ComponentChanged));
        REQUIRE((document.Revision().value == initialRevision.value + 1));
        REQUIRE((*document.Objects().front().components.light == edited));

        const auto noOp = commands.Execute(SetSceneObjectLightCommand{created.Value().object, edited});
        REQUIRE((noOp.HasValue()));
        REQUIRE((!noOp.Value().committed));
        REQUIRE((document.Revision().value == initialRevision.value + 1));

        LightComponent invalid = edited;
        invalid.outerConeRadians = invalid.innerConeRadians - 0.1F;
        const auto rejected = commands.Execute(SetSceneObjectLightCommand{created.Value().object, invalid});
        REQUIRE((rejected.HasError()));
        REQUIRE((document.Revision().value == initialRevision.value + 1));
        REQUIRE((*document.Objects().front().components.light == edited));

        invalid = edited;
        invalid.color.x = -0.1F;
        REQUIRE((commands.Execute(SetSceneObjectLightCommand{created.Value().object, invalid}).HasError()));
        invalid = edited;
        invalid.outerConeRadians = Math::Pi + 0.1F;
        REQUIRE((commands.Execute(SetSceneObjectLightCommand{created.Value().object, invalid}).HasError()));
        REQUIRE((document.Revision().value == initialRevision.value + 1));
        REQUIRE((*document.Objects().front().components.light == edited));

        REQUIRE((commands.Undo().HasValue()));
        REQUIRE((*document.Objects().front().components.light == original));
        REQUIRE((commands.Redo().HasValue()));
        REQUIRE((*document.Objects().front().components.light == edited));
    }

    TEST_CASE("Batch Transform Commits Atomically Into One History Entry", "[unit][editor]") {
        using namespace Horo;
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};

        const auto first = commands.Execute(CreateSceneObjectCommand{.name = "First"});
        const auto second = commands.Execute(CreateSceneObjectCommand{.name = "Second"});
        REQUIRE((first.HasValue() && second.HasValue()));

        const DocumentRevision beforeRevision = document.Revision();
        const SetSceneObjectTransformsCommand batch{{
            SceneObjectTransformUpdate{
                .object = first.Value().object,
                .localTransform = Math::Transform{.translation = {2.0F, 0.0F, 0.0F}},
            },
            SceneObjectTransformUpdate{
                .object = second.Value().object,
                .localTransform = Math::Transform{.translation = {0.0F, 3.0F, 0.0F}},
            },
        }};
        const Result<SceneCommandResult> committed = commands.Execute(batch);
        REQUIRE((committed.HasValue()));
        REQUIRE((committed.Value().committed));
        REQUIRE((committed.Value().affectedObjects == std::vector{first.Value().object, second.Value().object}));
        REQUIRE((document.Revision().value == beforeRevision.value + 1));
        REQUIRE((document.Objects()[0].localTransform.translation.x == 2.0F));
        REQUIRE((document.Objects()[1].localTransform.translation.y == 3.0F));

        REQUIRE((commands.Undo().HasValue()));
        REQUIRE((document.Objects()[0].localTransform.translation == Math::Vec3{}));
        REQUIRE((document.Objects()[1].localTransform.translation == Math::Vec3{}));

        REQUIRE((commands.Redo().HasValue()));
        REQUIRE((document.Objects()[0].localTransform.translation.x == 2.0F));
        REQUIRE((document.Objects()[1].localTransform.translation.y == 3.0F));
        REQUIRE((!history.CanRedo()));
    }

    TEST_CASE("Failed Commands Do Not Mutate Or Advance Revision", "[unit][editor]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        CreateSceneObjectCommand invalid{.name = "Invalid"};
        invalid.localTransform.translation.x = std::numeric_limits<float>::quiet_NaN();

        const auto result = commands.Execute(invalid);
        REQUIRE((result.HasError()));
        REQUIRE((document.Revision() == DocumentRevision{}));
        REQUIRE((document.Snapshot().objects.empty()));
        REQUIRE((!document.IsDirty()));
    }

    TEST_CASE("Euler Conversion Preserves The Composed Rotation", "[unit][editor]") {
        using namespace Horo::Math;
        const Vec3 authored{0.31F, -0.47F, 0.22F};
        const Quaternion rotation = Quaternion::FromEulerRadians(authored);
        const Vec3 recovered = rotation.ToEulerRadians();
        REQUIRE((Horo::Editor::SceneDocumentTestSupport::NearlyEqual(recovered.x, authored.x)));
        REQUIRE((Horo::Editor::SceneDocumentTestSupport::NearlyEqual(recovered.y, authored.y)));
        REQUIRE((Horo::Editor::SceneDocumentTestSupport::NearlyEqual(recovered.z, authored.z)));

        const Mat4 originalMatrix = Transform{.rotation = rotation}.ToMatrix();
        const Mat4 recoveredMatrix = Transform{.rotation = Quaternion::FromEulerRadians(recovered)}.ToMatrix();
        for (std::size_t index = 0; index < originalMatrix.values.size(); ++index) {
            REQUIRE((Horo::Editor::SceneDocumentTestSupport::NearlyEqual(originalMatrix.values[index], recoveredMatrix.values[index])));
        }
    }

    TEST_CASE("Affine Inverse Restores Transformed Points", "[unit][editor]") {
        using namespace Horo::Math;
        const Mat4 matrix =
            Transform{
                .translation = {2.0F, -3.0F, 4.0F},
                .rotation = Quaternion::FromEulerRadians({0.2F, -0.4F, 0.3F}),
                .scale = {2.0F, 1.5F, 0.5F},
            }
                .ToMatrix();
        const Horo::Result<Mat4> inverse = TryInverseAffine(matrix);
        REQUIRE((inverse.HasValue()));
        const Vec3 point{0.7F, -1.2F, 2.4F};
        const Vec3 restored = TransformAffinePoint(inverse.Value(), TransformAffinePoint(matrix, point));
        REQUIRE((Horo::Editor::SceneDocumentTestSupport::NearlyEqual(restored.x, point.x)));
        REQUIRE((Horo::Editor::SceneDocumentTestSupport::NearlyEqual(restored.y, point.y)));
        REQUIRE((Horo::Editor::SceneDocumentTestSupport::NearlyEqual(restored.z, point.z)));
        REQUIRE((TryInverseAffine(Transform{.scale = {0.0F, 1.0F, 1.0F}}.ToMatrix()).HasError()));
    }

    TEST_CASE("Commands Create Stable Objects And Track Saved Revision", "[unit][editor]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto created = commands.Execute(CreateSceneObjectCommand{
            .name = "Box",
            .primitiveMesh = PrimitiveMeshDescriptor{},
        });
        REQUIRE((created.HasValue()));
        REQUIRE((created.Value().object.IsValid()));
        REQUIRE((document.Revision().value == 1));
        REQUIRE((document.IsDirty()));
        REQUIRE((document.MarkSaved(document.Revision(), document.State()).HasValue()));
        REQUIRE((!document.IsDirty()));

        const auto renamed = commands.Execute(RenameSceneObjectCommand{created.Value().object, "Hero Box"});
        REQUIRE((renamed.HasValue()));
        REQUIRE((document.Revision().value == 2));
        REQUIRE((document.IsDirty()));
        REQUIRE((document.Snapshot().objects.front().name == "Hero Box"));
    }

    TEST_CASE("Unchanged Transform Does Not Create A History Entry", "[unit][editor]") {
        using namespace Horo::Editor;
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands{document, history};
        const auto created = commands.Execute(CreateSceneObjectCommand{.name = "Box"});
        REQUIRE((created.HasValue()));
        history.Clear();
        const SceneDocumentSnapshot before = document.Snapshot();

        const auto unchanged =
            commands.Execute(SetSceneObjectTransformCommand{created.Value().object, before.objects.front().localTransform});
        REQUIRE((unchanged.HasValue()));
        REQUIRE((!unchanged.Value().committed));
        REQUIRE((document.Revision() == before.revision));
        REQUIRE((document.State() == before.state));
        REQUIRE((!history.CanUndo()));
    }
}  // namespace
