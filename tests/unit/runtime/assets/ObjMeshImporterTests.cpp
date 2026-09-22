#include "Horo/Assets/AssetImporter.h"
#include "Horo/Assets/MeshEditorPayload.h"
#include "Horo/Foundation/CancellationToken.h"
#include "ObjMeshImporter.h"

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    AssetTypeId Type(const std::string_view value) {
        auto parsed = AssetTypeId::Parse(value);
        REQUIRE(parsed.HasValue());
        return parsed.Value();
    }

    /** @brief Helper: import a string as OBJ and return the editor payload bytes. */
    Result<std::vector<std::uint8_t>> ImportString(const char *objSource) {
        AssetImporterCatalog catalog;
        auto regResult = RegisterObjMeshImporter(catalog);
        REQUIRE(regResult.HasValue());
        auto snapshot = catalog.Publish();
        REQUIRE(snapshot.HasValue());

        auto *strategy = snapshot.Value()->FindByExtension("obj");
        REQUIRE(strategy != nullptr);

        std::string_view sv(objSource);
        AssetImportInput input{
            .sourceBytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(sv.data()), sv.size()),
            .sourceExtension = "obj",
            .settings = {},
        };

        CancellationToken cancellation;
        auto result = strategy->Import(input, cancellation);
        if (result.HasError())
            return Result<std::vector<std::uint8_t>>::Failure(result.ErrorValue());

        return Result<std::vector<std::uint8_t>>::Success(result.Value().editorPayload);
    }

    /** @brief Reads a u32 LE from a byte span at offset. */
    std::uint32_t ReadLE32(std::span<const std::uint8_t> bytes, std::size_t offset) {
        return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    }

    void AppendLE32(std::vector<std::uint8_t> &bytes, const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }

    void AppendFloat(std::vector<std::uint8_t> &bytes, const float value) {
        AppendLE32(bytes, std::bit_cast<std::uint32_t>(value));
    }

}  // namespace

TEST_CASE("OBJ importer parses a minimal triangle", "[native]") {
    const char *obj = R"(
v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 3
)";

    auto result = ImportString(obj);
    REQUIRE(result.HasValue());

    auto &payload = result.Value();
    REQUIRE(payload.size() > 28);  // header at minimum

    // Schema version 2 carries preview topology after the vertex streams.
    REQUIRE(ReadLE32(payload, 0) == MeshEditorPayloadSchemaVersion);

    // Vertex count = 3
    REQUIRE(ReadLE32(payload, 4) == 3);

    // Face count = 1
    REQUIRE(ReadLE32(payload, 8) == 1);

    // Three topology indices follow the 48-byte header and 36-byte position stream.
    REQUIRE(ReadLE32(payload, 84) == 3);
    REQUIRE(ReadLE32(payload, 88) == 0);
    REQUIRE(ReadLE32(payload, 92) == 1);
    REQUIRE(ReadLE32(payload, 96) == 2);
}

TEST_CASE("OBJ importer registers with catalog", "[native]") {
    AssetImporterCatalog catalog;

    auto result = RegisterObjMeshImporter(catalog);
    REQUIRE(result.HasValue());

    auto snapshot = catalog.Publish();
    REQUIRE(snapshot.HasValue());

    auto *strategy = snapshot.Value()->FindByExtension("obj");
    REQUIRE(strategy != nullptr);

    auto *contrib = snapshot.Value()->FindById("horo.asset-importer.obj-mesh");
    REQUIRE(contrib != nullptr);
    REQUIRE(contrib->builtIn);
    REQUIRE(contrib->packageId == "horo.builtin.assets");
}

TEST_CASE("OBJ importer rejects empty file", "[native]") {
    auto result = ImportString("");
    REQUIRE(result.HasError());
}

TEST_CASE("OBJ importer handles normals and texcoords", "[native]") {
    const char *obj = R"(
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
f 1/1/1 2/2/1 3/3/1
)";

    auto result = ImportString(obj);
    REQUIRE(result.HasValue());

    auto &payload = result.Value();
    REQUIRE(ReadLE32(payload, 0) == MeshEditorPayloadSchemaVersion);
    REQUIRE(ReadLE32(payload, 4) == 3);  // vertices
    REQUIRE(ReadLE32(payload, 8) == 1);  // faces
}

TEST_CASE("OBJ importer contribution produces a valid RGBA preview", "[native]") {
    const char *obj = R"(
v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 3
)";
    auto imported = ImportString(obj);
    REQUIRE(imported.HasValue());

    AssetImporterCatalog catalog;
    REQUIRE((RegisterObjMeshImporter(catalog).HasValue()));
    auto snapshot = catalog.Publish();
    REQUIRE(snapshot.HasValue());
    const AssetImporterContribution *contribution = snapshot.Value()->FindPreviewContribution(Type("core.mesh"));
    REQUIRE(contribution != nullptr);
    REQUIRE(contribution->previewProvider != nullptr);
    auto preview = contribution->previewProvider->GeneratePreview(
        AssetPreviewInput{
            .editorPayload = imported.Value(),
            .absoluteAssetPath = "/tmp/triangle.horoasset",
            .assetType = Type("core.mesh"),
            .width = 64,
            .height = 64,
        },
        CancellationToken{});
    REQUIRE(preview.HasValue());
    REQUIRE(preview.Value().IsValid());
    REQUIRE((std::ranges::any_of(preview.Value().pixels, [](const std::uint8_t value) {
        return value != 0;
    })));
}

TEST_CASE("OBJ preview provider renders topology-free legacy payloads", "[native]") {
    std::vector<std::uint8_t> payload;
    AppendLE32(payload, MeshEditorPayloadSchemaVersion);
    AppendLE32(payload, 2);
    AppendLE32(payload, 1);
    for (const float bound : {-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F})
        AppendFloat(payload, bound);
    AppendLE32(payload, 24);
    AppendLE32(payload, 0);
    AppendLE32(payload, 0);
    for (const float component : {-1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F})
        AppendFloat(payload, component);

    AssetImporterCatalog catalog;
    REQUIRE((RegisterObjMeshImporter(catalog).HasValue()));
    const auto snapshot = catalog.Publish();
    REQUIRE(snapshot.HasValue());
    const AssetImporterContribution *contribution = snapshot.Value()->FindPreviewContribution(Type("core.mesh"));
    REQUIRE(contribution != nullptr);
    REQUIRE(contribution->previewProvider != nullptr);

    const auto preview = contribution->previewProvider->GeneratePreview(
        AssetPreviewInput{
            .editorPayload = payload,
            .absoluteAssetPath = "/tmp/legacy.horoasset",
            .assetType = Type("core.mesh"),
            .width = 64,
            .height = 64,
        },
        CancellationToken{});
    REQUIRE(preview.HasValue());
    CHECK((std::ranges::any_of(preview.Value().pixels, [](const std::uint8_t value) {
        return value != 0;
    })));
}

TEST_CASE("OBJ importer produces deterministic output", "[native]") {
    const char *obj = R"(
v 1 2 3
f 1 1 1
)";

    auto result1 = ImportString(obj);
    auto result2 = ImportString(obj);
    REQUIRE(result1.HasValue());
    REQUIRE(result2.HasValue());
    REQUIRE(result1.Value() == result2.Value());
}

TEST_CASE("OBJ importer triangulates quads", "[native]") {
    const char *obj = R"(
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
f 1 2 3 4
)";

    auto result = ImportString(obj);
    REQUIRE(result.HasValue());

    // A quad triangulates to 2 faces
    REQUIRE(ReadLE32(result.Value(), 8) == 2);
}
