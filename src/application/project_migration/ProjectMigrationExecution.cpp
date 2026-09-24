#include "../project/ProjectErrors.h"
#include "Horo/Application/ProjectMigration.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Paths.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <system_error>
#include <unordered_set>

namespace Horo::Application {
    struct ProjectMigrationContext::State {
        struct Document {
            MigrationDocumentEntry entry;
            std::vector<std::byte> original;
            std::vector<std::byte> bytes;
            bool alive{true};
            bool changed{false};
            bool existedInSource{true};
        };

        std::filesystem::path authoritativeRoot;
        std::filesystem::path candidateRoot;
        ProjectMigrationLimits limits;
        std::uint32_t generation{1};
        std::vector<Document> documents;
        std::uint64_t inputBytes{};
        std::uint64_t outputBytes{};
    };

    struct PreparedProjectMigration::State {
        std::shared_ptr<ProjectMigrationContext::State> context;
        std::filesystem::path candidateRoot;
        std::vector<PreparedMigrationChange> changes;

        ~State() {
            std::error_code ignored;
            if (cleanupOwned)
                std::filesystem::remove_all(candidateRoot, ignored);
        }

        State() = default;
        State(const State &) = delete;
        State &operator=(const State &) = delete;

        bool cleanupOwned{true};
    };

    namespace {
        [[nodiscard]] Error MigrationError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        struct StringHash {
            using is_transparent = void;

            std::size_t operator()(const std::string_view sv) const noexcept {
                return std::hash<std::string_view>{}(sv);
            }
        };

        using TransparentStringSet = std::unordered_set<std::string, StringHash, std::equal_to<>>;

        constexpr std::string_view kMigrationDryRunRootPrefix = ".horo-migration-dry-run-";
        constexpr std::array<std::string_view, 6> kExcludedMigrationDirectoryPrefixes{".horo/local/", ".horo/cache/", ".horo/build/",
                                                                                      "build/",       "cache/",       ".git/"};

        [[nodiscard]] std::string PortablePathKey(std::string value) {
            std::ranges::transform(value, value.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

        [[nodiscard]] bool IsExcluded(const std::filesystem::path &relative) {
            const std::string generic = relative.generic_string();
            if (generic == ProjectLayout::AssetIndexPath)
                return true;
            if (generic.starts_with(kMigrationDryRunRootPrefix))
                return true;
            return std::ranges::any_of(kExcludedMigrationDirectoryPrefixes, [&generic](const std::string_view prefix) {
                return generic.starts_with(prefix);
            });
        }

        [[nodiscard]] bool HasSuffix(const std::string_view path, const std::initializer_list<std::string_view> suffixes) {
            return std::ranges::any_of(suffixes, [path](const std::string_view suffix) {
                return path.ends_with(suffix);
            });
        }

        [[nodiscard]] bool IsSceneDocument(const std::string_view path) {
            return HasSuffix(path, {".scene.horo", ".hscene"}) ||
                   ((path.starts_with("assets/scenes/") || path.starts_with("Assets/Scenes/")) && path.ends_with(".horo"));
        }

        [[nodiscard]] MigrationDocumentKind ClassifyDocument(const std::string_view path) {
            using enum MigrationDocumentKind;
            std::string folded{path};
            std::ranges::transform(folded, folded.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            if (path == ".horo/project.json")
                return ProjectMetadata;
            if (HasSuffix(folded, {".prefab.horo", ".horo.meta"}))
                return AssetSidecar;
            if (IsSceneDocument(folded))
                return Scene;
            if (HasSuffix(folded, {".prefab", ".hprefab"}))
                return Prefab;
            if (folded.find("/input") != std::string_view::npos)
                return Input;
            if (folded.ends_with(".material.horo"))
                return Material;
            if (folded.ends_with(".graph.horo"))
                return Graph;
            if (folded.starts_with(".horo/"))
                return ProjectSettings;
            return Other;
        }

        [[nodiscard]] Result<std::vector<std::byte>> ReadFile(const std::filesystem::path &path, const std::uint64_t maximum) {
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error)
                return Result<std::vector<std::byte>>::Failure(
                    MigrationError(ProjectErrors::MigrationInventoryInvalid, "Cannot inspect migration input: " + path.generic_string()));
            if (size > maximum || size > std::numeric_limits<std::size_t>::max())
                return Result<std::vector<std::byte>>::Failure(
                    MigrationError(ProjectErrors::MigrationInventoryLimit, "Migration input exceeds the configured byte limit."));
            std::vector<std::byte> bytes(size);
            if (std::ifstream stream(path, std::ios::binary);
                !stream ||
                (size > 0 && !stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size))))  // NOSONAR
                return Result<std::vector<std::byte>>::Failure(
                    MigrationError(ProjectErrors::MigrationInventoryInvalid, "Cannot read migration input: " + path.generic_string()));
            return Result<std::vector<std::byte>>::Success(std::move(bytes));
        }

