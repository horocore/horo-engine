#include "Horo/Editor/ProjectMigrationTransaction.h"

#include "../../application/project/ProjectErrors.h"
#include "GeneratedProjectCompatibility.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <unordered_set>

namespace Horo::Editor {
    namespace {
        using json = nlohmann::json;
        using namespace Application;

        struct Record {
            std::string path;
            PreparedMigrationChangeKind kind{PreparedMigrationChangeKind::Replace};
            MigrationContentHash originalHash{};
            MigrationContentHash stagedHash{};
            bool originalExists{};
            bool completed{};
        };

        struct Journal {
            std::string operationId;
            std::string state{"Preparing"};
            std::string writerVersion;
            std::string recoveryContract;
            std::string sourceVersion;
            std::string targetVersion;
            std::string projectId;
            std::string engineBuildIdentity;
            std::vector<Record> records;
        };

        [[nodiscard]] Error Failure(const ErrorCodeDescriptor &code, std::string message) {
            return MakeError(code, std::move(message));
        }

        [[nodiscard]] constexpr std::uint64_t SaturatingAdd(const std::uint64_t left, const std::uint64_t right) noexcept {
            return left > std::numeric_limits<std::uint64_t>::max() - right ? std::numeric_limits<std::uint64_t>::max() : left + right;
        }

        [[nodiscard]] constexpr std::uint64_t SaturatingMultiply(const std::uint64_t left, const std::uint64_t right) noexcept {
            return right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right ? std::numeric_limits<std::uint64_t>::max()
                                                                                          : left * right;
        }

        [[nodiscard]] const char *RecoveryActionName(const MigrationRecoveryAction action) noexcept {
            using enum MigrationRecoveryAction;
            switch (action) {
                case None:
                    return "none";
                case DiscardUnpublishedStaging:
                    return "discard_unpublished_staging";
                case ResumePublish:
                    return "resume_publish";
                case RestoreOriginals:
                    return "restore_originals";
                case FinalizeCommittedMigration:
                    return "finalize_committed_migration";
                case Unrecoverable:
                    return "unrecoverable";
            }
            return "unknown";
        }

        class Sha256 {
        public:
            void Update(const std::span<const std::byte> bytes) {
                for (const std::byte value : bytes) {
                    buffer_[bufferSize_++] = static_cast<std::uint8_t>(value);
                    bitCount_ += 8;
                    if (bufferSize_ == 64) {
                        Transform();
                        bufferSize_ = 0;
                    }
                }
            }

            [[nodiscard]] MigrationContentHash Final() {
                const std::uint64_t originalBits = bitCount_;
                buffer_[bufferSize_++] = 0x80;
                if (bufferSize_ > 56) {
                    while (bufferSize_ < 64)
                        buffer_[bufferSize_++] = 0;
                    Transform();
                    bufferSize_ = 0;
                }
                while (bufferSize_ < 56)
                    buffer_[bufferSize_++] = 0;
                for (int shift = 56; shift >= 0; shift -= 8)
                    buffer_[bufferSize_++] = static_cast<std::uint8_t>(originalBits >> shift);
                Transform();
                MigrationContentHash result;
                for (std::size_t i = 0; i < 8; ++i)
                    for (std::size_t j = 0; j < 4; ++j)
                        result.bytes[i * 4 + j] = static_cast<std::uint8_t>(state_[i] >> (24U - 8U * j));
                return result;
            }

        private:
            static constexpr std::array<std::uint32_t, 64> K{0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
                                                             0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
                                                             0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
                                                             0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                                                             0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
                                                             0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
                                                             0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
                                                             0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                                                             0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
                                                             0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
                                                             0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

            void Transform() {
                std::array<std::uint32_t, 64> w{};
                for (std::size_t i = 0; i < 16; ++i)
                    w[i] = (std::uint32_t(buffer_[i * 4]) << 24U) | (std::uint32_t(buffer_[i * 4 + 1]) << 16U) |
                           (std::uint32_t(buffer_[i * 4 + 2]) << 8U) | buffer_[i * 4 + 3];
                for (std::size_t i = 16; i < 64; ++i) {
                    const auto s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
                    const auto s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                auto [a, b, c, d, e, f, g, h] = state_;
                for (std::size_t i = 0; i < 64; ++i) {
                    const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
                    const auto ch = (e & f) ^ ((~e) & g);
                    const auto t1 = h + s1 + ch + K[i] + w[i];
                    const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
                    const auto maj = (a & b) ^ (a & c) ^ (b & c);
                    const auto t2 = s0 + maj;
                    h = g;
                    g = f;
                    f = e;
                    e = d + t1;
                    d = c;
                    c = b;
                    b = a;
                    a = t1 + t2;
                }
                state_[0] += a;
                state_[1] += b;
                state_[2] += c;
                state_[3] += d;
                state_[4] += e;
                state_[5] += f;
                state_[6] += g;
                state_[7] += h;
            }

            std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
            std::array<std::uint8_t, 64> buffer_{};
            std::size_t bufferSize_{};
            std::uint64_t bitCount_{};
        };

        [[nodiscard]] std::vector<std::byte> Bytes(const std::string_view text) {
            std::vector<std::byte> result(text.size());
            std::memcpy(result.data(), text.data(), text.size());
            return result;
        }

        [[nodiscard]] Result<std::vector<std::byte>> Read(const std::filesystem::path &path, const std::uint64_t limit) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > limit)
                return Result<std::vector<std::byte>>::Failure(
                    Failure(ProjectErrors::MigrationRecoveryFailed, "Cannot read bounded migration file: " + path.generic_string()));
            std::vector<std::byte> result(static_cast<std::size_t>(size));
            if (std::ifstream stream(path, std::ios::binary);
                !stream ||
                (size && !stream.read(reinterpret_cast<char *>(result.data()), static_cast<std::streamsize>(size))))  // NOSONAR(cpp:S6022)
                return Result<std::vector<std::byte>>::Failure(
                    Failure(ProjectErrors::MigrationRecoveryFailed, "Cannot read migration file: " + path.generic_string()));
            return Result<std::vector<std::byte>>::Success(std::move(result));
        }

        [[nodiscard]] MigrationContentHash Hash(const std::span<const std::byte> bytes) {
            Sha256 sha;
            sha.Update(bytes);
            return sha.Final();
        }

        [[nodiscard]] Result<MigrationContentHash> HashFile(const std::filesystem::path &path, const std::uint64_t limit) {
            const auto bytes = Read(path, limit);
            if (bytes.HasError())
                return Result<MigrationContentHash>::Failure(bytes.ErrorValue());
            return Result<MigrationContentHash>::Success(Hash(bytes.Value()));
        }

