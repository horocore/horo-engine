#include "GameplayBuildInputs.h"

#include "Horo/Foundation/PathUtils.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Gameplay/GameModule.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace Horo::Application::Detail {
    namespace {
        const ErrorDomainId Domain{"horo.application.gameplay_build"};
        const ErrorCodeDescriptor InvalidRequest{Domain,
                                                 ErrorCode{"invalid_request"},
                                                 ErrorSeverity::Error,
                                                 "Gameplay build request is invalid.",
                                                 "Verify the project and SDK paths.",
                                                 false,
                                                 true};
    }  // namespace

    [[nodiscard]] static bool NativeInputExtension(const std::filesystem::path &path) {
        static const std::set<std::string, std::less<>> extensions{".c",   ".cc",  ".cpp", ".cxx", ".h",    ".hh",
                                                                   ".hpp", ".hxx", ".inl", ".ixx", ".cppm", ".cmake"};
        return extensions.contains(path.extension().string());
    }

    [[nodiscard]] static bool HasPathPrefix(const std::filesystem::path &root, const std::filesystem::path &candidate) {
        return Horo::Foundation::Paths::HasPathPrefix(root, candidate);
    }

    /** @copydoc HashFile */
    [[nodiscard]] Result<std::string> HashFile(const std::filesystem::path &path, const std::uintmax_t maximumBytes) {
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error || size > maximumBytes)
            return Result<std::string>::Failure(MakeError(InvalidRequest, "File exceeds the bounded SHA-256 budget."));
        std::ifstream stream{path, std::ios::binary};
        std::vector<char> bytes(size);
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if ((!stream && !stream.eof()) || static_cast<std::uintmax_t>(stream.gcount()) != size)
            return Result<std::string>::Failure(MakeError(InvalidRequest, "File could not be hashed."));
        return Result<std::string>::Success(FormatSha256(ComputeSha256(std::as_bytes(std::span{bytes}))));
    }

    /** @copydoc ResolveCompilerIdentity */
    [[nodiscard]] Result<CompilerIdentity> ResolveCompilerIdentity(const std::filesystem::path &compiler) {
        std::error_code error;
        CompilerIdentity identity;
        identity.canonicalPath = std::filesystem::canonical(compiler, error);
        if (error || !std::filesystem::is_regular_file(identity.canonicalPath, error))
            return Result<CompilerIdentity>::Failure(MakeError(InvalidRequest, "Selected C++ compiler is unavailable."));
        identity.size = std::filesystem::file_size(identity.canonicalPath, error);
        const auto writeTime = std::filesystem::last_write_time(identity.canonicalPath, error);
        if (error || identity.size > 512U * 1024U * 1024U)
            return Result<CompilerIdentity>::Failure(MakeError(InvalidRequest, "Selected C++ compiler identity is unreadable."));
        identity.lastWrite = static_cast<std::int64_t>(writeTime.time_since_epoch().count());
#if defined(_WIN32)
        HANDLE handle =
            CreateFileW(identity.canonicalPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        BY_HANDLE_FILE_INFORMATION information{};
        if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &information)) {
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
            return Result<CompilerIdentity>::Failure(MakeError(InvalidRequest, "Compiler file identity is unavailable."));
        }
        CloseHandle(handle);
        identity.nativeFileIdentity =
            std::format("{}:{}:{}", information.dwVolumeSerialNumber, information.nFileIndexHigh, information.nFileIndexLow);
#else
        struct stat information = {};

        if (stat(identity.canonicalPath.c_str(), &information) != 0)
            return Result<CompilerIdentity>::Failure(MakeError(InvalidRequest, "Compiler file identity is unavailable."));
        identity.nativeFileIdentity =
            std::format("{} : {}", static_cast<std::uintmax_t>(information.st_dev), static_cast<std::uintmax_t>(information.st_ino));
