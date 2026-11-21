#include "OpenXRWindowsLoaderImage.h"

#include "Horo/XR/XRErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>

namespace Horo::Application::Internal {
    namespace {
        using Bytes = std::span<const std::byte>;
        constexpr std::size_t ImageHeaderOffset = 0x3c;
        constexpr std::size_t PeHeaderBytes = 24;
        constexpr std::size_t DirectoryBase = 112;
        constexpr std::size_t DirectoryBytes = 8;
        constexpr std::size_t SectionBytes = 40;
        constexpr std::size_t ImportBytes = 20;
        constexpr std::size_t MaximumDllNameBytes = 128;
        constexpr std::size_t MaximumSections = 96;

        /** @brief Reads an unaligned little-endian scalar from a previously admitted fixed record. */
        std::uint32_t LittleEndian(Bytes record, std::size_t offset, std::size_t width = 4) noexcept {
            std::uint32_t value{};
            for (std::size_t index = 0; index < width; ++index)
                value |= std::to_integer<std::uint32_t>(record[offset + index]) << (8 * index);
            return value;
        }

        /** @brief Tests a file or image slice without evaluating an overflowing endpoint. */
        constexpr bool Fits(std::size_t size, std::size_t offset, std::size_t count) noexcept {
            return offset <= size && count <= size - offset;
        }

        /** @brief Folds the case-insensitive ASCII basename vocabulary without locale or allocation. */
        constexpr char LowerAscii(char c) noexcept {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        }

        /** @brief Compares the canonical Windows case-insensitive names using the same portable grammar. */
        bool SameName(std::string_view first, std::string_view second) noexcept {
            return std::ranges::equal(first, second, std::ranges::equal_to{}, LowerAscii, LowerAscii);
        }

        /** @brief Carries the exact bounded file and section-table evidence for image RVA resolution. */
        struct ImportImage {
            Bytes image;
            Bytes sections;
            Bytes directories;
            std::uint32_t headersSize;
            std::uint32_t imageSize;
        };

        /** @brief Verifies physical section headers and the closed PE32+ AMD64 DLL optional-header shape. */
        std::optional<ImportImage> ReadImage(Bytes image) noexcept {
            constexpr std::size_t optionalMinimum = DirectoryBase + 16 * DirectoryBytes;
            if (image.size() < ImageHeaderOffset + 4 || image.size() > MaximumOpenXRLoaderImageBytes || LittleEndian(image, 0, 2) != 0x5a4d)
                return std::nullopt;
            const std::size_t pe = LittleEndian(image, ImageHeaderOffset);
            if (pe < ImageHeaderOffset + 4 || !Fits(image.size(), pe, PeHeaderBytes) || LittleEndian(image, pe) != 0x00004550)
                return std::nullopt;
            const auto coff = image.subspan(pe, PeHeaderBytes);
            const auto sections = LittleEndian(coff, 6, 2);
            const auto optionalBytes = LittleEndian(coff, 20, 2);
            constexpr unsigned executableDll = 0x2002;
            if (LittleEndian(coff, 4, 2) != 0x8664 || (LittleEndian(coff, 22, 2) & executableDll) != executableDll || sections == 0 ||
                sections > MaximumSections || optionalBytes < optionalMinimum)
                return std::nullopt;
            const auto metadata = pe + PeHeaderBytes;
            if (!Fits(image.size(), metadata, optionalBytes + sections * SectionBytes))
                return std::nullopt;
            const auto optional = image.subspan(metadata, optionalBytes);
            const auto headersSize = LittleEndian(optional, 60);
            const auto imageSize = LittleEndian(optional, 56);
            if (LittleEndian(optional, 0, 2) != 0x20b || LittleEndian(optional, 108) != 16 ||
                headersSize < metadata + optionalBytes + sections * SectionBytes || headersSize > image.size() || headersSize > imageSize)
                return std::nullopt;
            return ImportImage{image, image.subspan(metadata + optionalBytes, sections * SectionBytes),
                               optional.subspan(DirectoryBase, 16 * DirectoryBytes), headersSize, imageSize};
        }

        /** @brief Extracts fixed virtual and file ranges from an admitted section header. */
        struct Section final {
            std::size_t address, virtualBytes, rawStart, rawBytes;

            explicit Section(Bytes record) noexcept
                : address(LittleEndian(record, 12)), virtualBytes(LittleEndian(record, 8)), rawStart(LittleEndian(record, 20)),
                  rawBytes(LittleEndian(record, 16)) {}

            [[nodiscard]] std::size_t ImageBytes() const noexcept {
                return std::max(virtualBytes, rawBytes);
            }
        };

        /** @brief Compares ranges without overflow; validated sections already fit their corresponding images. */
        bool Overlap(std::size_t first, std::size_t firstBytes, std::size_t second, std::size_t secondBytes) noexcept {
            return firstBytes != 0 && secondBytes != 0 && (first <= second ? second - first < firstBytes : first - second < secondBytes);
        }

        /** @brief Rejects virtual aliases, overlapping raw ranges and sections outside the admitted header/image. */
        bool ValidSections(const ImportImage &source) noexcept {
            for (std::size_t offset = 0; offset < source.sections.size(); offset += SectionBytes) {
                const Section section{source.sections.subspan(offset, SectionBytes)};
                if (section.address < source.headersSize || section.ImageBytes() == 0 ||
                    !Fits(source.imageSize, section.address, section.ImageBytes()))
                    return false;
                if (section.rawBytes != 0 &&
                    (section.rawStart < source.headersSize || !Fits(source.image.size(), section.rawStart, section.rawBytes)))
                    return false;
                for (std::size_t prior = 0; prior < offset; prior += SectionBytes) {
                    const Section earlier{source.sections.subspan(prior, SectionBytes)};
                    if (Overlap(earlier.address, earlier.ImageBytes(), section.address, section.ImageBytes()) ||
                        Overlap(earlier.rawStart, earlier.rawBytes, section.rawStart, section.rawBytes))
                        return false;
                }
            }
            return true;
        }

