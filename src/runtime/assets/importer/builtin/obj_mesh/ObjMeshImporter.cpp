/**
 * @copydoc ObjMeshImporter.h
 */

#include "ObjMeshImporter.h"

#include "../../../AssetErrors.h"
#include "../fbx_mesh/FbxMeshImporter.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Assets/MeshEditorPayload.h"
#include "ObjMeshPreviewProvider.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {
    namespace {
        void WriteLE32(std::vector<std::uint8_t> &out, std::uint32_t value) {
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
        }

        void WriteFloat(std::vector<std::uint8_t> &out, const float value) {
            WriteLE32(out, std::bit_cast<std::uint32_t>(value));
        }

        struct Vec3 {
            float x = 0.0F;
            float y = 0.0F;
            float z = 0.0F;
        };

        struct FaceVertex {
            int positionIndex = -1;
            int texcoordIndex = -1;
            int normalIndex = -1;
        };

        struct ObjData {
            std::vector<Vec3> positions;
            std::vector<Vec3> texcoords;
            std::vector<Vec3> normals;
            std::vector<std::vector<FaceVertex>> faces;
            std::vector<std::string> warnings;
        };

        FaceVertex ParseFaceVertex(std::string_view token) {
            FaceVertex fv;
            const auto slash1 = token.find('/');
            if (slash1 == std::string_view::npos) {
                fv.positionIndex = std::stoi(std::string(token));
                return fv;
            }
            fv.positionIndex = std::stoi(std::string(token.substr(0, slash1)));
            const auto slash2 = token.find('/', slash1 + 1);
            if (slash2 == std::string_view::npos) {
                if (const std::string tcStr(token.substr(slash1 + 1)); !tcStr.empty())
                    fv.texcoordIndex = std::stoi(tcStr);
                return fv;
            }
            if (const std::string tcStr(token.substr(slash1 + 1, slash2 - slash1 - 1)); !tcStr.empty())
                fv.texcoordIndex = std::stoi(tcStr);
            if (const std::string nStr(token.substr(slash2 + 1)); !nStr.empty())
                fv.normalIndex = std::stoi(nStr);
            return fv;
        }

        void TrimLine(std::string &line) {
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
                line.erase(0, 1);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                line.pop_back();
        }

        void ParseObjFaceLine(std::istringstream &ls, ObjData &data) {
            std::vector<FaceVertex> face;
            std::string faceToken;
            while (ls >> faceToken)
                face.push_back(ParseFaceVertex(faceToken));
            if (face.size() < 3)
                return;
            if (face.size() == 3) {
                data.faces.push_back(std::move(face));
                return;
            }
            for (std::size_t i = 1; i + 1 < face.size(); ++i)
                data.faces.push_back({face[0], face[i], face[i + 1]});
        }

        Result<ObjData> ParseObj(std::string_view source) {
            ObjData data;
            std::istringstream stream{std::string(source)};
            std::string line;

            while (std::getline(stream, line)) {
                TrimLine(line);
                if (line.empty() || line[0] == '#')
                    continue;

                std::istringstream ls(line);
                std::string token;
                ls >> token;

                if (token == "v") {
                    Vec3 v;
                    if (ls >> v.x >> v.y >> v.z)
                        data.positions.push_back(v);
                } else if (token == "vt") {
                    Vec3 vt;
                    vt.z = 0.0f;
                    if (ls >> vt.x >> vt.y)
                        data.texcoords.push_back(vt);
                } else if (token == "vn") {
                    Vec3 vn;
                    if (ls >> vn.x >> vn.y >> vn.z)
                        data.normals.push_back(vn);
                } else if (token == "f") {
                    ParseObjFaceLine(ls, data);
                }
            }

            if (data.positions.empty())
                return Result<ObjData>::Failure(MakeError(ImportErrors::ObjNoVertices));
            return Result<ObjData>::Success(std::move(data));
        }

        class ObjMeshImporter final : public IAssetImporter {
        public:
            [[nodiscard]] Result<PreparedAssetImport> Import(const AssetImportInput &input,
                                                             const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<PreparedAssetImport>::Failure(MakeError(ImportErrors::ImportCancelled));

                std::string_view source(reinterpret_cast<const char *>(input.sourceBytes.data()), input.sourceBytes.size());
                auto objResult = ParseObj(source);
                if (objResult.HasError())
                    return Result<PreparedAssetImport>::Failure(objResult.ErrorValue());

                const auto &obj = objResult.Value();
                PreparedAssetImport result;
                result.type = AssetTypeId::Parse("core.mesh").Value();

                float minX = obj.positions[0].x;
                float minY = obj.positions[0].y;
                float minZ = obj.positions[0].z;
                float maxX = minX;
                float maxY = minY;
                float maxZ = minZ;
                for (const auto &p : obj.positions) {
                    minX = std::min(minX, p.x);
                    maxX = std::max(maxX, p.x);
                    minY = std::min(minY, p.y);
                    maxY = std::max(maxY, p.y);
                    minZ = std::min(minZ, p.z);
                    maxZ = std::max(maxZ, p.z);
                }

                auto &payload = result.editorPayload;
                WriteLE32(payload, MeshEditorPayloadSchemaVersion);
                WriteLE32(payload, static_cast<std::uint32_t>(obj.positions.size()));
                WriteLE32(payload, static_cast<std::uint32_t>(obj.faces.size()));
                WriteFloat(payload, minX);
                WriteFloat(payload, minY);
                WriteFloat(payload, minZ);
                WriteFloat(payload, maxX);
                WriteFloat(payload, maxY);
                WriteFloat(payload, maxZ);

                const auto posSize = static_cast<std::uint32_t>(obj.positions.size() * 3 * sizeof(float));
                const auto tcSize = static_cast<std::uint32_t>(obj.texcoords.size() * 2 * sizeof(float));
                const auto nSize = static_cast<std::uint32_t>(obj.normals.size() * 3 * sizeof(float));
                WriteLE32(payload, posSize);
                WriteLE32(payload, tcSize);
                WriteLE32(payload, nSize);

                for (const auto &p : obj.positions) {
                    WriteFloat(payload, p.x);
                    WriteFloat(payload, p.y);
                    WriteFloat(payload, p.z);
                }
                for (const auto &t : obj.texcoords) {
                    WriteFloat(payload, t.x);
                    WriteFloat(payload, t.y);
                }
                for (const auto &n : obj.normals) {
                    WriteFloat(payload, n.x);
                    WriteFloat(payload, n.y);
                    WriteFloat(payload, n.z);
                }

                std::vector<std::uint32_t> triangleIndices;
                triangleIndices.reserve(obj.faces.size() * 3U);
                const auto resolvePositionIndex = [&obj](const int index) -> std::optional<std::uint32_t> {
                    if (index == 0)
                        return std::nullopt;
                    const std::int64_t resolved =
                        index > 0 ? static_cast<std::int64_t>(index - 1) : static_cast<std::int64_t>(obj.positions.size()) + index;
                    if (resolved < 0 || resolved >= static_cast<std::int64_t>(obj.positions.size()))
                        return std::nullopt;
                    return static_cast<std::uint32_t>(resolved);
                };
                for (const auto &face : obj.faces) {
                    if (face.size() != 3)
                        continue;
                    const auto a = resolvePositionIndex(face[0].positionIndex);
                    const auto b = resolvePositionIndex(face[1].positionIndex);
                    const auto c = resolvePositionIndex(face[2].positionIndex);
                    if (!a.has_value() || !b.has_value() || !c.has_value())
                        continue;
                    triangleIndices.push_back(*a);
                    triangleIndices.push_back(*b);
                    triangleIndices.push_back(*c);
                }
                WriteLE32(payload, static_cast<std::uint32_t>(triangleIndices.size()));
                for (const std::uint32_t index : triangleIndices)
                    WriteLE32(payload, index);

                return Result<PreparedAssetImport>::Success(std::move(result));
            }
        };
    }  // namespace

    std::shared_ptr<const IAssetImporter> CreateObjMeshImporter() {
        return std::make_shared<const ObjMeshImporter>();
    }

    Result<void> RegisterObjMeshImporter(AssetImporterCatalog &catalog) {
        auto meshType = AssetTypeId::Parse("core.mesh");
        if (meshType.HasError())
            return Result<void>::Failure(meshType.ErrorValue());

        return catalog.Register(AssetImporterContribution{
            .contributionId = "horo.asset-importer.obj-mesh",
            .packageId = "horo.builtin.assets",
            .moduleId = "horo.builtin.assets.importer.obj",
            .moduleVersion = "1.0.0",
            .version = "1.0.0",
            .fileExtensions = {"obj"},
            .assetTypes = {meshType.Value()},
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
                        .id = "unitScale",
                        .labelKey = "asset_import.setting.unit_scale",
                        .descriptionKey = "asset_import.setting.unit_scale.description",
                        .kind = ImportSettingKind::Float,
                        .defaultValue = 1.0,
                        .minimum = 0.001,
                        .maximum = 1000.0,
                        .includeInPresets = true,
                    },
                    ImportSettingDescriptor{
                        .id = "importNormals",
                        .labelKey = "asset_import.setting.import_normals",
                        .descriptionKey = "asset_import.setting.import_normals.description",
                        .kind = ImportSettingKind::Choice,
                        .defaultValue = std::string{"Import from source"},
                        .choices =
                            {
                                {.id = "import", .labelKey = "asset_import.choice.normals_source", .value = std::string{"Import from source"}},
                                {.id = "smooth", .labelKey = "asset_import.choice.normals_smooth", .value = std::string{"Calculate smooth"}},
                                {.id = "flat", .labelKey = "asset_import.choice.normals_flat", .value = std::string{"Calculate flat"}},
                            },
                        .includeInPresets = true,
                    },
                },
            .builtIn = true,
            .strategy = CreateObjMeshImporter(),
            .previewProvider = CreateBuiltinMeshPreviewProvider(BuiltinMeshPreviewView::Isometric),
            .previewFallback = AssetPreviewFallback::Mesh,
        });
    }

    Result<void> RegisterAllBuiltinImporters(AssetImporterCatalog &catalog) {
        auto r = RegisterObjMeshImporter(catalog);
        if (r.HasError())
            return r;
        r = RegisterFbxMeshImporter(catalog);
        return r;
    }
}  // namespace Horo::Assets
