#include "NavigationBakeInternal.h"

#include <algorithm>
#include <array>
#include <set>

namespace Horo::Application::NavigationBakeDetail {
    namespace {
        /** @brief Maps the closed scheduler stage to retained output metadata. */
        [[nodiscard]] std::string_view Stage(const Navigation::NavigationBakeJobStage stage) noexcept {
            constexpr std::array<std::string_view, 4> names{"partition_gather", "tile_build", "validation", "publication"};
            const auto index = static_cast<std::size_t>(stage);
            return index < names.size() ? names[index] : "invalid";
        }
    }  // namespace

    /** @copydoc ObserveBake */
    void ObserveBake(const std::shared_ptr<NavigationBakeDiagnostics> &diagnostics,
                     const Navigation::NavigationBakeJobSnapshot &snapshot) noexcept {
        if (!diagnostics)
            return;
        try {
            NavigationBakeDiagnosticRecord record{.operation = snapshot.operation,
                                                  .stage = std::string{Stage(snapshot.stage)},
                                                  .progress = static_cast<float>(snapshot.Progress())};
            using enum Navigation::NavigationBakeJobState;
            switch (snapshot.state) {
                case Queued:
                case Running:
                    record.event = NavigationBakeDiagnosticEvent::Progress;
                    record.message = "Navigation bake in progress";
                    break;
                case Succeeded:
                    record.event = NavigationBakeDiagnosticEvent::Succeeded;
                    record.result = BuildOutputResult::Succeeded;
                    record.message = "Navigation bake completed";
                    break;
                case Failed:
                    record.event = NavigationBakeDiagnosticEvent::StageFailed;
                    record.result = BuildOutputResult::Failed;
                    record.message = "Navigation bake failed";
                    break;
                case Cancelled:
                    record.event = NavigationBakeDiagnosticEvent::Cancelled;
                    record.result = BuildOutputResult::Cancelled;
                    record.message = "Navigation bake cancelled after draining child jobs";
                    break;
            }
            if (snapshot.terminalError) {
                record.causeCode = snapshot.terminalError->code.Value();
                if (snapshot.state == Failed)
                    record.message = snapshot.terminalError->message;
            }
            diagnostics->Record(std::move(record));
        } catch (const std::exception &) {
            diagnostics->NoteSubmissionFailure();
        }
    }

    /** @copydoc ReportTileFailure */
    void ReportTileFailure(const ServiceState &state, const Attempt &attempt, const Navigation::NavigationPreparedTile &tile,
                           const Error &error) noexcept {
        using namespace Navigation;
        if (!state.config.diagnostics ||
            ErrorChainContains(error, NavigationErrors::BakeInputCancelled.domain, NavigationErrors::BakeInputCancelled.code))
            return;
        try {
            std::set<NavigationSourceObservation> sources;
            for (const auto &triangle : tile.triangles) {
                const auto &provenance = triangle.provenance;
                const NavigationSourceObservation observation{provenance.kind, provenance.producer, provenance.contribution,
                                                              provenance.revision, provenance.contentDigest};
                sources.insert(observation);
            }
            NavigationBakeDiagnosticRecord record{.operation = attempt.operation,
                                                  .event = NavigationBakeDiagnosticEvent::TileFailed,
                                                  .stage = "tile_build",
                                                  .message = error.message,
                                                  .tile = tile.tile.key,
                                                  .causeCode = std::string{error.code.Value()}};
            if (sources.empty())
                state.config.diagnostics->Record(record);
            for (const auto &source : sources) {
                const auto found = std::ranges::find(attempt.request.diagnosticSources, source, &NavigationDiagnosticSource::observation);
                record.source =
                    found == attempt.request.diagnosticSources.end() ? NavigationDiagnosticSource{.observation = source} : *found;
                state.config.diagnostics->Record(record);
            }
        } catch (const std::exception &) {
            state.config.diagnostics->NoteSubmissionFailure();
        }
    }
}  // namespace Horo::Application::NavigationBakeDetail
