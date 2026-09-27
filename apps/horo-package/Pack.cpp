#include "Horo/Foundation/Sha256.h"
#include "Horo/Packages/PackagePath.h"
#include "PackageCommand.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <string_view>
#include <system_error>

namespace Horo::PackageCommand {
    namespace {
        struct File final {
            std::string path;
            std::vector<std::byte> bytes;
            bool executable{};
        };

        [[nodiscard]] bool IsWithin(const std::filesystem::path &root, const std::filesystem::path &path) {
            const auto relative = path.lexically_relative(root);
            return !relative.empty() && *relative.begin() != ".." && !relative.is_absolute();
        }

        [[nodiscard]] bool ContainsPrivateKeyPem(const std::vector<std::byte> &bytes) {
            if (bytes.empty())
                return false;
            constexpr std::string_view prefix = "-----BEGIN ";
            constexpr std::string_view suffix = "-----";
            constexpr std::array<std::string_view, 5> keyTypes{"PRIVATE KEY", "EC PRIVATE KEY", "RSA PRIVATE KEY", "ENCRYPTED PRIVATE KEY",
                                                               "OPENSSH PRIVATE KEY"};
            const auto content = std::span{bytes};
            const auto startsWith = [](const std::span<const std::byte> input, const std::string_view text) {
                return input.size() >= text.size() && std::ranges::equal(input.first(text.size()), std::as_bytes(std::span{text}));
            };
            for (std::size_t offset = 0; offset + prefix.size() <= content.size(); ++offset) {
                if (!startsWith(content.subspan(offset), prefix))
                    continue;
                const auto following = content.subspan(offset + prefix.size());
                if (std::ranges::any_of(keyTypes, [&](const std::string_view type) {
                    return startsWith(following, type) && startsWith(following.subspan(type.size()), suffix);
                }))
                    return true;
            }
            return false;
        }

        [[nodiscard]] Outcome ValidateOutputLocation(const std::filesystem::path &root, const std::filesystem::path &output) {
            std::error_code error;
            const auto canonicalRoot = std::filesystem::canonical(root, error);
            if (error)
                return Failure("package.root_invalid", "Package root could not be resolved.");
            const auto absoluteOutput = std::filesystem::absolute(output, error);
            if (error)
                return Failure("package.output_unwritable", "Output location could not be resolved.");
            if (const auto outputParent = std::filesystem::weakly_canonical(absoluteOutput.parent_path(), error);
                error || IsWithin(canonicalRoot, outputParent / absoluteOutput.filename()))
                return Failure("package.output_inside_root", "Output must be outside the package root.");
            return Success();
        }

        [[nodiscard]] Outcome Gather(const std::filesystem::path &root, const std::filesystem::path &output, std::vector<File> &files) {
            std::error_code error;
            if (const auto rootStatus = std::filesystem::symlink_status(root, error);
                error || !std::filesystem::is_directory(rootStatus) || std::filesystem::is_symlink(rootStatus))
                return Failure("package.root_invalid", "Package root must be a real directory.");
            if (const auto checked = ValidateOutputLocation(root, output); !checked.success)
                return checked;
            std::uint64_t total{};
            bool hasManifest{};
            for (std::filesystem::recursive_directory_iterator iterator{root, error}, end; !error && iterator != end;
                 iterator.increment(error)) {
                const auto status = iterator->symlink_status(error);
                if (error || std::filesystem::is_symlink(status) ||
                    (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)))
                    return Failure("package.input_unsafe", "Package root contains a symlink or unsupported entry.");
                if (std::filesystem::is_directory(status))
                    continue;
                if (files.size() >= 4095U)
                    return Failure("package.input_limit", "Package has too many files.");
                const auto utf8Name = iterator->path().lexically_relative(root).generic_u8string();
                std::string name(utf8Name.size(), '\0');
                std::ranges::transform(utf8Name, name.begin(), [](const char8_t character) {
                    return static_cast<char>(character);
                });
                if (const auto parsed = Packages::PackagePath::Parse(name); parsed.HasError() || name == "files.manifest.json")
                    return Failure("package.path_invalid", "Package contains a noncanonical or reserved path.");
                const auto size = iterator->file_size(error);
                if (error || size > MaximumArtifactBytes || size > 256U * 1024U * 1024U - total)
                    return Failure("package.input_limit", "Package input exceeds bounded file or total size.");
                std::vector<std::byte> bytes;
                if (const auto read = ReadBounded(iterator->path(), MaximumArtifactBytes, bytes); !read.success)
                    return read;
                if (ContainsPrivateKeyPem(bytes))
                    return Failure("package.credential_input", "Package root contains private-key material.");
                const auto permissions = status.permissions();
                const auto execute =
                    std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
                files.emplace_back(File{name, std::move(bytes), (permissions & execute) != std::filesystem::perms::none});
                total += size;
                hasManifest |= name == "horo-package.toml";
            }
            if (error)
                return Failure("package.input_unreadable", "Package root traversal failed.");
            if (!hasManifest)
                return Failure("package.manifest_missing", "horo-package.toml is required.");
            std::ranges::sort(files, {}, &File::path);
            return Success();
        }

