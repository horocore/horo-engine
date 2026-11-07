#include "../../editor/PrefabDocumentSessionTestSupport.h"
#include "PrefabFuzzCorpus.h"

#include <fstream>
#include <iterator>
#include <map>
#include <optional>

namespace Horo::Editor {
    namespace {
        using PrefabDocumentSessionTestSupport::Fixture;
        using FileMap = std::map<std::string, std::string>;

        /** @brief Captures actual bounded native fixture files; a failed observation is a replayable property failure. */
        std::optional<FileMap> Files(const std::filesystem::path &root) {
            FileMap result;
            for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
                if (!entry.is_regular_file())
                    continue;
                if (entry.file_size() > 64U * 1024U || result.size() >= 32)
                    return std::nullopt;
                std::ifstream input{entry.path(), std::ios::binary};
                if (!input.is_open())
                    return std::nullopt;
                result.emplace(entry.path().lexically_relative(root).generic_string(),
                               std::string{std::istreambuf_iterator<char>{input}, {}});
            }
            return result;
        }

        /** @brief Reads the independently committed source family used by all parser fuzz tests. */
        std::string SourceBaseline() {
            const auto path = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "tests/fixtures/prefab-reference-corpus/multi.prefab";
            REQUIRE(std::filesystem::file_size(path) <= 64U * 1024U);
            std::ifstream input{path, std::ios::binary};
            REQUIRE(input.is_open());
            return {std::istreambuf_iterator<char>{input}, {}};
        }

        /** @brief Returns the full observable rollback field that changed, without lossy projection or pointer formatting. */
        std::string Unchanged(const PrefabDocumentSnapshot &before, const PrefabDocumentSnapshot &after) {
            if (after.identity != before.identity)
                return "recovery changed document identity";
            if (after.source != before.source)
                return "recovery replaced source";
            if (after.sourceRevision != before.sourceRevision)
                return "recovery changed sourceRevision";
            if (after.savedState != before.savedState)
                return "recovery changed savedState";
            if (after.acceptedSource != before.acceptedSource)
                return "recovery changed acceptedSource";
            if (after.fence.owner != before.fence.owner)
                return "recovery changed owner fence";
            if (after.fence.instance != before.fence.instance)
                return "recovery changed instance fence";
            if (after.fence.revision != before.fence.revision)
                return "recovery changed revision fence";
            if (after.recoveryRevision != before.recoveryRevision)
                return "recovery changed recoveryRevision";
            if (after.previewRevision != before.previewRevision)
                return "recovery changed previewRevision";
            if (after.stageActive != before.stageActive)
                return "recovery changed stageActive";
            if (after.externalConflict != before.externalConflict)
                return "recovery changed externalConflict";
            if (after.dirty != before.dirty)
                return "recovery changed dirty state";
            if (after.canUndo != before.canUndo)
                return "recovery changed undo state";
            if (after.canRedo != before.canRedo)
                return "recovery changed redo state";
            return {};
        }

        /** @brief Preserves typed rejection identity independently from presentation messages and temporary paths. */
        std::string Failure(const char *stage, const Error &error) {
            return std::string{stage} + "/" + std::string{error.domain.Value()} + "/" + std::string{error.code.Value()};
        }

