#include "AtomicShaderCookPackage.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "JsonUtils.h"

#include <bit>
#include <format>
#include <fstream>

namespace Horo::Editor::Offline {
    namespace {
        using namespace Render;
        using Application::Internal::AuthenticatedShaderCookConfiguration;
        using Foundation::Json;

        /** @brief Reserves the private staging namespace consistently using ASCII case folding. */
        bool PrivateCandidateName(std::string_view name, std::string_view PrivateCandidatePrefix) {
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

        /** @brief Retains one actionable offline publication recovery path. */
        Result<void> IoFailure() {
            return Result<void>::Failure(
                MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed,
                          "Use a new absolute package directory under an existing exclusively owned output parent."));
        }

        /** @brief Explains the required independent existing scratch ownership. */
        Result<void> ScratchFailure() {
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed,
                                                   "Use an existing independently owned absolute scratch directory "
                                                   "and a separate new output directory outside it."));
        }

        /** @brief Resolves actual directory identity before the adapter may mutate scratch. */
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

        /** @brief Checks ancestor identities so aliases cannot expose partial final output. */
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

        /** @brief Keeps fixed package inventory names inside the privately owned candidate. */
        bool SafeRelative(std::string_view value) {
            const std::filesystem::path path{value};
            if (value.empty() || path.is_absolute())
                return false;
            for (const auto &part : path)
                if (part == ".." || part == ".")
                    return false;
            return true;
        }

        /** @brief Bounds the synchronous borrowed inventory before dereferencing compilation records. */
        bool ValidInventory(const AtomicShaderCookPackage &family) {
            if (family.programs.empty() || family.programs.size() > 8 || !family.validateWritten)
                return false;
            if (!SafeRelative(family.provenanceFilename) || family.privateCandidatePrefix.empty())
                return false;
            for (const auto &program : family.programs)
                if (!program.manifest || !program.compiled || !SafeRelative(program.prefix))
                    return false;
            return true;
        }

        /** @brief Writes one exact bounded output into the claimed private candidate. */
        Result<void> Write(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream.write(std::bit_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            stream.close();
            return stream ? Result<void>::Success() : IoFailure();
        }

        /** @brief Rejects existing output and dangling symlink destinations without replacement. */
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

            Result<void> Claim(const std::filesystem::path &destination, std::string_view PrivateCandidatePrefix) {
                if (auto valid = ValidateAtomicShaderCookDestination(destination, PrivateCandidatePrefix); valid.HasError())
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

        /** @brief Binds informational integrity rows to the actual bytes written. */
        Result<void> WriteRecorded(const std::filesystem::path &root, const std::string &relative, std::span<const std::uint8_t> bytes,
                                   Json &files) {
            if (auto written = Write(root / relative, bytes); written.HasError())
                return written;
            files[relative] = FormatSha256(ComputeSha256(std::as_bytes(bytes)));
            return Result<void>::Success();
        }

        /** @brief Requires genuine bound artifact metadata before writing its pair. */
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

        /** @brief Records authenticated tool identities without copying paths or independent trust roots. */
        Json ToolRows(const AuthenticatedShaderCookConfiguration &host) {
            auto tools = Json::array();
            for (const auto &tool : host.configuration.tools)
                tools.push_back({{"role", tool.identity.tool},
                                 {"release", tool.identity.release},
                                 {"buildDigest", FormatSha256(tool.identity.buildDigest)},
                                 {"approvedExecutableDigest", FormatSha256(tool.executableDigest)}});
            return tools;
        }

        /** @brief Records actual source and artifact identities from the complete compilation family. */
        Json ProgramRows(const AtomicShaderCookPackage &family) {
            auto programs = Json::array();
            const auto programsInventory = family.programs;
            for (std::size_t index = 0; index < programsInventory.size(); ++index) {
                const auto &artifact = programsInventory[index].compiled->artifacts.front();
                programs.push_back({{"prefix", programsInventory[index].prefix},
                                    {"sourceIdentity", programsInventory[index].manifest->sourceIdentity},
                                    {"sourceRevision", programsInventory[index].manifest->sourceRevision},
                                    {"sourceDigest", FormatSha256(programsInventory[index].compiled->sourceDigest)},
                                    {"compiledKey", FormatSha256(artifact.artifactKey)},
                                    {"payloadDigest", FormatSha256(artifact.reflection->payloadDigest)},
                                    {"cookedKey", FormatSha256(artifact.reflection->cookedArtifactKey)}});
            }
            return programs;
        }

        /** @brief Emits informational integrity only after authenticated input bytes are available. */
        Result<void> WriteProvenance(const std::filesystem::path &root, const AtomicShaderCookPackage &family,
                                     const AuthenticatedShaderCookConfiguration &host, Json files) {
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
            return Write(root / family.provenanceFilename, {std::bit_cast<const std::uint8_t *>(encoded.data()), encoded.size()});
        }

        /** @brief Writes every canonical pair beneath the claimed private candidate. */
        Result<Json> WriteFamily(const std::filesystem::path &root, const AtomicShaderCookPackage &family) {
            using Files = Result<Json>;
            std::error_code error;
            std::filesystem::create_directories(root / "shaders/editor", error);
            if (error)
                return Files::Failure(IoFailure().ErrorValue());
            std::filesystem::create_directories(root / "shaders/runtime", error);
            if (error)
                return Files::Failure(IoFailure().ErrorValue());
            auto files = Json::object();
            const auto programsInventory = family.programs;
            for (std::size_t index = 0; index < programsInventory.size(); ++index)
                if (auto written = WriteProgram(root, programsInventory[index].prefix, *programsInventory[index].compiled, files);
                    written.HasError())
                    return Files::Failure(written.ErrorValue());
            return Files::Success(std::move(files));
        }
    }  // namespace

    Result<void> ValidateAtomicShaderCookDestination(const std::filesystem::path &destination, std::string_view PrivateCandidatePrefix) {
        if (PrivateCandidateName(destination.filename().string(), PrivateCandidatePrefix))
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed,
                                                   "The private cook staging filename namespace is reserved; choose another output name."));
        if (auto absent = DestinationAbsent(destination); absent.HasError())
            return absent;
        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::symlink_status(destination.parent_path(), error)) || error)
            return IoFailure();
        return Result<void>::Success();
    }

    Result<void> ValidateAtomicShaderCookPaths(const std::filesystem::path &destination, const std::filesystem::path &scratch,
                                               std::string_view PrivateCandidatePrefix) {
        if (auto valid = ValidateAtomicShaderCookDestination(destination, PrivateCandidatePrefix); valid.HasError())
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

    Result<void> PublishAtomicShaderCookPackage(const std::filesystem::path &destination, const AtomicShaderCookPackage &family,
                                                const Application::Internal::AuthenticatedShaderCookConfiguration &host) {
        if (!ValidInventory(family))
            return IoFailure();
        Candidate candidate;
        if (auto claimed = candidate.Claim(destination, family.privateCandidatePrefix); claimed.HasError())
            return claimed;
        auto files = WriteFamily(candidate.path, family);
        if (files.HasError())
            return Result<void>::Failure(files.ErrorValue());
        if (auto decoded = family.validateWritten(candidate.path, family.context); decoded.HasError())
            return Result<void>::Failure(decoded.ErrorValue());
        if (auto recorded = WriteProvenance(candidate.path, family, host, std::move(files).Value()); recorded.HasError())
            return recorded;
        return candidate.Publish(destination);
    }
}  // namespace Horo::Editor::Offline
