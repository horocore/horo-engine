#include "ColorOutputPackageInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Render::Internal::ColorPackageDetail {
    namespace {
        /** @brief Requires exact byte identity/count/path recorded by the original producer. */
        void CheckFile(const Json &record, std::string_view path, const Bytes &bytes) {
            Require(Foundation::HasAllowedFields(record, {"file", "bytes", "sha256"}));
            const auto &file = record.at("file");
            Require(file.is_string());
            Require(file.get_ref<const std::string &>() == path && UInt(record.at("bytes")) == bytes.size() &&
                    Digest(record.at("sha256")) == Hash(bytes));
        }

        /** @brief Requires all fixed auxiliary sampler/padding policies before constructing their typed counterparts. */
        void CheckTablePolicy(const Json &row, std::size_t index) {
            Require(row.at("dimension") == "2D" && row.at("padding") == (index == 0 ? "none" : "RGB-unchanged-alpha-one"));
            Require(row.at("registerAssignment") == "wrapper-owned-no-register-rewrite");
            const auto &s = row.at("sampler");
            Require(Foundation::HasAllowedFields(s, {"filter", "addressU", "addressV", "mipFilter"}));
            Require(s.at("filter") == "nearest" && s.at("addressU") == "clamp-to-edge" && s.at("addressV") == "clamp-to-edge" &&
                    s.at("mipFilter") == "none");
        }

        /** @brief Reconstructs exact typed table metadata only after validating all original producer policy fields. */
        Render::ColorOutputTableInput Table(const Json &row, std::size_t index, const std::array<Bytes, 7> &files) {
            Require(Foundation::HasAllowedFields(row,
                                                 {"ordinal", "textureName", "samplerName", "dimension", "width", "height", "sourceChannels",
                                                  "format", "source", "upload", "padding", "sampler", "registerAssignment"}));
            CheckTablePolicy(row, index);
            const auto names = ColorOutputAnalyticFilenames();
            CheckFile(row.at("source"), names[3 + index * 2], files[3 + index * 2]);
            CheckFile(row.at("upload"), names[4 + index * 2], files[4 + index * 2]);
            Render::ColorOutputTableDescriptor d;
            d.ordinal = UInt(row.at("ordinal"));
            d.textureName = row.at("textureName").get<std::string>();
            d.samplerName = row.at("samplerName").get<std::string>();
            d.extent = {UInt(row.at("width")), UInt(row.at("height"))};
            d.sourceChannels = UInt(row.at("sourceChannels"));
            Require(row.at("format") == (index == 0 ? "R32Float" : "Rgba32Float"));
            d.format = index == 0 ? Render::RenderTextureFormat::R32Float : Render::RenderTextureFormat::Rgba32Float;
            d.sourceDigest = Hash(files[3 + index * 2]);
            d.uploadDigest = Hash(files[4 + index * 2]);
            return {d, std::as_bytes(std::span{files[3 + index * 2]}), std::as_bytes(std::span{files[4 + index * 2]})};
        }

        /** @brief Requires finite float32-sized direct reference channel triples. */
        void ReferenceColor(const Json &value) {
            Require(value.is_array() && value.size() == 3);
            for (const auto &channel : value) {
                Require(channel.is_number());
                const auto scalar = channel.get<double>();
                Require(std::isfinite(scalar) && std::abs(scalar) <= std::numeric_limits<float>::max());
            }
        }

        /** @brief Validates bounded direct-CPU corpus structure without claiming independent evaluation here. */
        void References(const Bytes &bytes) {
            const auto reference = Parse(bytes);
            Require(Foundation::HasAllowedFields(reference, {"schemaVersion", "profile", "processor", "scope", "cases"}));
            Require(UInt(reference.at("schemaVersion")) == 1 && reference.at("profile") == "aces2-sdr100-rec709-linear-v1");
            Require(reference.at("processor") == "direct-OCIO-CPU-optimization-none" &&
                    reference.at("scope") == "reference-values-only-no-GPU-execution-or-conformance-claim");
            const auto &cases = reference.at("cases");
            Require(cases.is_array() && !cases.empty() && cases.size() <= 256);
            for (const auto &row : cases) {
                Require(Foundation::HasAllowedFields(row, {"name", "input", "displayLinearRgb"}));
                Require(row.at("name").is_string() && !row.at("name").get_ref<const std::string &>().empty() &&
                        row.at("name").get_ref<const std::string &>().size() <= 128);
                ReferenceColor(row.at("input"));
                ReferenceColor(row.at("displayLinearRgb"));
            }
        }

        /** @brief Checks the exact pinned source/output option record through its canonical producer hash. */
        Render::ColorOutputProgramDescriptor Descriptor(const Json &manifest, const std::array<Bytes, 7> &files) {
            Require(Foundation::HasAllowedFields(manifest, {"schemaVersion", "kind", "profile", "options", "optionsSHA256", "provenance",
                                                            "shader", "textures", "referenceCorpus", "qualification"}));
            Require(UInt(manifest.at("schemaVersion")) == 1 && manifest.at("kind") == "horo-analytic-color-processor" &&
                    manifest.at("profile") == "aces2-sdr100-rec709-linear-v1");
            Require(manifest.at("qualification") == "provenance-and-integrity-only-no-cook-trust-native-or-ACES-conformance");
            const auto &provenance = manifest.at("provenance");
            Require(Foundation::HasAllowedFields(provenance,
                                                 {"ocio", "arrayBridge", "config", "processorCacheID", "cpuCacheID", "gpuCacheID"}));
            Require(Foundation::HasAllowedFields(provenance.at("config"), {"file", "sha256", "bytes"}));
            Require(UInt(provenance.at("config").at("bytes")) > 0 && UInt(provenance.at("config").at("bytes")) <= 128U * 1024U);
            Require(provenance.at("ocio").at("version") == "2.5.0");
            Require(provenance.at("config").at("file") == "cg-config-v4.0.0_aces-v2.0_ocio-v2.5.ocio");
            const auto options = Encode(manifest.at("options"));
            Require(Hash(options) == Digest(manifest.at("optionsSHA256")));
            CheckFile(manifest.at("shader"), "processor.hlsl", files[1]);
            CheckFile(manifest.at("referenceCorpus"), "reference.json", files[2]);
            return {.schemaVersion = 1,
                    .configurationRelease = 40000,
                    .ocioVersion = 20500,
                    .configurationDigest = Digest(provenance.at("config").at("sha256")),
                    .optionsDigest = Hash(options),
                    .sourceDigest = Hash(files[1]),
                    .referenceDigest = Hash(files[2])};
        }
    }  // namespace

    /** @copydoc DecodeAnalytic */
    std::shared_ptr<const Render::CookedColorOutputProgram> DecodeAnalytic(const std::array<Bytes, 7> &files) {
        const auto manifest = Parse(files[0]);
        auto descriptor = Descriptor(manifest, files);
        References(files[2]);
        Require(manifest.at("textures").is_array() && manifest.at("textures").size() == 2);
        const std::array tables{Table(manifest.at("textures")[0], 0, files), Table(manifest.at("textures")[1], 1, files)};
        const auto &source = files[1];
        const std::string_view code{reinterpret_cast<const char *>(source.data()), source.size()};
        return Take(Render::CookedColorOutputProgram::Create(descriptor, code, tables));
    }
}  // namespace Horo::Render::Internal::ColorPackageDetail

