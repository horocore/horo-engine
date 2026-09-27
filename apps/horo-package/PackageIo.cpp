#include "PackageCommand.h"

#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Horo::PackageCommand {
    Outcome Success(std::string detail) {
        return {true, "package.ok", std::move(detail)};
    }

    Outcome Failure(const std::string_view code, const std::string_view detail) {
        return {false, std::string{code}, std::string{detail}};
    }

    Outcome ReadBounded(const std::filesystem::path &path, const std::size_t limit, std::vector<std::byte> &bytes) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status))
            return Failure("package.input_invalid", "Input must be a regular non-symlink file.");
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > limit || size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
            return Failure("package.input_limit", "Input exceeds the command's byte limit.");
        std::ifstream input{path, std::ios::binary};
        if (!input)
            return Failure("package.input_unreadable", "Input could not be read.");
        bytes.resize(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size()) || input.peek() != std::ifstream::traits_type::eof()) {
            bytes.clear();
            return Failure("package.input_changed", "Input changed during reading.");
        }
        return Success();
    }

    Outcome WriteNew(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
        // Exclusive creation prevents a concurrent file or symlink from being overwritten.
#ifdef _WIN32
        const int descriptor = _wopen(path.c_str(), _O_BINARY | _O_CREAT | _O_EXCL | _O_WRONLY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
        const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
#endif
        if (descriptor < 0)
            return Failure("package.output_unwritable", "Output exists or could not be created.");
        std::size_t written{};
        while (written < bytes.size()) {
#ifdef _WIN32
            const int count = _write(descriptor, bytes.data() + written, static_cast<unsigned int>(bytes.size() - written));
#else
            const auto count = ::write(descriptor, bytes.data() + written, bytes.size() - written);
#endif
            if (count <= 0)
                break;
            written += static_cast<std::size_t>(count);
        }
#ifdef _WIN32
        const int closed = _close(descriptor);
#else
        const int closed = ::close(descriptor);
#endif
        if (written != bytes.size() || closed != 0) {
            std::error_code error;
            std::filesystem::remove(path, error);
            return Failure("package.output_unwritable", "Output write failed.");
        }
        return Success();
    }

    Outcome VerifyArchive(const std::span<const std::byte> bytes, std::optional<Packages::ValidatedPackageArchive> &archive) {
        auto result = Packages::ValidatedPackageArchive::Verify(bytes);
        if (result.HasError())
            return Failure("package.archive_invalid", "Archive failed the installer inventory and content policy.");
        archive.emplace(std::move(result).Value());
        return Success();
    }
}  // namespace Horo::PackageCommand
