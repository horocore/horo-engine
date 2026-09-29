#include "UpdateTarGzipIndex.h"

#include "Horo/Release/UpdateStageReady.h"
#include "Horo/Release/UpdateTransferErrors.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <miniz.h>
#include <string>
#include <system_error>
#include <vector>

namespace Horo::Release {
    namespace {
        constexpr std::size_t TarBlockBytes = 512U;
        constexpr std::size_t StreamBlockBytes = 64U * 1024U;

        /** @brief Parses a canonical octal header field without accepting base-256 or overflow. */
        [[nodiscard]] bool ParseOctal(const std::span<const unsigned char> field, std::uint64_t &value) {
            value = 0U;
            bool hasDigit = false;
            bool ended = false;
            for (const unsigned char character : field) {
                if (character >= '0' && character <= '7') {
                    if (ended || value > (std::numeric_limits<std::uint64_t>::max() - 7U) / 8U)
                        return false;
                    value = value * 8U + static_cast<std::uint64_t>(character - '0');
                    hasDigit = true;
                } else if (character == 0U || character == ' ') {
                    ended |= hasDigit;
                } else {
                    return false;
                }
            }
            return hasDigit;
        }

        /** @brief Reads a zero-padded ustar text field without accepting embedded NUL aliases. */
        [[nodiscard]] bool ReadName(const std::span<const unsigned char> field, std::string &name) {
            const auto end = std::ranges::find(field, 0U);
            if (std::ranges::any_of(end, field.end(), [](const unsigned char value) {
                return value != 0U;
            }))
                return false;
            name.assign(field.begin(), end);
            return true;
        }

        class TarIndex final {
        public:
            TarIndex(const UpdateArchiveLimits &limits, const Detail::TarPayloadCallback &callback)
                : limits_(limits), callback_(callback) {}

            /** @brief Consumes a bounded decompressed chunk in tar-block order. */
            [[nodiscard]] bool Feed(const std::span<const unsigned char> bytes) {
                std::size_t offset = 0U;
                while (offset < bytes.size()) {
                    if (segment_ == Segment::Finished) {
                        if (bytes[offset++] != 0U)
                            return false;
                    } else if (segment_ == Segment::Header) {
                        if (!FeedHeader(bytes, offset))
                            return false;
                    } else if (!FeedPayload(bytes, offset)) {
                        return false;
                    }
                }
                return true;
            }

            /** @brief Requires a complete ustar terminator and shared archive-path admission. */
            [[nodiscard]] Result<std::vector<UpdateArchiveEntry>> Finish() && {
                if (segment_ != Segment::Finished || entries_.empty() ||
                    std::ranges::none_of(entries_, [](const UpdateArchiveEntry &entry) {
                    return entry.path == UpdateFileInventoryPath && entry.kind == UpdateArchiveEntryKind::File;
                }))
                    return Result<std::vector<UpdateArchiveEntry>>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
                if (auto checked = ValidateUpdateArchiveIndex(entries_, limits_); checked.HasError())
                    return Result<std::vector<UpdateArchiveEntry>>::Failure(checked.ErrorValue());
                return Result<std::vector<UpdateArchiveEntry>>::Success(std::move(entries_));
            }

        private:
            enum class Segment : std::uint8_t {
                Header,
                Body,
                Padding,
                Finished
            };

            /** @brief Completes one ustar header block without recursing into the stream loop. */
            [[nodiscard]] bool FeedHeader(const std::span<const unsigned char> bytes, std::size_t &offset) {
                const auto count = std::min(TarBlockBytes - headerUsed_, bytes.size() - offset);
                std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), count,
                            header_.begin() + static_cast<std::ptrdiff_t>(headerUsed_));
                offset += count;
                headerUsed_ += count;
                if (headerUsed_ != TarBlockBytes)
                    return true;
                headerUsed_ = 0U;
                if (std::ranges::all_of(header_, [](const unsigned char value) {
                    return value == 0U;
                })) {
                    ++zeroBlocks_;
                    if (zeroBlocks_ == 2U)
                        segment_ = Segment::Finished;
                    return true;
                }
                return zeroBlocks_ == 0U && AcceptHeader();
            }