        [[nodiscard]] Result<json> ParseCanonicalHistory(const std::span<const std::byte> bytes) {
            try {
                bool duplicate{};
                std::vector<std::unordered_set<std::string>> keys;
                const auto callback = [&duplicate, &keys](const int depth, const json::parse_event_t event, const json &parsed) {
                    // NOSONAR
                    if (depth > 16)
                        duplicate = true;
                    if (event == json::parse_event_t::object_start)
                        keys.emplace_back();
                    else if (event == json::parse_event_t::key) {
                        const auto key = parsed.get<std::string>();
                        if (keys.empty() || !keys.back().emplace(key).second)
                            duplicate = true;
                    } else if (event == json::parse_event_t::object_end && !keys.empty())
                        keys.pop_back();
                    return true;
                };
                json result = json::parse(bytes.begin(), bytes.end(), callback);

                if (const std::string canonical = result.dump() + "\n";
                    duplicate || canonical.size() != bytes.size() || std::memcmp(canonical.data(), bytes.data(), bytes.size()) != 0 ||
                    !result.is_object() || !result.contains("receipts") || !result["receipts"].is_array()) {
                    struct NonCanonicalException : std::exception {};

                    throw NonCanonicalException{};
                }
                return Result<json>::Success(std::move(result));
            } catch (const std::exception &) {  // NOSONAR(cpp:S1181)
                return Result<json>::Failure(Failure(ProjectErrors::MigrationRecoveryFailed, "Migration history is not canonical."));
            }
        }

        [[nodiscard]] std::string Hex(const MigrationContentHash &hash) {
            std::string out = "sha256:";
            out.reserve(7 + hash.bytes.size() * 2);
            for (const auto value : hash.bytes) {
                std::format_to(std::back_inserter(out), "{:02x}", static_cast<unsigned char>(value));
            }
            return out;
        }

        [[nodiscard]] std::string Hex(const MigrationDefinitionHash &hash) {
            MigrationContentHash value{hash.bytes};
            return Hex(value);
        }

        [[nodiscard]] Result<MigrationContentHash> ParseHash(const std::string_view text) {
            if (text.size() != 71 || !text.starts_with("sha256:"))
                return Result<MigrationContentHash>::Failure(Failure(ProjectErrors::MigrationRecoveryFailed, "Invalid hash."));
            MigrationContentHash result;
            for (std::size_t i = 0; i < 32; ++i) {
                const auto nibble = [](const char c) {
                    if (c >= '0' && c <= '9')
                        return c - '0';
                    if (c >= 'a' && c <= 'f')
                        return c - 'a' + 10;
                    return -1;
                };
                const int hi = nibble(text[7 + i * 2]);
                const int lo = nibble(text[8 + i * 2]);
                if (hi < 0 || lo < 0)
                    return Result<MigrationContentHash>::Failure(Failure(ProjectErrors::MigrationRecoveryFailed, "Invalid hash."));
                result.bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
            }
            return Result<MigrationContentHash>::Success(result);
        }

        [[nodiscard]] std::string OperationId() {
            std::random_device random;
            std::ostringstream out;
            out << std::hex << std::setfill('0');
            for (int i = 0; i < 16; ++i)
                out << std::setw(2) << (random() & 0xffU);
            return out.str();
        }

        [[nodiscard]] std::string UtcText(const std::chrono::system_clock::time_point value) {
            const std::time_t time = std::chrono::system_clock::to_time_t(value);
            std::tm utc{};
#if defined(_WIN32)
            gmtime_s(&utc, &time);
#else
            gmtime_r(&time, &utc);
#endif
            std::ostringstream out;
            out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
            return out.str();
        }

        [[nodiscard]] json JournalJson(const Journal &journal) {
            json records = json::array();
            const auto kindString = [](const PreparedMigrationChangeKind k) {
                if (k == PreparedMigrationChangeKind::Add)
                    return "add";
                if (k == PreparedMigrationChangeKind::Remove)
                    return "remove";
                return "replace";
            };
            for (const auto &record : journal.records)
                records.push_back({{"path", record.path},
                                   {"kind", kindString(record.kind)},
                                   {"originalExists", record.originalExists},
                                   {"originalHash", Hex(record.originalHash)},
                                   {"stagedHash", Hex(record.stagedHash)},
                                   {"completed", record.completed}});
            return {{"writerHoroVersion", journal.writerVersion},
                    {"recoveryContract", journal.recoveryContract},
                    {"operationId", journal.operationId},
                    {"state", journal.state},
                    {"sourceVersion", journal.sourceVersion},
                    {"targetVersion", journal.targetVersion},
                    {"projectId", journal.projectId},
                    {"engineBuildIdentity", journal.engineBuildIdentity},
                    {"records", std::move(records)}};
        }

        [[nodiscard]] Result<void> WriteJournal(DurableFileSystem &files, const std::filesystem::path &root, const Journal &journal) {
            const std::string text = JournalJson(journal).dump(2) + "\n";
            const auto temporary = root / "journal.json.tmp";
            if (auto written = files.WriteDurable(temporary, Bytes(text)); written.HasError())
                return written;
            return files.AtomicReplace(temporary, root / "journal.json");
        }

        struct InvalidJournalException : std::exception {};

        struct StringHash {
            using is_transparent = void;

            std::size_t operator()(const std::string_view sv) const noexcept {
                return std::hash<std::string_view>{}(sv);
            }
        };

        using TransparentStringSet = std::unordered_set<std::string, StringHash, std::equal_to<>>;

