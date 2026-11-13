#include "ColorOutputPackageInternal.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <fstream>
#include <limits>

namespace Horo::Render::Internal::ColorPackageDetail {
    /** @copydoc Require */
    void Require(bool valid) {
        if (!valid)
            throw Failure{Invalid()};
    }

    /** @copydoc Invalid */
    Error Invalid() {
        return MakeError(Render::ShaderCompilerPipelineErrors::ToolOutputInvalid,
                         "Color output generation is malformed, inconsistent or escapes its explicit root.");
    }

    /** @copydoc Root */
    std::filesystem::path Root(const std::filesystem::path &root) {
        Require(root.is_absolute());
        std::error_code error;
        const auto status = std::filesystem::symlink_status(root, error);
        Require(!error && std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status));
        auto canonical = std::filesystem::canonical(root, error);
        Require(!error);
        return canonical;
    }

    /** @copydoc Read */
    Bytes Read(const std::filesystem::path &root, std::string_view relative, std::size_t maximum) {
        const auto file = root / std::string{relative};
        std::error_code error;
        const auto status = std::filesystem::symlink_status(file, error);
        Require(!error && std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status));
        const auto canonical = std::filesystem::canonical(file, error);
        Require(!error && canonical.generic_string().starts_with(root.generic_string() + "/"));
        Require(maximum > 0 && maximum <= 32U * 1024U * 1024U);
        std::ifstream stream(file, std::ios::binary | std::ios::ate);
        Require(stream.good());
        const auto count = stream.tellg();
        Require(count >= 0 && static_cast<std::uint64_t>(count) <= maximum);
        Bytes data(static_cast<std::size_t>(count));
        stream.seekg(0);
        if (!data.empty())
            stream.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
        Require(stream.good());
        return data;
    }

    /** @copydoc Parse */
    Json Parse(const Bytes &bytes) {
        Foundation::JsonParseGuard guard{32};
        auto value = guard.Parse(bytes.begin(), bytes.end());
        Require(!guard.HasDuplicate() && !guard.IsTooDeep() && !value.is_discarded() && value.is_object());
        return value;
    }

    /** @copydoc Digest */
    Sha256Digest Digest(const Json &value) {
        Require(value.is_string());
        return Take(ParseSha256("sha256:" + value.get<std::string>()));
    }

    /** @copydoc UInt */
    std::uint32_t UInt(const Json &value) {
        Require(value.is_number_unsigned());
        const auto number = value.get<std::uint64_t>();
        Require(number <= std::numeric_limits<std::uint32_t>::max());
        return static_cast<std::uint32_t>(number);
    }

    /** @copydoc Hash */
    Sha256Digest Hash(const Bytes &bytes) noexcept {
        return ComputeSha256(std::as_bytes(std::span{bytes}));
    }

    /** @copydoc Encode */
    Bytes Encode(const Json &value) {
        const auto encoded = value.dump(2) + "\n";
        return {encoded.begin(), encoded.end()};
    }

}  // namespace Horo::Render::Internal::ColorPackageDetail

namespace Horo::Render::Internal {
    /** @copydoc ResolveColorPackageRoot */
    Result<std::filesystem::path> ResolveColorPackageRoot(const std::filesystem::path &root) {
        try {
            return Result<std::filesystem::path>::Success(ColorPackageDetail::Root(root));
        } catch (const ColorPackageDetail::Failure &f) {
            return Result<std::filesystem::path>::Failure(f.error);
        } catch (const std::exception &) {
            return Result<std::filesystem::path>::Failure(ColorPackageDetail::Invalid());
        }
    }

    /** @copydoc ReadColorPackageFile */
    Result<std::vector<std::uint8_t>> ReadColorPackageFile(const std::filesystem::path &root, std::string_view relative,
                                                           std::size_t maximum) {
        try {
            return Result<std::vector<std::uint8_t>>::Success(ColorPackageDetail::Read(root, relative, maximum));
        } catch (const ColorPackageDetail::Failure &f) {
            return Result<std::vector<std::uint8_t>>::Failure(f.error);
        } catch (const std::exception &) {
            return Result<std::vector<std::uint8_t>>::Failure(ColorPackageDetail::Invalid());
        }
    }
}  // namespace Horo::Render::Internal
