#include "Horo/Packages/PackageArchive.h"

#include "PackageValidationDetail.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <miniz.h>
#include <utility>

namespace Horo::Packages {
    namespace {
        /** @brief Stack-owned miniz reader; its byte buffer is owned by the calling validation transaction. */
        struct ZipReader {
            ZipReader() = default;
            ZipReader(const ZipReader &) = delete;
            ZipReader &operator=(const ZipReader &) = delete;
            mz_zip_archive value{};

            ~ZipReader() {
                mz_zip_reader_end(&value);
            }
        };

        using FileIndex = std::map<std::string, mz_zip_archive_file_stat, std::less<>>;

        /** @brief Reads a little-endian short only from a previously bounds-checked ZIP header. */
        [[nodiscard]] unsigned Read16(const std::span<const std::byte> bytes, const std::size_t offset) {
            return std::to_integer<unsigned>(bytes[offset]) | (std::to_integer<unsigned>(bytes[offset + 1]) << 8);
        }

        /** @brief Accepts only ZIP64, timestamp and UID/GID extras; link and alternate-name metadata is forbidden. */
        [[nodiscard]] bool HasSafeExtras(std::span<const std::byte> extra) {
            constexpr std::array<unsigned, 3> allowed{0x0001, 0x5455, 0x7875};
            while (!extra.empty()) {
                if (extra.size() < 4) {
                    return false;
                }
                const auto size = Read16(extra, 2);
                if (std::ranges::find(allowed, Read16(extra, 0)) == allowed.end() || size > extra.size() - 4) {
                    return false;
                }
                extra = extra.subspan(4 + size);
            }
            return true;
        }

        /** @brief Validates the central-directory extra field independently of its local-header counterpart. */
        [[nodiscard]] bool HasSafeCentralExtras(const mz_zip_archive &zip, const mz_zip_archive_file_stat &stat,
                                                const std::span<const std::byte> bytes) {
            const auto offset = zip.m_central_directory_file_ofs + stat.m_central_dir_ofs;
            if (offset > bytes.size()) {
                return false;
            }
            const auto header = bytes.subspan(static_cast<std::size_t>(offset));
            if (header.size() < 46) {
                return false;
            }
            const std::size_t start = 46 + Read16(header, 28);
            const std::size_t size = Read16(header, 30);
            if (start > header.size() || size > header.size() - start) {
                return false;
            }
            return HasSafeExtras(header.subspan(start, size));
        }

        /** @brief Checks local name/flags/method and data bounds, including empty files and directories. */
        [[nodiscard]] bool MatchesLocalHeader(const std::span<const std::byte> bytes, const mz_zip_archive_file_stat &stat,
                                              const std::string_view name) {
            if (stat.m_local_header_ofs > bytes.size()) {
                return false;
            }
            const auto local = bytes.subspan(static_cast<std::size_t>(stat.m_local_header_ofs));
            if (constexpr std::array signature{std::byte{0x50}, std::byte{0x4b}, std::byte{0x03}, std::byte{0x04}};
                local.size() < 30 || !std::ranges::equal(local.first(4), signature) || Read16(local, 6) != stat.m_bit_flag ||
                Read16(local, 8) != stat.m_method || Read16(local, 26) != name.size()) {
                return false;
            }
            if (const std::size_t dataOffset = 30 + name.size() + Read16(local, 28);
                dataOffset > local.size() || stat.m_comp_size > local.size() - dataOffset) {
                return false;
            }
            return std::ranges::equal(local.subspan(30, name.size()), std::as_bytes(std::span{name})) &&
                   HasSafeExtras(local.subspan(30 + name.size(), Read16(local, 28)));
        }

        /** @brief Rejects links, special files, privilege bits and unsupported ZIP encodings. */
        [[nodiscard]] bool IsRegularEntry(const mz_zip_archive_file_stat &stat) {
            constexpr unsigned FileAttributeReparsePoint = 0x400;
            const unsigned mode = stat.m_external_attr >> 16;
            const unsigned type = mode & 0170000;
            const unsigned expected = stat.m_is_directory ? 0040000 : 0100000;
            const std::array attributesAreSafe{
                stat.m_is_supported != 0,
                stat.m_is_encrypted == 0,
                type == 0 || type == expected,
                (mode & 07000) == 0,
                (stat.m_external_attr & FileAttributeReparsePoint) == 0,
            };
            return std::ranges::all_of(attributesAreSafe, std::identity{});
        }

