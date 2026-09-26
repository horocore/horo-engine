#include "Horo/Destruction/PreFracturedImport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace Horo::Destruction {
    namespace {
        Assets::PreFracturedSourceNode Tetrahedron(std::string name = "HoroChunk_42__Wall") {
            return {.name = std::move(name),
                    .sourcePath = "nodes/7",
                    .parent = std::nullopt,
                    .geometryToWorld = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
                    .positions = {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
                    .triangleIndices = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3},
                    .triangleMaterials = {"Stone", "Stone", "Stone", "Stone"}};
        }

        Assets::PreFracturedSource Source() {
            return {.sourceName = "wall.fbx", .nodes = {Tetrahedron()}};
        }

        DestructionLimits Limits() {
            return GetDestructionTierProfile(DestructionFeatureTier::High).Value().limits;
        }

        void CheckError(const Result<PreFracturedCandidate> &result, const ErrorCodeDescriptor &descriptor, const std::string &path) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
            REQUIRE(result.ErrorValue().diagnostics.size() == 1);
            CHECK(result.ErrorValue().diagnostics.front().location.source == "wall.fbx");
            CHECK(result.ErrorValue().diagnostics.front().path == path);
        }
    }  // namespace

    TEST_CASE("Pre-fractured candidate has stable authored IDs and canonical hierarchy", "[destruction][import]") {
        auto source = Source();
        auto child = Tetrahedron("HoroChunk_7__Detail");
        child.parent = 0;
        child.sourcePath = "nodes/8";
        source.nodes.push_back(child);
        auto prepared = ValidatePreFracturedSource(source, Limits(), CancellationToken{});
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value().SchemaVersion() == CurrentPreFracturedImportSchemaVersion);
        REQUIRE(prepared.Value().Chunks().size() == 2);
        CHECK(prepared.Value().Chunks()[0].id.Value() == 7);
        CHECK(prepared.Value().Chunks()[0].parent.Value() == 42);
        CHECK(prepared.Value().Chunks()[1].id.Value() == 42);
        CHECK(prepared.Value().Chunks()[1].parent.Value() == 0);
        source.nodes[0].name = "HoroChunk_42__Renamed";
        std::swap(source.nodes[0], source.nodes[1]);
        source.nodes[0].parent.reset();
        source.nodes[1].parent = 0;
        auto renamed = ValidatePreFracturedSource(source, Limits(), CancellationToken{});
        REQUIRE(renamed.HasValue());
        CHECK(renamed.Value().Chunks()[0].id.Value() == 7);
        CHECK(renamed.Value().Chunks()[1].id.Value() == 42);
    }

    TEST_CASE("Pre-fractured validation rejects missing and duplicate chunk tokens with source paths", "[destruction][import]") {
        auto source = Source();
        source.nodes[0].name = "Wall";
        CheckError(ValidatePreFracturedSource(source, Limits(), CancellationToken{}), PreFracturedImportErrors::MissingChunk, "nodes/7");
        source.nodes[0].name = "HoroChunk_0";
        CheckError(ValidatePreFracturedSource(source, Limits(), CancellationToken{}), PreFracturedImportErrors::MissingChunk, "nodes/7");
        source.nodes[0].name = "HoroChunk_042";
        CheckError(ValidatePreFracturedSource(source, Limits(), CancellationToken{}), PreFracturedImportErrors::MissingChunk, "nodes/7");
        source = Source();
        auto duplicate = Tetrahedron("HoroChunk_42__Other");
        duplicate.sourcePath = "nodes/9";
        source.nodes.push_back(duplicate);
        CheckError(ValidatePreFracturedSource(source, Limits(), CancellationToken{}), PreFracturedImportErrors::DuplicateChunk, "nodes/9");
    }

    TEST_CASE("Pre-fractured validation rejects an unsupported normalized source schema", "[destruction][import]") {
        auto source = Source();
        source.schemaVersion = Assets::CurrentPreFracturedSourceSchemaVersion + 1U;
        CheckError(ValidatePreFracturedSource(source, Limits(), CancellationToken{}), PreFracturedImportErrors::InvalidSchema, "");
    }

    TEST_CASE("Pre-fractured validation rejects invalid geometry topology and materials", "[destruction][import]") {
        auto source = Source();
        source.nodes[0].positions[0][0] = std::numeric_limits<float>::quiet_NaN();
        CheckError(ValidatePreFracturedSource(source, Limits(), CancellationToken{}), PreFracturedImportErrors::NonFinite, "nodes/7");
        source = Source();
        source.nodes[0].geometryToWorld[0] = std::numeric_limits<double>::infinity();
        CheckError(ValidatePreFracturedSource(source, Limits(), CancellationToken{}), PreFracturedImportErrors::NonFinite, "nodes/7");
        source = Source();
        source.nodes[0].triangleIndices.resize(9);
        source.nodes[0].triangleMaterials.resize(3);
        CheckError(ValidatePreFracturedSource(source, Limits(), CancellationToken{}), PreFracturedImportErrors::InvalidTopology, "nodes/7");
        source = Source();
        source.nodes[0].triangleIndices.insert(source.nodes[0].triangleIndices.end(), {0, 2, 1});
        source.nodes[0].triangleMaterials.push_back("Stone");
        CheckError(ValidatePreFracturedSource(source, Limits(), CancellationToken{}), PreFracturedImportErrors::InvalidTopology, "nodes/7");
        source = Source();
        source.nodes[0].triangleMaterials[0].clear();
        CheckError(ValidatePreFracturedSource(source, Limits(), CancellationToken{}), PreFracturedImportErrors::InvalidMaterial, "nodes/7");
    }

    TEST_CASE("Pre-fractured validation enforces bounded hierarchy work and cancellation", "[destruction][import]") {
        auto source = Source();
        source.nodes[0].parent = 0;
        CheckError(ValidatePreFracturedSource(source, Limits(), CancellationToken{}), PreFracturedImportErrors::InvalidHierarchy,
                   "nodes/7");
        source = Source();
        auto limits = Limits();
        limits.maximumChunksPerDestructible = 1;
        limits.maximumActiveChunkBodies = 1;
        auto excess = Tetrahedron("HoroChunk_43");
        excess.sourcePath = "nodes/8";
        source.nodes.push_back(excess);
        CheckError(ValidatePreFracturedSource(source, limits, CancellationToken{}), PreFracturedImportErrors::LimitExceeded, "nodes/8");
        source = Source();
        limits = Limits();
        limits.maximumChunksPerDestructible = 1;
        limits.maximumActiveChunkBodies = 1;
        limits.maximumWorkItemsPerTransition = 1;
        CheckError(ValidatePreFracturedSource(source, limits, CancellationToken{}), PreFracturedImportErrors::LimitExceeded, "nodes/7");
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CheckError(ValidatePreFracturedSource(source, Limits(), cancelled.Token()), PreFracturedImportErrors::Cancelled, "nodes/7");
    }

    TEST_CASE("Pre-fractured validation admits an exact chunk and depth boundary", "[destruction][import]") {
        auto limits = Limits();
        limits.maximumChunksPerDestructible = 1;
        limits.maximumActiveChunkBodies = 1;
        limits.maximumHierarchyDepth = 1;
        CHECK(ValidatePreFracturedSource(Source(), limits, CancellationToken{}).HasValue());

        auto source = Source();
        auto child = Tetrahedron("HoroChunk_43");
        child.parent = 0;
        child.sourcePath = "nodes/8";
        source.nodes.push_back(child);
        limits.maximumChunksPerDestructible = 2;
        limits.maximumActiveChunkBodies = 2;
        CheckError(ValidatePreFracturedSource(source, limits, CancellationToken{}), PreFracturedImportErrors::InvalidHierarchy, "nodes/8");
    }

    TEST_CASE("Pre-fractured owner retains prior candidate on stale acceptance and shutdown", "[destruction][import]") {
        auto first = ValidatePreFracturedSource(Source(), Limits(), CancellationToken{});
        REQUIRE(first.HasValue());
        PreFracturedImportOwner owner;
        const auto captured = owner.Revision();
        const auto firstToken = owner.Token();
        REQUIRE(owner.Accept(first.Value(), captured).HasValue());
        CHECK(firstToken.IsCancellationRequested());
        CHECK_FALSE(owner.Token().IsCancellationRequested());
        const auto published = owner.Snapshot();
        REQUIRE(published != nullptr);
        auto stale = owner.Accept(first.Value(), captured);
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == PreFracturedImportErrors::StaleCandidate.code.Value());
        CHECK(owner.Snapshot() == published);
        owner.Shutdown();
        CHECK(owner.Token().IsCancellationRequested());
        auto stopped = owner.Accept(first.Value(), owner.Revision());
        REQUIRE(stopped.HasError());
        CHECK(stopped.ErrorValue().code.Value() == PreFracturedImportErrors::Shutdown.code.Value());
        CHECK(owner.Snapshot() == published);
    }

    TEST_CASE("Source invalidation cancels in-flight import without replacing accepted content", "[destruction][import]") {
        auto prepared = ValidatePreFracturedSource(Source(), Limits(), CancellationToken{});
        REQUIRE(prepared.HasValue());
        PreFracturedImportOwner owner;
        REQUIRE(owner.Accept(prepared.Value(), owner.Revision()).HasValue());
        const auto published = owner.Snapshot();
        const auto staleRevision = owner.Revision();
        const auto staleToken = owner.Token();
        REQUIRE(owner.Invalidate().HasValue());
        CHECK(staleToken.IsCancellationRequested());
        CHECK_FALSE(owner.Token().IsCancellationRequested());
        CHECK(owner.Snapshot() == published);
        auto stale = owner.Accept(prepared.Value(), staleRevision);
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == PreFracturedImportErrors::StaleCandidate.code.Value());
        owner.Shutdown();
        CHECK(owner.Invalidate().HasError());
    }

    TEST_CASE("FBX source parser feeds the same validated pre-fractured candidate", "[destruction][import][native]") {
        const auto path = std::filesystem::path{HORO_UFBX_TEST_DATA_DIR} / "maya_cube_7100_ascii.fbx";
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        std::string ascii{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        const std::string oldName = "pCube1";
        const std::string newName = "HoroChunk_19__Cube";
        for (std::size_t at = ascii.find(oldName); at != std::string::npos; at = ascii.find(oldName, at + newName.size()))
            ascii.replace(at, oldName.size(), newName);
        const std::vector<std::uint8_t> bytes(ascii.begin(), ascii.end());
        auto parsed = Assets::ParsePreFracturedFbx(bytes, "wall.fbx", CancellationToken{});
        REQUIRE(parsed.HasValue());
        REQUIRE(parsed.Value().nodes.size() == 1);
        CHECK(parsed.Value().nodes[0].name == newName);
        CHECK_FALSE(parsed.Value().nodes[0].triangleMaterials.empty());
        auto prepared = PreparePreFracturedFbx(bytes, "wall.fbx", Limits(), CancellationToken{});
        REQUIRE(prepared.HasValue());
        REQUIRE(prepared.Value().Chunks().size() == 1);
        CHECK(prepared.Value().Chunks()[0].id.Value() == 19);
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CHECK(PreparePreFracturedFbx(bytes, "wall.fbx", Limits(), cancelled.Token()).HasError());
    }

    TEST_CASE("FBX normalization preserves a mirrored source transform and closed winding", "[destruction][import][native]") {
        const auto path = std::filesystem::path{HORO_UFBX_TEST_DATA_DIR} / "maya_cube_7100_ascii.fbx";
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        std::string ascii{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        const auto meshName = ascii.find("pCube1");
        REQUIRE(meshName != std::string::npos);
        const std::string newName = "HoroChunk_21__Cube";
        for (auto at = meshName; at != std::string::npos; at = ascii.find("pCube1", at + newName.size()))
            ascii.replace(at, 6U, newName);
        const auto model = ascii.find("Model::HoroChunk_21__Cube");
        REQUIRE(model != std::string::npos);
        const auto properties = ascii.find("P: \"ScalingMax\"", model);
        REQUIRE(properties != std::string::npos);
        ascii.insert(properties, "P: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",-1,1,1\n\t\t\t");
        const std::vector<std::uint8_t> bytes(ascii.begin(), ascii.end());
        auto parsed = Assets::ParsePreFracturedFbx(bytes, "mirror.fbx", CancellationToken{});
        REQUIRE(parsed.HasValue());
        REQUIRE(parsed.Value().nodes.size() == 1);
        CHECK(parsed.Value().nodes[0].geometryToWorld[0] < 0.0);
        auto prepared = PreparePreFracturedFbx(bytes, "mirror.fbx", Limits(), CancellationToken{});
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value().Chunks()[0].id.Value() == 21);
    }

    TEST_CASE("Pre-fractured source parser accepts binary FBX and reports malformed input with source context",
              "[destruction][import][native]") {
        const auto path = std::filesystem::path{HORO_UFBX_TEST_DATA_DIR} / "blender_279_nested_meshes_7400_binary.fbx";
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        const auto parsed = Assets::ParsePreFracturedFbx(bytes, "binary.fbx", CancellationToken{});
        REQUIRE(parsed.HasValue());
        CHECK_FALSE(parsed.Value().nodes.empty());
        const std::array<std::uint8_t, 3> malformed{1, 2, 3};
        const auto rejected = Assets::ParsePreFracturedFbx(malformed, "broken.fbx", CancellationToken{});
        REQUIRE(rejected.HasError());
        REQUIRE(rejected.ErrorValue().diagnostics.size() == 1);
        CHECK(rejected.ErrorValue().diagnostics[0].location.source == "broken.fbx");
    }
}  // namespace Horo::Destruction
