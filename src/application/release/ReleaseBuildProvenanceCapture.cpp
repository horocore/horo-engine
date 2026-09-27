#include "Horo/Release/ReleaseBuildProvenance.h"
#include "Horo/Release/ReleaseErrors.h"

#include <array>
#include <fstream>
#include <limits>
#include <span>
#include <system_error>
#include <utility>

namespace Horo::Release {
    namespace {
        constexpr std::size_t ReadBufferBytes = 64U * 1024U;
        constexpr std::size_t MaximumFiles = 65'536U;

        [[nodiscard]] Error InvalidTree() {
            return MakeError(ReleaseErrors::PipelineOutputInvalid);
        }

        /** @brief Hashes one regular file in bounded memory and rejects read or length overflow. */
        [[nodiscard]] Result<ReleaseUnsignedFile> CaptureFile(const std::filesystem::path &file, std::string relative) {
            std::ifstream input(file, std::ios::binary);
            if (!input)
                return Result<ReleaseUnsignedFile>::Failure(InvalidTree());
            Sha256Builder hash;
            std::array<char, ReadBufferBytes> buffer{};
            std::uint64_t bytes = 0U;
            while (input) {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = static_cast<std::size_t>(input.gcount());
                if (count > std::numeric_limits<std::uint64_t>::max() - bytes ||
                    !hash.Update(std::as_bytes(std::span{buffer.data(), count})))
                    return Result<ReleaseUnsignedFile>::Failure(InvalidTree());
                bytes += count;
            }
            if (!input.eof())
                return Result<ReleaseUnsignedFile>::Failure(InvalidTree());
            return Result<ReleaseUnsignedFile>::Success({std::move(relative), bytes, hash.Finalize()});
        }
    }  // namespace

    /** @copydoc CaptureReleaseBuildProvenance */
    Result<ReleaseBuildProvenance> CaptureReleaseBuildProvenance(const std::filesystem::path &root, ReleaseBuildProvenanceData inputs) {
        if (!inputs.files.empty())
            return Result<ReleaseBuildProvenance>::Failure(InvalidTree());
        std::error_code error;
        if (const auto rootStatus = std::filesystem::symlink_status(root, error); error || !std::filesystem::is_directory(rootStatus))
            return Result<ReleaseBuildProvenance>::Failure(InvalidTree());
        std::filesystem::recursive_directory_iterator entry{root, error};
        if (error)
            return Result<ReleaseBuildProvenance>::Failure(InvalidTree());
        const std::filesystem::recursive_directory_iterator end;
        while (entry != end) {
            const auto status = entry->symlink_status(error);
            if (error || std::filesystem::is_symlink(status))
                return Result<ReleaseBuildProvenance>::Failure(InvalidTree());
            if (std::filesystem::is_regular_file(status)) {
                if (inputs.files.size() == MaximumFiles)
                    return Result<ReleaseBuildProvenance>::Failure(InvalidTree());
                auto captured = CaptureFile(entry->path(), entry->path().lexically_relative(root).generic_string());
                if (captured.HasError())
                    return Result<ReleaseBuildProvenance>::Failure(std::move(captured).ErrorValue());
                inputs.files.push_back(std::move(captured).Value());
            } else if (!std::filesystem::is_directory(status)) {
                return Result<ReleaseBuildProvenance>::Failure(InvalidTree());
            }
            entry.increment(error);
            if (error)
                return Result<ReleaseBuildProvenance>::Failure(InvalidTree());
        }
        return ReleaseBuildProvenance::Create(std::move(inputs));
    }
}  // namespace Horo::Release