        [[nodiscard]] Result<void> WriteFile(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
                return Result<void>::Failure(
                    MigrationError(ProjectErrors::MigrationInventoryInvalid, "Cannot create disposable candidate directory."));
            if (std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                !stream || (!bytes.empty() && !stream.write(reinterpret_cast<const char *>(bytes.data()),  // NOSONAR
                                                            static_cast<std::streamsize>(bytes.size()))))  // NOSONAR
                return Result<void>::Failure(
                    MigrationError(ProjectErrors::MigrationInventoryInvalid, "Cannot write disposable candidate document."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CollectInventoryFiles(const std::filesystem::path &canonicalRoot,
                                                         std::vector<std::filesystem::path> &files) {
            std::error_code error;
            TransparentStringSet portablePaths;
            std::filesystem::recursive_directory_iterator iterator(canonicalRoot,
                                                                   std::filesystem::directory_options::skip_permission_denied, error);
            std::filesystem::recursive_directory_iterator end;
            while (iterator != end) {
                if (error)
                    return Result<void>::Failure(
                        MigrationError(ProjectErrors::MigrationInventoryInvalid, "Project inventory traversal failed."));
                if (const auto status = iterator->symlink_status(error); error || std::filesystem::is_symlink(status))
                    return Result<void>::Failure(MigrationError(ProjectErrors::MigrationInventoryInvalid,
                                                                "Symlinks are not accepted in authoritative migration inventory."));
                const std::filesystem::path relative = iterator->path().lexically_relative(canonicalRoot);
                if (const std::string relativeText = relative.generic_string();
                    relative.empty() || relativeText == ".." || relativeText.starts_with("../"))
                    return Result<void>::Failure(
                        MigrationError(ProjectErrors::MigrationInventoryInvalid, "Migration inventory path escapes the project root."));
                if (iterator->is_directory(error)) {
                    if (IsExcluded(relative))
                        iterator.disable_recursion_pending();
                    iterator.increment(error);
                    continue;
                }
                if (!iterator->is_regular_file(error) || IsExcluded(relative)) {
                    iterator.increment(error);
                    continue;
                }
                if (const std::string generic = relative.generic_string(); !portablePaths.emplace(PortablePathKey(generic)).second)
                    return Result<void>::Failure(MigrationError(ProjectErrors::MigrationInventoryInvalid,
                                                                "Portable case collision in migration inventory: " + generic));
                files.push_back(relative);
                iterator.increment(error);
            }
            std::ranges::sort(files, [](const auto &left, const auto &right) {
                return left.generic_string() < right.generic_string();
            });
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::shared_ptr<ProjectMigrationContext::State>> BuildInventory(const std::filesystem::path &root,
                                                                                             const ProjectMigrationLimits &limits,
                                                                                             const std::filesystem::path &candidateRoot) {
            std::error_code error;
            const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, error);
            if (error || !std::filesystem::is_directory(canonicalRoot))
                return Result<std::shared_ptr<ProjectMigrationContext::State>>::Failure(
                    MigrationError(ProjectErrors::MigrationInventoryInvalid, "Project root is not an accessible directory."));

            auto state = std::make_shared<ProjectMigrationContext::State>();
            state->authoritativeRoot = canonicalRoot;
            state->candidateRoot = candidateRoot;
            state->limits = limits;
            std::filesystem::create_directories(candidateRoot, error);  // NOSONAR(cpp:S2083) candidateRoot is validated by caller.
            if (error)
                return Result<std::shared_ptr<ProjectMigrationContext::State>>::Failure(
                    MigrationError(ProjectErrors::MigrationInventoryInvalid, "Cannot create migration candidate root."));
            if (limits.maxDocuments > std::numeric_limits<std::uint32_t>::max())
                return Result<std::shared_ptr<ProjectMigrationContext::State>>::Failure(
                    MigrationError(ProjectErrors::MigrationInventoryLimit, "Migration document limit exceeds handle capacity."));

            std::vector<std::filesystem::path> files;
            if (auto collect = CollectInventoryFiles(canonicalRoot, files); collect.HasError())
                return Result<std::shared_ptr<ProjectMigrationContext::State>>::Failure(collect.ErrorValue());

            if (files.size() > limits.maxDocuments)
                return Result<std::shared_ptr<ProjectMigrationContext::State>>::Failure(
                    MigrationError(ProjectErrors::MigrationInventoryLimit, "Migration document count exceeds the configured limit."));

            for (const std::filesystem::path &relative : files) {
                const auto read = ReadFile(canonicalRoot / relative, limits.maxInputBytes - state->inputBytes);
                if (read.HasError())
                    return Result<std::shared_ptr<ProjectMigrationContext::State>>::Failure(read.ErrorValue());
                ProjectMigrationContext::State::Document document;
                document.entry.handle =
                    MigrationDocumentHandle{.index = static_cast<std::uint32_t>(state->documents.size()), .generation = state->generation};
                document.entry.path = relative.generic_string();
                document.entry.kind = ClassifyDocument(document.entry.path);
                document.entry.inputBytes = read.Value().size();
                document.original = read.Value();
                document.bytes = read.Value();
                state->inputBytes += document.bytes.size();
                state->documents.push_back(std::move(document));
            }
            state->outputBytes = state->inputBytes;
            if (state->outputBytes > limits.maxOutputBytes)
                return Result<std::shared_ptr<ProjectMigrationContext::State>>::Failure(
                    MigrationError(ProjectErrors::MigrationInventoryLimit, "Initial candidate exceeds the configured output byte limit."));
            return Result<std::shared_ptr<ProjectMigrationContext::State>>::Success(std::move(state));
        }

        [[nodiscard]] Error AnnotateStageError(const Error &source, const StableMigrationId &definition, const MigrationStageId &stage,
                                               const std::optional<std::string_view> document = std::nullopt) {
            Error annotated = source;
            annotated.message = "Migration " + definition.value + " stage " + stage.value;
            if (document.has_value())
                annotated.message += " document " + std::string(*document);
            annotated.message += ": " + source.message;
            return annotated;
        }

        [[nodiscard]] Result<void> MergeExecutionResults(std::vector<std::optional<MigrationDocumentChange>> &results,
                                                         const ProjectMigrationContext &context) {
            std::unordered_set<std::uint64_t> writes;
            for (auto &result : results) {
                if (!result.has_value() || !result->changed)
                    continue;
                const MigrationDocumentHandle handle = result->document;
                if (const std::uint64_t key = (static_cast<std::uint64_t>(handle.generation) << 32U) | handle.index;
                    !writes.emplace(key).second)
                    return Result<void>::Failure(
                        MigrationError(ProjectErrors::MigrationWriteConflict, "Parallel document stage produced duplicate write targets."));
                const Result<void> merged =
                    result->remove ? context.RemoveDocument(handle) : context.ReplaceDocument(handle, std::move(result->replacement));
                if (merged.HasError())
                    return merged;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ExecuteForEach(const ProjectMigrationDefinition &definition, const ProjectMigrationNode &node,
                                                  const ProjectMigrationContext &context, JobSystem &jobs,
                                                  const ProjectMigrationLimits &limits, const CancellationToken &cancellation) {
            const MigrationStageDescriptor descriptor = node.documentStage->Describe();
            const std::vector<MigrationDocumentEntry> documents = context.ListDocuments(node.query);
            const std::size_t batchSize = std::max<std::size_t>(1, limits.maxConcurrency);
            for (std::size_t begin = 0; begin < documents.size(); begin += batchSize) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(
                        MigrationError(ProjectErrors::MigrationCancelled, "Migration cancellation was requested."));
                const std::size_t end = std::min(documents.size(), begin + batchSize);
                std::vector<std::optional<MigrationDocumentChange>> results(end - begin);
                TaskGroup group(jobs, TaskGroupFailurePolicy::FailFast, cancellation);
                for (std::size_t index = begin; index < end; ++index) {
                    const Result<ProjectDocumentView> source = context.ReadDocument(documents[index].handle);
                    if (source.HasError()) {
                        group.RequestCancel();
                        static_cast<void>(group.Join());
                        return Result<void>::Failure(source.ErrorValue());
                    }
                    const ProjectDocumentView view = source.Value();
                    const auto spawned = group.Spawn({}, [index, begin, view, &node, &definition, &descriptor,
                                                          &results](const CancellationToken &childCancellation) {
                        Result<MigrationDocumentChange> changed =
                            node.documentStage->Execute(view,
                                                        MigrationStageContext{.definitionId = definition.id, .stageId = descriptor.id},
                                                        childCancellation);
                        if (changed.HasError())
                            return Result<void>::Failure(AnnotateStageError(changed.ErrorValue(), definition.id, descriptor.id, view.path));
                        results[index - begin] = std::move(changed).Value();
                        return Result<void>::Success();
                    });
                    if (spawned.HasError()) {
                        group.RequestCancel();
                        static_cast<void>(group.Join());
                        return Result<void>::Failure(spawned.ErrorValue());
                    }
                }
                if (const Result<void> joined = group.Join(); joined.HasError())
                    return joined;
                if (const auto merged = MergeExecutionResults(results, context); merged.HasError())
                    return merged;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MaterializeCandidate(const std::shared_ptr<ProjectMigrationContext::State> &state) {
            for (const auto &document : state->documents) {
                const std::filesystem::path target = state->candidateRoot / document.entry.path;
                if (!document.alive) {
                    std::error_code ignored;
                    std::filesystem::remove(target, ignored);
                } else if (document.changed) {
                    const Result<void> written = WriteFile(target, document.bytes);
                    if (written.HasError())
                        return written;
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> VerifyAuthoritativeUnchanged(const std::shared_ptr<ProjectMigrationContext::State> &state) {
            for (const auto &document : state->documents) {
                if (!document.existedInSource)
                    continue;
                const auto current = ReadFile(state->authoritativeRoot / document.entry.path, state->limits.maxInputBytes);
                if (current.HasError() || current.Value() != document.original)
                    return Result<void>::Failure(
                        MigrationError(ProjectErrors::MigrationInventoryInvalid,
                                       "Authoritative source changed after migration inspection: " + document.entry.path));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ExecuteNode(const ProjectMigrationDefinition &definition, const ProjectMigrationNode &node,
                                               ProjectMigrationContext &context, JobSystem &jobs, const ProjectMigrationLimits &limits,
                                               const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MigrationError(ProjectErrors::MigrationCancelled, "Migration cancellation was requested."));
            Result<void> executed = Result<void>::Success();
            if (node.kind == ProjectMigrationNodeKind::ForEach)
                executed = ExecuteForEach(definition, node, context, jobs, limits, cancellation);
            else if (node.kind == ProjectMigrationNodeKind::Then) {
                const auto descriptor = node.stage->Describe();
                executed = node.stage->Execute(context, cancellation);
                if (executed.HasError())
                    executed = Result<void>::Failure(AnnotateStageError(executed.ErrorValue(), definition.id, descriptor.id));
            } else {
                const auto descriptor = node.validator->Describe();
                executed = node.validator->Validate(context, cancellation);
                if (executed.HasError())
                    executed = Result<void>::Failure(AnnotateStageError(executed.ErrorValue(), definition.id, descriptor.id));
            }
            return executed;
        }

        [[nodiscard]] Result<void> ExecutePlan(const ProjectMigrationPlan &plan, ProjectMigrationContext &context, JobSystem &jobs,
                                               const ProjectMigrationLimits &limits, const CancellationToken &cancellation) {
            for (const ProjectMigrationDefinition &definition : plan.definitions) {
                for (const ProjectMigrationNode &node : definition.pipeline->Nodes()) {
                    if (const Result<void> executed = ExecuteNode(definition, node, context, jobs, limits, cancellation);
                        executed.HasError())
                        return executed;
                }
            }
            return Result<void>::Success();
        }

        void RebuildPreparedChanges(PreparedProjectMigration::State &prepared) {
            prepared.changes.clear();
            using enum PreparedMigrationChangeKind;
            for (const auto &document : prepared.context->documents) {
                if (!document.changed)
                    continue;
                auto kind = Replace;
                if (!document.existedInSource)
                    kind = Add;
                else if (!document.alive)
                    kind = Remove;
                prepared.changes.push_back({.path = document.entry.path,
                                            .kind = kind,
                                            .originalBytes = document.original.size(),
                                            .stagedBytes = document.alive ? document.bytes.size() : 0});
            }
            std::ranges::sort(prepared.changes, {}, &PreparedMigrationChange::path);
        }
    }  // namespace

    PreparedProjectMigration::PreparedProjectMigration(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    PreparedProjectMigration::~PreparedProjectMigration() = default;
    PreparedProjectMigration::PreparedProjectMigration(PreparedProjectMigration &&) noexcept = default;
    PreparedProjectMigration &PreparedProjectMigration::operator=(PreparedProjectMigration &&) noexcept = default;

    const std::filesystem::path &PreparedProjectMigration::CandidateRoot() const noexcept {
        return state_->candidateRoot;
    }

    std::span<const PreparedMigrationChange> PreparedProjectMigration::Changes() const noexcept {
        return state_->changes;
    }

    std::uint64_t PreparedProjectMigration::InputBytes() const noexcept {
        return state_->context->inputBytes;
    }

    std::uint64_t PreparedProjectMigration::OutputBytes() const noexcept {
        return state_->context->outputBytes;
    }

    /** @copydoc PreparedProjectMigration::ReadCandidateDocument */
    Result<std::vector<std::byte>> PreparedProjectMigration::ReadCandidateDocument(const std::string_view projectRelativePath) const {
        const auto document = std::ranges::find_if(state_->context->documents,
                                                   [projectRelativePath](const ProjectMigrationContext::State::Document &candidate) {
            return candidate.alive && candidate.entry.path == projectRelativePath;
        });
        if (document == state_->context->documents.end())
            return Result<std::vector<std::byte>>::Failure(
                MigrationError(ProjectErrors::MigrationDocumentStale,
                               "Candidate document is missing or removed: " + std::string(projectRelativePath)));
        return Result<std::vector<std::byte>>::Success(document->bytes);
    }

    Result<void> PreparedProjectMigration::VerifySourceUnchanged() const {
        return VerifyAuthoritativeUnchanged(state_->context);
    }

    void PreparedProjectMigration::PreserveForRecovery() const noexcept {
        state_->cleanupOwned = false;
    }

    std::vector<MigrationDocumentEntry> ProjectMigrationContext::ListDocuments(const MigrationDocumentQuery &query) const {
        std::vector<MigrationDocumentEntry> matches;
        for (const State::Document &document : state_->documents)
            if (document.alive && (!query.kind.has_value() || document.entry.kind == *query.kind))
                matches.push_back(document.entry);
        std::ranges::sort(matches, [](const MigrationDocumentEntry &left, const MigrationDocumentEntry &right) {
            return left.path < right.path;
        });
        return matches;
    }

    Result<ProjectDocumentView> ProjectMigrationContext::ReadDocument(const MigrationDocumentHandle document) const {
        if (document.generation != state_->generation || document.index >= state_->documents.size())
            return Result<ProjectDocumentView>::Failure(
                MigrationError(ProjectErrors::MigrationDocumentStale, "Document handle generation is stale."));
        const State::Document &slot = state_->documents[document.index];
        if (!slot.alive)
            return Result<ProjectDocumentView>::Failure(
                MigrationError(ProjectErrors::MigrationDocumentStale, "Document was removed from the candidate."));
        return Result<ProjectDocumentView>::Success(
            ProjectDocumentView{.handle = slot.entry.handle, .path = slot.entry.path, .kind = slot.entry.kind, .bytes = slot.bytes});
    }

    Result<void> ProjectMigrationContext::ReplaceDocument(MigrationDocumentHandle document, std::vector<std::byte> replacement) const {
        if (document.generation != state_->generation || document.index >= state_->documents.size() ||
            !state_->documents[document.index].alive)
            return Result<void>::Failure(MigrationError(ProjectErrors::MigrationDocumentStale, "Cannot replace a stale document."));
        State::Document &slot = state_->documents[document.index];
        const std::uint64_t nextOutput = state_->outputBytes - slot.bytes.size() + replacement.size();
        if (nextOutput > state_->limits.maxOutputBytes)
            return Result<void>::Failure(MigrationError(ProjectErrors::MigrationInventoryLimit, "Migration output exceeds byte limits."));
        state_->outputBytes = nextOutput;
        slot.changed = slot.changed || slot.bytes != replacement;
        slot.bytes = std::move(replacement);
        return Result<void>::Success();
    }

    Result<void> ProjectMigrationContext::AddDocument(const std::string &projectRelativePath, MigrationDocumentKind kind,
                                                      std::vector<std::byte> document) const {
        const std::filesystem::path normalized = std::filesystem::path(projectRelativePath).lexically_normal();
        const std::string generic = normalized.generic_string();
        if (normalized.is_absolute() || normalized.empty() || generic == ".." || generic.starts_with("../"))
            return Result<void>::Failure(
                MigrationError(ProjectErrors::MigrationInventoryInvalid, "Added document path escapes the project."));
        if (const std::string key = PortablePathKey(generic);
            std::ranges::any_of(state_->documents, [&key](const State::Document &existing) {
            return existing.alive && PortablePathKey(existing.entry.path) == key;
        }))
            return Result<void>::Failure(MigrationError(ProjectErrors::MigrationWriteConflict, "Added document path already exists."));
        if (state_->documents.size() >= state_->limits.maxDocuments ||
            state_->outputBytes + document.size() > state_->limits.maxOutputBytes)
            return Result<void>::Failure(
                MigrationError(ProjectErrors::MigrationInventoryLimit, "Added document exceeds migration limits."));
        State::Document added;
        added.entry.handle =
            MigrationDocumentHandle{.index = static_cast<std::uint32_t>(state_->documents.size()), .generation = state_->generation};
        added.entry.path = generic;
        added.entry.kind = kind;
        added.bytes = std::move(document);
        added.changed = true;
        added.existedInSource = false;
        state_->outputBytes += added.bytes.size();
        state_->documents.push_back(std::move(added));
        return Result<void>::Success();
    }

    Result<void> ProjectMigrationContext::RemoveDocument(MigrationDocumentHandle document) const {
        if (document.generation != state_->generation || document.index >= state_->documents.size() ||
            !state_->documents[document.index].alive)
            return Result<void>::Failure(MigrationError(ProjectErrors::MigrationDocumentStale, "Cannot remove a stale document."));
        State::Document &slot = state_->documents[document.index];
        state_->outputBytes -= slot.bytes.size();
        slot.alive = false;
        slot.changed = true;
        return Result<void>::Success();
    }

    Result<PreparedProjectMigration> ProjectMigrationExecutor::Prepare(const std::filesystem::path &projectRoot,
                                                                       const std::filesystem::path &candidateRoot,
                                                                       const ProjectMigrationPlan &plan, JobSystem &jobs,
                                                                       const ProjectMigrationLimits &limits,
                                                                       const CancellationToken cancellation) {
        const std::string sourceText = FormatHoroVersion(plan.source.value);
        const std::string targetText = FormatHoroVersion(plan.target.value);
        LOG_INFO("application.project_migration.execute", "Preparing migration source=%s target=%s.", sourceText.c_str(),
                 targetText.c_str());
        LOG_DEBUG("application.project_migration.execute", "Execution aggregate definitions=%zu.", plan.definitions.size());
        const auto built = BuildInventory(projectRoot, limits, candidateRoot);
        if (built.HasError()) {
            LOG_ERROR("application.project_migration.execute", "Inventory failed code=%s.", built.ErrorValue().code.Value().c_str());
            return Result<PreparedProjectMigration>::Failure(built.ErrorValue());
        }
        auto state = built.Value();
        LOG_DEBUG("application.project_migration.execute", "Inventory ready documents=%zu input_bytes=%llu.", state->documents.size(),
                  static_cast<unsigned long long>(state->inputBytes));
        ProjectMigrationContext context(state);
        if (const Result<void> executed = ExecutePlan(plan, context, jobs, limits, cancellation); executed.HasError()) {
            LOG_ERROR("application.project_migration.execute", "Execution failed code=%s.", executed.ErrorValue().code.Value().c_str());
            return Result<PreparedProjectMigration>::Failure(executed.ErrorValue());
        }

        if (const Result<void> materialized = MaterializeCandidate(state); materialized.HasError())
            return Result<PreparedProjectMigration>::Failure(materialized.ErrorValue());
        if (const Result<void> unchanged = VerifyAuthoritativeUnchanged(state); unchanged.HasError())
            return Result<PreparedProjectMigration>::Failure(unchanged.ErrorValue());

        auto prepared = std::make_unique<PreparedProjectMigration::State>();
        prepared->context = state;
        prepared->candidateRoot = candidateRoot;
        RebuildPreparedChanges(*prepared);
        LOG_INFO("application.project_migration.execute", "Migration candidate prepared source=%s target=%s.", sourceText.c_str(),
                 targetText.c_str());
        LOG_DEBUG("application.project_migration.execute", "Prepared candidate aggregate changed_files=%zu output_bytes=%llu.",
                  prepared->changes.size(), static_cast<unsigned long long>(state->outputBytes));
        return Result<PreparedProjectMigration>::Success(PreparedProjectMigration(std::move(prepared)));
    }

    /** @copydoc ProjectMigrationExecutor::Finalize */
    Result<void> ProjectMigrationExecutor::Finalize(PreparedProjectMigration &prepared,
                                                    const std::span<const PreparedMigrationDocument> documents,
                                                    std::shared_ptr<const IProjectMigrationValidator> targetValidator,
                                                    const CancellationToken cancellation) {
        if (!prepared.state_)
            return Result<void>::Failure(
                MigrationError(ProjectErrors::MigrationInventoryInvalid, "Prepared migration has no candidate state."));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MigrationError(ProjectErrors::MigrationCancelled, "Migration cancellation was requested."));

        ProjectMigrationContext context(prepared.state_->context);
        for (const PreparedMigrationDocument &document : documents) {
            const std::filesystem::path normalized = std::filesystem::path(document.path).lexically_normal();
            const std::string path = normalized.generic_string();
            if (normalized.is_absolute() || normalized.empty() || path == ".." || path.starts_with("../"))
                return Result<void>::Failure(MigrationError(ProjectErrors::MigrationInventoryInvalid,
                                                            "Transaction-owned candidate document escapes the project root."));

            const auto existing = std::ranges::find_if(prepared.state_->context->documents,
                                                       [&path](const ProjectMigrationContext::State::Document &candidate) {
                return candidate.alive && candidate.entry.path == path;
            });
            Result<void> applied = existing == prepared.state_->context->documents.end()
                                       ? context.AddDocument(path, document.kind, document.bytes)
                                       : context.ReplaceDocument(existing->entry.handle, document.bytes);
            if (applied.HasError())
                return applied;
        }

        if (targetValidator) {
            const auto descriptor = targetValidator->Describe();
            const Result<void> validated = targetValidator->Validate(context, cancellation);
            if (validated.HasError())
                return Result<void>::Failure(AnnotateStageError(validated.ErrorValue(), {"horo.project.target_contract"}, descriptor.id));
        }
        if (const Result<void> materialized = MaterializeCandidate(prepared.state_->context); materialized.HasError())
            return materialized;
        if (const Result<void> unchanged = VerifyAuthoritativeUnchanged(prepared.state_->context); unchanged.HasError())
            return unchanged;
        RebuildPreparedChanges(*prepared.state_);
        return Result<void>::Success();
    }

    Result<ProjectMigrationDryRunResult> ProjectMigrationExecutor::VerifiedDryRun(const std::filesystem::path &projectRoot,
                                                                                  const ProjectMigrationPlan &plan, JobSystem &jobs,
                                                                                  const ProjectMigrationLimits &limits,
                                                                                  const CancellationToken cancellation) {
        static std::atomic<std::uint64_t> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto candidateRoot = projectRoot / std::format("{}{}-{}", kMigrationDryRunRootPrefix, stamp, sequence.fetch_add(1));
        auto prepared = Prepare(projectRoot, candidateRoot, plan, jobs, limits, cancellation);
        if (prepared.HasError())
            return Result<ProjectMigrationDryRunResult>::Failure(prepared.ErrorValue());
        PreparedProjectMigration candidate = std::move(prepared).Value();
        if (const Result<void> finalized = Finalize(candidate, {}, plan.targetValidator, cancellation); finalized.HasError())
            return Result<ProjectMigrationDryRunResult>::Failure(finalized.ErrorValue());
        ProjectMigrationDryRunResult result{.inputBytes = candidate.InputBytes(), .outputBytes = candidate.OutputBytes()};
        for (const auto &change : candidate.Changes())
            result.changedFiles.push_back(change.path);
        return Result<ProjectMigrationDryRunResult>::Success(std::move(result));
    }
}  // namespace Horo::Application