            /** @brief Advances ordinary payload or validates zero padding. */
            [[nodiscard]] bool FeedPayload(const std::span<const unsigned char> bytes, std::size_t &offset) {
                using enum Segment;
                auto &remaining = segment_ == Body ? bodyRemaining_ : paddingRemaining_;
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, bytes.size() - offset));
                if (segment_ == Body && callback_ &&
                    !callback_(entries_.back(), bytes.subspan(offset, count), entries_.back().expandedBytes - remaining))
                    return false;
                if (segment_ == Padding && std::ranges::any_of(bytes.subspan(offset, count), [](const unsigned char value) {
                    return value != 0U;
                }))
                    return false;
                remaining -= count;
                offset += count;
                if (remaining == 0U)
                    segment_ = segment_ == Body && paddingRemaining_ != 0U ? Padding : Header;
                return true;
            }

            /** @brief Accepts only ordinary ustar files and directories with valid checksums. */
            [[nodiscard]] bool AcceptHeader() {
                if (entries_.size() >= limits_.maximumEntries || !std::equal(header_.begin() + 257, header_.begin() + 263, "ustar\0") ||
                    header_[263] != '0' || header_[264] != '0' ||
                    std::ranges::any_of(std::span{header_}.subspan(157, 100), [](const unsigned char value) {
                    return value != 0U;
                }))
                    return false;
                std::uint64_t expectedChecksum{};
                std::uint64_t size{};
                if (std::uint64_t mode{}; !ParseOctal(std::span{header_}.subspan(148, 8), expectedChecksum) ||
                                          !ParseOctal(std::span{header_}.subspan(124, 12), size) ||
                                          !ParseOctal(std::span{header_}.subspan(100, 8), mode) || (mode & 07000U) != 0U)
                    return false;
                std::uint64_t checksum = 0U;
                for (std::size_t index = 0U; index < TarBlockBytes; ++index)
                    checksum += index >= 148U && index < 156U ? static_cast<std::uint64_t>(' ') : header_[index];
                if (checksum != expectedChecksum)
                    return false;
                std::string name;
                std::string prefix;
                if (!ReadName(std::span{header_}.first(100), name) || !ReadName(std::span{header_}.subspan(345, 155), prefix) ||
                    name.empty())
                    return false;
                if (!prefix.empty())
                    name = prefix + '/' + name;
                const auto type = header_[156];
                const bool directory = type == '5';
                if (type != '0' && type != 0U && !directory)
                    return false;
                if (size > limits_.maximumFileBytes)
                    return false;
                if (directory) {
                    if (size != 0U || !name.ends_with('/'))
                        return false;
                    name.pop_back();
                } else if (name.ends_with('/')) {
                    return false;
                }
                entries_.emplace_back(std::move(name), directory ? UpdateArchiveEntryKind::Directory : UpdateArchiveEntryKind::File, size);
                bodyRemaining_ = size;
                paddingRemaining_ = (TarBlockBytes - size % TarBlockBytes) % TarBlockBytes;
                segment_ = bodyRemaining_ != 0U ? Segment::Body : Segment::Header;
                return true;
            }

            const UpdateArchiveLimits &limits_;
            const Detail::TarPayloadCallback &callback_;
            std::vector<UpdateArchiveEntry> entries_;
            std::array<unsigned char, TarBlockBytes> header_{};
            Segment segment_{Segment::Header};
            std::size_t headerUsed_{};
            std::uint64_t bodyRemaining_{};
            std::uint64_t paddingRemaining_{};
            unsigned zeroBlocks_{};
        };

        struct InflateGuard final {
            mz_stream stream{};

            InflateGuard() = default;
            InflateGuard(const InflateGuard &) = delete;
            InflateGuard &operator=(const InflateGuard &) = delete;
            InflateGuard(InflateGuard &&) = delete;
            InflateGuard &operator=(InflateGuard &&) = delete;

            ~InflateGuard() {
                mz_inflateEnd(&stream);
            }
        };

        /** @brief Rejects unbounded expansion before allocating or indexing archive entries. */
        [[nodiscard]] bool WithinTarBudget(const UpdateArchiveLimits &limits, const std::uint64_t produced) {
            constexpr auto max = std::numeric_limits<std::uint64_t>::max();
            if (limits.maximumEntries > (max - 1024U) / 1024U)
                return false;
            const auto overhead = limits.maximumEntries * 1024U + 1024U;
            return limits.maximumExpandedBytes <= max - overhead && produced <= limits.maximumExpandedBytes + overhead;
        }

        /** @brief Streams raw DEFLATE between strict gzip header and trailer boundaries. */
        [[nodiscard]] Result<std::vector<UpdateArchiveEntry>> ReadIndex(const std::filesystem::path &path,
                                                                        const UpdateArchiveLimits &limits,
                                                                        const Detail::TarPayloadCallback &callback) {
            const auto invalid = [] {
                return Result<std::vector<UpdateArchiveEntry>>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            };
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size < 18U || size > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
                return invalid();
            std::ifstream input(path, std::ios::binary);
            std::array<unsigned char, 10> header{};
            input.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
            if (!input || header[0] != 0x1fU || header[1] != 0x8bU || header[2] != 8U || header[3] != 0U)
                return invalid();
            std::array<unsigned char, 8> trailer{};
            input.seekg(static_cast<std::streamoff>(size - trailer.size()));
            input.read(reinterpret_cast<char *>(trailer.data()), static_cast<std::streamsize>(trailer.size()));
            if (!input)
                return invalid();
            input.seekg(static_cast<std::streamoff>(header.size()));
            InflateGuard inflater;
            if (mz_inflateInit2(&inflater.stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
                return invalid();
            std::array<unsigned char, StreamBlockBytes> compressed{};
            std::array<unsigned char, StreamBlockBytes> expanded{};
            std::uint64_t compressedRemaining = size - header.size() - trailer.size();
            std::uint64_t expandedBytes = 0U;
            mz_ulong crc = MZ_CRC32_INIT;
            TarIndex index(limits, callback);
            int status = MZ_OK;
            while (status != MZ_STREAM_END) {
                if (inflater.stream.avail_in == 0U && compressedRemaining != 0U) {
                    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(compressedRemaining, compressed.size()));
                    input.read(reinterpret_cast<char *>(compressed.data()), static_cast<std::streamsize>(count));
                    if (!input)
                        return invalid();
                    compressedRemaining -= count;
                    inflater.stream.next_in = compressed.data();
                    inflater.stream.avail_in = static_cast<mz_uint>(count);
                }
                inflater.stream.next_out = expanded.data();
                inflater.stream.avail_out = static_cast<mz_uint>(expanded.size());
                const auto before = inflater.stream.avail_in;
                status = mz_inflate(&inflater.stream, MZ_NO_FLUSH);
                const auto produced = expanded.size() - inflater.stream.avail_out;
                if ((status != MZ_OK && status != MZ_STREAM_END) || (before == inflater.stream.avail_in && produced == 0U) ||
                    produced > std::numeric_limits<std::uint64_t>::max() - expandedBytes ||
                    !WithinTarBudget(limits, expandedBytes + produced) || !index.Feed(std::span{expanded}.first(produced)))
                    return invalid();
                crc = mz_crc32(crc, expanded.data(), produced);
                expandedBytes += produced;
            }
            if (compressedRemaining != 0U || inflater.stream.avail_in != 0U ||
                static_cast<std::uint32_t>(expandedBytes) !=
                    (static_cast<std::uint32_t>(trailer[4]) | static_cast<std::uint32_t>(trailer[5]) << 8U |
                     static_cast<std::uint32_t>(trailer[6]) << 16U | static_cast<std::uint32_t>(trailer[7]) << 24U) ||
                static_cast<std::uint32_t>(crc) !=
                    (static_cast<std::uint32_t>(trailer[0]) | static_cast<std::uint32_t>(trailer[1]) << 8U |
                     static_cast<std::uint32_t>(trailer[2]) << 16U | static_cast<std::uint32_t>(trailer[3]) << 24U))
                return invalid();
            return std::move(index).Finish();
        }
    }  // namespace

    /** @copydoc Detail::ReadTarGzipIndex */
    Result<std::vector<UpdateArchiveEntry>> Detail::ReadTarGzipIndex(const std::filesystem::path &path, const UpdateArchiveLimits &limits,
                                                                     const TarPayloadCallback &callback) {
        return ReadIndex(path, limits, callback);
    }

    /** @copydoc IndexVerifiedTarGzipPackage */
    Result<std::vector<UpdateArchiveEntry>> IndexVerifiedTarGzipPackage(const UpdatePackageRecord &package,
                                                                        const UpdateTransferCheckpoint &checkpoint,
                                                                        const std::filesystem::path &packageFile,
                                                                        const UpdateArchiveLimits &limits,
                                                                        const Security::ArtifactVerifier &verifier) {
        if (package.selection.format != DistributionPackageFormat::TarGzip ||
            package.selection.artifact.platform != DistributionPlatform::Linux ||
            package.selection.artifact.artifactClass != DistributionArtifactClass::InstallableProduct)
            return Result<std::vector<UpdateArchiveEntry>>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
        if (auto selection = ValidateDistributionPackageSelection(package.selection.artifact, package.selection.format);
            selection.HasError() || selection.Value() != package.selection)
            return Result<std::vector<UpdateArchiveEntry>>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
        if (auto verified = VerifyCompletedUpdateTransfer(package, checkpoint, packageFile, verifier); verified.HasError())
            return Result<std::vector<UpdateArchiveEntry>>::Failure(verified.ErrorValue());
        return Detail::ReadTarGzipIndex(packageFile, limits);
    }
}  // namespace Horo::Release
