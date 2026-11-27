#include "editor/screens/workspace/prefab/assets/PrefabApplyTransaction.h"

#include "Horo/Editor/DocumentRecoveryService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <random>

namespace Horo::Editor {
    namespace {
        /** @brief Prevents a borrowed filesystem provider from reentering the source authoring owner. */
        class SourceWriteGuard final {
        public:
            explicit SourceWriteGuard(bool &busy) noexcept : busy_(busy) {
                busy_ = true;
            }

            ~SourceWriteGuard() {
                busy_ = false;
            }

            SourceWriteGuard(const SourceWriteGuard &) = delete;
            SourceWriteGuard &operator=(const SourceWriteGuard &) = delete;

        private:
            bool &busy_;
        };

        /** @brief Owns the exact old bytes and proof of any replacement until all owners commit. */
        struct ApplyFile final {
            std::filesystem::path path;
            Assets::AssetSourceIdentity expected;
            std::string replacement;
            std::optional<ExclusiveFileLock> lock;
            Assets::AssetSourceCapture original;
            Assets::AssetSourceIdentity planned;
            std::optional<Assets::AssetSourceIdentity> installed;
        };

        constexpr std::size_t MaximumCapsuleBytes = 64U * 1024U * 1024U;
        constexpr std::string_view LegacyCapsuleMagic = "HoroPrefabApplyRecoveryV1";
        constexpr std::string_view DispositionCapsuleMagic = "HoroPrefabApplyRecoveryV2";
        constexpr std::string_view CapsuleMagic = "HoroPrefabApplyRecoveryV3";

        /** @brief Durable publication state, relative to the two identities in the operation. */
        enum class JournalDisposition : std::uint8_t {
            Prepared = 1,
            RollbackRequired = 2,
            Committed = 3,
        };

        /** @brief Owns exact capsule bytes and the latest native tracked publication receipt. */
        struct ApplyJournal final {
            std::vector<std::byte> bytes;
            Assets::AssetSourceIdentity identity;
            Assets::AssetId attempt;
            JournalDisposition disposition{JournalDisposition::Prepared};
        };

        constexpr std::size_t MaximumRecoveryPathBytes = 4096;

        /** @brief Generates a fresh opaque staging nonce at an explicit authoring boundary. */
        Assets::AssetId NewPublicationAttempt() {
            std::random_device random;
            std::array<std::uint8_t, 16> bytes;
            for (auto &value : bytes)
                value = static_cast<std::uint8_t>(random());
            // Canonical random UUID layout also guarantees a nonzero identifier.
            bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
            bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
            return Assets::AssetId::FromBytes(bytes);
        }

        /** @brief Stable private recovery namespace for one unresolved paired operation per project. */
        std::filesystem::path RecoveryPath(const std::filesystem::path &root) {
            return root / ".horo/local/recovery/prefab-apply-pending.bin";
        }

