#include "ClusteredLightingCook.h"
#include "EditorClusteredLightingAssets.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "JsonUtils.h"

#include <bit>
#include <format>
#include <fstream>

namespace Horo::Editor::Offline {
    namespace {
        using namespace Render;
        using Application::Internal::AuthenticatedMetalCookConfiguration;
        using Foundation::Json;

        constexpr std::string_view PrivateCandidatePrefix{".clustered-lighting-package-"};

        bool PrivateCandidateName(std::string_view name) {
            if (name.size() < PrivateCandidatePrefix.size())
                return false;
            for (std::size_t index = 0; index < PrivateCandidatePrefix.size(); ++index) {
                auto character = name[index];
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character + ('a' - 'A'));
                if (character != PrivateCandidatePrefix[index])
                    return false;
            }
            return true;
        }

        Result<void> IoFailure() {
            return Result<void>::Failure(
                MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed,
                          "Use a new absolute package directory under an existing exclusively owned output parent."));
        }

        Result<void> ScratchFailure() {
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed,
                                                   "Use an existing independently owned absolute scratch directory "
                                                   "and a separate new output directory outside it."));
        }

        Result<std::filesystem::path> ExistingScratch(const std::filesystem::path &scratch) {
            using Path = Result<std::filesystem::path>;
            if (!scratch.is_absolute())
                return Path::Failure(ScratchFailure().ErrorValue());
            std::error_code error;
            const auto canonical = std::filesystem::canonical(scratch, error);
            if (error)
                return Path::Failure(ScratchFailure().ErrorValue());
            if (!std::filesystem::is_directory(canonical, error) || error)
                return Path::Failure(ScratchFailure().ErrorValue());
            return Path::Success(canonical);
        }

        Result<void> OutsideScratch(std::filesystem::path ancestor, const std::filesystem::path &scratch) {
            for (;;) {
                std::error_code error;
                const bool same = std::filesystem::equivalent(ancestor, scratch, error);
                if (error || same)
                    return ScratchFailure();
                const auto parent = ancestor.parent_path();
                if (parent == ancestor)
                    return Result<void>::Success();
                ancestor = parent;
            }
        }

        Result<void> Write(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream.write(std::bit_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            stream.close();
            return stream ? Result<void>::Success() : IoFailure();
        }

        Result<void> DestinationAbsent(const std::filesystem::path &destination) {
            if (!destination.is_absolute() || destination.filename().empty())
                return IoFailure();
            std::error_code error;
            const auto status = std::filesystem::symlink_status(destination, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return IoFailure();
            if (std::filesystem::exists(status) || std::filesystem::is_symlink(status))
                return IoFailure();
            return Result<void>::Success();
        }

        /** @brief Owns only its successfully created staging directory; publication transfers that ownership. */
        class Candidate final {
        public:
            std::filesystem::path path;
            bool owned{};
            Candidate() = default;
            Candidate(const Candidate &) = delete;
            Candidate &operator=(const Candidate &) = delete;

            ~Candidate() {
                if (owned) {
                    std::error_code ignored;
                    std::filesystem::remove_all(path, ignored);
                }
            }

            Result<void> Claim(const std::filesystem::path &destination) {
                if (auto valid = ValidateClusteredLightingCookDestination(destination); valid.HasError())
                    return valid;
                std::error_code error;
                for (unsigned attempt = 0; attempt < 1024; ++attempt) {
                    path = destination.parent_path() / std::format("{}{}", PrivateCandidatePrefix, attempt);
                    owned = std::filesystem::create_directory(path, error);
                    if (owned)
                        return Result<void>::Success();
                    if (error && error != std::errc::file_exists)
                        return IoFailure();
                    error.clear();
                }
                return IoFailure();
            }

            Result<void> Publish(const std::filesystem::path &destination) {
                if (auto absent = DestinationAbsent(destination); absent.HasError())
                    return absent;
                std::error_code error;
                std::filesystem::rename(path, destination, error);
                if (error)
                    return IoFailure();
                owned = false;
                return Result<void>::Success();
            }
        };

        Result<void> WriteRecorded(const std::filesystem::path &root, const std::string &relative, std::span<const std::uint8_t> bytes,
                                   Json &files) {
            if (auto written = Write(root / relative, bytes); written.HasError())
                return written;
            files[relative] = FormatSha256(ComputeSha256(std::as_bytes(bytes)));
            return Result<void>::Success();
        }

        Result<void> WriteProgram(const std::filesystem::path &root, std::string_view prefix, const ShaderCompilationBatch &batch,
                                  Json &files) {
            if (batch.artifacts.size() != 1 || batch.sourceDigest == Sha256Digest{})
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            const auto &artifact = batch.artifacts.front();
            if (!artifact.reflection || artifact.payload.empty() || artifact.payload.size() > 16U * 1024U * 1024U)
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            if (auto bound = ValidateReflectedShaderArtifact(artifact, *artifact.reflection); bound.HasError())
                return bound;
            const auto metadata = EncodeReflectedShaderArtifact(*artifact.reflection);
            if (metadata.HasError())
                return Result<void>::Failure(metadata.ErrorValue());
            if (auto payload = WriteRecorded(root, std::string{prefix} + ".horoshdr", artifact.payload, files); payload.HasError())
                return payload;
            return WriteRecorded(root, std::string{prefix} + ".reflection.json", metadata.Value(), files);
        }

        Json ToolRows(const AuthenticatedMetalCookConfiguration &host) {
            auto tools = Json::array();
            for (const auto &tool : host.configuration.tools)
                tools.push_back({{"role", tool.identity.tool},
                                 {"release", tool.identity.release},
                                 {"buildDigest", FormatSha256(tool.identity.buildDigest)},
                                 {"approvedExecutableDigest", FormatSha256(tool.executableDigest)}});
            return tools;
        }

        Json ProgramRows(const ClusteredLightingCookFamily &family) {
            auto programs = Json::array();
            const auto prefixes = PackagedMetalClusteredLightingPrefixes();
            for (std::size_t index = 0; index < prefixes.size(); ++index) {
                const auto &artifact = family.compiled[index].artifacts.front();
                programs.push_back({{"prefix", prefixes[index]},
                                    {"sourceIdentity", family.manifests[index].sourceIdentity},
                                    {"sourceRevision", family.manifests[index].sourceRevision},
                                    {"sourceDigest", FormatSha256(family.compiled[index].sourceDigest)},
                                    {"compiledKey", FormatSha256(artifact.artifactKey)},
                                    {"payloadDigest", FormatSha256(artifact.reflection->payloadDigest)},
                                    {"cookedKey", FormatSha256(artifact.reflection->cookedArtifactKey)}});
            }
            return programs;
        }

        Result<void> WriteProvenance(const std::filesystem::path &root, const ClusteredLightingCookFamily &family,
                                     const AuthenticatedMetalCookConfiguration &host, Json files) {
            if (!host.configuration.verifiedCatalog || !host.catalogBytes || !host.signatureBytes)
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolNotApproved));
            if (auto catalog = WriteRecorded(root, "shader-host-catalog.json", *host.catalogBytes, files); catalog.HasError())
                return catalog;
            if (auto signature = WriteRecorded(root, "shader-host-catalog.signature.json", *host.signatureBytes, files);
                signature.HasError())
                return signature;
            const Json provenance{{"schema", 1},
                                  {"kind", "informational-integrity"},
                                  {"host", host.configuration.hostPlatform},
                                  {"catalogDigest", FormatSha256(ComputeSha256(std::as_bytes(std::span{*host.catalogBytes})))},
                                  {"tools", ToolRows(host)},
                                  {"programs", ProgramRows(family)},
                                  {"files", std::move(files)}};
            const auto encoded = provenance.dump(2) + "\n";
            if (encoded.size() > 65536)
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            return Write(root / "clustered-cook-provenance.json", {std::bit_cast<const std::uint8_t *>(encoded.data()), encoded.size()});
        }

        Result<Json> WriteFamily(const std::filesystem::path &root, const ClusteredLightingCookFamily &family) {
            using Files = Result<Json>;
            std::error_code error;
            std::filesystem::create_directories(root / "shaders/editor", error);
            if (error)
                return Files::Failure(IoFailure().ErrorValue());
            std::filesystem::create_directories(root / "shaders/runtime", error);
            if (error)
                return Files::Failure(IoFailure().ErrorValue());
            auto files = Json::object();
            const auto prefixes = PackagedMetalClusteredLightingPrefixes();
            for (std::size_t index = 0; index < prefixes.size(); ++index)
                if (auto written = WriteProgram(root, prefixes[index], family.compiled[index], files); written.HasError())
                    return Files::Failure(written.ErrorValue());
            return Files::Success(std::move(files));
        }
    }  // namespace

    Result<void> ValidateClusteredLightingCookDestination(const std::filesystem::path &destination) {
        if (PrivateCandidateName(destination.filename().string()))
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed,
                                                   "The .clustered-lighting-package- filename namespace is reserved "
                                                   "for private candidates; choose another output name."));
        if (auto absent = DestinationAbsent(destination); absent.HasError())
            return absent;
        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::symlink_status(destination.parent_path(), error)) || error)
            return IoFailure();
        return Result<void>::Success();
    }

    Result<void> ValidateClusteredLightingCookPaths(const std::filesystem::path &destination, const std::filesystem::path &scratch) {
        if (auto valid = ValidateClusteredLightingCookDestination(destination); valid.HasError())
            return valid;
        const auto existing = ExistingScratch(scratch);
        if (existing.HasError())
            return Result<void>::Failure(existing.ErrorValue());
        std::error_code error;
        const auto parent = std::filesystem::canonical(destination.parent_path(), error);
        if (error)
            return ScratchFailure();
        return OutsideScratch(parent, existing.Value());
    }

    Result<void> PublishClusteredLightingCook(const std::filesystem::path &destination, const ClusteredLightingCookFamily &family,
                                              const Application::Internal::AuthenticatedMetalCookConfiguration &host) {
        Candidate candidate;
        if (auto claimed = candidate.Claim(destination); claimed.HasError())
            return claimed;
        auto files = WriteFamily(candidate.path, family);
        if (files.HasError())
            return Result<void>::Failure(files.ErrorValue());
        if (auto decoded = LoadPackagedMetalClusteredLighting(candidate.path); decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        if (auto recorded = WriteProvenance(candidate.path, family, host, std::move(files).Value()); recorded.HasError())
            return recorded;
        return candidate.Publish(destination);
    }
}  // namespace Horo::Editor::Offline