        [[nodiscard]] std::string Inventory(const std::vector<File> &files) {
            nlohmann::json entries = nlohmann::json::array();
            for (const File &file : files)
                entries.push_back({{"path", file.path},
                                   {"size", file.bytes.size()},
                                   {"sha256", FormatSha256(ComputeSha256(file.bytes))},
                                   {"executable", file.executable},
                                   {"contributionRoot", nullptr}});
            return nlohmann::json{{"schemaVersion", 1}, {"files", std::move(entries)}}.dump();
        }

        void Write32(std::vector<std::byte> &bytes, const std::size_t offset, const std::uint32_t value) {
            for (unsigned shift = 0; shift != 32; shift += 8)
                bytes[offset + shift / 8] = static_cast<std::byte>((value >> shift) & 0xffU);
        }

        [[nodiscard]] Outcome SetModes(std::vector<std::byte> &bytes, const std::vector<File> &files) {
            mz_zip_archive reader{};
            if (!mz_zip_reader_init_mem(&reader, bytes.data(), bytes.size(), 0))
                return Failure("package.archive_invalid", "Packed archive could not be inspected.");
            bool valid = mz_zip_reader_get_num_files(&reader) == files.size() + 1;
            for (mz_uint index = 0; index < files.size(); ++index) {
                if (!valid)
                    break;
                mz_zip_archive_file_stat stat{};
                valid = mz_zip_reader_file_stat(&reader, index, &stat);
                if (!valid)
                    break;
                const auto offset = static_cast<std::size_t>(reader.m_central_directory_file_ofs + stat.m_central_dir_ofs + 38U);
                valid = offset <= bytes.size() && bytes.size() - offset >= 4U;
                if (valid)
                    Write32(bytes, offset, (files[index].executable ? 0100755U : 0100644U) << 16U);
            }
            mz_zip_reader_end(&reader);
            return valid ? Success() : Failure("package.archive_invalid", "Packed file metadata could not be finalized.");
        }

        [[nodiscard]] Outcome MakeArchive(const std::vector<File> &files, std::vector<std::byte> &bytes) {
            const std::string inventory = Inventory(files);
            mz_zip_archive writer{};
            if (!mz_zip_writer_init_heap(&writer, 0, 0))
                return Failure("package.pack_failed", "Archive writer could not start.");
            bool valid = true;
            for (const File &file : files)
                valid &= mz_zip_writer_add_mem(&writer, file.path.c_str(), file.bytes.data(), file.bytes.size(), MZ_BEST_COMPRESSION) != 0;
            valid &= mz_zip_writer_add_mem(&writer, "files.manifest.json", inventory.data(), inventory.size(), MZ_BEST_COMPRESSION) != 0;
            void *buffer{};
            std::size_t size{};
            valid &= mz_zip_writer_finalize_heap_archive(&writer, &buffer, &size) != 0;
            const std::unique_ptr<void, void (*)(void *)> owned{buffer, [](void *memory) {
                std::free(memory);
            }};
            if (valid && size <= MaximumArtifactBytes) {
                const auto *first = static_cast<const std::byte *>(buffer);
                bytes.assign(first, first + size);
            } else {
                valid = false;
            }
            mz_zip_writer_end(&writer);
            if (!valid)
                return Failure("package.pack_failed", "Archive assembly failed or exceeded the package limit.");
            return SetModes(bytes, files);
        }
    }  // namespace

    Outcome Pack(const std::filesystem::path &root, const std::filesystem::path &output) {
        std::vector<File> files;
        if (const auto gathered = Gather(root, output, files); !gathered.success)
            return gathered;
        std::vector<std::byte> bytes;
        if (const auto packed = MakeArchive(files, bytes); !packed.success)
            return packed;
        std::optional<Packages::ValidatedPackageArchive> verified;
        if (const auto checked = VerifyArchive(bytes, verified); !checked.success)
            return checked;
        if (const auto written = WriteNew(output, bytes); !written.success)
            return written;
        return Success(std::format("packed {} files; sha256={}", files.size(), FormatSha256(verified->Digest())));
    }
}  // namespace Horo::PackageCommand