        /** @brief Reads the complete filename instead of miniz's potentially truncated stat buffer. */
        [[nodiscard]] Result<PackagePath> EntryPath(mz_zip_archive &zip, const mz_zip_archive_file_stat &stat,
                                                    const std::span<const std::byte> bytes) {
            const auto length = mz_zip_reader_get_filename(&zip, stat.m_file_index, nullptr, 0);
            if (const std::array lengthIsValid{length >= 2, length <= 1026}; !std::ranges::all_of(lengthIsValid, std::identity{})) {
                return Result<PackagePath>::Failure(MakeError(Detail::InvalidArchive));
            }
            std::string name(length, '\0');
            mz_zip_reader_get_filename(&zip, stat.m_file_index, name.data(), length);
            name.pop_back();
            if (const std::array metadataIsValid{MatchesLocalHeader(bytes, stat, name), HasSafeCentralExtras(zip, stat, bytes)};
                !std::ranges::all_of(metadataIsValid, std::identity{})) {
                return Result<PackagePath>::Failure(MakeError(Detail::InvalidArchive));
            }
            if (const std::array isMarkedDirectory{stat.m_is_directory != 0, name.ends_with('/')};
                std::ranges::all_of(isMarkedDirectory, std::identity{})) {
                name.pop_back();
            }
            return PackagePath::Parse(name);
        }