        /** @brief Resolves an RVA only to the initialized file-backed range of a previously validated section. */
        Bytes SectionFromRva(const ImportImage &source, std::uint32_t rva) noexcept {
            for (std::size_t offset = 0; offset < source.sections.size(); offset += SectionBytes) {
                const Section section{source.sections.subspan(offset, SectionBytes)};
                if (rva < section.address)
                    continue;
                const auto displacement = rva - section.address;
                const auto mapped = std::min(section.virtualBytes, section.rawBytes);
                if (displacement < mapped)
                    return source.image.subspan(section.rawStart + displacement, mapped - displacement);
            }
            return {};
        }

        /** @brief Reads the bounded NUL-terminated module basename from the approved initialized section. */
        std::string_view ImportName(const ImportImage &source, std::uint32_t rva) noexcept {
            const auto bytes = SectionFromRva(source, rva);
            const auto name = bytes.first(std::min(bytes.size(), MaximumDllNameBytes + 1));
            const auto end = std::ranges::find(name, std::byte{});
            if (name.empty() || end == name.end())
                return {};
            return {std::bit_cast<const char *>(name.data()), static_cast<std::size_t>(end - name.begin())};
        }

        /** @brief Derives only the offered reviewed normal-import directory for this closed deployment profile. */
        Bytes ImportDirectory(const ImportImage &source) noexcept {
            constexpr auto importIndex = 1;
            const auto offset = importIndex * DirectoryBytes;
            const auto count = LittleEndian(source.directories, offset + 4);
            const auto bytes = SectionFromRva(source, LittleEndian(source.directories, offset));
            return count >= 2 * ImportBytes && count <= (MaximumOpenXRAbiDependencies + 1) * ImportBytes && count % ImportBytes == 0 &&
                           count <= bytes.size()
                       ? bytes.first(count)
                       : Bytes{};
        }

        /** @brief Rejects auxiliary import mechanisms that could bypass the signed direct inventory. */
        bool HasUnsupportedDirectories(Bytes directories) noexcept {
            for (const auto index : {11U, 13U, 14U}) {
                const auto offset = index * DirectoryBytes;
                if (LittleEndian(directories, offset) != 0 || LittleEndian(directories, offset + 4) != 0)
                    return true;
            }
            return false;
        }

        /** @brief Matches each descriptor exactly once and requires the bounded table's null terminator. */
        bool MatchesImports(const ImportImage &source, std::span<const std::string> expected) noexcept {
            const auto descriptors = ImportDirectory(source);
            if (descriptors.size() != (expected.size() + 1) * ImportBytes)
                return false;
            std::array<bool, MaximumOpenXRAbiDependencies> seen{};
            for (std::size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
                const auto descriptor = descriptors.subspan(ordinal * ImportBytes, ImportBytes);
                if (LittleEndian(descriptor, 4) != 0 || SectionFromRva(source, LittleEndian(descriptor, 16)).size() < 8)
                    return false;
                const auto name = ImportName(source, LittleEndian(descriptor, 12));
                if (!IsWindowsOpenXRAbiDll(name))
                    return false;
                const auto found = std::ranges::find_if(expected, [name](const auto &expectedName) {
                    return SameName(name, expectedName);
                });
                if (found == expected.end())
                    return false;
                const auto index = static_cast<std::size_t>(found - expected.begin());
                if (seen[index])
                    return false;
                seen[index] = true;
            }
            return std::ranges::all_of(descriptors.last(ImportBytes), [](std::byte value) {
                return value == std::byte{};
            });
        }
    }  // namespace

    /** @copydoc IsWindowsOpenXRAbiDll */
    bool IsWindowsOpenXRAbiDll(std::string_view name) noexcept {
        if (name.size() <= 4 || name.size() > MaximumDllNameBytes || !SameName(name.substr(name.size() - 4), ".dll"))
            return false;
        return std::ranges::all_of(name.substr(0, name.size() - 4), [](char c) {
            const auto lower = LowerAscii(c);
            return (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') || lower == '-' || lower == '_';
        });
    }

    /** @copydoc VerifyWindowsOpenXRLoaderImage */
    Result<void> VerifyWindowsOpenXRLoaderImage(Bytes bytes, std::string_view architecture, std::span<const std::string> signedImports) {
        if (architecture != "x64")
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnsupported, "Windows OpenXR import verification requires x64."));
        const auto image = ReadImage(bytes);
        if (!image || !ValidSections(*image))
            return Result<void>::Failure(MakeError(XR::XRErrors::LoaderIncompatible, "Expected a bounded Windows AMD64 PE32+ DLL image."));
        if (HasUnsupportedDirectories(image->directories))
            return Result<void>::Failure(
                MakeError(XR::XRErrors::OperationUnsupported, "Windows OpenXR requires ordinary unbound native imports."));
        if (signedImports.empty() || signedImports.size() > MaximumOpenXRAbiDependencies || !MatchesImports(*image, signedImports))
            return Result<void>::Failure(
                MakeError(XR::XRErrors::RuntimeRejected, "Windows loader imports differ from the signed OS ABI inventory."));
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