        /** @brief A recovery capsule never authorizes workspace control files or different asset types. */
        bool ValidRecoveryPath(const std::string_view relative, const std::size_t index) {
            if (relative.empty() || relative.size() > MaximumRecoveryPathBytes || !IsValidUtf8ScalarSequence(relative) ||
                relative.find_first_of(":\\") != std::string_view::npos || relative.find('\0') != std::string_view::npos)
                return false;
            const auto normalized = ProjectPath::Parse(relative);
            if (normalized.HasError() || normalized.Value().String() != relative)
                return false;
            std::string first{relative.substr(0, relative.find('/'))};
            std::ranges::transform(first, first.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            const auto path = std::filesystem::path{std::u8string(relative.begin(), relative.end())};
            if (first == ".horo" || std::ranges::any_of(path, [](const auto &part) {
                const auto &name = part.native();
                return name.ends_with('.') || name.ends_with(' ');
            }))
                return false;
            return path.extension() == (index == 0 ? ".prefab" : ".horo");
        }

        /** @brief Appends a bounded canonical raw field. */
        template <typename Range> void Append(std::vector<std::byte> &output, const Range &value) {
            const auto bytes = std::as_bytes(std::span{value});
            output.insert(output.end(), bytes.begin(), bytes.end());
        }

        /** @brief Uses fixed big-endian widths independent of native size_t. */
        void AppendSize(std::vector<std::byte> &output, const std::size_t value) {
            for (unsigned shift = 32; shift > 0; shift -= 8)
                output.push_back(static_cast<std::byte>((value >> (shift - 8)) & 0xff));
        }

        /** @brief Reads only checked fields from bounded local recovery bytes. */
        struct RecoveryReader final {
            std::span<const std::byte> remaining;
            bool valid{true};

            std::span<const std::byte> Take(const std::size_t size) {
                if (!valid || size > remaining.size()) {
                    valid = false;
                    return {};
                }
                const auto field = remaining.first(size);
                remaining = remaining.subspan(size);
                return field;
            }

            std::size_t Size() {
                std::size_t value{};
                for (const auto byte : Take(4))
                    value = (value << 8) | std::to_integer<std::uint8_t>(byte);
                return value;
            }

            Sha256Digest Digest() {
                Sha256Digest result;
                const auto bytes = Take(result.bytes.size());
                std::ranges::transform(bytes, result.bytes.begin(), [](const auto byte) {
                    return std::to_integer<std::uint8_t>(byte);
                });
                return result;
            }
        };

        /** @brief Saves exact original bytes and identities of the already prepared canonical pair. */
        Result<std::vector<std::byte>> EncodeRecovery(const std::filesystem::path &root, const std::array<ApplyFile, 2> &files,
                                                      const Assets::AssetId attempt) {
            using Return = Result<std::vector<std::byte>>;
            std::vector<std::byte> encoded;
            Append(encoded, CapsuleMagic);
            encoded.push_back(static_cast<std::byte>(JournalDisposition::Prepared));
            Append(encoded, attempt.Bytes());
            for (std::size_t index = 0; index < files.size(); ++index) {
                const auto &file = files[index];
                const auto &original = file.original;
                const auto name = file.path.lexically_relative(root).generic_u8string();
                const std::string relative{name.begin(), name.end()};
                if (!original.identity.exists || !ValidRecoveryPath(relative, index))
                    return Return::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
                const auto &planned = file.planned;
                const auto payloadSize = planned != original.identity ? original.bytes.size() : 0;
                const auto overhead = 32U + 12U + 64U + relative.size();
                if (overhead > MaximumCapsuleBytes - encoded.size() || payloadSize > MaximumCapsuleBytes - encoded.size() - overhead)
                    return Return::Failure(MakeError(Assets::AssetSourceErrors::Limit));
                AppendSize(encoded, relative.size());
                AppendSize(encoded, original.identity.bytes);
                AppendSize(encoded, planned.bytes);
                Append(encoded, original.identity.digest.bytes);
                Append(encoded, planned.digest.bytes);
                Append(encoded, relative);
                if (planned != original.identity)
                    Append(encoded, original.bytes);
            }
            Append(encoded, ComputeSha256(encoded).bytes);
            return Return::Success(std::move(encoded));
        }

        /** @brief Decoded spans borrow the one checksummed capsule until the restart operation finishes. */
        struct RecoveryTarget final {
            std::filesystem::path path;
            std::span<const std::byte> originalBytes;
            Assets::AssetSourceIdentity original;
            Assets::AssetSourceIdentity planned;
            Assets::AssetSourceIdentity observed;
            std::optional<ExclusiveFileLock> lock;
        };

        /** @brief Validated journal header and targets, borrowing original bytes from the bounded capture. */
        struct RecoveryRecord final {
            JournalDisposition disposition{JournalDisposition::RollbackRequired};
            std::optional<Assets::AssetId> attempt;
            std::array<RecoveryTarget, 2> targets;
        };

        /** @brief Validates wire shape, stable ownership paths and byte identities before any source admission. */
        Result<RecoveryRecord> DecodeRecovery(const std::filesystem::path &root, std::span<const std::byte> bytes) {
            using Return = Result<RecoveryRecord>;
            const auto invalid = [] {
                return Return::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            };
            if (bytes.size() < CapsuleMagic.size() + 32)
                return invalid();
            RecoveryReader tail{bytes.last(32)};
            const auto content = bytes.first(bytes.size() - 32);
            if (ComputeSha256(content) != tail.Digest())
                return invalid();
            RecoveryReader reader{content};
            const auto version = reader.Take(CapsuleMagic.size());
            RecoveryRecord record;
            const bool hasAttempt = std::ranges::equal(version, std::as_bytes(std::span{CapsuleMagic}));
            if (hasAttempt || std::ranges::equal(version, std::as_bytes(std::span{DispositionCapsuleMagic}))) {
                const auto disposition = reader.Take(1);
                if (disposition.empty())
                    return invalid();
                record.disposition = static_cast<JournalDisposition>(disposition.front());
                if (record.disposition != JournalDisposition::Prepared && record.disposition != JournalDisposition::RollbackRequired &&
                    record.disposition != JournalDisposition::Committed)
                    return invalid();
            } else if (!std::ranges::equal(version, std::as_bytes(std::span{LegacyCapsuleMagic})))
                return invalid();
            if (hasAttempt) {
                std::array<std::uint8_t, 16> id{};
                const auto raw = reader.Take(id.size());
                std::ranges::transform(raw, id.begin(), [](const auto value) {
                    return std::to_integer<std::uint8_t>(value);
                });
                auto attempt = Assets::AssetId::FromBytes(id);
                if (!reader.valid || !attempt.IsValid())
                    return invalid();
                record.attempt = attempt;
            }
            auto &targets = record.targets;
            for (std::size_t index = 0; index < targets.size(); ++index) {
                const auto nameSize = reader.Size();
                const auto originalSize = reader.Size();
                const auto plannedSize = reader.Size();
                const Assets::AssetSourceIdentity originalIdentity{true, originalSize, reader.Digest()};
                const Assets::AssetSourceIdentity planned{true, plannedSize, reader.Digest()};
                const auto name = reader.Take(nameSize);
                const auto original = reader.Take(originalIdentity == planned ? 0 : originalSize);
                if (!reader.valid || name.empty() || originalSize > MaximumCapsuleBytes || plannedSize > MaximumCapsuleBytes ||
                    (originalIdentity != planned && ComputeSha256(original) != originalIdentity.digest))
                    return invalid();
                const std::string_view relative{reinterpret_cast<const char *>(name.data()), name.size()};
                if (!ValidRecoveryPath(relative, index))
                    return invalid();
                targets[index] = {.path = root / std::filesystem::path{std::u8string(relative.begin(), relative.end())},
                                  .originalBytes = original,
                                  .original = originalIdentity,
                                  .planned = planned};
            }
            if (!reader.remaining.empty() || targets.front().path == targets.back().path)
                return invalid();
            return Return::Success(std::move(record));
        }

        /** @brief Replaces the typed disposition using the latest exact durable-recovery receipt. */
        Result<void> PublishJournal(const std::filesystem::path &root, ApplyJournal &journal, const JournalDisposition disposition,
                                    DurableFileSystem &storage) {
            auto &bytes = journal.bytes;
            auto &tag = bytes[CapsuleMagic.size()];
            const auto encoded = static_cast<std::byte>(disposition);
            if (tag != encoded) {
                tag = encoded;
                const auto digest = ComputeSha256(std::span{bytes}.first(bytes.size() - Sha256Digest{}.bytes.size()));
                std::ranges::copy(std::as_bytes(std::span{digest.bytes}), bytes.end() - digest.bytes.size());
            }
            // The initial journal cannot own its own bootstrap stage. A fresh nonce keeps partial bootstrap bytes isolated.
            const Assets::AssetSourcePublicationAttempt attempt{NewPublicationAttempt(),
                                                                {true, bytes.size(), ComputeSha256(bytes)},
                                                                Assets::AssetSourceAttemptPlacement::ProjectLocal};
            auto publication = PublishDocumentRecoveryAttempt(root, RecoveryPath(root), bytes, journal.identity, attempt, storage,
                                                              {.maximumRecordBytes = MaximumCapsuleBytes});
            if (publication.HasError())
                return Result<void>::Failure(std::move(publication).ErrorValue());
            journal.identity = publication.Value().identity;
            journal.disposition = disposition;
            if (publication.Value().durabilityError)
                return Result<void>::Failure(std::move(*publication.Value().durabilityError));
            if (publication.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
            return Result<void>::Success();
        }

        /** @brief Makes first-use journal directories reachable after a filesystem interruption. */
        Result<void> SyncJournalAncestors(const std::filesystem::path &root, DurableFileSystem &storage) {
            auto directory = RecoveryPath(root).parent_path();
            while (directory != root) {
                directory = directory.parent_path();
                if (auto synced = storage.SyncDirectory(directory); synced.HasError())
                    return synced;
            }
            return Result<void>::Success();
        }

        /** @brief Retires only the exact capsule while its own publication lock is held. */
        Result<void> ClearJournal(const std::filesystem::path &root, const Assets::AssetSourceIdentity &expected,
                                  DurableFileSystem &storage) {
            const auto path = RecoveryPath(root);
            const auto lockPath = Assets::AssetSourcePublicationLockPath(root, path);
            if (lockPath.HasError())
                return Result<void>::Failure(lockPath.ErrorValue());
            auto held = storage.TryAcquireExclusive(lockPath.Value(), "prefab.apply.clear");
            if (held.HasError())
                return Result<void>::Failure(held.ErrorValue());
            const auto capsule = Assets::CaptureAssetSource(root, path, MaximumCapsuleBytes);
            if (capsule.HasError())
                return Result<void>::Failure(capsule.ErrorValue());
            if (capsule.Value().identity != expected)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            return storage.RemoveDurable(path);
        }

        /** @brief Refuses new paired operations while restart evidence exists, even if it is unreadable. */
        Result<void> AdmitNewJournal(const std::filesystem::path &root) {
            const auto pending = Assets::CaptureAssetSource(root, RecoveryPath(root), MaximumCapsuleBytes);
            if (pending.HasError())
                return Result<void>::Failure(pending.ErrorValue());
            if (pending.Value().identity.exists)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
            return Result<void>::Success();
        }

        /** @brief Observes both current identities, using source locks when resolving a reviewed capsule. */
        Result<void> ObserveRecovery(const std::filesystem::path &root, RecoveryRecord &record, DurableFileSystem *storage) {
            for (auto &target : record.targets) {
                if (storage) {
                    const auto path = Assets::AssetSourcePublicationLockPath(root, target.path);
                    if (path.HasError())
                        return Result<void>::Failure(path.ErrorValue());
                    auto lock = storage->TryAcquireExclusive(path.Value(), "prefab.apply.resolve");
                    if (lock.HasError())
                        return Result<void>::Failure(lock.ErrorValue());
                    target.lock.emplace(std::move(lock).Value());
                }
                const auto current = Assets::CaptureAssetSource(root, target.path, MaximumCapsuleBytes);
                if (current.HasError())
                    return Result<void>::Failure(current.ErrorValue());
                target.observed = current.Value().identity;
            }
            return Result<void>::Success();
        }

        /** @brief A coherent planned pair survives interrupted commit-marker installation. */
        PrefabApplyRecoveryCandidate ReviewRecovery(const Assets::AssetSourceIdentity &identity, const RecoveryRecord &record) {
            const auto &targets = record.targets;
            const bool fullyPublished = std::ranges::all_of(targets, [](const auto &target) {
                return target.observed == target.planned;
            });
            const bool keep = record.disposition == JournalDisposition::Committed ||
                              (record.disposition == JournalDisposition::Prepared && fullyPublished);
            return {identity,
                    {targets[0].path, targets[0].original, targets[0].planned, targets[0].observed},
                    {targets[1].path, targets[1].original, targets[1].planned, targets[1].observed},
                    keep ? PrefabApplyRecoveryAction::KeepPublished : PrefabApplyRecoveryAction::RestoreOriginals};
        }

        /** @brief Admits both canonical file destinations before either is changed. */
        Result<void> CaptureFiles(std::array<ApplyFile, 2> &files, const std::filesystem::path &root, DurableFileSystem &storage,
                                  const CancellationToken &cancellation) {
            for (auto &file : files) {
                const auto lockPath = Assets::AssetSourcePublicationLockPath(root, file.path);
                if (lockPath.HasError())
                    return Result<void>::Failure(lockPath.ErrorValue());
                auto lock = storage.TryAcquireExclusive(lockPath.Value(), "prefab.apply");
                if (lock.HasError())
                    return Result<void>::Failure(std::move(lock).ErrorValue());
                file.lock.emplace(std::move(lock).Value());
                auto current = Assets::CaptureAssetSource(root, file.path, 64U * 1024U * 1024U, cancellation);
                if (current.HasError())
                    return Result<void>::Failure(std::move(current).ErrorValue());
                if (!file.expected.exists || current.Value().identity != file.expected)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
                file.original = std::move(current).Value();
                if (file.replacement.size() > MaximumCapsuleBytes)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
                file.planned = {true, file.replacement.size(), ComputeSha256(std::as_bytes(std::span{file.replacement}))};
            }
            return Result<void>::Success();
        }

        /** @brief Leaves a mismatched or partial private attempt in its own directory without blocking a fresh write. */
        void RetireAttempt(const std::filesystem::path &root, const std::filesystem::path &path,
                           const Assets::AssetSourcePublicationAttempt &attempt, const ExclusiveFileLock &lock,
                           DurableFileSystem &storage) noexcept {
            try {
                if (auto retired = Assets::RetireAssetSourcePublicationAttempt(root, path, attempt, lock, storage); retired.HasError())
                    LOG_WARN("editor.prefab", "Private Prefab publication attempt retained: %s", retired.ErrorValue().message.c_str());
            } catch (...) {
                LOG_WARN("editor.prefab", "Private Prefab publication attempt retirement failed.");
            }
        }

        /** @brief Publishes restoration through an independent attempt so a torn old stage cannot obstruct it. */
        Result<Assets::AssetSourcePublication> PublishOriginal(const std::filesystem::path &root, const std::filesystem::path &path,
                                                               const Assets::AssetSourceIdentity &expected,
                                                               const Assets::AssetSourceIdentity &original,
                                                               const std::span<const std::byte> bytes, const ExclusiveFileLock &lock,
                                                               DurableFileSystem &storage) {
            const Assets::AssetSourcePublicationAttempt attempt{NewPublicationAttempt(), original};
            return Assets::PublishAssetSourceAttempt(root, path, expected, bytes, attempt, lock, storage);
        }

        /** @brief Publishes the tracked replacement while retaining post-rename failure evidence. */
        Result<void> PublishFile(ApplyFile &file, const std::filesystem::path &root, const Assets::AssetId attempt,
                                 DurableFileSystem &storage, const CancellationToken &cancellation) {
            const Assets::AssetSourcePublicationAttempt chosen{attempt, file.planned};
            auto receipt = Assets::PublishAssetSourceAttempt(root, file.path, file.expected, std::as_bytes(std::span{file.replacement}),
                                                             chosen, *file.lock, storage, cancellation);
            if (receipt.HasError())
                return Result<void>::Failure(std::move(receipt).ErrorValue());
            file.installed = receipt.Value().identity;
            if (receipt.Value().durabilityError)
                return Result<void>::Failure(std::move(*receipt.Value().durabilityError));
            if (receipt.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
            return Result<void>::Success();
        }

        /** @brief Checks both completed receipts again before publishing either authored owner. */
        Result<void> VerifyInstalledFiles(const std::array<ApplyFile, 2> &files, const std::filesystem::path &root,
                                          const CancellationToken &cancellation) {
            for (const auto &file : files) {
                auto current = Assets::CaptureAssetSource(root, file.path, 64U * 1024U * 1024U, cancellation);
                if (current.HasError())
                    return Result<void>::Failure(std::move(current).ErrorValue());
                if (!file.installed || *file.installed != file.planned || current.Value().identity != file.planned)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            }
            return Result<void>::Success();
        }

        /** @brief Compensates only still-owned installed bytes and leaves explicit evidence on uncertainty. */
        void Compensate(std::array<ApplyFile, 2> &files, const std::filesystem::path &root, DurableFileSystem &storage,
                        PrefabApplyOutcome &outcome, std::array<Error, 2> &fallback) noexcept {
            for (std::size_t index = files.size(); index > 0; --index) {
                auto &file = files[index - 1];
                if (!file.installed)
                    continue;
                auto failure = std::move(fallback[index - 1]);
                std::optional<Assets::AssetSourceIdentity> restoredUnconfirmed;
                try {
                    auto restored =
                        PublishOriginal(root, file.path, *file.installed, file.original.identity, file.original.bytes, *file.lock, storage);
                    if (restored.HasValue() && restored.Value().status == Assets::AssetSourcePublicationStatus::Durable)
                        continue;
                    if (restored.HasError())
                        failure = std::move(restored).ErrorValue();
                    else {
                        restoredUnconfirmed = restored.Value().identity;
                        if (restored.Value().durabilityError)
                            failure = std::move(*restored.Value().durabilityError);
                    }
                } catch (...) {
                    // The preallocated recovery error preserves the original attempt failure.
                }
                outcome.recovery[index - 1].emplace(std::move(file.path), std::move(file.original), *file.installed, restoredUnconfirmed,
                                                    std::move(failure));
            }
        }
    }  // namespace

    /** @copydoc PrefabApplyTransaction::InspectPendingRecovery */
    Result<std::optional<PrefabApplyRecoveryCandidate>> PrefabApplyTransaction::InspectPendingRecovery(const std::filesystem::path &root) {
        using Return = Result<std::optional<PrefabApplyRecoveryCandidate>>;
        const auto capsule = Assets::CaptureAssetSource(root, RecoveryPath(root), MaximumCapsuleBytes);
        if (capsule.HasError())
            return Return::Failure(capsule.ErrorValue());
        const auto &identity = capsule.Value().identity;
        if (!identity.exists)
            return Return::Success(std::nullopt);
        auto decoded = DecodeRecovery(root, capsule.Value().bytes);
        if (decoded.HasError())
            return Return::Failure(decoded.ErrorValue());
        auto record = std::move(decoded).Value();
        if (auto observed = ObserveRecovery(root, record, nullptr); observed.HasError())
            return Return::Failure(observed.ErrorValue());
        return Return::Success(ReviewRecovery(identity, record));
    }

    /** @copydoc PrefabApplyTransaction::ResolvePendingRecovery */
    Result<void> PrefabApplyTransaction::ResolvePendingRecovery(const std::filesystem::path &root,
                                                                const PrefabApplyRecoveryCandidate &expected,
                                                                const ProjectMutationCoordinator &mutations, DurableFileSystem &storage) {
        using Return = Result<void>;
        const auto conflict = [] {
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        };
        if (!expected.capsule.exists)
            return conflict();
        auto project = mutations.TryAcquire({root, ProjectMutationOwner::Save, "prefab.apply.resolve"});
        if (project.HasError())
            return Return::Failure(project.ErrorValue());
        const auto path = RecoveryPath(root);
        const auto lockPath = Assets::AssetSourcePublicationLockPath(root, path);
        if (lockPath.HasError())
            return Return::Failure(lockPath.ErrorValue());
        auto capsuleLock = storage.TryAcquireExclusive(lockPath.Value(), "prefab.apply.resolve");
        if (capsuleLock.HasError())
            return Return::Failure(capsuleLock.ErrorValue());
        const auto capsule = Assets::CaptureAssetSource(root, path, MaximumCapsuleBytes);
        if (capsule.HasError())
            return Return::Failure(capsule.ErrorValue());
        if (capsule.Value().identity != expected.capsule)
            return conflict();
        auto decoded = DecodeRecovery(root, capsule.Value().bytes);
        if (decoded.HasError())
            return Return::Failure(decoded.ErrorValue());
        auto record = std::move(decoded).Value();
        if (auto observed = ObserveRecovery(root, record, &storage); observed.HasError())
            return observed;
        if (ReviewRecovery(expected.capsule, record) != expected)
            return conflict();

        const bool keep = expected.action == PrefabApplyRecoveryAction::KeepPublished;
        const auto selected = [keep](const auto &target) -> const Assets::AssetSourceIdentity & {
            return keep ? target.planned : target.original;
        };
        const auto &targets = record.targets;
        for (const auto &target : targets) {
            if (target.observed != selected(target) && (keep || target.observed != target.planned))
                return conflict();
        }
        if (record.attempt) {
            for (const auto &target : targets)
                RetireAttempt(root, target.path, {*record.attempt, target.planned}, *target.lock, storage);
        }
        // Compensate owned replacements in reverse order. Keeping a pair never writes either source.
        for (auto target = targets.rbegin(); target != targets.rend(); ++target) {
            if (target->observed == selected(*target)) {
                if (auto synced = storage.SyncDirectory(target->path.parent_path()); synced.HasError())
                    return synced;
                continue;
            }
            auto restored =
                PublishOriginal(root, target->path, target->planned, target->original, target->originalBytes, *target->lock, storage);
            if (restored.HasError())
                return Return::Failure(restored.ErrorValue());
            if (restored.Value().durabilityError)
                return Return::Failure(*restored.Value().durabilityError);
            if (restored.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
        }
        for (const auto &target : targets) {
            const auto current = Assets::CaptureAssetSource(root, target.path, MaximumCapsuleBytes);
            if (current.HasError())
                return Return::Failure(current.ErrorValue());
            if (current.Value().identity != selected(target))
                return conflict();
        }
        const auto current = Assets::CaptureAssetSource(root, path, MaximumCapsuleBytes);
        if (current.HasError())
            return Return::Failure(current.ErrorValue());
        if (current.Value().identity != expected.capsule)
            return conflict();
        return storage.RemoveDurable(path);
    }

    /** @copydoc PrefabApplyTransaction::Execute */
    PrefabApplyOutcome PrefabApplyTransaction::Execute(SceneDocumentTransactions &scene, PrefabDocumentSession &prefab,
                                                       const PrefabApplyTransactionRequest &request,
                                                       const Assets::AssetRegistrySnapshot &registry, const PrefabHierarchySources &sources,
                                                       std::shared_ptr<const Prefab::PrefabValueSchemaSnapshot> schemas,
                                                       const Prefab::PrefabLimitProfile &limits,
                                                       const ProjectMutationCoordinator &mutations, DurableFileSystem &storage) {
        auto admitted = prefab.Admit(request.source);
        if (admitted.HasError())
            return {.error = std::move(admitted).ErrorValue()};
        auto project = mutations.TryAcquire({prefab.root_, ProjectMutationOwner::Save, "prefab.apply"});
        if (project.HasError())
            return {.error = std::move(project).ErrorValue()};
        try {
            if (const auto admittedJournal = AdmitNewJournal(prefab.root_); admittedJournal.HasError())
                return {.error = admittedJournal.ErrorValue()};
            if (auto current = sources.inputs.VerifyUnchanged(request.scene.cancellation); current.HasError())
                return {.error = std::move(current).ErrorValue()};
            const auto commitments = sources.resolver.CanonicalSourceCommitments();
            const auto base =
                std::ranges::find(commitments, prefab.source_->Data().assetId, &Prefab::PrefabCanonicalSourceCommitment::asset);
            if (base == commitments.end() || base->digest != prefab.state_)
                return {.error = MakeError(Prefab::PrefabErrors::OverrideSourceStale)};
            const auto snapshot = scene.Snapshot();
            if (snapshot.HasError())
                return {.error = snapshot.ErrorValue()};
            const auto &current = snapshot.Value();
            SceneDocumentSnapshot view{{current.revision}, {current.state}, current.objects, current.prefabInstances};
            auto content = PreparePrefabApply(view, request.selection, registry, sources.resolver, std::move(schemas), limits,
                                              request.scene.cancellation);
            if (content.HasError())
                return {.error = std::move(content).ErrorValue()};
            auto prepared = std::move(content).Value();
            auto sceneState = scene.PreparePrefabValues(request.scene, prepared.placements, request.selection.expectedReplacementEpoch);
            if (sceneState.HasError())
                return {.error = std::move(sceneState).ErrorValue()};
            const auto next = scene.PreparedContext(*sceneState.Value());
            const PrefabApplyHistoryGroup group{next.session.owner, next.session.generation, next.state};
            auto sourceState = prefab.PrepareApply(request.source, std::move(prepared.source), group);
            if (sourceState.HasError())
                return {.error = std::move(sourceState).ErrorValue()};
            auto preparedSource = std::move(sourceState).Value();
            return Publish(scene, prefab, *sceneState.Value(), preparedSource,
                           {request.scene, request.source, request.scenePath, request.sceneIdentity, group}, storage);
        } catch (...) {
            return {.error = MakeError(Assets::AssetSourceErrors::ReadFailed, "Prefab apply preparation failed.")};
        }
    }

    /** @copydoc PrefabApplyTransaction::NextSceneGroup */
    Result<std::optional<PrefabApplyHistoryGroup>> PrefabApplyTransaction::NextSceneGroup(const SceneDocumentTransactions &scene,
                                                                                          const SceneTransactionContext &context,
                                                                                          const bool redo) {
        auto state = scene.NextPrefabValueState(context, redo);
        if (state.HasError())
            return Result<std::optional<PrefabApplyHistoryGroup>>::Failure(std::move(state).ErrorValue());
        if (!state.Value())
            return Result<std::optional<PrefabApplyHistoryGroup>>::Success(std::nullopt);
        return Result<std::optional<PrefabApplyHistoryGroup>>::Success(
            PrefabApplyHistoryGroup{context.session.owner, context.session.generation, *state.Value()});
    }

    /** @copydoc PrefabApplyTransaction::ReconcileHistory */
    void PrefabApplyTransaction::ReconcileHistory(SceneDocumentTransactions &scene, const SceneTransactionContext &context,
                                                  const std::span<const std::unique_ptr<PrefabDocumentSession>> sources) {
        if (scene.Admit(context).HasError() || std::ranges::any_of(sources, [](const auto &source) {
            return source->busy_;
        }))
            return;
        const std::function<bool(std::uint64_t, bool)> unavailable = [&](const std::uint64_t state, const bool redo) {
            const PrefabApplyHistoryGroup group{context.session.owner, context.session.generation, state};
            return std::ranges::none_of(sources, [&](const auto &source) {
                return !source->closed_ && source->history_.Contains(redo, [&](const auto &entry) {
                    return entry.applyGroup == group;
                });
            });
        };
        bool retired;
        do {
            retired = scene.RetirePrefabValueHistory(unavailable);
            for (const auto &source : sources) {
                const bool retiredSource = source->history_.RetireUnavailable([&](const auto &entry, const bool redo) {
                    const auto &group = entry.applyGroup;
                    return group && (group->sceneOwner != context.session.owner || group->sceneOpening != context.session.generation ||
                                     !scene.HasPrefabValueHistory(group->sceneState, redo));
                });
                retired = retired || retiredSource;
            }
        } while (retired);
    }

    /** @copydoc PrefabApplyTransaction::InspectSource */
    Result<void> PrefabApplyTransaction::InspectSource(PrefabDocumentSession &source) {
        auto inspected = source.InspectExternal();
        if (inspected.HasValue() && source.conflict_)
            source.history_.RetireUnavailable([](const auto &entry, bool) {
                return entry.applyGroup.has_value();
            });
        return inspected;
    }

    /** @copydoc PrefabApplyTransaction::Replay */
    PrefabApplyOutcome PrefabApplyTransaction::Replay(SceneDocumentTransactions &scene, PrefabDocumentSession &prefab,
                                                      const PrefabApplyReplayRequest &request, const ProjectMutationCoordinator &mutations,
                                                      DurableFileSystem &storage) {
        auto admitted = prefab.Admit(request.source);
        if (admitted.HasError())
            return {.error = std::move(admitted).ErrorValue()};
        auto project = mutations.TryAcquire({prefab.root_, ProjectMutationOwner::Save, "prefab.apply.replay"});
        if (project.HasError())
            return {.error = std::move(project).ErrorValue()};
        try {
            if (const auto admittedJournal = AdmitNewJournal(prefab.root_); admittedJournal.HasError())
                return {.error = admittedJournal.ErrorValue()};
            const auto group = NextSceneGroup(scene, request.scene, request.redo);
            if (group.HasError())
                return {.error = group.ErrorValue()};
            if (group.Value() != request.group)
                return {.error = MakeError(Prefab::PrefabErrors::OverrideSourceStale)};
            auto sceneState = scene.PrepareReplay(request.scene, request.redo);
            if (sceneState.HasError())
                return {.error = std::move(sceneState).ErrorValue()};
            auto sourceState = prefab.PrepareApplyReplay(request.source, request.group, request.redo);
            if (sourceState.HasError())
                return {.error = std::move(sourceState).ErrorValue()};
            auto preparedSource = std::move(sourceState).Value();
            return Publish(scene, prefab, *sceneState.Value(), preparedSource, request, storage);
        } catch (...) {
            return {.error = MakeError(Assets::AssetSourceErrors::ReadFailed, "Prefab apply replay preparation failed.")};
        }
    }

    /** @copydoc PrefabApplyTransaction::Publish */
    PrefabApplyOutcome PrefabApplyTransaction::Publish(SceneDocumentTransactions &scene, PrefabDocumentSession &prefab,
                                                       SceneDocumentTransactions::Prepared &preparedScene,
                                                       PrefabDocumentSession::PreparedApply &preparedSource,
                                                       const PrefabApplyReplayRequest &request, DurableFileSystem &storage) {
        if (request.scenePath == prefab.path_)
            return {.error = MakeError(Prefab::PrefabErrors::DocumentInvalid)};
        PrefabApplyOutcome outcome;
        std::array<Error, 2> rollbackFailure{MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed),
                                             MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed)};
        auto exception = MakeError(Assets::AssetSourceErrors::ReadFailed, "Prefab apply publication failed.");
        auto recoveryException = MakeError(Assets::AssetSourceErrors::ReadFailed, "Prefab apply recovery publication failed.");
        std::array<ApplyFile, 2> files{{{prefab.path_, prefab.accepted_}, {request.scenePath, request.sceneIdentity}}};
        ApplyJournal journal;
        try {
            const auto run = [&]() -> Result<void> {
                auto sourceBytes = preparedSource.source->SerializeCanonical();
                if (sourceBytes.HasError())
                    return Result<void>::Failure(std::move(sourceBytes).ErrorValue());
                files[0].replacement = std::move(sourceBytes).Value();
                files[1].replacement = scene.EncodePreparedScene(preparedScene);
                if (auto valid = scene.ValidatePrepared(preparedScene); valid.HasError())
                    return valid;
                auto resultingScene = scene.PreparedContext(preparedScene);
                auto completingScene = request.scene;
                completingScene.cancellation = {};
                resultingScene.cancellation = {};
                {
                    const SourceWriteGuard guard(prefab.busy_);
                    if (auto captured = CaptureFiles(files, prefab.root_, storage, request.scene.cancellation); captured.HasError())
                        return captured;
                    outcome.sceneChecksum = FormatSha256(files[1].planned.digest);
                    journal.attempt = NewPublicationAttempt();
                    auto encoded = EncodeRecovery(prefab.root_, files, journal.attempt);
                    if (encoded.HasError())
                        return Result<void>::Failure(std::move(encoded).ErrorValue());
                    journal.bytes = std::move(encoded).Value();
                    if (auto prepared = PublishJournal(prefab.root_, journal, JournalDisposition::Prepared, storage); prepared.HasError())
                        return prepared;
                    if (auto anchored = SyncJournalAncestors(prefab.root_, storage); anchored.HasError())
                        return anchored;
                    for (auto &file : files)
                        if (auto published = PublishFile(file, prefab.root_, journal.attempt, storage, request.scene.cancellation);
                            published.HasError())
                            return published;
                    if (auto current = VerifyInstalledFiles(files, prefab.root_, request.scene.cancellation); current.HasError())
                        return current;
                }
                if (auto valid = scene.ValidatePrepared(preparedScene); valid.HasError())
                    return valid;
                if (auto valid = prefab.Admit(request.source); valid.HasError())
                    return valid;
                {
                    const SourceWriteGuard guard(prefab.busy_);
                    auto decision = PublishJournal(prefab.root_, journal, JournalDisposition::Committed, storage);
                    if (decision.HasError()) {
                        if (journal.disposition != JournalDisposition::Committed)
                            return decision;
                        outcome.recoveryPersistenceError = std::move(decision).ErrorValue();
                    }
                    // Once the commit decision is visible, rollback could contradict crash recovery.
                    if (auto current = VerifyInstalledFiles(files, prefab.root_, {}); current.HasError())
                        return current;
                }
                if (auto valid = scene.Admit(completingScene); valid.HasError())
                    return valid;
                if (auto valid = prefab.Admit(request.source); valid.HasError())
                    return valid;
                prefab.PublishApply(preparedSource, files[0].planned);
                outcome.committed = scene.PublishPrepared(preparedScene);
                outcome.group = request.group;
                outcome.sceneIdentity = files[1].planned;
                return scene.MarkSaved(resultingScene);
            };
            if (auto applied = run(); applied.HasError())
                outcome.error = std::move(applied).ErrorValue();
        } catch (...) {
            outcome.error = std::move(exception);
        }

        const SourceWriteGuard guard(prefab.busy_);
        if (!outcome.committed && journal.disposition != JournalDisposition::Committed) {
            if (std::ranges::any_of(files, [](const auto &file) {
                return file.installed.has_value();
            })) {
                try {
                    if (auto recorded = PublishJournal(prefab.root_, journal, JournalDisposition::RollbackRequired, storage);
                        recorded.HasError())
                        outcome.recoveryPersistenceError = std::move(recorded).ErrorValue();
                } catch (...) {
                    outcome.recoveryPersistenceError = std::move(recoveryException);
                }
                Compensate(files, prefab.root_, storage, outcome, rollbackFailure);
            }
        }
        const bool incomplete = outcome.committed ? outcome.recoveryPersistenceError.has_value()
                                                  : journal.disposition == JournalDisposition::Committed ||
                                                        std::ranges::any_of(outcome.recovery, [](const auto &item) {
            return item.has_value();
        });
        if (journal.identity.exists && !incomplete) {
            try {
                if (auto cleared = ClearJournal(prefab.root_, journal.identity, storage); cleared.HasValue())
                    journal.identity = {};
                else
                    outcome.recoveryPersistenceError = std::move(cleared).ErrorValue();
            } catch (...) {
                if (!outcome.recoveryPersistenceError)
                    outcome.recoveryPersistenceError = std::move(recoveryException);
            }
        }
        outcome.pendingRecovery = journal.identity.exists;
        if (outcome.committed)
            prefab.ReleaseStage();
        return outcome;
    }
}  // namespace Horo::Editor