#endif
        const std::string cacheKey = std::format("{}\n{}\n{}\n{}", identity.canonicalPath.generic_string(), identity.nativeFileIdentity,
                                                 identity.size, identity.lastWrite);
        static std::mutex cacheMutex;
        static std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>> hashes;
        {
            std::lock_guard lock(cacheMutex);
            if (const auto found = hashes.find(cacheKey); found != hashes.end()) {
                identity.binaryHash = found->second;
                return Result<CompilerIdentity>::Success(std::move(identity));
            }
        }
        Result<std::string> binaryHash = HashFile(identity.canonicalPath, 512U * 1024U * 1024U);
        if (binaryHash.HasError())
            return Result<CompilerIdentity>::Failure(binaryHash.ErrorValue());
        identity.binaryHash = std::move(binaryHash).Value();
        {
            std::lock_guard lock(cacheMutex);
            hashes.try_emplace(cacheKey, identity.binaryHash);
        }
        return Result<CompilerIdentity>::Success(std::move(identity));
    }

    [[nodiscard]] static Result<void> CollectNativeInputs(const std::filesystem::path &root, std::vector<std::filesystem::path> &inputs) {
        std::error_code error;
        for (const char *directoryName : {"cmake", "source", "include"}) {
            const std::filesystem::path directory = root / directoryName;
            if (!std::filesystem::is_directory(directory, error)) {
                error.clear();
                continue;
            }
            std::filesystem::recursive_directory_iterator iterator{directory, error};
            const std::filesystem::recursive_directory_iterator end;
            if (error)
                return Result<void>::Failure(MakeError(InvalidRequest, "Gameplay build inputs could not be enumerated."));
            while (iterator != end) {
                const std::filesystem::file_status status = iterator->symlink_status(error);
                if (error)
                    return Result<void>::Failure(MakeError(InvalidRequest, "Gameplay build inputs could not be inspected."));
                if (std::filesystem::is_symlink(status))
                    return Result<void>::Failure(MakeError(InvalidRequest, "Gameplay build inputs may not use symlinks."));
                if (std::filesystem::is_regular_file(status) && NativeInputExtension(iterator->path()))
                    inputs.push_back(iterator->path());
                iterator.increment(error);
                if (error)
                    return Result<void>::Failure(MakeError(InvalidRequest, "Gameplay build inputs could not be enumerated."));
            }
        }
        return Result<void>::Success();
    }

    [[nodiscard]] static Result<bool> CollectResolvedInputs(const std::filesystem::path &root, const std::filesystem::path &manifestPath,
                                                            std::vector<std::filesystem::path> &inputs) {
        std::ifstream manifest{manifestPath, std::ios::binary};
        if (!manifest)
            return Result<bool>::Success(false);

        std::error_code error;
        std::string relativeInput;
        while (std::getline(manifest, relativeInput)) {
            if (!relativeInput.empty() && relativeInput.back() == '\r')
                relativeInput.pop_back();
            if (relativeInput.empty())
                continue;
            const std::filesystem::path unresolved{relativeInput};
            if (unresolved.is_absolute())
                return Result<bool>::Failure(MakeError(InvalidRequest, "Gameplay build input must be project-relative."));
            const std::filesystem::path canonicalInput = std::filesystem::weakly_canonical(root / unresolved, error);
            if (error || !HasPathPrefix(root, canonicalInput) || !std::filesystem::is_regular_file(canonicalInput, error))
                return Result<bool>::Failure(
                    MakeError(InvalidRequest, "Resolved gameplay build input escapes the project or is unavailable."));
            inputs.push_back(canonicalInput);
        }
        return Result<bool>::Success(true);
    }

    [[nodiscard]] static Result<std::string> ResolveCompilerHash(const GameplayBuildRequest &request) {
        if (!request.environment.cxxCompiler.has_value())
            return Result<std::string>::Success({});
        Result<CompilerIdentity> compiler = ResolveCompilerIdentity(*request.environment.cxxCompiler);
        if (compiler.HasError())
            return Result<std::string>::Failure(compiler.ErrorValue());
        return Result<std::string>::Success(std::move(compiler).Value().binaryHash);
    }

    [[nodiscard]] static std::string BuildInputHashPrefix(const GameplayBuildRequest &request, const std::string_view compilerHash,
                                                          const std::string_view resolvedInputStructureHash) {
        return std::format("{}\n{}\n{}\n{}\n{}\n{}\n{}\n{}\n{}\n{}\n", Gameplay::CurrentGameplayBuildFingerprint(),
                           request.environment.configuration, request.environment.gameplaySdkPackage.generic_string(),
                           request.environment.cxxCompiler.value_or(std::filesystem::path{}).generic_string(),
                           request.environment.generator.value_or(""), request.environment.generatorPlatform.value_or(""),
                           request.environment.generatorToolset.value_or(""),
                           request.environment.toolchainFile.value_or(std::filesystem::path{}).generic_string(), compilerHash,
                           resolvedInputStructureHash);
    }

    [[nodiscard]] static Result<std::string> FinalizeInputHash(std::string bytes) {
        return Result<std::string>::Success(FormatSha256(ComputeSha256(std::as_bytes(std::span{bytes.data(), bytes.size()}))));
    }

    [[nodiscard]] static Result<std::string> HashBuildInputs(const std::filesystem::path &root,
                                                             const std::vector<std::filesystem::path> &inputs, std::string bytes) {
        constexpr std::uintmax_t MaximumInputBytes = 64U * 1024U * 1024U;
        std::error_code error;
        for (const std::filesystem::path &path : inputs) {
            if (const std::uintmax_t size = std::filesystem::file_size(path, error);
                error || size > MaximumInputBytes || bytes.size() + size > MaximumInputBytes)
                return Result<std::string>::Failure(MakeError(InvalidRequest, "Gameplay build inputs exceed the bounded hash budget."));
            std::ifstream stream{path, std::ios::binary};
            if (!stream)
                return Result<std::string>::Failure(MakeError(InvalidRequest, "Gameplay build input could not be read."));
            bytes.append(std::filesystem::relative(path, root, error).generic_string()).push_back('\0');
            bytes.append(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});
            bytes.push_back('\0');
        }
        return FinalizeInputHash(std::move(bytes));
    }

    [[nodiscard]] static Result<std::string> HashInputStructure(const std::filesystem::path &root,
                                                                const std::vector<std::filesystem::path> &inputs) {
        constexpr std::size_t MaximumStructureBytes = 1U * 1024U * 1024U;
        std::string bytes;
        for (const std::filesystem::path &path : inputs) {
            std::error_code error;
            const std::string relative = std::filesystem::relative(path, root, error).generic_string();
            if (error || bytes.size() + relative.size() + 1U > MaximumStructureBytes)
                return Result<std::string>::Failure(MakeError(InvalidRequest, "Gameplay input structure exceeds the bounded hash budget."));
            bytes.append(relative).push_back('\0');
        }
        return FinalizeInputHash(std::move(bytes));
    }

    /** @copydoc ComputeInputHash */
    [[nodiscard]] Result<std::string> ComputeInputHash(const GameplayBuildRequest &request) {
        std::error_code error;
        const std::filesystem::path root = std::filesystem::weakly_canonical(request.projectRoot, error);
        if (error || !std::filesystem::is_directory(root))
            return Result<std::string>::Failure(MakeError(InvalidRequest, "Project root is unavailable."));

        std::vector<std::filesystem::path> inputs;
        if (const std::filesystem::path cmakeLists = root / "CMakeLists.txt"; std::filesystem::is_regular_file(cmakeLists, error))
            inputs.push_back(cmakeLists);
        if (Result<void> collected = CollectNativeInputs(root, inputs); collected.HasError())
            return Result<std::string>::Failure(collected.ErrorValue());
        const std::filesystem::path resolvedManifest = root / ".horo/local/gameplay_build_inputs.txt";
        if (Result<bool> resolvedInputs = CollectResolvedInputs(root, resolvedManifest, inputs); resolvedInputs.HasError())
            return Result<std::string>::Failure(resolvedInputs.ErrorValue());
        std::ranges::sort(inputs);
        inputs.erase(std::ranges::unique(inputs).begin(), inputs.end());

        Result<std::string> inputStructureHash = HashInputStructure(root, inputs);
        if (inputStructureHash.HasError())
            return Result<std::string>::Failure(inputStructureHash.ErrorValue());
        Result<std::string> compilerHash = ResolveCompilerHash(request);
        if (compilerHash.HasError())
            return Result<std::string>::Failure(compilerHash.ErrorValue());
        return HashBuildInputs(root, inputs, BuildInputHashPrefix(request, compilerHash.Value(), inputStructureHash.Value()));
    }

    /** @copydoc ReadSuccessfulHash */
    [[nodiscard]] std::optional<std::string> ReadSuccessfulHash(const std::filesystem::path &root) {
        std::ifstream stream{root / ".horo/local/gameplay_build_state.json", std::ios::binary};
        if (!stream)
            return std::nullopt;
        try {
            const nlohmann::json document = nlohmann::json::parse(stream, nullptr, true, true);
            if (document.value("schemaVersion", 0) != 1 || !document.contains("successfulInputHash") ||
                !document["successfulInputHash"].is_string())
                return std::nullopt;
            return document["successfulInputHash"].get<std::string>();
        } catch (const nlohmann::json::exception &) {
            return std::nullopt;
        }
    }

}  // namespace Horo::Application::Detail