        /** @brief Preflights all archive metadata and aggregate sizes before decompressing even the inventory. */
        [[nodiscard]] Result<FileIndex> IndexFiles(mz_zip_archive &zip, const std::span<const std::byte> bytes,
                                                   const PackageValidationLimits &limits) {
            FileIndex files;
            Detail::PathInventory inventory;
            std::uint64_t total = 0;
            for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&zip); ++index) {
                mz_zip_archive_file_stat stat{};
                if (!mz_zip_reader_file_stat(&zip, index, &stat) || !IsRegularEntry(stat)) {
                    return Result<FileIndex>::Failure(MakeError(Detail::InvalidArchive));
                }
                if (const auto available =
                        std::min<std::uint64_t>({limits.fileBytes, limits.expandedBytes - total, std::numeric_limits<std::size_t>::max()});
                    stat.m_uncomp_size > available) {
                    return Result<FileIndex>::Failure(MakeError(Detail::ResourceLimit));
                }
                total += stat.m_uncomp_size;
                auto path = EntryPath(zip, stat, bytes);
                if (path.HasError()) {
                    return Result<FileIndex>::Failure(path.ErrorValue());
                }
                if (!inventory.Add(path.Value(), stat.m_is_directory != 0)) {
                    return Result<FileIndex>::Failure(MakeError(Detail::InvalidArchive));
                }
                if (!stat.m_is_directory) {
                    files.try_emplace(path.Value().Value(), stat);
                } else if (stat.m_uncomp_size != 0) {
                    return Result<FileIndex>::Failure(MakeError(Detail::InvalidArchive));
                }
            }
            return Result<FileIndex>::Success(std::move(files));
        }

        /** @brief Decompresses into a preflight-bounded buffer; miniz also verifies data length and CRC. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadFile(mz_zip_archive &zip, const mz_zip_archive_file_stat &file) {
            std::vector<std::byte> output(static_cast<std::size_t>(file.m_uncomp_size));
            std::byte empty{};
            if (void *destination = output.empty() ? &empty : output.data();
                !mz_zip_reader_extract_to_mem(&zip, file.m_file_index, destination, output.size(), 0)) {
                return Result<std::vector<std::byte>>::Failure(MakeError(Detail::InvalidArchive));
            }
            return Result<std::vector<std::byte>>::Success(std::move(output));
        }

        /** @brief Reads the required package-intent manifest after archive metadata has been validated. */
        [[nodiscard]] Result<Sha256Digest> ReadPackageManifestDigest(mz_zip_archive &zip, const FileIndex &files) {
            const auto found = files.find("horo-package.toml");
            if (found == files.end())
                return Result<Sha256Digest>::Failure(MakeError(Detail::InventoryMismatch));
            auto bytes = ReadFile(zip, found->second);
            if (bytes.HasError())
                return Result<Sha256Digest>::Failure(bytes.ErrorValue());
            return Result<Sha256Digest>::Success(ComputeSha256(bytes.Value()));
        }

        /** @brief Checks declared size and mode before allocating content and comparing the actual digest. */
        [[nodiscard]] Result<void> MatchFile(mz_zip_archive &zip, const mz_zip_archive_file_stat &stat, const PackageFileEntry &entry) {
            const bool executable = ((stat.m_external_attr >> 16) & 0111) != 0;
            if (const std::array metadataMatches{stat.m_uncomp_size == entry.size, executable == entry.executable};
                !std::ranges::all_of(metadataMatches, std::identity{})) {
                return Result<void>::Failure(MakeError(Detail::InventoryMismatch));
            }
            auto content = ReadFile(zip, stat);
            if (content.HasError()) {
                return Result<void>::Failure(content.ErrorValue());
            }
            if (ComputeSha256(content.Value()) != entry.digest) {
                return Result<void>::Failure(MakeError(Detail::InventoryMismatch));
            }
            return Result<void>::Success();
        }

        /** @brief Compares the complete inventory, including required package intent, with archive regular files. */
        [[nodiscard]] Result<ValidatedPackageFileManifestV1> MatchInventory(mz_zip_archive &zip, const FileIndex &files,
                                                                            const PackageValidationLimits &limits) {
            const auto inventory = files.find("files.manifest.json");
            if (inventory == files.end() || !files.contains("horo-package.toml")) {
                return Result<ValidatedPackageFileManifestV1>::Failure(MakeError(Detail::InventoryMismatch));
            }
            if (inventory->second.m_uncomp_size > limits.manifestBytes) {
                return Result<ValidatedPackageFileManifestV1>::Failure(MakeError(Detail::ResourceLimit));
            }
            auto content = ReadFile(zip, inventory->second);
            if (content.HasError()) {
                return Result<ValidatedPackageFileManifestV1>::Failure(content.ErrorValue());
            }
            const auto &data = content.Value();
            std::string manifestJson(data.size(), '\0');
            std::ranges::transform(data, manifestJson.begin(), [](const std::byte value) {
                return static_cast<char>(std::to_integer<unsigned char>(value));
            });
            auto manifest = ValidatedPackageFileManifestV1::Parse(manifestJson, limits);
            if (manifest.HasError()) {
                return manifest;
            }
            if (manifest.Value().Entries().size() != files.size() - 1) {
                return Result<ValidatedPackageFileManifestV1>::Failure(MakeError(Detail::InventoryMismatch));
            }
            for (const auto &entry : manifest.Value().Entries()) {
                const auto found = files.find(entry.id.path.Value());
                if (found == files.end()) {
                    return Result<ValidatedPackageFileManifestV1>::Failure(MakeError(Detail::InventoryMismatch));
                }
                auto match = MatchFile(zip, found->second, entry);
                if (match.HasError()) {
                    return Result<ValidatedPackageFileManifestV1>::Failure(match.ErrorValue());
                }
            }
            return manifest;
        }
    }  // namespace

    /** @copydoc ValidatedPackageArchive::Verify */
    Result<ValidatedPackageArchive> ValidatedPackageArchive::Verify(const std::span<const std::byte> bytes,
                                                                    const PackageValidationLimits &limits) {
        if (bytes.size() > limits.archiveBytes) {
            return Result<ValidatedPackageArchive>::Failure(MakeError(Detail::ResourceLimit));
        }
        std::vector<std::byte> snapshot(bytes.begin(), bytes.end());
        ZipReader zip;
        if (!mz_zip_reader_init_mem(&zip.value, snapshot.data(), snapshot.size(), 0)) {
            return Result<ValidatedPackageArchive>::Failure(MakeError(Detail::InvalidArchive));
        }
        if (mz_zip_reader_get_num_files(&zip.value) > limits.entries) {
            return Result<ValidatedPackageArchive>::Failure(MakeError(Detail::ResourceLimit));
        }
        auto files = IndexFiles(zip.value, snapshot, limits);
        if (files.HasError()) {
            return Result<ValidatedPackageArchive>::Failure(files.ErrorValue());
        }
        auto manifest = MatchInventory(zip.value, files.Value(), limits);
        if (manifest.HasError()) {
            return Result<ValidatedPackageArchive>::Failure(manifest.ErrorValue());
        }
        auto packageManifestDigest = ReadPackageManifestDigest(zip.value, files.Value());
        if (packageManifestDigest.HasError()) {
            return Result<ValidatedPackageArchive>::Failure(packageManifestDigest.ErrorValue());
        }
        const auto digest = ComputeSha256(snapshot);
        return Result<ValidatedPackageArchive>::Success(
            ValidatedPackageArchive{std::move(snapshot), std::move(manifest).Value(), digest, packageManifestDigest.Value()});
    }

    /** @copydoc ValidatedPackageArchive::ValidatedPackageArchive */
    ValidatedPackageArchive::ValidatedPackageArchive(std::vector<std::byte> bytes, ValidatedPackageFileManifestV1 manifest,
                                                     const Sha256Digest &digest, const Sha256Digest &packageManifestDigest)
        : m_bytes(std::move(bytes)), m_manifest(std::move(manifest)), m_digest(digest), m_packageManifestDigest(packageManifestDigest) {}

    /** @copydoc ValidatedPackageArchive::Manifest */
    const ValidatedPackageFileManifestV1 &ValidatedPackageArchive::Manifest() const noexcept {
        return m_manifest;
    }

    /** @copydoc ValidatedPackageArchive::Bytes */
    std::span<const std::byte> ValidatedPackageArchive::Bytes() const noexcept {
        return m_bytes;
    }

    /** @copydoc ValidatedPackageArchive::Digest */
    const Sha256Digest &ValidatedPackageArchive::Digest() const noexcept {
        return m_digest;
    }

    /** @copydoc ValidatedPackageArchive::PackageManifestDigest */
    const Sha256Digest &ValidatedPackageArchive::PackageManifestDigest() const noexcept {
        return m_packageManifestDigest;
    }
}  // namespace Horo::Packages