        /** @brief Exercises complete persisted recovery, including actual durable-record and source-base fences. */
        std::string Recover(const std::string &baseline, const Prefab::Fuzz::Recipe &recipe) {
            Fixture fixture;
            const auto before = fixture.document->Snapshot();
            auto input = Prefab::Fuzz::Source(baseline, recipe).bytes;
            const auto version = input.find("0.2.0");
            if (version != std::string::npos)
                input.replace(version, 5, Application::FormatHoroVersion(before.source->Data().projectVersion));
            for (auto offset = input.find("00000000-0000-0000-0000-000000000002"); offset != std::string::npos;
                 offset = input.find("00000000-0000-0000-0000-000000000002", offset + 36))
                input.replace(offset, 36, fixture.project.asset.ToString());
            const auto parsed =
                Prefab::PrefabDocument::Parse(input, fixture.limits, {.expectedProjectVersion = before.source->Data().projectVersion});
            const bool validPayload = recipe.words[0] == 0 || recipe.words[0] == 4 || recipe.words[0] == 7;
            if (parsed.HasValue() != validPayload)
                return "recovery payload admission oracle mismatch";
            const std::string canonical = validPayload ? parsed.Value().SerializeCanonical().Value() : input;
            DocumentRecoveryRecord record{.asset = fixture.project.asset,
                                          .document = before.identity.key,
                                          .sessionOwner = before.fence.owner,
                                          .sessionGeneration = before.fence.instance.Value(),
                                          .revision = before.fence.revision + 1,
                                          .capturedAtUnixSeconds = 1'700'000'000ULL + recipe.words[1] % 100,
                                          .acceptedSource = before.acceptedSource,
                                          .state = ComputeSha256(std::as_bytes(std::span(canonical))),
                                          .payload = {}};
            const auto bytes = std::as_bytes(std::span(input));
            record.payload.assign(bytes.begin(), bytes.end());
            DocumentRecoveryService recovery(fixture.project.directory.path, fixture.project.mutations, fixture.project.files);
            const auto written = recovery.Write(record);
            if (written.HasError())
                return Failure("persist recovery", written.ErrorValue());
            const auto inspected = recovery.Inspect(record.asset, record.document);
            if (inspected.HasError())
                return Failure("inspect recovery", inspected.ErrorValue());
            if (!inspected.Value() || *inspected.Value() != record)
                return "persisted recovery replay mismatch";
            auto submitted = record;
            if (recipe.words[0] == 4) {
                // Parse/state admission succeeds, then exact durable recovery-record equality must reject this later revision.
                ++submitted.revision;
            } else if (recipe.words[0] == 7) {
                // The exact persisted candidate remains valid, but real canonical source bytes drift after capture.
                fixture.project.WriteSource("External replacement " + std::to_string(recipe.words[1] % 100));
            }
            const auto files = Files(fixture.project.directory.path);
            if (!files)
                return "filesystem pre-recovery capture failed";
            const auto recovered = fixture.document->Recover(before.fence, submitted, fixture.project.mutations, fixture.project.files);
            const auto after = fixture.document->Snapshot();
            const auto retainedFiles = Files(fixture.project.directory.path);
            if (!retainedFiles || *retainedFiles != *files)
                return "recovery changed native files before canonical save";
            const auto retainedRecord = recovery.Inspect(record.asset, record.document);
            if (retainedRecord.HasError())
                return Failure("inspect retained recovery", retainedRecord.ErrorValue());
            if (!retainedRecord.Value() || *retainedRecord.Value() != record)
                return "recovery changed durable candidate";
            if (recipe.words[0] == 0) {
                if (recovered.HasError())
                    return Failure("valid recovery denied", recovered.ErrorValue());
                if (after.source->Data() != parsed.Value().Data() || !after.dirty || after.fence.revision != before.fence.revision + 1 ||
                    after.recoveryRevision != after.fence.revision || after.acceptedSource != before.acceptedSource ||
                    after.savedState != before.savedState || after.sourceRevision.contentDigest != record.state || after.canUndo ||
                    after.canRedo || after.stageActive)
                    return "successful recovery semantic mismatch";
                const auto saved = fixture.document->Save(after.fence, fixture.project.mutations, fixture.project.files);
                if (saved.HasError())
                    return Failure("recovered canonical save", saved.ErrorValue());
                if (saved.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                    return "recovered source was not durable";
                const auto savedView = fixture.document->Snapshot();
                if (savedView.dirty || savedView.savedState != record.state || savedView.acceptedSource != saved.Value().identity)
                    return "recovered canonical publication state mismatch";
                const auto actual = Assets::CaptureAssetSource(fixture.project.directory.path, fixture.project.source,
                                                               Prefab::PrefabHardLimits::SourceDocumentBytes);
                if (actual.HasError())
                    return Failure("read recovered canonical source", actual.ErrorValue());
                const auto expectedBytes = std::as_bytes(std::span(canonical));
                if (!std::ranges::equal(actual.Value().bytes, expectedBytes))
                    return "recovered canonical file bytes mismatch";
                const auto publishedFiles = Files(fixture.project.directory.path);
                const auto metadata = fixture.project.source.lexically_relative(fixture.project.directory.path).generic_string() + ".horo";
                if (!publishedFiles || !publishedFiles->contains(metadata) || !files->contains(metadata) ||
                    publishedFiles->at(metadata) != files->at(metadata))
                    return "recovered save changed identity sidecar";
                const auto publishedRecord = recovery.Inspect(record.asset, record.document);
                if (publishedRecord.HasError())
                    return Failure("inspect recovery after save", publishedRecord.ErrorValue());
                if (!publishedRecord.Value() || *publishedRecord.Value() != record)
                    return "canonical save changed recovery candidate";
            } else {
                if (recovered.HasValue())
                    return "malformed or stale recovery unexpectedly admitted";
                const auto expectedCode = recipe.words[0] == 4 || recipe.words[0] == 7 || input.empty()
                                              ? Assets::AssetSourceErrors::Conflict.code.Value()
                                              : parsed.ErrorValue().code.Value();
                if (recovered.ErrorValue().code.Value() != expectedCode)
                    return Failure("wrong recovery denial", recovered.ErrorValue());
                if (auto changed = Unchanged(before, after); !changed.empty())
                    return changed;
            }
            return {};
        }
    }  // namespace

    TEST_CASE("Prefab malformed recovery fuzz preserves real source sidecar and session publication",
              "[prefab][prefab-fuzz][persistence]") {
        const auto baseline = SourceBaseline();
        Prefab::Fuzz::Run("persisted recovery", [&](const Prefab::Fuzz::Recipe &recipe) {
            return Recover(baseline, recipe);
        }, 8);
    }
}  // namespace Horo::Editor
