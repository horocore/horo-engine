/**
 * @copydoc FbxMeshImporter.h
 * Built-in FBX mesh importer with declarative settings.
 */

#include "FbxMeshImporter.h"

#include "../../../AssetErrors.h"
#include "../obj_mesh/ObjMeshPreviewProvider.h"
#include "FbxMeshParser.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Assets/MeshEditorPayload.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace Horo::Assets {
    namespace {
        void WriteLE32(std::vector<std::uint8_t> &out, std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
        }

        void WriteFloat(std::vector<std::uint8_t> &out, const float value) {
            WriteLE32(out, std::bit_cast<std::uint32_t>(value));
        }

        class FbxMeshImporter final : public IAssetImporter {
        public:
            [[nodiscard]] Result<PreparedAssetImport> Import(const AssetImportInput &input,
                                                             const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<PreparedAssetImport>::Failure(MakeError(ImportErrors::ImportCancelled));

                auto parsed = ParseFbxMesh(input.sourceBytes);
                if (parsed.HasError())
                    return Result<PreparedAssetImport>::Failure(parsed.ErrorValue());
                const FbxMeshGeometry &geometry = parsed.Value();
                if (geometry.positions.size() > std::numeric_limits<std::uint32_t>::max() ||
                    geometry.triangleIndices.size() > std::numeric_limits<std::uint32_t>::max()) {
                    return Result<PreparedAssetImport>::Failure(MakeError(ImportErrors::FbxMalformed));
                }

                PreparedAssetImport result;
                result.type = AssetTypeId::Parse("core.mesh").Value();
                auto &payload = result.editorPayload;

                std::array<float, 3> minimum = geometry.positions.front();
                std::array<float, 3> maximum = minimum;
                for (const auto &position : geometry.positions) {
                    for (std::size_t component = 0; component < position.size(); ++component) {
                        minimum[component] = std::min(minimum[component], position[component]);
                        maximum[component] = std::max(maximum[component], position[component]);
                    }
                }

                WriteLE32(payload, MeshEditorPayloadSchemaVersion);
                WriteLE32(payload, static_cast<std::uint32_t>(geometry.positions.size()));
                WriteLE32(payload, static_cast<std::uint32_t>(geometry.triangleIndices.size() / 3U));
                for (const float component : minimum)
                    WriteFloat(payload, component);
                for (const float component : maximum)
                    WriteFloat(payload, component);

                const std::uint64_t positionByteCount = geometry.positions.size() * 3U * sizeof(float);
                if (positionByteCount > std::numeric_limits<std::uint32_t>::max())
                    return Result<PreparedAssetImport>::Failure(MakeError(ImportErrors::FbxMalformed));
                WriteLE32(payload, static_cast<std::uint32_t>(positionByteCount));
                WriteLE32(payload, 0);
                WriteLE32(payload, 0);
                for (const auto &position : geometry.positions) {
                    for (const float component : position)
                        WriteFloat(payload, component);
                }
                WriteLE32(payload, static_cast<std::uint32_t>(geometry.triangleIndices.size()));
                for (const std::uint32_t index : geometry.triangleIndices)
                    WriteLE32(payload, index);
                return Result<PreparedAssetImport>::Success(std::move(result));
            }
        };
    }  // namespace

    std::shared_ptr<const IAssetImporter> CreateFbxMeshImporter() {
        return std::make_shared<const FbxMeshImporter>();
    }

    Result<void> RegisterFbxMeshImporter(AssetImporterCatalog &catalog) {
        auto mt = AssetTypeId::Parse("core.mesh");
        if (mt.HasError())
            return Result<void>::Failure(mt.ErrorValue());

        return catalog.Register(AssetImporterContribution{
            .contributionId = "horo.asset-importer.fbx-mesh",
            .packageId = "horo.builtin.assets",
            .moduleId = "horo.builtin.assets.importer.fbx",
            .moduleVersion = "1.0.0",
            .version = "1.0.0",
            .fileExtensions = {"fbx"},
            .assetTypes = {mt.Value()},
            .subfolderCategory = "Meshes",
            .settings =
                {
                    ImportSettingDescriptor{
                        .id = "coordinateSystem",
                        .labelKey = "asset_import.setting.coordinate_system",
                        .descriptionKey = "asset_import.setting.coordinate_system.description",
                        .kind = ImportSettingKind::Choice,
                        .defaultValue = std::string{"Y-up (engine)"},
                        .choices =
                            {
                                {.id = "yup", .labelKey = "asset_import.choice.y_up", .value = std::string{"Y-up (engine)"}},
                                {.id = "zup", .labelKey = "asset_import.choice.z_up", .value = std::string{"Z-up"}},
                            },
                        .includeInPresets = true,
                    },
                    ImportSettingDescriptor{
                        .id = "importMaterials",
                        .labelKey = "asset_import.setting.import_materials",
                        .descriptionKey = "asset_import.setting.import_materials.description",
                        .kind = ImportSettingKind::Boolean,
                        .defaultValue = true,
                        .includeInPresets = true,
                    },
                    ImportSettingDescriptor{
                        .id = "importAnimations",
                        .labelKey = "asset_import.setting.import_animations",
                        .descriptionKey = "asset_import.setting.import_animations.description",
                        .kind = ImportSettingKind::Boolean,
                        .defaultValue = false,
                        .includeInPresets = true,
                    },
                    ImportSettingDescriptor{
                        .id = "meshCompression",
                        .labelKey = "asset_import.setting.mesh_compression",
                        .descriptionKey = "asset_import.setting.mesh_compression.description",
                        .kind = ImportSettingKind::Choice,
                        .defaultValue = std::string{"None"},
                        .choices =
                            {
                                {.id = "none", .labelKey = "asset_import.choice.compression_none", .value = std::string{"None"}},
                                {.id = "draco", .labelKey = "asset_import.choice.compression_draco", .value = std::string{"Draco"}},
                                {.id = "meshopt", .labelKey = "asset_import.choice.compression_meshopt", .value = std::string{"MeshOpt"}},
                            },
                        .includeInPresets = true,
                    },
                },
            .builtIn = true,
            .strategy = CreateFbxMeshImporter(),
            .previewProvider = CreateBuiltinMeshPreviewProvider(BuiltinMeshPreviewView::NegativeX),
            .previewFallback = AssetPreviewFallback::Mesh,
        });
    }
}  // namespace Horo::Assets
