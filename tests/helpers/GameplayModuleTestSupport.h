#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Horo::Tests {
    inline std::uint64_t ReadDescriptorRevision(const std::filesystem::path &path) {
        std::ifstream stream{path};
        std::uint64_t revision{};
        if (!(stream >> revision) || revision == 0)
            throw std::runtime_error{"test gameplay descriptor revision is missing or invalid"};
        return revision;
    }

    inline std::filesystem::path WriteGameplayModuleManifest(const std::filesystem::path &projectRoot,
                                                             const std::filesystem::path &artifactPath, const std::string_view moduleId,
                                                             const std::string_view buildFingerprint,
                                                             const std::uint64_t descriptorRevision) {
        const std::filesystem::path manifestPath = projectRoot / ".horo" / "local" / "gameplay_module.json";
        std::filesystem::create_directories(manifestPath.parent_path());
        std::ofstream stream{manifestPath, std::ios::binary | std::ios::trunc};
        if (!stream)
            throw std::runtime_error{"test gameplay module manifest could not be opened"};
        const auto artifactUtf8 = artifactPath.u8string();
        stream << "{\n  \"schemaVersion\": 1,\n  \"moduleId\": " << std::quoted(std::string{moduleId})
               << ",\n  \"buildFingerprint\": " << std::quoted(std::string{buildFingerprint})
               << ",\n  \"descriptorRevision\": " << descriptorRevision
               << ",\n  \"artifactPath\": " << std::quoted(std::string{artifactUtf8.begin(), artifactUtf8.end()}) << "\n}\n";
        stream.close();
        if (!stream)
            throw std::runtime_error{"test gameplay module manifest could not be written"};
        return manifestPath;
    }

    inline void AdvanceLastWriteTime(const std::filesystem::path &path) {
        std::error_code error;
        const std::filesystem::file_time_type current = std::filesystem::last_write_time(path, error);
        if (error)
            throw std::runtime_error{"test file write time could not be read"};
        std::filesystem::last_write_time(path, current + std::chrono::seconds{2}, error);
        if (error)
            throw std::runtime_error{"test file write time could not be advanced"};
    }
}  // namespace Horo::Tests
