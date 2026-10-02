#pragma once

/** @file NavigationBakeDiagnostics.h
 * @brief Retained navigation bake evidence, shared output projection and validated source routing.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/Diagnostics/OperationHistory.h"
#include "Horo/Navigation/NavigationTileDependencies.h"
#include "Horo/Runtime/Scene/SceneIdentity.h"

#include <deque>
#include <mutex>
#include <unordered_map>

namespace Horo::Application {
    struct NavigationDiagnosticProjectTag;
    /** @brief Persistent project identity, supplied by the host independently of paths and sessions. */
    using NavigationDiagnosticProjectId = Navigation::NavigationIdentity<NavigationDiagnosticProjectTag>;

    /** @brief Stable source destination; paths are hints and never substitute for identity. */
    struct NavigationDiagnosticTarget {
        Assets::AssetId asset;
        Runtime::SceneDefinitionId scene;
        Runtime::SceneObjectId object;
        std::string relativePath;
        [[nodiscard]] auto operator<=>(const NavigationDiagnosticTarget &) const noexcept = default;
    };

    /** @brief Exact captured source identity and its host-resolved authored destination. */
    struct NavigationDiagnosticSource {
        Navigation::NavigationSourceObservation observation;
        NavigationDiagnosticTarget target;
        [[nodiscard]] auto operator<=>(const NavigationDiagnosticSource &) const noexcept = default;
    };

    /** @brief Closed producer events with stable codes and remediation text. */
    enum class NavigationBakeDiagnosticEvent : std::uint8_t {
        Queued,
        Progress,
        TileFailed,
        StageFailed,
        Succeeded,
        Cancelled,
        Superseded,
        Suppressed,
        Count
    };

    /** @brief Owned diagnostic checkpoint; recovered operation IDs are historical and never control live work. */
    struct NavigationBakeDiagnosticRecord {
        std::uint64_t sequence{};
        OperationId operation{};
        std::optional<BuildOutputSessionId> session;
        NavigationBakeDiagnosticEvent event{NavigationBakeDiagnosticEvent::Progress};
        std::string stage;
        DiagnosticCode code;
        DiagnosticSeverity severity{DiagnosticSeverity::Note};
        BuildOutputResult result{BuildOutputResult::None};
        std::string message;
        std::string suggestedFix;
        std::optional<Navigation::NavigationBakeTileKey> tile;
        std::optional<NavigationDiagnosticSource> source;
        std::optional<float> progress;
        std::string causeCode;
        std::uint64_t suppressedCount{};
        std::uint64_t totalSuppressedRecords{};
        std::uint64_t totalDroppedRecords{};
        bool recovered{};
    };

    /** @brief Bounded retained projection and explicit loss counters. */
    struct NavigationBakeDiagnosticSnapshot {
        std::uint64_t revision{};
        std::uint64_t droppedRecords{};
        std::uint64_t suppressedRecords{};
        std::uint64_t persistenceDrops{};
        std::uint64_t dispatcherDrops{};    /**< Process-wide telemetry delivery loss, separate from journal submission rejection. */
        std::uint64_t historyFailures{};    /**< Failures of this journal's persistence sink. */
        std::uint64_t submissionFailures{}; /**< Rejected or failed diagnostic construction, separate from detail suppression. */
        std::vector<NavigationBakeDiagnosticRecord> records;
    };

    /** @brief Project-owned composition; register the journal in the process telemetry sinks before baking. */
    struct NavigationBakeDiagnosticsConfig {
        NavigationDiagnosticProjectId project;
        Assets::AssetId definition;
        std::filesystem::path projectRoot;
        std::shared_ptr<BuildOutputStore> output;
        std::shared_ptr<Diagnostics::OperationHistorySink> history;
        std::size_t capacity{1024};
        std::size_t maximumRecordsPerOperation{128};
    };

    /** @brief Host-owned, exception-free adapter for one synchronous authored-source navigation action.
     * @details The adapter converts private filesystem/UI exceptions at their owned boundary before returning.
     * Navigation is tooling-only; this interface enforces typed failure propagation without storing a callback owner.
     */
    class INavigationDiagnosticNavigator {
    public:
        virtual ~INavigationDiagnosticNavigator() = default;
        /** @brief Navigates the validated authored destination on its owning host thread.
         * @param target Stable Scene object or asset identity validated against current ownership.
         * @param path Validated absolute source file path.
         * @return True for completed navigation, false for a declined action, or an owned typed failure with its causes. */
        [[nodiscard]] virtual Result<bool> Navigate(const NavigationDiagnosticTarget &target,
                                                    const std::filesystem::path &path) noexcept = 0;
    };

    /** @brief Project-lifetime diagnostic consumer and producer, independent of panels and service facades.
     * @details Record/Snapshot/Export are thread-safe. Navigation is host-thread-only and receives
     * a fresh complete source mapping. The shared dispatcher alone performs persistent I/O.
     * Stop producers, drain telemetry, then release this sink and its output/history owners.
     */
    class NavigationBakeDiagnostics final : public Telemetry::ISink {
        struct ConstructionKey {
        private:
            friend class NavigationBakeDiagnostics;
            ConstructionKey() = default;
        };

    public:
        /** @brief Internal factory-only construction; the private key prevents bypassing Create validation.
         * @param config Validated composition. */
        NavigationBakeDiagnostics(ConstructionKey, NavigationBakeDiagnosticsConfig config);
        /** @brief Validates composition and recovers retained checkpoints into shared Build Output.
         * @param config Persistent project identity, canonical root, stores and positive bounded policy.
         * @return Shared sink or typed invalid composition error. */
        [[nodiscard]] static Result<std::shared_ptr<NavigationBakeDiagnostics>> Create(NavigationBakeDiagnosticsConfig config);
        /** @brief Records a producer checkpoint without filesystem I/O or presentation callbacks.
         * @param record Owned evidence. Terminal and suppression summaries bypass the detail limit.
         * @return True if retained; false for suppressed, invalid or rejected evidence. Persistence loss is reported in Snapshot. */
        bool Record(NavigationBakeDiagnosticRecord record) noexcept;
        /** @brief Counts producer-side construction failure without altering operation terminal truth. */
        void NoteSubmissionFailure() noexcept;
        /** @brief Returns an owned bounded snapshot. @return Records and explicit loss counts. */
        [[nodiscard]] NavigationBakeDiagnosticSnapshot Snapshot() const;
        /** @brief Checks the fixed definition binding at service composition. @param definition Requested asset owner. @return True for
         * this journal's owner. */
        [[nodiscard]] bool Owns(Assets::AssetId definition) const noexcept;
        /** @brief Revalidates project, definition, exact source revision/digest/target and every path component before routing.
         * @param sequence Retained record identity from this journal's snapshot.
         * @param project Current persistent project identity. @param definition Current definition identity.
         * @param sources Fresh authoritative source ownership mappings, including current object/asset existence.
         * @param navigator Host-owned synchronous adapter receiving only the validated destination and canonical file.
         * @return Typed stale/invalid error or the adapter result unchanged, including its cause chain.
         * Rejected input never invokes the adapter. */
        [[nodiscard]] Result<bool> Navigate(std::uint64_t sequence, NavigationDiagnosticProjectId project, Assets::AssetId definition,
                                            std::span<const NavigationDiagnosticSource> sources,
                                            INavigationDiagnosticNavigator &navigator) const;
        /** @copydoc Telemetry::ISink::Export */
        void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *descriptor) override;
        /** @copydoc Telemetry::ISink::Flush */
        void Flush() override;

    private:
        void Retain(NavigationBakeDiagnosticRecord record);
        void Project(const NavigationBakeDiagnosticRecord &record) const;
        void Recover();
        bool RecordImpl(NavigationBakeDiagnosticRecord record);
        bool AdmitRecord(NavigationBakeDiagnosticRecord &record);
        bool ApplyDetailLimit(NavigationBakeDiagnosticRecord &record);
        void Persist(const NavigationBakeDiagnosticRecord &record);
        NavigationBakeDiagnosticsConfig config_;
        // Serializes producer ordering and protects retained records/counters. No I/O or callbacks under this lock.
        mutable std::mutex mutex_;
        std::deque<NavigationBakeDiagnosticRecord> records_;
        std::unordered_map<OperationId, std::uint64_t> counts_;
        std::unordered_map<OperationId, BuildOutputSessionId> sessions_;
        std::unordered_map<OperationId, std::uint64_t> suppressed_;
        std::uint64_t nextSequence_{1};
        std::uint64_t revision_{};
        std::uint64_t dropped_{};
        std::uint64_t suppressedTotal_{};
        std::uint64_t persistenceDrops_{};
        std::uint64_t historyFailures_{};
        std::uint64_t submissionFailures_{};
    };
}  // namespace Horo::Application
