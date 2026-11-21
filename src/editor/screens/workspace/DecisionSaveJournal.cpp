#include "DecisionSaveJournal.h"

#include "DecisionGraphLayout.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Editor/DocumentRecoveryService.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Horo::Editor {
    namespace {
        using Identity = Assets::AssetSourceIdentity;
        using Capture = Assets::AssetSourceCapture;
        constexpr std::string_view Magic = "HORODSJ1";
        constexpr auto GraphLimit = AI::DecisionCodecLimits{}.maximumBytes;
        constexpr auto DigestBytes = Sha256Digest{}.bytes.size();
        constexpr auto IdentityBytes = 1 + sizeof(std::uint64_t) + DigestBytes;
        constexpr auto HeaderBytes = Magic.size() + DigestBytes + 3 * IdentityBytes;
        constexpr auto JournalLimit = GraphLimit + HeaderBytes + DigestBytes;

        std::filesystem::path JournalPath(const std::filesystem::path &root, Assets::AssetId asset) {
            return root / ".horo/local/recovery" / ("decision-save-" + asset.ToString() + ".journal");
        }

        Identity Identify(std::span<const std::byte> bytes) noexcept {
            return {true, bytes.size(), ComputeSha256(bytes)};
        }

        void Append(std::vector<std::byte> &bytes, std::span<const std::byte> field) {
            bytes.insert(bytes.end(), field.begin(), field.end());
        }

        void AppendIdentity(std::vector<std::byte> &bytes, const Identity &identity) {
            bytes.push_back(identity.exists ? std::byte{1} : std::byte{0});
            for (int shift = 56; shift >= 0; shift -= 8)
                bytes.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(identity.bytes) >> shift) & 0xff));
            Append(bytes, std::as_bytes(std::span{identity.digest.bytes}));
        }

        Sha256Digest ReadDigest(std::span<const std::byte> bytes) {
            Sha256Digest digest;
            std::ranges::copy(bytes.first(DigestBytes), std::as_writable_bytes(std::span{digest.bytes}).begin());
            return digest;
        }

        std::optional<Identity> ReadIdentity(std::span<const std::byte> bytes, std::size_t maximum) {
            const auto marker = bytes.front();
            std::uint64_t size{};
            for (const auto byte : bytes.subspan(1, sizeof(size)))
                size = (size << 8) | std::to_integer<std::uint64_t>(byte);
            const auto digest = ReadDigest(bytes.last(DigestBytes));
            if (size > maximum || (marker != std::byte{0} && marker != std::byte{1}))
                return std::nullopt;
            if (marker == std::byte{0} && (size || digest != Sha256Digest{}))
                return std::nullopt;
            return Identity{marker == std::byte{1}, static_cast<std::size_t>(size), digest};
        }

        struct JournalView final {
            Identity graphAfter;
            Identity layoutBefore;
            Identity layoutAfter;
            std::span<const std::byte> graphBefore;
        };

        Result<JournalView> DecodeJournal(std::span<const std::byte> bytes, std::string_view sourceKey) {
            using Decoded = Result<JournalView>;
            if (bytes.size() <= HeaderBytes + DigestBytes ||
                !std::ranges::equal(bytes.first(Magic.size()), std::as_bytes(std::span{Magic})))
                return Decoded::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto checksum = ReadDigest(bytes.last(DigestBytes));
            if (ComputeSha256(bytes.first(bytes.size() - DigestBytes)) != checksum ||
                ReadDigest(bytes.subspan(Magic.size(), DigestBytes)) != ComputeSha256(std::as_bytes(std::span{sourceKey})))
                return Decoded::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto ids = bytes.subspan(Magic.size() + DigestBytes, 3 * IdentityBytes);
            const auto graphAfter = ReadIdentity(ids.first(IdentityBytes), GraphLimit);
            const auto layoutBefore = ReadIdentity(ids.subspan(IdentityBytes, IdentityBytes), MaximumDecisionLayoutBytes);
            const auto layoutAfter = ReadIdentity(ids.last(IdentityBytes), MaximumDecisionLayoutBytes);
            if (!graphAfter || !graphAfter->exists || !layoutBefore || !layoutAfter || !layoutAfter->exists)
                return Decoded::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            return Decoded::Success(
                {*graphAfter, *layoutBefore, *layoutAfter, bytes.subspan(HeaderBytes, bytes.size() - HeaderBytes - DigestBytes)});
        }

        Result<ExclusiveFileLock> Acquire(const std::filesystem::path &root, const std::filesystem::path &path, DurableFileSystem &files) {
            const auto lockPath = Assets::AssetSourcePublicationLockPath(root, path);
            if (lockPath.HasError())
                return Result<ExclusiveFileLock>::Failure(lockPath.ErrorValue());
            return files.TryAcquireExclusive(lockPath.Value(), "decision-save-journal");
        }

        Result<void> RequireDurable(const Assets::AssetSourcePublication &receipt) {
            if (receipt.durabilityError)
                return Result<void>::Failure(*receipt.durabilityError);
            if (receipt.status != Assets::AssetSourcePublicationStatus::Durable)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
            return Result<void>::Success();
        }

        /** @brief The layout replacement is the commit point. No path is mutated on an unknown observed pair. */
        Result<void> ReconcilePair(const std::filesystem::path &root, const std::filesystem::path &graphPath, const JournalView &journal,
                                   DurableFileSystem &files) {
            using Reconciled = Result<void>;
            auto graphLease = Acquire(root, graphPath, files);
            if (graphLease.HasError())
                return Reconciled::Failure(graphLease.ErrorValue());
            const auto layoutPath = DecisionLayoutPath(graphPath);
            auto layoutLease = Acquire(root, layoutPath, files);
            if (layoutLease.HasError())
                return Reconciled::Failure(layoutLease.ErrorValue());
            const auto graph = Assets::CaptureAssetSource(root, graphPath, GraphLimit);
            if (graph.HasError())
                return Reconciled::Failure(graph.ErrorValue());
            const auto layout = Assets::CaptureAssetSource(root, layoutPath, MaximumDecisionLayoutBytes);
            if (layout.HasError())
                return Reconciled::Failure(layout.ErrorValue());
            const auto before = Identify(journal.graphBefore);
            if (graph.Value().identity == before && layout.Value().identity == journal.layoutBefore)
                return files.SyncDirectory(graphPath.parent_path());
            if (graph.Value().identity != journal.graphAfter)
                return Reconciled::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            if (layout.Value().identity == journal.layoutAfter)
                return files.SyncDirectory(graphPath.parent_path());
            if (layout.Value().identity != journal.layoutBefore)
                return Reconciled::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto restored =
                Assets::PublishAssetSource(root, graphPath, journal.graphAfter, journal.graphBefore, graphLease.Value(), files);
            return restored.HasError() ? Reconciled::Failure(restored.ErrorValue()) : RequireDurable(restored.Value());
        }
    }  // namespace

    Result<std::optional<Assets::AssetSourceIdentity>> ReadDecisionSaveJournalIdentity(const std::filesystem::path &root,
                                                                                       Assets::AssetId asset) {
        using Observed = Result<std::optional<Identity>>;
        const auto journal = Assets::CaptureAssetSource(root, JournalPath(root, asset), JournalLimit);
        if (journal.HasError())
            return Observed::Failure(journal.ErrorValue());
        return Observed::Success(journal.Value().identity.exists ? std::optional{journal.Value().identity} : std::nullopt);
    }

    Result<void> AdmitDecisionSaveJournal(const std::filesystem::path &root, Assets::AssetId asset) {
        const auto journal = ReadDecisionSaveJournalIdentity(root, asset);
        if (journal.HasError())
            return Result<void>::Failure(journal.ErrorValue());
        if (journal.Value())
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        return Result<void>::Success();
    }

    Result<void> AcceptDecisionSaveJournal(const std::filesystem::path &root, const DecisionSaveJournalKey &key, const Identity &journal,
                                           const Identity &graph, const Identity &layout, DurableFileSystem &files) {
        using Accepted = Result<void>;
        auto graphLease = Acquire(root, key.sourcePath, files);
        if (graphLease.HasError())
            return Accepted::Failure(graphLease.ErrorValue());
        const auto layoutPath = DecisionLayoutPath(key.sourcePath);
        auto layoutLease = Acquire(root, layoutPath, files);
        if (layoutLease.HasError())
            return Accepted::Failure(layoutLease.ErrorValue());
        if (const auto synced = files.SyncDirectory(key.sourcePath.parent_path()); synced.HasError())
            return synced;
        const auto currentGraph = Assets::CaptureAssetSource(root, key.sourcePath, GraphLimit);
        if (currentGraph.HasError())
            return Accepted::Failure(currentGraph.ErrorValue());
        const auto currentLayout = Assets::CaptureAssetSource(root, layoutPath, MaximumDecisionLayoutBytes);
        if (currentLayout.HasError())
            return Accepted::Failure(currentLayout.ErrorValue());
        if (currentGraph.Value().identity != graph || currentLayout.Value().identity != layout)
            return Accepted::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        return ClearDecisionSaveJournal(root, key.asset, journal, files);
    }

    Result<Assets::AssetSourceIdentity> PrepareDecisionSaveJournal(const std::filesystem::path &root, const DecisionSaveJournalKey &key,
                                                                   std::span<const std::byte> previousGraph,
                                                                   std::span<const std::byte> nextGraph, Identity previousLayout,
                                                                   std::span<const std::byte> nextLayout, DurableFileSystem &files) {
        using Prepared = Result<Identity>;
        const auto path = JournalPath(root, key.asset);
        std::vector<std::byte> bytes;
        bytes.reserve(HeaderBytes + previousGraph.size() + DigestBytes);
        const auto source = ComputeSha256(std::as_bytes(std::span{key.sourceKey}));
        Append(bytes, std::as_bytes(std::span{Magic}));
        Append(bytes, std::as_bytes(std::span{source.bytes}));
        AppendIdentity(bytes, Identify(nextGraph));
        AppendIdentity(bytes, previousLayout);
        AppendIdentity(bytes, Identify(nextLayout));
        Append(bytes, previousGraph);
        const auto checksum = ComputeSha256(bytes);
        Append(bytes, std::as_bytes(std::span{checksum.bytes}));
        const auto written = PublishDocumentRecovery(root, path, bytes, {}, files, {.maximumRecordBytes = JournalLimit});
        if (written.HasError())
            return Prepared::Failure(written.ErrorValue());
        if (const auto durable = RequireDurable(written.Value()); durable.HasError())
            return Prepared::Failure(durable.ErrorValue());
        for (const auto &parent : std::array{root / ".horo/local", root / ".horo", root}) {
            if (const auto synced = files.SyncDirectory(parent); synced.HasError())
                return Prepared::Failure(synced.ErrorValue());
        }
        return Prepared::Success(written.Value().identity);
    }

    Result<void> ClearDecisionSaveJournal(const std::filesystem::path &root, Assets::AssetId asset, const Identity &identity,
                                          DurableFileSystem &files) {
        const auto path = JournalPath(root, asset);
        const auto lease = Acquire(root, path, files);
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        const auto captured = Assets::CaptureAssetSource(root, path, JournalLimit);
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        if (captured.Value().identity != identity)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        return files.RemoveDurable(path);
    }

    Result<void> RecoverDecisionSaveJournal(const std::filesystem::path &root, const DecisionSaveJournalKey &key,
                                            DurableFileSystem &files) {
        using Recovered = Result<void>;
        const auto capsule = Assets::CaptureAssetSource(root, JournalPath(root, key.asset), JournalLimit);
        if (capsule.HasError())
            return Recovered::Failure(capsule.ErrorValue());
        if (!capsule.Value().identity.exists)
            return Recovered::Success();
        const auto journal = DecodeJournal(capsule.Value().bytes, key.sourceKey);
        if (journal.HasError())
            return Recovered::Failure(journal.ErrorValue());
        if (const auto reconciled = ReconcilePair(root, key.sourcePath, journal.Value(), files); reconciled.HasError())
            return reconciled;
        return ClearDecisionSaveJournal(root, key.asset, capsule.Value().identity, files);
    }
}  // namespace Horo::Editor
