#include "Horo/Release/UpdateZipPackageProducer.h"

#include "Horo/Release/ReleaseErrors.h"
#include "Horo/Release/UpdateStagedTree.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <limits>
#include <miniz.h>
#include <set>
#include <system_error>
#include <vector>

namespace Horo::Release {
    namespace {
        constexpr std::string_view PackageName = "update.zip";
        constexpr MZ_TIME_T StableZipTime = 315532800;  // 1980-01-01, the earliest DOS ZIP date.

        struct ZipWriter final {
            ~ZipWriter() {
                if (opened)
                    mz_zip_writer_end(&archive);
            }

            std::ofstream output;
            mz_zip_archive archive{};
            bool opened{};
        };

        struct SourceReader final {
            std::ifstream input;
            Sha256Builder digest;
            std::uint64_t expectedSize{};
            std::uint64_t consumed{};
            bool failed{};
        };

        /** @brief Writes at miniz's requested offset through a filesystem-path-aware stream. */
        [[nodiscard]] std::size_t WriteArchive(void *opaque, const mz_uint64 offset, const void *buffer, const std::size_t size) {
            auto &output = *static_cast<std::ofstream *>(opaque);
            if (offset > static_cast<mz_uint64>(std::numeric_limits<std::streamoff>::max()) ||
                size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
                return 0U;
            output.seekp(static_cast<std::streamoff>(offset));
            output.write(static_cast<const char *>(buffer), static_cast<std::streamsize>(size));
            return output ? size : 0U;
        }

        /** @brief Supplies exact sequential source bytes and hashes the bytes actually archived. */
        [[nodiscard]] std::size_t ReadSource(void *opaque, const mz_uint64 offset, void *buffer, const std::size_t size) {
            auto &reader = *static_cast<SourceReader *>(opaque);
            if (reader.failed || offset != reader.consumed || offset > reader.expectedSize)
                return 0U;
            const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(size, reader.expectedSize - offset));
            if (count == 0)
                return 0U;
            reader.input.read(static_cast<char *>(buffer), count);
            const auto read = reader.input.gcount();
            if (read != count || !reader.digest.Update(std::span{static_cast<const std::byte *>(buffer), static_cast<std::size_t>(read)})) {
                reader.failed = true;
                return 0U;
            }
            reader.consumed += static_cast<std::uint64_t>(read);
            return static_cast<std::size_t>(read);
        }

        /** @brief Hashes the complete closed package without loading it into memory. */
        [[nodiscard]] Result<ReleaseArtifactRecord> PackageEvidence(const std::filesystem::path &path) {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return Result<ReleaseArtifactRecord>::Failure(MakeError(ReleaseErrors::PipelineStagingIoFailed));
            Sha256Builder digest;
            std::array<char, 64U * 1024U> buffer{};
            std::uint64_t size = 0U;
            while (input) {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto read = input.gcount();
                if (read > 0) {
                    if (!digest.Update(std::as_bytes(std::span{buffer}.first(static_cast<std::size_t>(read)))))
                        return Result<ReleaseArtifactRecord>::Failure(MakeError(ReleaseErrors::PipelineStagingIoFailed));
                    size += static_cast<std::uint64_t>(read);
                }
            }
            if (input.bad())
                return Result<ReleaseArtifactRecord>::Failure(MakeError(ReleaseErrors::PipelineStagingIoFailed));
            return Result<ReleaseArtifactRecord>::Success(
                {std::string{PackageName}, ReleaseArtifactRole::NativePackage, size, digest.Finalize()});
        }

        /** @brief Rejects failed input or output without claiming that a package was produced. */
        [[nodiscard]] Result<ReleasePackageResult> InvalidPackage() {
            return Result<ReleasePackageResult>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        }

        /** @brief Binds explicit executable intent to the frozen source inventory before signing. */
        [[nodiscard]] Result<std::string> ProductInventory(const ReleasePackageRequest &request, const UpdateArchiveLimits &limits) {
            const auto invalid = [] {
                return Result<std::string>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            };
            const std::set<std::string, std::less<>> executables(request.executablePaths.begin(), request.executablePaths.end());
            if (request.productEntrypoint.empty() || !executables.contains(request.productEntrypoint) ||
                executables.size() != request.executablePaths.size())
                return invalid();
            std::size_t declaredExecutables = 0U;
            std::vector<UpdateStagedFile> files;
            files.reserve(request.sourceInventory.Artifacts().size());
            for (const auto &file : request.sourceInventory.Artifacts()) {
                const bool executable = executables.contains(file.path);
                declaredExecutables += executable ? 1U : 0U;
                files.emplace_back(file.path, file.size, file.digest, executable ? UpdateFileMode::Executable : UpdateFileMode::Regular,
                                   file.path == request.productEntrypoint ? UpdateFileRole::Entrypoint : UpdateFileRole::Content);
            }
            return declaredExecutables == executables.size() ? BuildCanonicalUpdateFileInventory(files, limits) : invalid();
        }

