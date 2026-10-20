#include "navigation/NavigationProjectBakeInputFixture.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Application {
    namespace {
        namespace Nav = Navigation;
        namespace Inputs = NavigationInputTestSupport;
        using Nav::TestSupport::Id;
        using Nav::TestSupport::RequireError;
        using NavigationSourceTestSupport::Asset;

        auto Capture(const Inputs::Project &project, const NavigationProjectBakeInputLimits &limits = {}) {
            return CaptureNavigationProjectBakeInput(project.Capture(), Asset(1), Inputs::Project::Revisions(), limits);
        }
    }  // namespace

    TEST_CASE("Project input captures real material bytes and repeated Scene-local IDs into isolated complete partitions",
              "[navigation][project][bake_input]") {
        Inputs::Project project;
        const auto source = project.Capture();
        REQUIRE(source->Materials().size() == 1);
        CHECK(source->Materials().front().material.Id().Asset() == Asset(4));
        const auto captured = CaptureNavigationProjectBakeInput(source, Asset(1), Inputs::Project::Revisions());
        REQUIRE(captured.HasValue());
        const auto &value = captured.Value();
        CHECK(value.source == source);
        REQUIRE(value.input->Partitions().size() == 2);
        REQUIRE(value.contributions.size() == 2);
        REQUIRE(value.surfaces.size() == 2);
        REQUIRE(value.tiles.size() == 2);
        CHECK(value.contributions[0].observation.producer != value.contributions[1].observation.producer);
        for (const auto &entry : value.contributions) {
            CHECK(entry.key.object.value == 1);
            CHECK(entry.key.surface == Id<Nav::SurfaceId>(10));
            CHECK(entry.key.body.object.value == 1);
            CHECK(entry.key.collider.value == 3);
            CHECK(entry.material == Asset(4));
        }
        const auto compatibility = Nav::TestSupport::IncrementalBakeFixture{}.compatibility;
        for (const auto &tile : value.tiles) {
            CHECK(tile.key.scope.IsSceneScoped());
            CHECK(tile.key.surface == Id<Nav::SurfaceId>(10));
            CHECK(tile.key.tile.x == 0);
            CHECK(tile.key.tile.z == 0);
            CHECK(tile.tileSizeMeters == 8);
            CHECK(Nav::PrepareNavigationBakeTile(*value.input, tile, compatibility).HasValue());
        }
        CHECK(value.tiles[0].key.scope.SceneBytes() == Asset(2).Bytes());
        CHECK(value.tiles[1].key.scope.SceneBytes() == Asset(3).Bytes());
    }

    TEST_CASE("Canonical project bake identity survives source rename without accepting an obsolete source owner",
              "[navigation][project][bake_input][identity]") {
        Inputs::Project project;
        const auto previous = Capture(project);
        REQUIRE(previous.HasValue());
        const auto root = project.source.directory.Path();
        std::filesystem::rename(root / "Assets/first.horo", root / "Assets/renamed ü.horo");
        std::filesystem::rename(root / "Assets/first.horo.horo", root / "Assets/renamed ü.horo.horo");
        const auto current = Capture(project);
        REQUIRE(current.HasValue());
        CHECK(previous.Value().input->Fingerprint() == current.Value().input->Fingerprint());
        REQUIRE(previous.Value().contributions.size() == current.Value().contributions.size());
        for (std::size_t i = 0; i < current.Value().contributions.size(); ++i) {
            CHECK(previous.Value().contributions[i].key == current.Value().contributions[i].key);
            CHECK(previous.Value().contributions[i].observation == current.Value().contributions[i].observation);
        }
        CHECK(previous.Value().source->VerifyCurrent().HasError());
        CHECK(current.Value().source->VerifyCurrent().HasValue());
    }

    TEST_CASE("Project geometry limits apply across Scenes instead of restarting for every Scene",
              "[navigation][project][bake_input][budget]") {
        Inputs::Project project;
        NavigationProjectBakeInputLimits limits;
        SECTION("contributions") {
            limits.scene.geometry.storage.maxContributions = 1;
        }
        SECTION("vertices") {
            limits.scene.geometry.storage.maxVertices = 8;
        }
        SECTION("triangles") {
            limits.scene.geometry.storage.maxTriangles = 12;
        }
        SECTION("authored surfaces") {
            limits.maximumSurfaces = 1;
        }
        SECTION("complete tiles") {
            limits.maximumTiles = 1;
        }
        const auto captured = Capture(project, limits);
        RequireError(captured, Nav::NavigationErrors::CapacityExceeded);
    }

    TEST_CASE("Profile multiplication is counted before allocating complete surface bindings",
              "[navigation][project][bake_input][budget]") {
        Inputs::Project project;
        NavigationProjectBakeInputLimits limits;
        limits.input.maxSurfaceBindings = 1;
        RequireError(Capture(project, limits), Nav::NavigationErrors::BakeInputCapacityExceeded);
    }

    TEST_CASE("Clipped empty Scene sources preserve complete authored bounds and freshness contributions",
              "[navigation][project][bake_input][empty]") {
        Inputs::Project project;
        auto object = Inputs::Project::Object();
        auto &surface = *object.components.navigationSurface;
        surface.bakeScope = Runtime::NavigationBakeScope::LocalBounds;
        surface.localBounds = Runtime::NavigationLocalBounds{{4, 0, 4}, {4, 2, 4}};
        object.components.colliders.front().localPose.translation.x = 100;
        project.Scenes(object);
        const auto result = Capture(project);
        REQUIRE(result.HasValue());
        CHECK(result.Value().contributions.size() == 2);
        CHECK(result.Value().input->Triangles().empty());
        CHECK(result.Value().input->Partitions().size() == 2);
        REQUIRE(result.Value().tiles.size() == 2);
        for (const auto &tile : result.Value().tiles) {
            CHECK(tile.bounds.minimum.x == 0);
            CHECK(tile.bounds.maximum.x == 8);
        }
    }

    TEST_CASE("Complete source removal retains authored catalog without retaining stale generated partitions",
              "[navigation][project][bake_input][empty]") {
        Inputs::Project project;
        auto object = Inputs::Project::Object();
        object.components.navigationSources.clear();
        project.Scenes(object);
        const auto result = Capture(project);
        REQUIRE(result.HasValue());
        CHECK(result.Value().contributions.empty());
        CHECK(result.Value().input->Partitions().empty());
        CHECK(result.Value().tiles.empty());
        CHECK(result.Value().surfaces.size() == 2);
    }

    TEST_CASE("Disabled authored surfaces retain bounded selection metadata without producing geometry or tiles",
              "[navigation][project][bake_input][disabled]") {
        Inputs::Project project;
        auto object = Inputs::Project::Object();
        object.components.navigationSurface->enabled = false;
        object.components.navigationModifier =
            Runtime::NavigationModifierComponent{.id = Id<Nav::NavigationModifierId>(1), .surface = Id<Nav::SurfaceId>(10)};
        project.Scenes(object);
        const auto result = Capture(project);
        REQUIRE(result.HasValue());
        CHECK(result.Value().surfaces.size() == 2);
        CHECK(result.Value().contributions.empty());
        CHECK(result.Value().input->Partitions().empty());
        CHECK(result.Value().tiles.empty());
        NavigationProjectBakeInputLimits limits;
        limits.maximumSurfaces = 1;
        RequireError(Capture(project, limits), Nav::NavigationErrors::CapacityExceeded);
    }

    TEST_CASE("Negative exact grid edges do not allocate a spurious positive tile", "[navigation][project][bake_input][grid]") {
        Inputs::Project project;
        auto object = Inputs::Project::Object();
        object.components.colliders.front().localPose.translation = {-4, -0.5F, -4};
        project.Scenes(object);
        const auto result = Capture(project);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().tiles.size() == 2);
        for (const auto &tile : result.Value().tiles) {
            CHECK(tile.key.tile.x == -1);
            CHECK(tile.key.tile.z == -1);
            CHECK(tile.bounds.minimum.x == -8);
            CHECK(tile.bounds.maximum.x == 0);
            CHECK(tile.bounds.maximum.z == 0);
        }
    }

    TEST_CASE("Each selected profile uses its own authored voxel extent in one sorted complete grid",
              "[navigation][project][bake_input][profiles]") {
        Inputs::Project project;
        Nav::TestSupport::NavigationDataQualificationCorpus corpus;
        const auto original = corpus.Definition();
        Nav::NavigationDefinitionInput input;
        input.profiles.assign(original.Profiles().begin(), original.Profiles().end());
        input.profiles[1].buildGeometry.cellSizeMeters = 1;
        input.areas.assign(original.Registry().Areas().begin(), original.Registry().Areas().end());
        input.filters.assign(original.Registry().Filters().begin(), original.Registry().Filters().end());
        input.tiles = original.Tiles();
        const auto definition = Nav::NavigationDefinition::Create(std::move(input));
        REQUIRE(definition.HasValue());
        project.Definition(definition.Value());
        auto object = Inputs::Project::Object();
        object.components.navigationSurface->profiles = {Id<Nav::NavigationAgentProfileId>(2), Id<Nav::NavigationAgentProfileId>(1)};
        project.Scenes(object);
        const auto result = Capture(project);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().tiles.size() == 4);
        CHECK(std::ranges::is_sorted(result.Value().tiles, {}, &Nav::NavigationBakeTile::key));
        for (const auto &tile : result.Value().tiles)
            CHECK(tile.tileSizeMeters == (tile.key.profile.Value() == 1 ? 8 : 16));
    }

    TEST_CASE("Project capture refuses missing physical material and preserves actual-byte freshness",
              "[navigation][project][bake_input][material]") {
        Inputs::Project project;
        SECTION("missing registered material") {
            const auto root = project.source.directory.Path();
            REQUIRE(std::filesystem::remove(root / "Assets/floor.horoasset"));
            REQUIRE(std::filesystem::remove(root / "Assets/floor.horoasset.horo"));
            RequireError(Capture(project), Nav::NavigationErrors::BakeInputReferenceMissing);
        }
        SECTION("changed material bytes") {
            const auto source = project.Capture();
            project.Material(2);
            RequireError(CaptureNavigationProjectBakeInput(source, Asset(1), Inputs::Project::Revisions()),
                         Nav::NavigationErrors::BakeInputStale);
        }
    }

    TEST_CASE("Material decoding rejects payload identity substitution and corrupt canonical bytes",
              "[navigation][project][bake_input][material]") {
        Inputs::Project project;
        SECTION("identity substitution") {
            const auto material =
                Physics::PhysicsMaterialAsset::Create({.id = Physics::PhysicsMaterialAssetId::FromAssetId(Asset(99)), .sourceRevision = 1});
            REQUIRE(material.HasValue());
            project.source.WriteBytes("Assets/floor.horoasset", Physics::EncodePhysicsMaterialAsset(material.Value()).Value());
        }
        SECTION("truncated canonical payload") {
            project.source.Write("Assets/floor.horoasset", "incomplete");
        }
        const auto result =
            NavigationProjectBakeSource::Capture(project.source.files, project.source.Context(), project.source.Compatibility());
        CHECK(result.HasError());
    }

    TEST_CASE("Unassembled enabled authored semantics fail explicitly rather than yielding partial baked intent",
              "[navigation][project][bake_input][admission]") {
        Inputs::Project project;
        auto object = Inputs::Project::Object();
        SECTION("modifier") {
            object.components.navigationModifier =
                Runtime::NavigationModifierComponent{.id = Id<Nav::NavigationModifierId>(1), .surface = Id<Nav::SurfaceId>(10)};
        }
        SECTION("link") {
            object.components.navigationLink = Runtime::NavigationLinkComponent{.id = Id<Nav::NavigationLinkId>(1),
                                                                                .start = {Id<Nav::SurfaceId>(10), {1, 0, 1}, 0.5F},
                                                                                .end = {Id<Nav::SurfaceId>(10), {6, 0, 6}, 0.5F},
                                                                                .profiles = {Id<Nav::NavigationAgentProfileId>(1)}};
        }
        project.Scenes(object);
        RequireError(Capture(project), Nav::NavigationErrors::OperationUnsupported);
    }

    TEST_CASE("Project input cancellation and authoritative identity/profile admission precede work",
              "[navigation][project][bake_input][admission]") {
        Inputs::Project project;
        const auto source = project.Capture();
        auto revisions = Inputs::Project::Revisions();
        SECTION("cancelled") {
            CancellationSource stop;
            stop.RequestCancellation();
            RequireError(CaptureNavigationProjectBakeInput(source, Asset(1), revisions, {}, stop.Token()),
                         Nav::NavigationErrors::BakeInputCancelled);
        }
        SECTION("profile authority") {
            revisions.projectProfile = Id<Nav::NavigationProjectProfileRevision>(2);
            RequireError(CaptureNavigationProjectBakeInput(source, Asset(1), revisions), Nav::NavigationErrors::BakeInputInvalid);
        }
        SECTION("definition identity") {
            RequireError(CaptureNavigationProjectBakeInput(source, Asset(99), revisions), Nav::NavigationErrors::BakeInputReferenceMissing);
        }
        SECTION("missing retained owner") {
            RequireError(CaptureNavigationProjectBakeInput({}, Asset(1), revisions), Nav::NavigationErrors::BakeInputInvalid);
        }
    }

    TEST_CASE("Project input consumes inherited bake-job cancellation without publishing a partial capture",
              "[navigation][project][bake_input][cancellation]") {
        Inputs::Project project;
        const auto source = project.Capture();
        CancellationSource parent;
        CancellationSource child{parent.Token()};
        parent.RequestCancellation();
        RequireError(CaptureNavigationProjectBakeInput(source, Asset(1), Inputs::Project::Revisions(), {}, child.Token()),
                     Nav::NavigationErrors::BakeInputCancelled);
    }

    TEST_CASE("Horo Scene Physics capture rejects a mismatched declared coordinate policy even with no registered Scenes",
              "[navigation][project][bake_input][coordinates]") {
        Inputs::Project project;
        Nav::TestSupport::NavigationDataQualificationCorpus corpus;
        const auto original = corpus.Definition();
        Nav::NavigationDefinitionInput input;
        input.profiles.assign(original.Profiles().begin(), original.Profiles().end());
        input.areas.assign(original.Registry().Areas().begin(), original.Registry().Areas().end());
        input.filters.assign(original.Registry().Filters().begin(), original.Registry().Filters().end());
        input.tiles = original.Tiles();
        input.scope = original.Scope();
        SECTION("axis convention") {
            input.coordinates.system = Nav::NavigationSourceCoordinateSystem::RightHandedZUp;
        }
        SECTION("unit convention") {
            input.coordinates.metersPerUnit = 0.01F;
        }
        const auto definition = Nav::NavigationDefinition::Create(std::move(input));
        REQUIRE(definition.HasValue());
        project.Definition(definition.Value());
        RequireError(Capture(project), Nav::NavigationErrors::SourceGeometryInvalid);
        REQUIRE(std::filesystem::remove(project.source.directory.Path() / "Assets/first.horo"));
        REQUIRE(std::filesystem::remove(project.source.directory.Path() / "Assets/first.horo.horo"));
        REQUIRE(std::filesystem::remove(project.source.directory.Path() / "Assets/second.horo"));
        REQUIRE(std::filesystem::remove(project.source.directory.Path() / "Assets/second.horo.horo"));
        const auto emptySource = project.Capture();
        REQUIRE(emptySource->Scenes().empty());
        RequireError(CaptureNavigationProjectBakeInput(emptySource, Asset(1), Inputs::Project::Revisions()),
                     Nav::NavigationErrors::SourceGeometryInvalid);
    }

}  // namespace Horo::Application
