#include "ColorOutputPackageInternal.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"

namespace Horo::Render::Internal {
    /** @copydoc ColorOutputMetalCookTarget */
    ShaderTargetRequirement ColorOutputMetalCookTarget() {
        return {.backend = ShaderTargetBackend::Metal,
                .payloadFormat = ShaderPayloadFormat::MetalLibrary24,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 16,
                .maximumInlineConstantBytes = 128,
                .supportsStorageResources = true};
    }

    /** @copydoc ColorOutputCookPrefixes */
    std::span<const std::string_view, 2> ColorOutputCookPrefixes() noexcept {
        static constexpr std::array<std::string_view, 2> paths{"shaders/runtime/color-output-scene-metal",
                                                               "shaders/runtime/color-output-encode-metal"};
        return paths;
    }

    /** @copydoc ColorOutputCookPrefix */
    std::string_view ColorOutputCookPrefix(std::size_t index) noexcept {
        if (index < 2)
            return ColorOutputCookPrefixes()[index];
        return index == 2 ? "shaders/runtime/color-output-look-metal" : std::string_view{};
    }

    /** @copydoc ColorOutputSourceFilenames */
    std::span<const std::string_view, 5> ColorOutputSourceFilenames() noexcept {
        static constexpr std::array<std::string_view, 5> paths{"sources/color_output.combined.hlsl", "sources/color_output.wrapper.hlsl",
                                                               "sources/color_encode_sdr.hlsl", "sources/color_output_look.combined.hlsl",
                                                               "sources/color_output_look.wrapper.hlsl"};
        return paths;
    }

    namespace {
        using namespace ColorPackageDetail;

        /** @brief Requires real standard native stage envelopes and selected canonical entries, never synthetic metadata substitution. */
        void NativeStages(const CompiledShaderArtifact &artifact) {
            const auto package =
                Take(DecodeShaderArtifactStages(artifact.payload, artifact.backend, artifact.payloadFormat, 16U * 1024U * 1024U));
            Require(package.stageCount == 2);
            Require(package.stages[0].stage == ShaderStage::Vertex && package.stages[0].entryPoint == "VSMain" &&
                    !package.stages[0].bytes.empty());
            Require(package.stages[1].stage == ShaderStage::Fragment && package.stages[1].entryPoint == "PSMain" &&
                    !package.stages[1].bytes.empty());
        }

        /** @brief Decodes one exact product artifact against the canonical stage recipe. */
        CompiledShaderArtifact LoadProgram(const std::filesystem::path &root, std::size_t index) {
            const auto prefix = std::string{ColorOutputCookPrefix(index)};
            auto payload = Read(root, prefix + ".horoshdr", 16U * 1024U * 1024U);
            const auto metadata = Read(root, prefix + ".reflection.json", 16U * 1024U * 1024U);
            const auto target = ColorOutputMetalCookTarget();
            const auto manifest = Take(MakeColorOutputShaderManifest(static_cast<ColorOutputShaderStage>(index), target));
            auto reflection = Take(DecodeReflectedShaderArtifact(manifest, target, payload, metadata));
            CompiledShaderArtifact artifact{.backend = target.backend,
                                            .payloadFormat = target.payloadFormat,
                                            .artifactKey = reflection.compiledArtifactKey,
                                            .payload = std::move(payload),
                                            .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(reflection))};
            Take(ValidateReflectedShaderArtifact(artifact, *artifact.reflection));
            NativeStages(artifact);
            return artifact;
        }

        /** @brief Requires original analytic bytes concatenated exactly once with the selected wrapper. */
        void CheckAssembly(const ColorOutputAnalyticBundle &analytic, const Bytes &combined, const Bytes &wrapper) {
            auto expected = analytic.files[1];
            expected.push_back('\n');
            expected.insert(expected.end(), wrapper.begin(), wrapper.end());
            Require(expected == combined);
        }

        /** @brief Produces the one canonical actual program identity row for both publication and read-only validation. */
        Json ProgramRow(std::size_t index, const Bytes &source, const CompiledShaderArtifact &artifact) {
            Require(artifact.reflection != nullptr);
            Take(ValidateReflectedShaderArtifact(artifact, *artifact.reflection));
            return {{"prefix", ColorOutputCookPrefix(index)},
                    {"sourceDigest", FormatSha256(Hash(source))},
                    {"compiledKey", FormatSha256(artifact.artifactKey)},
                    {"cookedKey", FormatSha256(artifact.reflection->cookedArtifactKey)}};
        }