        /** @brief Adds one verified file with stable ZIP metadata. */
        [[nodiscard]] bool AddSourceFile(mz_zip_archive &archive, const ReleasePackageRequest &request, const ReleaseArtifactRecord &file) {
            SourceReader reader{.input = std::ifstream(request.sourceRoot / file.path, std::ios::binary), .expectedSize = file.size};
            if (!reader.input)
                return false;
            const bool added =
                mz_zip_writer_add_read_buf_callback(&archive, file.path.c_str(), ReadSource, &reader, file.size, &StableZipTime, nullptr,
                                                    0U, MZ_NO_COMPRESSION, nullptr, 0U, nullptr, 0U) != 0;
            return added && !reader.failed && reader.consumed == file.size && reader.digest.Finalize() == file.digest;
        }

        /** @brief Writes the reserved inventory entry using the same stable ZIP metadata. */
        [[nodiscard]] bool AddInventory(mz_zip_archive &archive, const std::string_view inventory) {
            auto modified = StableZipTime;
            return mz_zip_writer_add_mem_ex_v2(&archive, UpdateFileInventoryPath.data(), inventory.data(), inventory.size(), nullptr, 0U,
                                               MZ_NO_COMPRESSION, 0U, 0U, &modified, nullptr, 0U, nullptr, 0U) != 0;
        }
    }  // namespace

    /** @copydoc UpdateZipPackageProducer::UpdateZipPackageProducer */
    UpdateZipPackageProducer::UpdateZipPackageProducer(const UpdateArchiveLimits &limits) noexcept : limits_(limits) {}

    /** @copydoc UpdateZipPackageProducer::Format */
    DistributionPackageFormat UpdateZipPackageProducer::Format() const noexcept {
        return DistributionPackageFormat::ZipArchive;
    }

    /** @copydoc UpdateZipPackageProducer::Produce */
    Result<ReleasePackageResult> UpdateZipPackageProducer::Produce(const ReleasePackageRequest &request) {
        if (request.selection.format != Format())
            return InvalidPackage();
        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::symlink_status(request.privateOutputRoot, error)) || error)
            return InvalidPackage();
        const auto output = request.privateOutputRoot / PackageName;
        if (const auto outputStatus = std::filesystem::symlink_status(output, error);
            outputStatus.type() != std::filesystem::file_type::not_found || (error && error != std::errc::no_such_file_or_directory))
            return InvalidPackage();

        auto inventory = ProductInventory(request, limits_);
        if (inventory.HasError())
            return Result<ReleasePackageResult>::Failure(inventory.ErrorValue());

        bool produced = false;
        {
            ZipWriter writer;
            writer.output.open(output, std::ios::binary | std::ios::trunc);
            writer.archive.m_pWrite = WriteArchive;
            writer.archive.m_pIO_opaque = &writer.output;
            writer.opened = writer.output && mz_zip_writer_init(&writer.archive, 0U) != 0;
            if (writer.opened) {
                produced = true;
                for (const auto &file : request.sourceInventory.Artifacts()) {
                    if (!AddSourceFile(writer.archive, request, file)) {
                        produced = false;
                        break;
                    }
                }
                produced = produced && AddInventory(writer.archive, inventory.Value()) && mz_zip_writer_finalize_archive(&writer.archive);
                writer.output.close();
                produced = produced && !writer.output.fail();
            }
        }
        if (!produced) {
            std::filesystem::remove(output, error);
            return Result<ReleasePackageResult>::Failure(MakeError(ReleaseErrors::PipelineStagingIoFailed));
        }
        auto evidence = PackageEvidence(output);
        if (evidence.HasError()) {
            std::filesystem::remove(output, error);
            return Result<ReleasePackageResult>::Failure(evidence.ErrorValue());
        }
        return Result<ReleasePackageResult>::Success({Format(), {std::move(evidence).Value()}});
    }
}  // namespace Horo::Release