namespace Horo::Render::Internal {
    /** @copydoc ColorOutputAnalyticFilenames */
    std::span<const std::string_view, 7> ColorOutputAnalyticFilenames() noexcept {
        static constexpr std::array<std::string_view, 7> paths{"manifest.json",
                                                               "processor.hlsl",
                                                               "reference.json",
                                                               "textures/000.source.f32le",
                                                               "textures/000.upload.f32le",
                                                               "textures/001.source.f32le",
                                                               "textures/001.upload.f32le"};
        return paths;
    }

    /** @copydoc LoadColorOutputAnalyticBundle */
    Result<ColorOutputAnalyticBundle> LoadColorOutputAnalyticBundle(const std::filesystem::path &root) {
        try {
            ColorOutputAnalyticBundle bundle;
            const auto canonical = ColorPackageDetail::Root(root);
            const auto paths = ColorOutputAnalyticFilenames();
            for (std::size_t index = 0; index < paths.size(); ++index)
                bundle.files[index] = ColorPackageDetail::Read(canonical, paths[index], 128U * 1024U);
            bundle.program = ColorPackageDetail::DecodeAnalytic(bundle.files);
            return Result<ColorOutputAnalyticBundle>::Success(std::move(bundle));
        } catch (const ColorPackageDetail::Failure &failed) {
            return Result<ColorOutputAnalyticBundle>::Failure(failed.error);
        } catch (const std::exception &) {
            return Result<ColorOutputAnalyticBundle>::Failure(ColorPackageDetail::Invalid());
        }
    }
}  // namespace Horo::Render::Internal
