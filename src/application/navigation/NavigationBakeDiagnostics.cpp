#include "Horo/Application/NavigationBakeDiagnostics.h"

#include "Horo/Foundation/Utf8.h"
#include "NavigationBakeDiagnosticCodec.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Horo::Application {
    namespace {
        constexpr std::string_view CheckpointName = "navigation.bake.diagnostic_checkpoint.v1";

        /** @brief Truncates producer fallback text at a UTF-8 scalar boundary. */
        void BoundText(std::string &text, const std::size_t bytes) {
            if (text.size() > bytes)
                text.resize(bytes);
            while (!text.empty() && !IsValidUtf8ScalarSequence(text))
                text.pop_back();
        }

        /** @brief Checks portable path text before filesystem interpretation. */
        [[nodiscard]] bool SafePathText(const std::string &relative) {
            return !relative.empty() && relative.size() <= 1024 && relative.find('\0') == std::string::npos &&
                   relative.find_first_of("\\:") == std::string::npos && IsValidUtf8ScalarSequence(relative);
        }

        /** @brief Checks every absolute ancestor without following a symlink. */
        [[nodiscard]] bool HasSafeAncestors(const std::filesystem::path &absolute) {
            auto candidate = absolute.root_path();
            std::error_code error;
            for (const auto &part : absolute.relative_path()) {
                candidate /= part;
                const auto status = std::filesystem::symlink_status(candidate, error);
                if (error || std::filesystem::is_symlink(status))
                    return false;
            }
            return true;
        }

        /** @brief Rejects unsafe relative components and ancestors before resolving a regular file. */
        [[nodiscard]] Result<std::filesystem::path> ResolvePath(const std::filesystem::path &root, const std::string &relative) {
            using namespace Navigation;
            if (!SafePathText(relative))
                return Result<std::filesystem::path>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            const std::u8string utf8{relative.begin(), relative.end()};
            const std::filesystem::path path{utf8};
            if (path.has_root_path() || path.lexically_normal().generic_u8string() != utf8 ||
                std::ranges::any_of(path, [](const auto &part) {
                return part == ".." || part == "." || part.empty();
            }))
                return Result<std::filesystem::path>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            auto candidate = root / path;
            if (!HasSafeAncestors(candidate))
                return Result<std::filesystem::path>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            if (std::error_code error; !std::filesystem::is_regular_file(candidate, error) || error)
                return Result<std::filesystem::path>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            return Result<std::filesystem::path>::Success(std::move(candidate));
        }
    }  // namespace

    /** @copydoc NavigationBakeDiagnostics::NavigationBakeDiagnostics */
    NavigationBakeDiagnostics::NavigationBakeDiagnostics(ConstructionKey, NavigationBakeDiagnosticsConfig config)
        : config_(std::move(config)) {}

    /** @copydoc NavigationBakeDiagnostics::Create */
    Result<std::shared_ptr<NavigationBakeDiagnostics>> NavigationBakeDiagnostics::Create(NavigationBakeDiagnosticsConfig config) {
        using namespace Navigation;
        if (!config.project.IsValid() || !config.definition.IsValid() || !config.output || !config.history ||
            !config.projectRoot.is_absolute() || config.capacity == 0 || config.capacity > 4096 || config.maximumRecordsPerOperation == 0 ||
            config.maximumRecordsPerOperation > 4096)
            return Result<std::shared_ptr<NavigationBakeDiagnostics>>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        std::error_code error;
        config.projectRoot = std::filesystem::canonical(config.projectRoot, error);
        if (error)
            return Result<std::shared_ptr<NavigationBakeDiagnostics>>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        if (!std::filesystem::is_directory(config.projectRoot, error) || error)
            return Result<std::shared_ptr<NavigationBakeDiagnostics>>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        auto journal = std::make_shared<NavigationBakeDiagnostics>(ConstructionKey{}, std::move(config));
        journal->Recover();
        return Result<std::shared_ptr<NavigationBakeDiagnostics>>::Success(std::move(journal));
    }

    /** @brief Retains one value under the journal lock; overwrite loss remains explicit. */
    void NavigationBakeDiagnostics::Retain(NavigationBakeDiagnosticRecord record) {
        record.sequence = nextSequence_++;
        if (records_.size() == config_.capacity) {
            records_.pop_front();
            ++dropped_;
        }
        if (!record.recovered) {
            record.totalSuppressedRecords = suppressedTotal_;
            record.totalDroppedRecords = dropped_;
        }
        records_.push_back(std::move(record));
        ++revision_;
    }

    /** @brief Projects owned evidence without creating a second live operation or an unchecked source path. */
    void NavigationBakeDiagnostics::Project(const NavigationBakeDiagnosticRecord &record) const {
        std::string message = record.message;
        if (record.tile)
            message += std::format(" [profile={}, surface={}, tile=({},{},{})]", record.tile->profile.Value(), record.tile->surface.Value(),
                                   record.tile->tile.x, record.tile->tile.z, record.tile->tile.layer);
        if (record.source)
            message += std::format(" [producer={}, contribution={}, revision={}]", record.source->observation.producer.Value(),
                                   record.source->observation.contribution.Value(), record.source->observation.revision.Value());
        if (!record.suggestedFix.empty())
            message += " " + record.suggestedFix;
        config_.output->Append({.timestampUtc = std::chrono::system_clock::now(),
                                .sessionId = record.session,
                                .operationId = record.recovered ? std::nullopt : std::optional{record.operation},
                                .severity = record.severity,
                                .result = record.result,
                                .stage = record.stage,
                                .code = record.code,
                                .toolCode = record.causeCode.empty() ? std::nullopt : std::optional{record.causeCode},
                                .message = std::move(message)});
    }

    /** @copydoc NavigationBakeDiagnostics::Record */
    bool NavigationBakeDiagnostics::Record(NavigationBakeDiagnosticRecord record) noexcept {
        try {
            return RecordImpl(std::move(record));
        } catch (const std::exception &) {
            NoteSubmissionFailure();
            return false;
        }
    }

    /** @copydoc NavigationBakeDiagnostics::NoteSubmissionFailure */
    void NavigationBakeDiagnostics::NoteSubmissionFailure() noexcept {
        std::lock_guard lock(mutex_);
        ++submissionFailures_;
        ++revision_;
    }

    /** @brief Admits a bounded operation session under the producer lock. */
    bool NavigationBakeDiagnostics::AdmitRecord(NavigationBakeDiagnosticRecord &record) {
        if (!counts_.contains(record.operation) && counts_.size() >= 128) {
            ++submissionFailures_;
            ++revision_;
            return false;
        }
        if (!sessions_.contains(record.operation)) {
            auto session = config_.output->BeginSession();
            if (!session) {
                ++submissionFailures_;
                ++revision_;
                return false;
            }
            sessions_.emplace(record.operation, *session);
        }
        record.session = sessions_.at(record.operation);
        return true;
    }

    /** @brief Converts excess detail into an explicitly counted summary under the producer lock. */
    bool NavigationBakeDiagnostics::ApplyDetailLimit(NavigationBakeDiagnosticRecord &record) {
        if (record.result != BuildOutputResult::None || record.event == NavigationBakeDiagnosticEvent::Suppressed)
            return true;
        if (const auto count = counts_[record.operation]++; count < config_.maximumRecordsPerOperation)
            return true;
        ++suppressedTotal_;
        const auto total = ++suppressed_[record.operation];
        record.event = NavigationBakeDiagnosticEvent::Suppressed;
        record.message = std::format("{} navigation bake detail records suppressed", total);
        record.suppressedCount = 1;
        record.tile.reset();
        record.source.reset();
        return false;
    }

    /** @brief Applies bounded producer policy before projecting and enqueuing one checkpoint. */
    bool NavigationBakeDiagnostics::RecordImpl(NavigationBakeDiagnosticRecord record) {
        using enum NavigationBakeDiagnosticEvent;
        if (record.operation == 0 || record.event >= Count || record.recovered ||
            (record.progress.has_value() && (!std::isfinite(*record.progress) || *record.progress < 0 || *record.progress > 1))) {
            NoteSubmissionFailure();
            return false;
        }
        BoundText(record.stage, 64);
        BoundText(record.message, 1024);
        BoundText(record.causeCode, 160);
        if (record.source && record.source->target.relativePath.size() > 1024)
            record.source->target.relativePath.clear();
        const bool terminal = record.result != BuildOutputResult::None;
        std::lock_guard lock(mutex_);
        if (!AdmitRecord(record))
            return false;
        const bool retained = ApplyDetailLimit(record);
        NavigationBakeDetail::DescribeDiagnostic(record);
        Retain(record);
        record = records_.back();
        if (terminal) {
            counts_.erase(record.operation);
            sessions_.erase(record.operation);
            suppressed_.erase(record.operation);
        }
        Project(record);
        Persist(record);
        return retained;
    }

    /** @brief Enqueues a replay-valid checkpoint under the producer lock; never performs persistent I/O. */
    void NavigationBakeDiagnostics::Persist(const NavigationBakeDiagnosticRecord &record) {
        auto bytes = NavigationBakeDetail::EncodeDiagnostic(record, config_);
        bool accepted{};
        if (NavigationBakeDetail::DecodeDiagnostic(bytes, config_).has_value())
            accepted = Telemetry::Runtime::EmitRecord(
                {.subsystem = "navigation.bake",
                 .payload = Telemetry::SpanRecord{.name = std::string{CheckpointName},
                                                  .status = Telemetry::SpanStatus::Succeeded,
                                                  .fields = {{.key = "diagnostic", .value = std::move(bytes)}}}});
        if (!accepted) {
            const bool first = ++persistenceDrops_ == 1;
            ++revision_;
            if (first)
                config_.output->Append(
                    {.timestampUtc = std::chrono::system_clock::now(),
                     .severity = DiagnosticSeverity::Warning,
                     .stage = "history",
                     .code = DiagnosticCode{"navigation.bake.history_unavailable"},
                     .message = "Navigation diagnostic persistence rejected a checkpoint; inspect the diagnostic loss counters."});
        }
    }

    /** @copydoc NavigationBakeDiagnostics::Snapshot */
    NavigationBakeDiagnosticSnapshot NavigationBakeDiagnostics::Snapshot() const {
        std::lock_guard lock(mutex_);
        return {.revision = revision_,
                .droppedRecords = dropped_,
                .suppressedRecords = suppressedTotal_,
                .persistenceDrops = persistenceDrops_,
                .dispatcherDrops = Telemetry::Runtime::GetStatistics().droppedRecords,
                .historyFailures = historyFailures_,
                .submissionFailures = submissionFailures_,
                .records = {records_.begin(), records_.end()}};
    }

    /** @copydoc NavigationBakeDiagnostics::Owns */
    bool NavigationBakeDiagnostics::Owns(const Assets::AssetId definition) const noexcept {
        return definition == config_.definition;
    }

    /** @brief Replays only typed checkpoints for this owner; recovered IDs never enter the live OperationStore. */
    void NavigationBakeDiagnostics::Recover() {
        std::uint64_t retainedDropCount{};
        for (const auto &history : config_.history->Snapshot()) {
            if (history.name != CheckpointName || history.subsystem != "navigation.bake")
                continue;
            for (const auto &field : history.fields) {
                const auto *bytes = std::get_if<std::string>(&field.value);
                if (field.key != "diagnostic" || bytes == nullptr)
                    continue;
                if (auto decoded = NavigationBakeDetail::DecodeDiagnostic(*bytes, config_)) {
                    suppressedTotal_ = std::max(suppressedTotal_, decoded->totalSuppressedRecords);
                    retainedDropCount = std::max(retainedDropCount, decoded->totalDroppedRecords);
                    Retain(*decoded);
                    Project(records_.back());
                }
            }
        }
        dropped_ = std::max(dropped_, retainedDropCount);
    }

    /** @copydoc NavigationBakeDiagnostics::Navigate */
    Result<bool> NavigationBakeDiagnostics::Navigate(const std::uint64_t sequence, const NavigationDiagnosticProjectId project,
                                                     const Assets::AssetId definition,
                                                     const std::span<const NavigationDiagnosticSource> sources,
                                                     INavigationDiagnosticNavigator &navigator) const {
        using namespace Navigation;
        if (project != config_.project || definition != config_.definition || sources.size() > 4096)
            return Result<bool>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        std::optional<NavigationDiagnosticSource> source;
        {
            std::lock_guard lock(mutex_);
            const auto found = std::ranges::find(records_, sequence, &NavigationBakeDiagnosticRecord::sequence);
            if (found != records_.end())
                source = found->source;
        }
        if (!source)
            return Result<bool>::Failure(MakeError(NavigationErrors::BakeInputStale));
        const auto &target = source->target;
        if (const auto sameIdentity =
                [&source](const auto &current) {
            return current.observation.producer == source->observation.producer &&
                   current.observation.contribution == source->observation.contribution;
        };
            target.scene.IsValid() != target.object.IsValid() || (!target.asset.IsValid() && !target.object.IsValid()) ||
            std::ranges::count(sources, *source) != 1 || std::ranges::count_if(sources, sameIdentity) != 1)
            return Result<bool>::Failure(MakeError(NavigationErrors::BakeInputStale));
        auto path = ResolvePath(config_.projectRoot, target.relativePath);
        if (path.HasError())
            return Result<bool>::Failure(path.ErrorValue());
        return navigator.Navigate(target, path.Value());
    }

    /** @copydoc NavigationBakeDiagnostics::Export */
    void NavigationBakeDiagnostics::Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *descriptor) {
        const auto *span = std::get_if<Telemetry::SpanRecord>(&record.payload);
        if (record.subsystem != "navigation.bake" || span == nullptr || span->name != CheckpointName)
            return;
        const auto found = std::ranges::find(span->fields, std::string_view{"diagnostic"}, &Telemetry::Field::key);
        if (found == span->fields.end())
            return;
        const auto *bytes = std::get_if<std::string>(&found->value);
        if (bytes && NavigationBakeDetail::DecodeDiagnostic(*bytes, config_)) {
            try {
                config_.history->Export(record, descriptor);
            } catch (const std::exception &) {
                std::lock_guard lock(mutex_);
                ++historyFailures_;
                ++revision_;
                throw;
            }
        }
    }

    /** @copydoc NavigationBakeDiagnostics::Flush */
    void NavigationBakeDiagnostics::Flush() {
        config_.history->Flush();
    }
}  // namespace Horo::Application
