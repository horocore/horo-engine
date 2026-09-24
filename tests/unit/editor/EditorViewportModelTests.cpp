#include "editor/project_model/EditorViewportModel.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace {
    [[nodiscard]] bool NearlyEqual(const float lhs, const float rhs) noexcept {
        return std::fabs(lhs - rhs) < 0.0001F;
    }

    TEST_CASE("Navigation Moves The Authoritative Camera And Publishes Once", "[unit][editor]") {
        using namespace Horo::Editor;
        EditorDataBus events;
        EditorViewportModel viewport{events};
        std::vector<ViewportChangedEvent> published;
        auto subscription = events.Subscribe<ViewportChangedEvent>([&published](const ViewportChangedEvent &event) {
            published.push_back(event);
        });

        const auto moved = viewport.Navigate(EditorViewportNavigationDelta{.moveForward = 1.0F});
        REQUIRE((moved.HasValue()));
        REQUIRE((viewport.Current().revision == ViewportRevision{1}));
        REQUIRE((NearlyEqual(viewport.Current().camera.position.z, 3.0F)));
        REQUIRE((NearlyEqual(viewport.Current().camera.target.z, -1.0F)));
        REQUIRE((published.size() == 1));
        REQUIRE((published.front().kind == ViewportChangeKind::CameraMoved));
        static_cast<void>(subscription);
    }

    TEST_CASE("Orbit Keeps The Target Fixed And Produces A Valid Camera", "[unit][editor]") {
        using namespace Horo::Editor;
        EditorDataBus events;
        EditorViewportModel viewport{events};
        const Horo::Math::Vec3 target = viewport.Current().camera.target;

        const auto orbited = viewport.Navigate(EditorViewportNavigationDelta{.yawRadians = 0.4F, .pitchRadians = -0.2F, .orbit = true});
        REQUIRE((orbited.HasValue()));
        REQUIRE((viewport.Current().camera.target == target));
        REQUIRE((viewport.Current().camera.IsValid()));
        REQUIRE((!NearlyEqual(viewport.Current().camera.position.x, 0.0F)));
    }

    TEST_CASE("Compass axis alignment preserves target and lets both vertical views navigate", "[unit][editor]") {
        using namespace Horo::Editor;
        using Horo::Math::Vec3;
        EditorDataBus events;
        EditorViewportModel viewport{events};
        constexpr std::array views{
            std::pair{EditorViewportAxisView::PositiveX, Vec3{1.0F, 0.0F, 0.0F}},
            std::pair{EditorViewportAxisView::NegativeX, Vec3{-1.0F, 0.0F, 0.0F}},
            std::pair{EditorViewportAxisView::PositiveY, Vec3{0.0F, 1.0F, 0.0F}},
            std::pair{EditorViewportAxisView::NegativeY, Vec3{0.0F, -1.0F, 0.0F}},
            std::pair{EditorViewportAxisView::PositiveZ, Vec3{0.0F, 0.0F, 1.0F}},
            std::pair{EditorViewportAxisView::NegativeZ, Vec3{0.0F, 0.0F, -1.0F}},
        };
        const Vec3 target = viewport.Current().camera.target;
        const float distance = Horo::Math::Length(viewport.Current().camera.position - target);
        for (const auto &[view, direction] : views) {
            REQUIRE(viewport.AlignToAxis(view).HasValue());
            const EditorViewportCamera &camera = viewport.Current().camera;
            REQUIRE(camera.IsValid());
            REQUIRE(camera.target == target);
            REQUIRE(NearlyEqual(Horo::Math::Length(camera.position - target), distance));
            REQUIRE(camera.position == target + direction * distance);
        }
        for (const EditorViewportAxisView vertical : {EditorViewportAxisView::PositiveY, EditorViewportAxisView::NegativeY}) {
            REQUIRE(viewport.AlignToAxis(vertical).HasValue());
            REQUIRE(viewport.Navigate({.moveRight = 0.25F}).HasValue());
            REQUIRE(viewport.Navigate({.pitchRadians = 0.2F, .orbit = true}).HasValue());
            REQUIRE(viewport.Current().camera.IsValid());
        }
        const ViewportRevision beforeInvalid = viewport.Current().revision;
        REQUIRE(viewport.AlignToAxis(static_cast<EditorViewportAxisView>(255)).HasError());
        REQUIRE(viewport.Current().revision == beforeInvalid);
    }

    TEST_CASE("Empty And Invalid Navigation Do Not Publish Or Mutate", "[unit][editor]") {
        using namespace Horo::Editor;
        EditorDataBus events;
        EditorViewportModel viewport{events};
        std::vector<ViewportChangedEvent> published;
        auto subscription = events.Subscribe<ViewportChangedEvent>([&published](const ViewportChangedEvent &event) {
            published.push_back(event);
        });

        REQUIRE((viewport.Navigate({}).HasValue()));
        EditorViewportNavigationDelta invalid;
        invalid.moveRight = std::numeric_limits<float>::quiet_NaN();
        REQUIRE((viewport.Navigate(invalid).HasError()));
        REQUIRE((viewport.Current().revision == ViewportRevision{}));
        REQUIRE((published.empty()));
        static_cast<void>(subscription);
    }

    TEST_CASE("Projection Changes Preserve Target Plane Scale", "[unit][editor]") {
        using namespace Horo::Editor;
        EditorDataBus events;
        EditorViewportModel viewport{events};
        const float initialDistance = Horo::Math::Length(viewport.Current().camera.target - viewport.Current().camera.position);
        const float expectedHeight = 2.0F * initialDistance * std::tan(viewport.Current().camera.verticalFovRadians * 0.5F);
        REQUIRE((viewport.SetProjection(Horo::Runtime::CameraProjection::Orthographic).HasValue()));
        REQUIRE((NearlyEqual(viewport.Current().camera.orthographicHeight, expectedHeight)));
        REQUIRE((viewport.SetProjection(Horo::Runtime::CameraProjection::Perspective).HasValue()));
        REQUIRE((NearlyEqual(Horo::Math::Length(viewport.Current().camera.target - viewport.Current().camera.position), initialDistance)));
        REQUIRE((viewport.Current().revision == ViewportRevision{2}));
    }

    TEST_CASE("Focus And Dolly Are Projection Aware", "[unit][editor]") {
        using namespace Horo::Editor;
        EditorDataBus events;
        EditorViewportModel viewport{events};
        const Horo::Math::Aabb bounds{{-1.0F, -2.0F, -1.0F}, {1.0F, 2.0F, 1.0F}};
        REQUIRE((viewport.Focus(bounds, 0.5F).HasValue()));
        REQUIRE((viewport.Current().camera.target == bounds.Center()));
        const float perspectiveDistance = Horo::Math::Length(viewport.Current().camera.target - viewport.Current().camera.position);
        REQUIRE((viewport.Navigate(EditorViewportNavigationDelta{.dollyScale = 0.5F}).HasValue()));
        REQUIRE((Horo::Math::Length(viewport.Current().camera.target - viewport.Current().camera.position) < perspectiveDistance));

        REQUIRE((viewport.SetProjection(Horo::Runtime::CameraProjection::Orthographic).HasValue()));
        REQUIRE((viewport.Focus(bounds, 0.5F).HasValue()));
        const float orthographicHeight = viewport.Current().camera.orthographicHeight;
        REQUIRE((orthographicHeight >= 2.0F * 2.0F * 1.2F / 0.5F));
        REQUIRE((viewport.Navigate(EditorViewportNavigationDelta{.dollyScale = 0.5F}).HasValue()));
        REQUIRE((NearlyEqual(viewport.Current().camera.orthographicHeight, orthographicHeight * 0.5F)));
        REQUIRE((viewport.Focus(bounds, 0.0F).HasError()));

        const ViewportRevision revision = viewport.Current().revision;
        const float extreme = std::numeric_limits<float>::max();
        REQUIRE((viewport.Focus(Horo::Math::Aabb{{-extreme, -extreme, -extreme}, {extreme, extreme, extreme}}, 1.0F).HasError()));
        REQUIRE((viewport.Current().revision == revision));
    }

    TEST_CASE("Transform Preview Is Authoritative Workspace State", "[unit][editor]") {
        using namespace Horo::Editor;
        EditorDataBus events;
        EditorViewportModel viewport{events};
        std::vector<ViewportChangedEvent> published;
        auto subscription = events.Subscribe<ViewportChangedEvent>([&published](const ViewportChangedEvent &event) {
            published.push_back(event);
        });

        const SceneObjectTransformPreview preview{
            .object = SceneObjectId{42},
            .localTransform = Horo::Math::Transform{.translation = {1.0F, 2.0F, 3.0F}},
        };
        REQUIRE((viewport.SetTransformPreview(preview).HasValue()));
        REQUIRE((viewport.Current().transformPreviews == std::vector{preview}));
        REQUIRE((viewport.Current().revision == ViewportRevision{1}));
        REQUIRE((published.size() == 1 && published.back().kind == ViewportChangeKind::ScenePreviewChanged));

        REQUIRE((viewport.SetTransformPreview(preview).HasValue()));
        REQUIRE((published.size() == 1));
        REQUIRE((viewport.ClearTransformPreview()));
        REQUIRE((viewport.Current().transformPreviews.empty()));
        REQUIRE((viewport.Current().revision == ViewportRevision{2}));
        REQUIRE((published.size() == 2 && published.back().kind == ViewportChangeKind::ScenePreviewChanged));
        REQUIRE((!viewport.ClearTransformPreview()));

        SceneObjectTransformPreview invalid = preview;
        invalid.localTransform.translation.x = std::numeric_limits<float>::quiet_NaN();
        REQUIRE((viewport.SetTransformPreview(invalid).HasError()));
        REQUIRE((viewport.Current().revision == ViewportRevision{2}));
        REQUIRE((published.size() == 2));

        const std::array duplicate{preview, preview};
        REQUIRE((viewport.SetTransformPreviews(duplicate).HasError()));
        REQUIRE((viewport.Current().transformPreviews.empty()));
        static_cast<void>(subscription);
    }

    TEST_CASE("Light Preview Is Authoritative Workspace State", "[unit][editor]") {
        using namespace Horo::Editor;
        EditorDataBus events;
        EditorViewportModel viewport{events};
        std::vector<ViewportChangedEvent> published;
        auto subscription = events.Subscribe<ViewportChangedEvent>([&published](const ViewportChangedEvent &event) {
            published.push_back(event);
        });

        const SceneObjectLightPreview preview{
            .object = SceneObjectId{43},
            .light =
                Horo::Runtime::LightComponent{
                    .kind = Horo::Runtime::LightKind::Spot,
                    .intensity = 3.0F,
                    .range = 20.0F,
                    .innerConeRadians = 0.25F,
                    .outerConeRadians = 0.75F,
                },
        };
        REQUIRE((viewport.SetLightPreview(preview).HasValue()));
        REQUIRE((viewport.Current().lightPreview == preview));
        REQUIRE((viewport.Current().revision == ViewportRevision{1}));
        REQUIRE((published.size() == 1));
        REQUIRE((published.back().kind == ViewportChangeKind::ScenePreviewChanged));
        REQUIRE((viewport.SetLightPreview(preview).HasValue()));
        REQUIRE((published.size() == 1));
        REQUIRE((viewport.ClearLightPreview()));
        REQUIRE((!viewport.Current().lightPreview.has_value()));
        REQUIRE((viewport.Current().revision == ViewportRevision{2}));
        REQUIRE((!viewport.ClearLightPreview()));

        SceneObjectLightPreview invalid = preview;
        invalid.light.intensity = -1.0F;
        REQUIRE((viewport.SetLightPreview(invalid).HasError()));
        REQUIRE((viewport.Current().revision == ViewportRevision{2}));
        static_cast<void>(subscription);
    }
}  // namespace