        void ParseJournalRecords(Journal &journal, const json &root) {
            TransparentStringSet portablePaths;
            for (const auto &item : root.at("records")) {
                const std::string pathText = item.at("path").get<std::string>();
                if (const auto normalized = std::filesystem::path(pathText).lexically_normal();
                    normalized.is_absolute() || pathText.empty() || pathText.size() > 4096 || pathText != normalized.generic_string() ||
                    pathText == ".." || pathText.starts_with("../"))
                    throw InvalidJournalException{};
                std::string portable = pathText;
                std::ranges::transform(portable, portable.begin(), [](const unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
                if (!portablePaths.emplace(std::move(portable)).second)
                    throw InvalidJournalException{};
                const auto original = ParseHash(item.at("originalHash").get<std::string>());
                const auto staged = ParseHash(item.at("stagedHash").get<std::string>());
                if (original.HasError() || staged.HasError())
                    throw InvalidJournalException{};
                const std::string kind = item.at("kind").get<std::string>();
                if (kind != "add" && kind != "remove" && kind != "replace")
                    throw InvalidJournalException{};
                const auto parseKind = [](const std::string_view k) {
                    using enum PreparedMigrationChangeKind;
                    if (k == "add")
                        return Add;
                    if (k == "remove")
                        return Remove;
                    return Replace;
                };
                journal.records.push_back({.path = pathText,
                                           .kind = parseKind(kind),
                                           .originalHash = original.Value(),
                                           .stagedHash = staged.Value(),
                                           .originalExists = item.at("originalExists").get<bool>(),
                                           .completed = item.at("completed").get<bool>()});
            }
        }

        [[nodiscard]] Result<Journal> LoadJournal(const std::filesystem::path &path) {  // NOSONAR(cpp:S3776)
            const auto bytes = Read(path, 128ULL * 1024ULL * 1024ULL);
            if (bytes.HasError())
                return Result<Journal>::Failure(bytes.ErrorValue());
            try {
                bool invalidStructure = false;
                std::vector<std::unordered_set<std::string>> objectKeys;
                const auto callback = [&invalidStructure, &objectKeys](const int depth, const json::parse_event_t event,
                                                                       const json &parsed) {
                    // NOSONAR
                    if (depth > 16)
                        invalidStructure = true;
                    if (event == json::parse_event_t::object_start)
                        objectKeys.emplace_back();
                    else if (event == json::parse_event_t::key) {
                        const std::string key = parsed.get<std::string>();
                        if (key.size() > 128 || objectKeys.empty() || !objectKeys.back().emplace(key).second)
                            invalidStructure = true;
                    } else if (event == json::parse_event_t::object_end && !objectKeys.empty())
                        objectKeys.pop_back();
                    return true;
                };
                const auto root =
                    json::parse(reinterpret_cast<const char *>(bytes.Value().data()),                                    // NOSONAR
                                reinterpret_cast<const char *>(bytes.Value().data() + bytes.Value().size()), callback);  // NOSONAR
                if (invalidStructure || !root.is_object())
                    throw InvalidJournalException{};
                Journal journal{.operationId = root.at("operationId").get<std::string>(),
                                .state = root.at("state").get<std::string>(),
                                .writerVersion = root.at("writerHoroVersion").get<std::string>(),
                                .recoveryContract = root.at("recoveryContract").get<std::string>(),
                                .sourceVersion = root.at("sourceVersion").get<std::string>(),
                                .targetVersion = root.at("targetVersion").get<std::string>(),
                                .projectId = root.at("projectId").get<std::string>(),
                                .engineBuildIdentity = root.at("engineBuildIdentity").get<std::string>()};
                if (journal.state.size() > 32 || journal.writerVersion.size() > 64 || journal.recoveryContract.size() != 71 ||
                    journal.sourceVersion.size() > 64 || journal.targetVersion.size() > 64 || journal.projectId.size() > 256 ||
                    journal.engineBuildIdentity.size() > 512)
                    throw InvalidJournalException{};
                if (journal.operationId.size() != 32 || !std::ranges::all_of(journal.operationId, [](const char value) {
                    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                }))
                    throw InvalidJournalException{};
                if (const auto writer = ParseHoroVersion(journal.writerVersion); writer.HasError())
                    throw InvalidJournalException{};
                const auto journalContract = ParseMigrationRecoveryContractId(journal.recoveryContract);
                if (const bool supportedRecoveryContract =
                        journalContract.HasValue() && std::ranges::any_of(Generated::kHoroSupportedMigrationRecoveryContracts,
                                                                          [&](const auto &supported) {
                    const auto parsed = ParseMigrationRecoveryContractId(supported.contract);
                    return std::string_view(supported.reader) == "journal-v1" && parsed.HasValue() &&
                           parsed.Value() == journalContract.Value();
                });
                    !supportedRecoveryContract)
                    throw InvalidJournalException{};
                if (!root.at("records").is_array() || root.at("records").size() > 100'000)
                    throw InvalidJournalException{};
                ParseJournalRecords(journal, root);
                return Result<Journal>::Success(std::move(journal));
            } catch (const std::exception &) {  // NOSONAR(cpp:S1181)
                return Result<Journal>::Failure(Failure(ProjectErrors::MigrationRecoveryFailed, "Migration journal is malformed."));
            }
        }

        [[nodiscard]] std::vector<std::filesystem::path> JournalPaths(const std::filesystem::path &projectRoot) {
            std::vector<std::filesystem::path> result;
            std::error_code error;
            const auto migrationRoot = projectRoot / ".horo/local/migration";
            if (!std::filesystem::is_directory(migrationRoot, error))
                return result;
            for (const auto &entry : std::filesystem::directory_iterator(migrationRoot, error))
                if (entry.is_directory() && std::filesystem::exists(entry.path() / "journal.json"))
                    result.push_back(entry.path() / "journal.json");
            std::ranges::sort(result);
            return result;
        }

        [[nodiscard]] std::pair<std::uint64_t, std::size_t> CleanupUsage(const std::filesystem::path &projectRoot,
                                                                         const ProjectMigrationStoragePolicy &policy) {
            std::uint64_t bytes{};
            std::size_t directories{};
            std::error_code error;
            const auto cleanupRoot = projectRoot / ".horo/local/migration-cleanup";
            if (!std::filesystem::is_directory(cleanupRoot, error))
                return {};
            for (const auto &entry : std::filesystem::directory_iterator(cleanupRoot, error)) {
                if (error || directories >= policy.maximumCleanupDirectories || bytes >= policy.maximumCleanupBytes)
                    break;
                if (!entry.is_directory(error))
                    continue;
                ++directories;
                std::filesystem::recursive_directory_iterator it(entry.path(), std::filesystem::directory_options::skip_permission_denied,
                                                                 error);
                std::filesystem::recursive_directory_iterator end;
                while (it != end && !error) {
                    if (!it->is_regular_file(error)) {
                        it.increment(error);
                        continue;
                    }
                    const auto size = it->file_size(error);
                    if (const auto boundedSize = std::min<std::uint64_t>(static_cast<std::uint64_t>(size), policy.maximumCleanupBytes);
                        error || bytes > policy.maximumCleanupBytes - boundedSize) {
                        bytes = policy.maximumCleanupBytes;
                        break;
                    }
                    bytes += size;
                    it.increment(error);
                }
            }
            return {bytes, directories};
        }

        [[nodiscard]] Result<void> MoveToCleanup(DurableFileSystem &files, const std::filesystem::path &projectRoot,
                                                 const std::filesystem::path &operationRoot, const std::string_view operationId) {
            const auto cleanupRoot = projectRoot / ".horo/local/migration-cleanup";
            std::error_code error;
            std::filesystem::create_directories(cleanupRoot, error);
            if (error)
                return Result<void>::Failure(Failure(ProjectErrors::MigrationRecoveryFailed, error.message()));
            const auto destination = cleanupRoot / operationId;
            std::filesystem::rename(operationRoot, destination, error);
            if (error)
                return Result<void>::Failure(Failure(ProjectErrors::MigrationRecoveryFailed, error.message()));
            if (auto synced = files.SyncDirectory(operationRoot.parent_path()); synced.HasError())
                return synced;
            return files.SyncDirectory(cleanupRoot);
        }

        [[nodiscard]] int PublishRank(const std::string_view path) {
            if (path == ".horo/project.json")
                return 2;
            if (path == ".horo/migration_history.json")
                return 1;
            return 0;
        }

        [[nodiscard]] Result<std::pair<std::uint64_t, std::uint64_t>> InventorySize(const std::filesystem::path &root,
                                                                                    const ProjectMigrationLimits &limits) {
            std::uint64_t bytes = 0;
            std::uint64_t documents = 0;
            std::error_code error;
            std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, error);
            std::filesystem::recursive_directory_iterator end;
            while (iterator != end) {
                if (error)
                    return Result<std::pair<std::uint64_t, std::uint64_t>>::Failure(
                        Failure(ProjectErrors::MigrationInventoryInvalid, "Capacity inventory failed."));
                if (const auto relative = iterator->path().lexically_relative(root).generic_string();
                    iterator->is_directory() &&
                    (relative == ".horo/local" || relative.starts_with(".horo/local/") || relative == "build" || relative == ".git")) {
                    iterator.disable_recursion_pending();
                    iterator.increment(error);
                    continue;
                }
                if (!iterator->is_regular_file()) {
                    iterator.increment(error);
                    continue;
                }
                const auto size = iterator->file_size(error);
                if (error || size > limits.maxInputBytes || bytes > limits.maxInputBytes - size)
                    return Result<std::pair<std::uint64_t, std::uint64_t>>::Failure(
                        Failure(ProjectErrors::MigrationInventoryLimit, "Capacity inventory exceeds limits."));
                bytes += size;
                if (++documents > limits.maxDocuments)
                    return Result<std::pair<std::uint64_t, std::uint64_t>>::Failure(
                        Failure(ProjectErrors::MigrationInventoryLimit, "Capacity inventory exceeds document limits."));
                iterator.increment(error);
            }
            return Result<std::pair<std::uint64_t, std::uint64_t>>::Success({bytes, documents});
        }

        [[nodiscard]] std::uint64_t EstimatedOutput(const ProjectMigrationTransactionRequest &request, const std::uint64_t input,
                                                    const std::uint64_t documents) {
            std::uint64_t output = input;
            for (const auto &definition : request.plan.definitions) {
                const auto &estimate = definition.storageEstimate;
                const std::uint64_t ratio = SaturatingMultiply(output, estimate.maximumOutputRatioPermille) / 1000U;
                const std::uint64_t perDocument = SaturatingMultiply(documents, estimate.maximumAddedBytesPerDocument);
                output =
                    std::min(request.limits.maxOutputBytes, SaturatingAdd(SaturatingAdd(ratio, perDocument), estimate.maximumFixedBytes));
            }
            return output;
        }

        [[nodiscard]] Result<void> Publish(DurableFileSystem &files, const std::filesystem::path &projectRoot,
                                           const std::filesystem::path &operationRoot, Journal &journal) {
            journal.state = "Publishing";
            LOG_DEBUG("editor.project_migration.transaction", "Journal state operation=%s state=Publishing records=%zu.",
                      journal.operationId.c_str(), journal.records.size());
            if (auto result = WriteJournal(files, operationRoot, journal); result.HasError())
                return result;
            for (auto &record : journal.records) {
                if (record.completed)
                    continue;
                const auto destination = projectRoot / record.path;
                if (record.kind == PreparedMigrationChangeKind::Remove) {
                    if (auto result = files.RemoveDurable(destination); result.HasError())
                        return result;
                } else {
                    const auto staged = operationRoot / "staging" / record.path;
                    if (auto result = files.AtomicReplace(staged, destination); result.HasError())
                        return result;
                }
                record.completed = true;
                if (auto result = WriteJournal(files, operationRoot, journal); result.HasError())
                    return result;
            }
            journal.state = "Committed";
            LOG_DEBUG("editor.project_migration.transaction", "Journal state operation=%s state=Committed.", journal.operationId.c_str());
            return WriteJournal(files, operationRoot, journal);
        }

        struct RecoveryEvidence {
            bool canResume{true};
            bool canRestore{true};
            bool allPublished{true};
            std::vector<bool> published;
        };

        [[nodiscard]] bool Matches(const std::filesystem::path &path, const MigrationContentHash &expected) {
            if (!std::filesystem::is_regular_file(path))
                return false;
            const auto actual = HashFile(path, 4ULL * 1024ULL * 1024ULL * 1024ULL);
            return actual.HasValue() && actual.Value() == expected;
        }

        [[nodiscard]] RecoveryEvidence InspectEvidence(const std::filesystem::path &projectRoot, const std::filesystem::path &operationRoot,
                                                       const Journal &journal) {
            RecoveryEvidence evidence;
            evidence.published.reserve(journal.records.size());
            for (const Record &record : journal.records) {
                const auto destination = projectRoot / record.path;
                const bool published = record.kind == PreparedMigrationChangeKind::Remove ? !std::filesystem::exists(destination)
                                                                                          : Matches(destination, record.stagedHash);
                const bool original =
                    record.originalExists ? Matches(destination, record.originalHash) : !std::filesystem::exists(destination);
                const bool staged = record.kind == PreparedMigrationChangeKind::Remove ||
                                    Matches(operationRoot / "staging" / record.path, record.stagedHash);
                const bool rollback = !record.originalExists || Matches(operationRoot / "rollback" / record.path, record.originalHash);
                evidence.published.push_back(published);
                evidence.allPublished = evidence.allPublished && published;
                evidence.canResume = evidence.canResume && (published || (original && staged));
                evidence.canRestore = evidence.canRestore && rollback;
            }
            return evidence;
        }
    }  // namespace

    /** @copydoc ParseMigrationRecoveryContractId */
    Result<MigrationRecoveryContractId> ParseMigrationRecoveryContractId(const std::string_view text) {
        const auto parsed = ParseHash(text);
        if (parsed.HasError())
            return Result<MigrationRecoveryContractId>::Failure(parsed.ErrorValue());
        return Result<MigrationRecoveryContractId>::Success({parsed.Value().bytes});
    }

    /** @copydoc FormatMigrationRecoveryContractId */
    std::string FormatMigrationRecoveryContractId(const MigrationRecoveryContractId &id) {
        return Hex(MigrationContentHash{id.bytes});
    }

    /** @copydoc CurrentMigrationRecoveryContractId */
    MigrationRecoveryContractId CurrentMigrationRecoveryContractId() {
        const auto parsed = ParseMigrationRecoveryContractId(Generated::kHoroMigrationRecoveryContract);
        if (parsed.HasError())
            std::terminate();
        return parsed.Value();
    }

    /** @copydoc ProjectMigrationStorageAdmission::RequiredBytes */
    std::uint64_t ProjectMigrationStorageAdmission::RequiredBytes() const noexcept {
        const auto add = [](const std::uint64_t left, const std::uint64_t right) {
            return left > std::numeric_limits<std::uint64_t>::max() - right ? std::numeric_limits<std::uint64_t>::max() : left + right;
        };
        return add(add(add(stagedOutputBytes, rollbackBytes), journalAndHistoryBytes), add(cleanupRemnantBytes, safetyMarginBytes));
    }

    /** @copydoc ProjectMigrationTransactionService::ProjectMigrationTransactionService */
    ProjectMigrationTransactionService::ProjectMigrationTransactionService(DurableFileSystem &files, WallClock &wallClock,
                                                                           ProjectMutationCoordinator &mutations, JobSystem &jobs,
                                                                           const ProjectMigrationStoragePolicy &storagePolicy,
                                                                           ProjectMigrationExecutionServices executionServices) noexcept
        : files_(files), wallClock_(wallClock), mutations_(mutations), jobs_(jobs), storagePolicy_(storagePolicy),
          executionServices_(std::move(executionServices)) {}

    /** @copydoc ProjectMigrationTransactionService::InspectStorageAdmission */
    Result<ProjectMigrationStorageAdmission> ProjectMigrationTransactionService::InspectStorageAdmission(
        const ProjectMigrationTransactionRequest &request) const {
        const auto inventorySize = InventorySize(request.projectRoot, request.limits);
        if (inventorySize.HasError())
            return Result<ProjectMigrationStorageAdmission>::Failure(inventorySize.ErrorValue());
        ProjectMigrationStorageAdmission admission{.stagedOutputBytes =
                                                       EstimatedOutput(request, inventorySize.Value().first, inventorySize.Value().second),
                                                   .rollbackBytes = inventorySize.Value().first,
                                                   .journalAndHistoryBytes =
                                                       SaturatingAdd(request.limits.maxJournalBytes, request.limits.maxHistoryBytes),
                                                   .cleanupRemnantBytes = CleanupUsage(request.projectRoot, storagePolicy_).first};
        const std::uint64_t withoutReserve = admission.RequiredBytes();
        const std::uint64_t percentageMargin = SaturatingMultiply(withoutReserve, storagePolicy_.safetyMarginPermille) / 1000U;
        admission.safetyMarginBytes = std::max(storagePolicy_.minimumSafetyMarginBytes, percentageMargin);
        const auto available = files_.AvailableBytes(request.projectRoot);
        if (available.HasError())
            return Result<ProjectMigrationStorageAdmission>::Failure(available.ErrorValue());
        admission.availableBytes = available.Value();
        return Result<ProjectMigrationStorageAdmission>::Success(admission);
    }

    /** @copydoc ProjectMigrationTransactionService::Execute */
    Result<ProjectMigrationTransactionResult> ProjectMigrationTransactionService::Execute(  // NOSONAR(cpp:S3776)
        const ProjectMigrationTransactionRequest &request) {
        const std::string operationId = OperationId();
        const std::string sourceVersion = FormatHoroVersion(request.sourceMetadata.horoVersion.value);
        const std::string targetVersion = FormatHoroVersion(request.targetDecision.release.value);
        LOG_INFO("editor.project_migration.transaction", "Migration transaction started operation=%s source=%s target=%s.",
                 operationId.c_str(), sourceVersion.c_str(), targetVersion.c_str());
        LOG_DEBUG("editor.project_migration.transaction", "Transaction aggregate operation=%s definitions=%zu.", operationId.c_str(),
                  request.plan.definitions.size());
        auto acquiredLease = mutations_.TryAcquire({request.projectRoot, ProjectMutationOwner::Migration, operationId});
        if (acquiredLease.HasError())
            return Result<ProjectMigrationTransactionResult>::Failure(acquiredLease.ErrorValue());
        // The lease must span every authoritative read, publish, and cleanup decision below.
        [[maybe_unused]] ProjectMutationLease mutationLease = std::move(acquiredLease).Value();
        if (InspectPendingRecovery(request.projectRoot).action != MigrationRecoveryAction::None)
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationRecoveryFailed,
                        "Pending migration recovery must finish before starting another transaction."));
        const auto authoritativeMetadata = LoadProjectMetadata(request.projectRoot);
        if (authoritativeMetadata.HasError() || authoritativeMetadata.Value().projectId != request.sourceMetadata.projectId ||
            authoritativeMetadata.Value().horoVersion != request.sourceMetadata.horoVersion ||
            authoritativeMetadata.Value().persistentContract != request.sourceMetadata.persistentContract ||
            request.plan.source != request.sourceBaseline || request.plan.target != request.targetDecision.contractBaseline ||
            (!request.plan.definitions.empty() &&
             request.plan.definitions.back().targetContract != request.targetDecision.persistentContract))
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationInputChanged,
                        "Migration request no longer matches authoritative project metadata or target contract."));
        const auto admission = InspectStorageAdmission(request);
        if (admission.HasError())
            return Result<ProjectMigrationTransactionResult>::Failure(admission.ErrorValue());
        LOG_DEBUG("editor.project_migration.transaction",
                  "Storage admission operation=%s staged=%llu rollback=%llu journal_history=%llu cleanup=%llu safety=%llu available=%llu "
                  "required=%llu.",
                  operationId.c_str(), static_cast<unsigned long long>(admission.Value().stagedOutputBytes),
                  static_cast<unsigned long long>(admission.Value().rollbackBytes),
                  static_cast<unsigned long long>(admission.Value().journalAndHistoryBytes),
                  static_cast<unsigned long long>(admission.Value().cleanupRemnantBytes),
                  static_cast<unsigned long long>(admission.Value().safetyMarginBytes),
                  static_cast<unsigned long long>(admission.Value().availableBytes),
                  static_cast<unsigned long long>(admission.Value().RequiredBytes()));
        const std::uint64_t estimatedOutput = admission.Value().stagedOutputBytes;
        if (admission.Value().RequiredBytes() > admission.Value().availableBytes)
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationCapacityInsufficient,
                        std::format("Conservative migration storage preflight failed: required={}, available={}.",
                                    admission.Value().RequiredBytes(), admission.Value().availableBytes)));
        if (request.cancellation.IsCancellationRequested())
            return Result<ProjectMigrationTransactionResult>::Failure(Failure(ProjectErrors::MigrationCancelled, "Migration cancelled."));
        const auto operationRoot = request.projectRoot / ".horo/local/migration" / operationId;
        const auto stagingRoot = operationRoot / "staging";
        std::error_code error;
        if (!std::filesystem::create_directories(stagingRoot, error) || error)  // NOSONAR(cpp:S2083)
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationPublishFailed, "Cannot reserve migration operation directory."));
        Journal journal{.operationId = operationId,
                        .writerVersion = FormatHoroVersion(CurrentEngineReleaseVersion().value),
                        .recoveryContract = Generated::kHoroMigrationRecoveryContract,
                        .sourceVersion = FormatHoroVersion(request.sourceMetadata.horoVersion.value),
                        .targetVersion = FormatHoroVersion(request.targetDecision.release.value),
                        .projectId = request.sourceMetadata.projectId,
                        .engineBuildIdentity = request.engineBuildIdentity};
        if (auto result = WriteJournal(files_, operationRoot, journal); result.HasError())
            return Result<ProjectMigrationTransactionResult>::Failure(result.ErrorValue());
        LOG_DEBUG("editor.project_migration.transaction", "Journal state operation=%s state=Preparing.", operationId.c_str());
        auto prepared = ProjectMigrationExecutor::Prepare(request.projectRoot, stagingRoot, request.plan, jobs_, request.limits,
                                                          request.cancellation, executionServices_);
        if (prepared.HasError()) {
            std::filesystem::remove_all(operationRoot, error);
            return Result<ProjectMigrationTransactionResult>::Failure(prepared.ErrorValue());
        }
        PreparedProjectMigration candidate = std::move(prepared).Value();
        if (candidate.OutputBytes() > estimatedOutput)
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationCapacityInsufficient, "Migration output exceeded its frozen storage-growth declaration."));
        if (auto unchanged = candidate.VerifySourceUnchanged(); unchanged.HasError())
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationInputChanged, unchanged.ErrorValue().message));

        json history = {{"receipts", json::array()}};
        if (const auto historyPath = request.projectRoot / ".horo/migration_history.json"; std::filesystem::exists(historyPath)) {
            const auto old = Read(historyPath, request.limits.maxHistoryBytes);
            if (old.HasError())
                return Result<ProjectMigrationTransactionResult>::Failure(old.ErrorValue());
            if (!authoritativeMetadata.Value().migrationHistoryHead.has_value() ||
                Hash(old.Value()).bytes != authoritativeMetadata.Value().migrationHistoryHead->content.bytes)
                return Result<ProjectMigrationTransactionResult>::Failure(
                    Failure(ProjectErrors::MigrationRecoveryFailed, "Migration history does not match the authoritative history head."));
            auto parsedHistory = ParseCanonicalHistory(old.Value());
            if (parsedHistory.HasError())
                return Result<ProjectMigrationTransactionResult>::Failure(parsedHistory.ErrorValue());
            history = std::move(parsedHistory).Value();
        } else if (authoritativeMetadata.Value().migrationHistoryHead.has_value())
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationRecoveryFailed, "Project metadata references a missing migration history."));
        json definitions = json::array();
        for (const auto &definition : request.plan.definitions)
            definitions.push_back({{"id", definition.id.value}, {"hash", Hex(definition.hash)}});
        json receipt = {{"operationId", operationId},
                        {"sourceRelease", FormatHoroVersion(request.sourceMetadata.horoVersion.value)},
                        {"sourceBaseline", FormatHoroVersion(request.sourceBaseline.value)},
                        {"targetRelease", FormatHoroVersion(request.targetDecision.release.value)},
                        {"definitions", std::move(definitions)},
                        {"engineBuildIdentity", request.engineBuildIdentity},
                        {"completedAt", UtcText(wallClock_.UtcNow())}};
        receipt["previousHead"] = request.sourceMetadata.migrationHistoryHead.has_value()
                                      ? json(Hex(request.sourceMetadata.migrationHistoryHead->content))
                                      : json(nullptr);
        history["receipts"].push_back(std::move(receipt));
        const std::string historyText = history.dump() + "\n";
        const MigrationHistoryHead historyHead{Hash(Bytes(historyText))};
        const auto migratedProject = candidate.ReadCandidateDocument(".horo/project.json");
        if (migratedProject.HasError())
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationPublishFailed, "Migration candidate removed the authoritative project metadata."));
        json metadata;
        try {
            metadata =
                json::parse(reinterpret_cast<const char *>(migratedProject.Value().data()),                                    // NOSONAR
                            reinterpret_cast<const char *>(migratedProject.Value().data() + migratedProject.Value().size()));  // NOSONAR
        } catch (const std::exception &) {  // NOSONAR(cpp:S1181)
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationPublishFailed, "Project metadata is malformed."));
        }
        if (!metadata.is_object() || !metadata.contains("projectId") || !metadata["projectId"].is_string() ||
            metadata["projectId"].get_ref<const std::string &>() != request.sourceMetadata.projectId)
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationPublishFailed, "Migration candidate changed the immutable project identity."));
        metadata["horoVersion"] = FormatHoroVersion(request.targetDecision.release.value);
        metadata["persistentContract"] = FormatPersistentContractHash(request.targetDecision.persistentContract);
        metadata["migrationHistoryHead"] = Hex(historyHead.content);
        metadata.erase("compatibilityProof");
        const std::string metadataText = metadata.dump(2) + "\n";
        std::vector<PreparedMigrationDocument> transactionDocuments;
        transactionDocuments.push_back(
            {.path = ".horo/migration_history.json", .kind = MigrationDocumentKind::ProjectSettings, .bytes = Bytes(historyText)});
        transactionDocuments.push_back(
            {.path = ".horo/project.json", .kind = MigrationDocumentKind::ProjectMetadata, .bytes = Bytes(metadataText)});
        if (auto finalized =
                ProjectMigrationExecutor::Finalize(candidate, transactionDocuments, request.plan.targetValidator, request.cancellation);
            finalized.HasError())
            return Result<ProjectMigrationTransactionResult>::Failure(finalized.ErrorValue());
        if (const std::uint64_t maximumDeclaredOutput =
                SaturatingAdd(SaturatingAdd(estimatedOutput, request.limits.maxHistoryBytes), 64ULL * 1024ULL);
            candidate.OutputBytes() > maximumDeclaredOutput)
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationCapacityInsufficient,
                        "Final migration candidate exceeded its conservative storage declaration."));

        for (const auto &change : candidate.Changes())
            journal.records.push_back({.path = change.path, .kind = change.kind});
        std::ranges::sort(journal.records, [](const Record &left, const Record &right) {
            const int lr = PublishRank(left.path);
            const int rr = PublishRank(right.path);
            return lr != rr ? lr < rr : left.path < right.path;
        });

        std::uint64_t required = 64ULL * 1024ULL * 1024ULL;
        const auto accountFileSize = [&](const std::filesystem::path &path) {
            error.clear();
            const std::uint64_t size = std::filesystem::file_size(path, error);
            if (error)
                return Result<void>::Failure(
                    Failure(ProjectErrors::MigrationInventoryInvalid, "Failed to inspect migration transaction storage usage."));
            if (required > std::numeric_limits<std::uint64_t>::max() - size)
                return Result<void>::Failure(
                    Failure(ProjectErrors::MigrationCapacityInsufficient, "Migration transaction storage requirement overflowed."));
            required += size;
            return Result<void>::Success();
        };
        for (auto &record : journal.records) {
            const auto authoritative = request.projectRoot / record.path;
            record.originalExists = std::filesystem::exists(authoritative);
            if (record.originalExists) {
                const auto original = HashFile(authoritative, request.limits.maxInputBytes);
                if (original.HasError())
                    return Result<ProjectMigrationTransactionResult>::Failure(original.ErrorValue());
                record.originalHash = original.Value();
                const auto rollback = operationRoot / "rollback" / record.path;
                if (auto copied = files_.CopyDurable(authoritative, rollback); copied.HasError())
                    return Result<ProjectMigrationTransactionResult>::Failure(copied.ErrorValue());
                if (auto accounted = accountFileSize(authoritative); accounted.HasError())
                    return Result<ProjectMigrationTransactionResult>::Failure(accounted.ErrorValue());
            }
            if (record.kind != PreparedMigrationChangeKind::Remove) {
                const auto staged = HashFile(stagingRoot / record.path, request.limits.maxOutputBytes);
                if (staged.HasError())
                    return Result<ProjectMigrationTransactionResult>::Failure(staged.ErrorValue());
                record.stagedHash = staged.Value();
                if (auto accounted = accountFileSize(stagingRoot / record.path); accounted.HasError())
                    return Result<ProjectMigrationTransactionResult>::Failure(accounted.ErrorValue());
                const auto bytes = Read(stagingRoot / record.path, request.limits.maxOutputBytes);
                if (bytes.HasError())
                    return Result<ProjectMigrationTransactionResult>::Failure(bytes.ErrorValue());
                if (auto durable = files_.WriteDurable(stagingRoot / record.path, bytes.Value()); durable.HasError())
                    return Result<ProjectMigrationTransactionResult>::Failure(durable.ErrorValue());
            }
        }
        const auto available = files_.AvailableBytes(request.projectRoot);
        if (available.HasError())
            return Result<ProjectMigrationTransactionResult>::Failure(available.ErrorValue());
        if (available.Value() < required)
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationCapacityInsufficient, "Insufficient migration transaction capacity."));
        if (auto unchanged = candidate.VerifySourceUnchanged(); unchanged.HasError())
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationInputChanged, unchanged.ErrorValue().message));
        journal.state = "PublishReady";
        if (auto written = WriteJournal(files_, operationRoot, journal); written.HasError())
            return Result<ProjectMigrationTransactionResult>::Failure(written.ErrorValue());
        LOG_DEBUG("editor.project_migration.transaction", "Journal state operation=%s state=PublishReady records=%zu.", operationId.c_str(),
                  journal.records.size());
        candidate.PreserveForRecovery();
        const bool cancellationDeferred = request.cancellation.IsCancellationRequested();
        if (cancellationDeferred)
            LOG_WARN("editor.project_migration.transaction", "Cancellation deferred during publication operation=%s.", operationId.c_str());
        if (auto published = Publish(files_, request.projectRoot, operationRoot, journal); published.HasError())
            return Result<ProjectMigrationTransactionResult>::Failure(
                Failure(ProjectErrors::MigrationPublishFailed, published.ErrorValue().message));
        std::vector<std::string> changed;
        for (const auto &record : journal.records)
            changed.push_back(record.path);
        bool cleanupDeferred{};
        if (auto moved = MoveToCleanup(files_, request.projectRoot, operationRoot, operationId); moved.HasError()) {
            // The committed project remains authoritative. A later bounded janitor will retry cleanup.
            cleanupDeferred = true;
        } else {
            static_cast<void>(CleanupCommittedMigrations(request.projectRoot));
        }
        if (cleanupDeferred)
            LOG_WARN("editor.project_migration.transaction", "Committed cleanup deferred operation=%s.", operationId.c_str());
        LOG_INFO("editor.project_migration.transaction", "Migration transaction committed operation=%s.", operationId.c_str());
        LOG_DEBUG("editor.project_migration.transaction",
                  "Committed transaction aggregate operation=%s changed_files=%zu cleanup_deferred=%s.", operationId.c_str(),
                  changed.size(), cleanupDeferred ? "true" : "false");
        return Result<ProjectMigrationTransactionResult>::Success(
            {operationId, historyHead, std::move(changed), cancellationDeferred, cleanupDeferred});
    }

    /** @copydoc ProjectMigrationTransactionService::InspectPendingRecovery */
    MigrationRecoverySnapshot ProjectMigrationTransactionService::InspectPendingRecovery(const std::filesystem::path &projectRoot) const {
        const auto paths = JournalPaths(projectRoot);
        if (paths.empty())
            return {};
        if (paths.size() != 1)
            return {MigrationRecoveryAction::Unrecoverable, std::nullopt,
                    Failure(ProjectErrors::MigrationRecoveryFailed, "Multiple unfinished migration journals exist.")};
        const auto journal = LoadJournal(paths.front());
        if (journal.HasError())
            return {MigrationRecoveryAction::Unrecoverable, std::nullopt, journal.ErrorValue()};
        using enum MigrationRecoveryAction;
        MigrationRecoveryAction action = Unrecoverable;
        if (journal.Value().state == "Preparing")
            action = DiscardUnpublishedStaging;
        else if (journal.Value().state == "PublishReady" || journal.Value().state == "Publishing") {
            const auto root = paths.front().parent_path();
            const RecoveryEvidence evidence = InspectEvidence(projectRoot, root, journal.Value());
            if (evidence.canResume)
                action = ResumePublish;
            else if (evidence.canRestore)
                action = RestoreOriginals;
            else
                action = Unrecoverable;
        } else if (journal.Value().state == "Committed") {
            const RecoveryEvidence evidence = InspectEvidence(projectRoot, paths.front().parent_path(), journal.Value());
            if (evidence.allPublished)
                action = FinalizeCommittedMigration;
            else if (evidence.canRestore)
                action = RestoreOriginals;
            else
                action = Unrecoverable;
        }
        LOG_INFO("editor.project_migration.recovery", "Recovery classified operation=%s action=%s.", journal.Value().operationId.c_str(),
                 RecoveryActionName(action));
        return {action, journal.Value().operationId, std::nullopt};
    }

    /** @copydoc ProjectMigrationTransactionService::Recover */
    Result<void> ProjectMigrationTransactionService::Recover(const std::filesystem::path &projectRoot,  // NOSONAR(cpp:S3776)
                                                             const CancellationToken cancellation) {
        const auto initialSnapshot = InspectPendingRecovery(projectRoot);

        if (initialSnapshot.action == MigrationRecoveryAction::None)
            return Result<void>::Success();
        auto acquiredLease =
            mutations_.TryAcquire({projectRoot, ProjectMutationOwner::Migration, initialSnapshot.operationId.value_or("recovery")});
        if (acquiredLease.HasError())
            return Result<void>::Failure(acquiredLease.ErrorValue());
        // Recovery must hold the same mutation exclusion until evidence is reconciled and cleanup is durable.
        [[maybe_unused]] ProjectMutationLease mutationLease = std::move(acquiredLease).Value();
        const auto snapshot = InspectPendingRecovery(projectRoot);
        if (snapshot.action == MigrationRecoveryAction::None)
            return Result<void>::Success();
        LOG_INFO("editor.project_migration.recovery", "Recovery started operation=%s action=%s.",
                 snapshot.operationId.value_or("unknown").c_str(), RecoveryActionName(snapshot.action));
        if (snapshot.action == MigrationRecoveryAction::Unrecoverable) {
            Error error = snapshot.diagnostic.value_or(
                Failure(ProjectErrors::MigrationRecoveryFailed, "Migration recovery evidence is unrecoverable."));
            LOG_ERROR("editor.project_migration.recovery", "Recovery failed operation=%s code=%s.",
                      snapshot.operationId.value_or("unknown").c_str(), error.code.Value().c_str());
            return Result<void>::Failure(std::move(error));
        }
        const auto paths = JournalPaths(projectRoot);
        const auto operationRoot = paths.front().parent_path();
        auto journal = LoadJournal(paths.front());
        if (journal.HasError())
            return Result<void>::Failure(journal.ErrorValue());
        std::error_code error;
        if (snapshot.action == MigrationRecoveryAction::DiscardUnpublishedStaging ||
            snapshot.action == MigrationRecoveryAction::FinalizeCommittedMigration) {
            if (snapshot.action == MigrationRecoveryAction::FinalizeCommittedMigration) {
                auto finalized = MoveToCleanup(files_, projectRoot, operationRoot, journal.Value().operationId);
                if (finalized.HasValue())
                    LOG_INFO("editor.project_migration.recovery", "Recovery completed operation=%s action=%s.",
                             journal.Value().operationId.c_str(), RecoveryActionName(snapshot.action));
                return finalized;
            }
            std::filesystem::remove_all(operationRoot, error);
            if (error)
                return Result<void>::Failure(Failure(ProjectErrors::MigrationRecoveryFailed, error.message()));
            LOG_INFO("editor.project_migration.recovery", "Recovery completed operation=%s action=%s.", journal.Value().operationId.c_str(),
                     RecoveryActionName(snapshot.action));
            return Result<void>::Success();
        }
        if (snapshot.action == MigrationRecoveryAction::ResumePublish) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(Failure(ProjectErrors::MigrationCancelled, "Recovery cancelled before publish."));
            Journal recovered = journal.Value();
            const RecoveryEvidence evidence = InspectEvidence(projectRoot, operationRoot, recovered);
            for (std::size_t index = 0; index < recovered.records.size(); ++index)
                recovered.records[index].completed = evidence.published[index];
            if (auto written = WriteJournal(files_, operationRoot, recovered); written.HasError())
                return written;
            if (auto result = Publish(files_, projectRoot, operationRoot, recovered); result.HasError())
                return result;
            auto moved = MoveToCleanup(files_, projectRoot, operationRoot, recovered.operationId);
            if (moved.HasValue())
                LOG_INFO("editor.project_migration.recovery", "Recovery completed operation=%s action=%s.", recovered.operationId.c_str(),
                         RecoveryActionName(snapshot.action));
            return moved;
        }
        for (auto iterator = journal.Value().records.rbegin(); iterator != journal.Value().records.rend(); ++iterator) {
            const auto destination = projectRoot / iterator->path;
            if (iterator->originalExists) {
                if (!Matches(operationRoot / "rollback" / iterator->path, iterator->originalHash))
                    return Result<void>::Failure(
                        Failure(ProjectErrors::MigrationRecoveryFailed, "Rollback original failed hash verification: " + iterator->path));
                const auto temporary = operationRoot / "restore" / iterator->path;
                if (auto copy = files_.CopyDurable(operationRoot / "rollback" / iterator->path, temporary); copy.HasError())
                    return copy;
                if (auto replace = files_.AtomicReplace(temporary, destination); replace.HasError())
                    return replace;
            } else if (std::filesystem::exists(destination))
                if (auto remove = files_.RemoveDurable(destination); remove.HasError())
                    return remove;
        }
        std::filesystem::remove_all(operationRoot, error);
        if (error)
            return Result<void>::Failure(Failure(ProjectErrors::MigrationRecoveryFailed, error.message()));
        LOG_INFO("editor.project_migration.recovery", "Recovery completed operation=%s action=%s.", journal.Value().operationId.c_str(),
                 RecoveryActionName(snapshot.action));
        return Result<void>::Success();
    }

    /** @copydoc ProjectMigrationTransactionService::CleanupCommittedMigrations */
    Result<void> ProjectMigrationTransactionService::CleanupCommittedMigrations(const std::filesystem::path &projectRoot) const {
        const auto cleanupRoot = projectRoot / ".horo/local/migration-cleanup";
        std::error_code error;
        if (!std::filesystem::is_directory(cleanupRoot, error))
            return Result<void>::Success();
        std::size_t visited{};
        for (const auto &entry : std::filesystem::directory_iterator(cleanupRoot, error)) {
            if (error)
                return Result<void>::Failure(Failure(ProjectErrors::MigrationRecoveryFailed, error.message()));
            if (++visited > storagePolicy_.maximumCleanupDirectories)
                break;
            if (!entry.is_directory(error))
                continue;
            if (const auto journal = LoadJournal(entry.path() / "journal.json"); journal.HasError() || journal.Value().state != "Committed")
                continue;
            std::filesystem::remove_all(entry.path(), error);
            if (error)
                return Result<void>::Failure(Failure(ProjectErrors::MigrationRecoveryFailed, error.message()));
        }
        return files_.SyncDirectory(cleanupRoot);
    }
}  // namespace Horo::Editor