        /** @brief Constructs the complete bounded schema1 baseline or schema2 look-enabled generation identity. */
        Json Generation(const ColorOutputAnalyticBundle &analytic, std::span<const CompiledShaderArtifact> programs,
                        std::span<const Bytes> sources) {
            Require(analytic.program != nullptr);
            Require(programs.size() == 2 || programs.size() == 3);
            Require(sources.size() == (programs.size() == 2 ? 3u : 5u));
            CheckAssembly(analytic, sources[0], sources[1]);
            auto rows = Json::array();
            constexpr std::array<std::size_t, 3> sourceIndices{0, 2, 3};
            for (std::size_t i = 0; i < programs.size(); ++i)
                rows.push_back(ProgramRow(i, sources[sourceIndices[i]], programs[i]));
            const auto &d = analytic.program->Descriptor();
            Json value{{"schemaVersion", programs.size() == 2 ? 1 : 2},
                       {"kind", "horo-color-output-product"},
                       {"profile", "aces2-sdr100-rec709-linear-v1"},
                       {"analyticManifestDigest", FormatSha256(Hash(analytic.files[0]))},
                       {"processorSourceDigest", FormatSha256(d.sourceDigest)},
                       {"referenceDigest", FormatSha256(d.referenceDigest)},
                       {"optionsDigest", FormatSha256(d.optionsDigest)},
                       {"wrapperDigest", FormatSha256(Hash(sources[1]))},
                       {"encodeSourceDigest", FormatSha256(Hash(sources[2]))},
                       {"programs", rows}};
            if (programs.size() == 3) {
                CheckAssembly(analytic, sources[3], sources[4]);
                value["lookWrapperDigest"] = FormatSha256(Hash(sources[4]));
            }
            return value;
        }

        /** @brief Reads only a recognized version before any optional native artifact is touched. */
        std::size_t ProgramCount(const Json &value) {
            const auto schema = UInt(value.at("schemaVersion"));
            Require(schema == 1 || schema == 2);
            const auto count = schema == 1 ? 2u : 3u;
            Require(value.at("programs").is_array() && value.at("programs").size() == count);
            return count;
        }
    }  // namespace

    /** @copydoc EncodeColorOutputGeneration */
    Result<std::vector<std::uint8_t>> EncodeColorOutputGeneration(const ColorOutputAnalyticBundle &analytic,
                                                                  std::span<const CompiledShaderArtifact> programs,
                                                                  std::span<const std::vector<std::uint8_t>> sources) {
        try {
            return Result<std::vector<std::uint8_t>>::Success(Encode(Generation(analytic, programs, sources)));
        } catch (const Failure &f) {
            return Result<std::vector<std::uint8_t>>::Failure(f.error);
        } catch (const std::exception &) {
            return Result<std::vector<std::uint8_t>>::Failure(Invalid());
        }
    }

    /** @copydoc LoadColorOutputCookPackage */
    Result<ColorOutputCookPackage> LoadColorOutputCookPackage(const std::filesystem::path &root) {
        try {
            const auto canonical = Root(root);
            const auto generation = Parse(Read(canonical, "color-output-generation.json", 64U * 1024U));
            ColorOutputCookPackage package;
            package.programCount = ProgramCount(generation);
            package.analytic = Take(LoadColorOutputAnalyticBundle(canonical / "analytic"));
            for (std::size_t i = 0; i < package.programCount; ++i)
                package.programs[i] = LoadProgram(canonical, i);
            std::vector<Bytes> sources(package.programCount == 2 ? 3 : 5);
            for (std::size_t i = 0; i < sources.size(); ++i)
                sources[i] = Read(canonical, ColorOutputSourceFilenames()[i], 1024U * 1024U);
            const auto expected = Generation(package.analytic, std::span{package.programs}.first(package.programCount), sources);
            Require(generation == expected);
            return Result<ColorOutputCookPackage>::Success(std::move(package));
        } catch (const Failure &f) {
            return Result<ColorOutputCookPackage>::Failure(f.error);
        } catch (const std::exception &) {
            return Result<ColorOutputCookPackage>::Failure(Invalid());
        }
    }
}  // namespace Horo::Render::Internal
