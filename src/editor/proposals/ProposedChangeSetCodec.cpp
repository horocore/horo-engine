#include "Horo/Editor/ProposedChangeSet.h"
#include "ProposedPatchInternal.h"

#include <algorithm>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <set>

namespace Horo::Editor::Proposals {
    namespace {
        using Json = nlohmann::json;

        struct InvalidWire final {};

        struct WireBudgetExceeded final {};

        template <class T> Result<T> Fail(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Rejects missing, duplicate-decoded and unknown schema fields. */
        void Shape(const Json &value, std::initializer_list<const char *> keys) {
            if (!value.is_object() || value.size() != keys.size())
                throw InvalidWire{};
            for (const auto *key : keys)
                if (!value.contains(key))
                    throw InvalidWire{};
        }

        /** @brief Rejects negative, floating-point and overflowing integer representations. */
        std::uint64_t Number(const Json &value) {
            if (!value.is_number_unsigned())
                throw InvalidWire{};
            return value.get<std::uint64_t>();
        }

        /** @brief Bounds a string before copying it out of the parsed envelope. */
        std::string String(const Json &value, std::size_t maximum) {
            if (!value.is_string() || value.get_ref<const std::string &>().size() > maximum)
                throw InvalidWire{};
            return value.get<std::string>();
        }

        /** @brief Requires the exact JSON boolean type. */
        bool Boolean(const Json &value) {
            if (!value.is_boolean())
                throw InvalidWire{};
            return value.get<bool>();
        }

        /** @brief Reuses canonical Foundation digest parsing. */
        Sha256Digest Digest(const Json &value) {
            const auto parsed = ParseSha256(String(value, 71));
            if (parsed.HasError())
                throw InvalidWire{};
            return parsed.Value();
        }

        /** @brief Reuses canonical source identity parsing before current-policy admission. */
        SourceDocumentId Path(const Json &value) {
            auto parsed = SourceDocumentId::Parse(String(value, MaximumSourceDocumentIdBytes));
            if (parsed.HasError())
                throw InvalidWire{};
            return std::move(parsed).Value();
        }

        /** @brief Rejects duplicate keys and nesting beyond sixteen before retaining unbounded structure. */
        Json Parse(std::string_view text, std::size_t remainingEvents = std::numeric_limits<std::size_t>::max()) {
            std::vector<std::set<std::string>> objects;
            const auto callback = [&objects, &remainingEvents](int depth, Json::parse_event_t event, const Json &value) {
                if (remainingEvents == 0)
                    throw WireBudgetExceeded{};
                --remainingEvents;
                if (depth > 16)
                    throw InvalidWire{};
                if (event == Json::parse_event_t::object_start)
                    objects.emplace_back();
                else if (event == Json::parse_event_t::object_end) {
                    if (objects.empty())
                        throw InvalidWire{};
                    objects.pop_back();
                } else if (event == Json::parse_event_t::key && (objects.empty() || !objects.back().insert(String(value, 128)).second))
                    throw InvalidWire{};
                return true;
            };
            return Json::parse(text.begin(), text.end(), callback);
        }

        /** @brief Serializes stable source-base evidence without open-session authority. */
        Json Base(const FileBase &base) {
            return {{"path", base.path.Value()},
                    {"revision", base.revision},
                    {"diskRevision", base.diskRevision},
                    {"bytes", base.bytes},
                    {"bom", base.utf8Bom},
                    {"content", FormatSha256(base.content)},
                    {"diskContent", FormatSha256(base.diskContent)}};
        }

        /** @brief Reads the complete fixed base schema without filesystem access. */
        FileBase Base(const Json &value) {
            Shape(value, {"path", "revision", "diskRevision", "bytes", "bom", "content", "diskContent"});
            return {Path(value.at("path")),   Number(value.at("revision")), Number(value.at("diskRevision")), Number(value.at("bytes")),
                    Boolean(value.at("bom")), Digest(value.at("content")),  Digest(value.at("diskContent"))};
        }

        /** @brief Emits canonical typed byte hunks, never a provider patch format. */
        Json Hunks(std::span<const TextHunk> hunks) {
            Json result = Json::array();
            for (const auto &hunk : hunks)
                result.push_back({{"offset", hunk.offset}, {"before", hunk.before}, {"after", hunk.after}});
            return result;
        }

        /** @brief Bounds hunk admission before allocating copied hunk storage. */
        std::vector<TextHunk> Hunks(const Json &values, const Limits &limits) {
            if (!values.is_array() || values.size() > limits.maximumHunksPerFile)
                throw InvalidWire{};
            std::vector<TextHunk> result;
            result.reserve(values.size());
            for (const auto &value : values) {
                Shape(value, {"offset", "before", "after"});
                result.emplace_back(Number(value.at("offset")), String(value.at("before"), limits.maximumTextBytes),
                                    String(value.at("after"), limits.maximumTextBytes));
            }
            return result;
        }

        /** @brief Encodes exactly one closed operation variant. */
        Json Operation(const FileOperation &operation) {
            return std::visit([]<typename VisitedValue>(const VisitedValue &value) -> Json {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, CreateFile>)
                    return {{"kind", "create"}, {"path", value.path.Value()}, {"text", value.text}};
                else if constexpr (std::is_same_v<T, EditFile>)
                    return {{"kind", "edit"}, {"base", Base(value.base)}, {"hunks", Hunks(value.hunks)}};
                else if constexpr (std::is_same_v<T, DeleteFile>)
                    return {{"kind", "delete"}, {"base", Base(value.base)}};
                else
                    return {{"kind", "rename"},
                            {"base", Base(value.base)},
                            {"destination", value.destination.Value()},
                            {"hunks", Hunks(value.hunks)}};
            }, operation);
        }

        /** @brief Rejects unknown operation kinds and kind-specific field drift. */
        FileOperation Operation(const Json &value, const Limits &limits) {
            if (!value.is_object() || !value.contains("kind"))
                throw InvalidWire{};
            const auto kind = String(value.at("kind"), 16);
            if (kind == "create") {
                Shape(value, {"kind", "path", "text"});
                return CreateFile{Path(value.at("path")), String(value.at("text"), limits.maximumTextBytes)};
            }
            if (kind == "edit") {
                Shape(value, {"kind", "base", "hunks"});
                return EditFile{Base(value.at("base")), Hunks(value.at("hunks"), limits)};
            }
            if (kind == "delete") {
                Shape(value, {"kind", "base"});
                return DeleteFile{Base(value.at("base"))};
            }
            if (kind == "rename") {
                Shape(value, {"kind", "base", "destination", "hunks"});
                return RenameFile{Base(value.at("base")), Path(value.at("destination")), Hunks(value.at("hunks"), limits)};
            }
            throw InvalidWire{};
        }

        /** @brief Validates a model's exact field set before any lookup of admitted base evidence. */
        std::string StructuredOperationKind(const Json &value) {
            const auto kind = String(value.at("kind"), 16);
            if (kind == "create")
                Shape(value, {"kind", "path", "text"});
            else if (kind == "edit")
                Shape(value, {"kind", "path", "hunks"});
            else if (kind == "delete")
                Shape(value, {"kind", "path"});
            else if (kind == "rename")
                Shape(value, {"kind", "path", "destination", "hunks"});
            else
                throw InvalidWire{};
            return kind;
        }

        /** @brief Resolves an edit against the host snapshot and checks every original-byte hunk before review. */
        Result<FileOperation> StructuredOperation(const Json &value, const ProjectPolicy &policy,
                                                  std::span<const SourceDocumentSnapshot> sources, const Limits &limits,
                                                  std::vector<const SourceDocumentSnapshot *> &used) {
            const auto kind = StructuredOperationKind(value);
            auto path = Path(value.at("path"));
            if (!policy.Allows(path))
                return Fail<FileOperation>(Errors::PolicyDenied);
            if (kind == "create")
                return Result<FileOperation>::Success(CreateFile{std::move(path), String(value.at("text"), limits.maximumTextBytes)});
            const auto *source = PatchDetail::ResolveBase(path, sources);
            if (!source)
                return Fail<FileOperation>(Errors::StaleBase);
            auto base = FileBase::Capture(*source);
            if (base.HasError())
                return Result<FileOperation>::Failure(base.ErrorValue());
            used.push_back(source);
            if (kind == "delete")
                return Result<FileOperation>::Success(DeleteFile{std::move(base).Value()});
            auto hunks = Hunks(value.at("hunks"), limits);
            if (const auto checked = ValidateAgainstSnapshot(base.Value(), hunks, *source); checked.HasError())
                return Result<FileOperation>::Failure(checked.ErrorValue());
            if (kind == "edit")
                return Result<FileOperation>::Success(EditFile{std::move(base).Value(), std::move(hunks)});
            return Result<FileOperation>::Success(RenameFile{std::move(base).Value(), Path(value.at("destination")), std::move(hunks)});
        }

        /** @brief Produces the sole canonical recovery payload and captured-policy commitment. */
        Json Payload(const ChangeSet &proposal) {
            Json files = Json::array();
            for (const auto &operation : proposal.Files())
                files.push_back(Operation(operation));
            const auto &draft = proposal.Values();
            return {{"project", proposal.Project()},
                    {"policyRevision", proposal.PolicyRevision()},
                    {"policy", FormatSha256(proposal.PolicyDigest())},
                    {"id", draft.id},
                    {"revision", draft.revision},
                    {"provenance",
                     {{"provider", draft.provenance.provider}, {"model", draft.provenance.model}, {"request", draft.provenance.request}}},
                    {"files", std::move(files)}};
        }

        /** @brief Checks the complete content-free identity portion of an audit record. */
        bool AuditIdentityValid(const AuditRecord &record) {
            return record.proposalIdentity != Sha256Digest{} && record.revision && record.digest != Sha256Digest{} &&
                   record.policy != Sha256Digest{};
        }

        /** @brief Checks an optional closed enum independently from other audit dispositions. */
        template <class Enum> bool OptionalEnumKnown(const std::optional<Enum> value, const Enum last) {
            return !value || *value <= last;
        }

        /** @brief Rejects unknown review/application/save enum values before interpreting their counts. */
        bool AuditEnumsValid(const AuditRecord &record) {
            return OptionalEnumKnown(record.review, ReviewChoice::Reject) &&
                   OptionalEnumKnown(record.application, ApplicationState::PartiallyApplied) &&
                   OptionalEnumKnown(record.save, DiskSaveState::Failed);
        }

        /** @brief Totals the fixed operation-kind projection without trusting narrow integer arithmetic. */
        std::uint64_t AuditOperationTotal(const AuditRecord &record) {
            std::uint64_t total{};
            for (const auto count : record.operationCounts)
                total += count;
            return total;
        }

        /** @brief Matches audit counts to the declared complete/partial/unmodified application observation. */
        bool AuditApplicationValid(const AuditRecord &record, const std::uint64_t total) {
            if (record.application == ApplicationState::AppliedToWorkspace)
                return record.appliedOperations == total;
            if (record.application == ApplicationState::PartiallyApplied)
                return record.appliedOperations > 0 && record.appliedOperations < total;
            return record.appliedOperations == 0;
        }

        /** @brief A save observation requires an application observation, and a real save requires complete application. */
        bool AuditSaveValid(const AuditRecord &record) {
            if (!record.save)
                return true;
            if (*record.save == DiskSaveState::NotRequested)
                return record.application.has_value();
            return record.application == ApplicationState::AppliedToWorkspace;
        }

        /** @brief Checks content-free audit identity, closed dispositions and their independent count/state invariants. */
        bool AuditValid(const AuditRecord &record) {
            if (!AuditIdentityValid(record) || !AuditEnumsValid(record))
                return false;
            const auto total = AuditOperationTotal(record);
            if (!total || total > Limits{}.maximumFiles || record.appliedOperations > total)
                return false;
            if (record.appliedOperations && record.review != ReviewChoice::Accept)
                return false;
            return AuditApplicationValid(record, total) && AuditSaveValid(record);
        }

        /** @brief Verifies the recovery format and exact canonical payload commitment before decoding proposal values. */
        const Json &RecoveryPayload(const Json &root) {
            if (String(root.at("format"), 64) != "horo.source-proposal")
                throw InvalidWire{};
            const auto &body = root.at("payload");
            Shape(body, {"project", "policyRevision", "policy", "id", "revision", "provenance", "files"});
            if (const auto bytes = body.dump(); ComputeSha256(std::as_bytes(std::span{bytes})) != Digest(root.at("commitment")))
                throw InvalidWire{};
            return body;
        }

        /** @brief Compares all host policy evidence before any recovered proposal is admitted. */
        bool RecoveryPolicyMatches(const Json &body, const ProjectPolicy &policy) {
            return String(body.at("project"), 128) == policy.Project() && Number(body.at("policyRevision")) == policy.Revision() &&
                   Digest(body.at("policy")) == policy.Digest();
        }

        /** @brief Reads bounded draft values after envelope/policy checks; authority is never part of this schema. */
        Result<Draft> RecoveryDraft(const Json &body, const Limits &limits) {
            const auto &provenance = body.at("provenance");
            Shape(provenance, {"provider", "model", "request"});
            const auto &files = body.at("files");
            if (!files.is_array() || files.size() > limits.maximumFiles || files.size() > Limits{}.maximumFiles)
                return Fail<Draft>(Errors::Bounds);
            Draft draft{String(body.at("id"), 128),
                        Number(body.at("revision")),
                        {String(provenance.at("provider"), 128), String(provenance.at("model"), 128),
                         String(provenance.at("request"), 128)},
                        {}};
            for (const auto &file : files)
                draft.files.push_back(Operation(file, limits));
            return Result<Draft>::Success(std::move(draft));
        }

        /** @brief Reads one optional closed disposition with exact JSON type and range checks. */
        template <class Enum> std::optional<Enum> ReadOptionalEnum(const Json &value, const Enum last) {
            if (value.is_null())
                return std::nullopt;
            const auto number = Number(value);
            if (number > static_cast<std::uint64_t>(last))
                throw InvalidWire{};
            return static_cast<Enum>(number);
        }

        /** @brief Reads the fixed operation-kind counts with a hard per-count bound. */
        std::array<std::uint32_t, 4> AuditCounts(const Json &counts) {
            if (!counts.is_array() || counts.size() != 4)
                throw InvalidWire{};
            std::array<std::uint32_t, 4> result{};
            for (std::size_t index = 0; index < result.size(); ++index) {
                const auto count = Number(counts[index]);
                if (count > Limits{}.maximumFiles)
                    throw InvalidWire{};
                result[index] = static_cast<std::uint32_t>(count);
            }
            return result;
        }

        /** @brief Reads bounded application progress independently from the application's disposition. */
        std::uint32_t AuditAppliedCount(const Json &value) {
            const auto applied = Number(value);
            if (applied > Limits{}.maximumFiles)
                throw InvalidWire{};
            return static_cast<std::uint32_t>(applied);
        }

        /** @brief Decodes the complete inert audit data after version admission. */
        AuditRecord AuditPayload(const Json &root) {
            Shape(root, {"format", "version", "id", "revision", "digest", "policy", "counts", "review", "application", "appliedOperations",
                         "save"});
            if (String(root.at("format"), 64) != "horo.source-proposal.audit")
                throw InvalidWire{};
            const auto &counts = root.at("counts");
            AuditRecord result{Digest(root.at("id")), Number(root.at("revision")), Digest(root.at("digest")), Digest(root.at("policy"))};
            result.operationCounts = AuditCounts(counts);
            result.review = ReadOptionalEnum(root.at("review"), ReviewChoice::Reject);
            result.appliedOperations = AuditAppliedCount(root.at("appliedOperations"));
            result.application = ReadOptionalEnum(root.at("application"), ApplicationState::PartiallyApplied);
            result.save = ReadOptionalEnum(root.at("save"), DiskSaveState::Failed);
            return result;
        }

        /** @brief Encodes the existing redacted proposal audit schema. */
        Json AuditPayload(const AuditRecord &record) {
            Json choice = nullptr;
            if (record.review)
                choice = static_cast<std::uint64_t>(*record.review);
            Json application = nullptr;
            if (record.application)
                application = static_cast<std::uint64_t>(*record.application);
            Json save = nullptr;
            if (record.save)
                save = static_cast<std::uint64_t>(*record.save);
            return {{"format", "horo.source-proposal.audit"},
                    {"version", WireVersion},
                    {"id", FormatSha256(record.proposalIdentity)},
                    {"revision", record.revision},
                    {"digest", FormatSha256(record.digest)},
                    {"policy", FormatSha256(record.policy)},
                    {"counts", record.operationCounts},
                    {"review", choice},
                    {"application", application},
                    {"appliedOperations", record.appliedOperations},
                    {"save", save}};
        }

        constexpr std::string_view SourceReviewAuditFormat = "horo.source-review.audit";
        constexpr std::uint64_t SourceReviewAuditVersion = 2;
        constexpr auto SourceReviewAuditEvents = Limits{}.maximumFiles * (Limits{}.maximumHunksPerFile + 12) + 64;

        /** @brief Checks that a file has only bounded individual review decisions. */
        bool SourceReviewChoicesValid(const std::vector<ReviewState> &choices) {
            return !choices.empty() && choices.size() <= Limits{}.maximumHunksPerFile &&
                   std::ranges::all_of(choices, [](ReviewState choice) {
                return choice <= ReviewState::Rejected;
            });
        }

        /** @brief Requires one ordered revision witness for each file with accepted units. */
        bool SourceReviewRevisionsValid(const SourceReviewAudit &audit) {
            auto revision = audit.revisions.begin();
            for (std::size_t file = 0; file < audit.choices.size(); ++file) {
                const auto &choices = audit.choices[file];
                if (std::ranges::find(choices, ReviewState::Accepted) == choices.end())
                    continue;
                if (revision == audit.revisions.end() || revision->file != file || !revision->before || revision->after < revision->before)
                    return false;
                ++revision;
            }
            return revision == audit.revisions.end();
        }

        /** @brief Confirms this audit is the source owner's projection of the proposal and accepted units. */
        bool SourceReviewAuditValid(const SourceReviewAudit &audit) {
            const auto &proposal = audit.proposal;
            return AuditValid(proposal) && AuditOperationTotal(proposal) == audit.choices.size() && !proposal.review &&
                   !proposal.application && !proposal.appliedOperations && !proposal.save &&
                   std::ranges::all_of(audit.choices, SourceReviewChoicesValid) && !audit.revisions.empty() &&
                   SourceReviewRevisionsValid(audit);
        }

        /** @brief Validates whether this is an unstored record or one exact persisted application. */
        bool SourceReviewOutcomeValid(const SourceReviewAudit &audit) {
            using enum SourceReviewDisposition;
            return audit.historySequence ? audit.disposition > Historical && audit.disposition <= Stale : audit.disposition == Historical;
        }

        /** @brief Constructs the fixed redacted application payload around the existing proposal audit format. */
        Json SourceReviewPayload(const SourceReviewAudit &audit) {
            Json revisions = Json::array();
            for (const auto &revision : audit.revisions)
                revisions.push_back(Json::array({revision.file, revision.before, revision.after}));
            return {{"proposal", AuditPayload(audit.proposal)},
                    {"decisions", FormatSha256(audit.decisionDigest)},
                    {"choices", audit.choices},
                    {"revisions", std::move(revisions)},
                    {"sequence", audit.historySequence},
                    {"disposition", static_cast<std::uint64_t>(audit.disposition)}};
        }

        /** @brief Parses one bounded list of canonical decisions without accepting aggregate review states. */
        std::vector<ReviewState> ReadSourceReviewChoices(const Json &units) {
            if (!units.is_array() || units.empty() || units.size() > Limits{}.maximumHunksPerFile)
                throw InvalidWire{};
            std::vector<ReviewState> choices;
            choices.reserve(units.size());
            for (const auto &unit : units) {
                const auto value = Number(unit);
                if (value > static_cast<std::uint64_t>(ReviewState::Rejected))
                    throw InvalidWire{};
                choices.push_back(static_cast<ReviewState>(value));
            }
            return choices;
        }

        /** @brief Parses typed revision witnesses in canonical proposal coordinates. */
        SourceReviewRevision ReadSourceReviewRevision(const Json &revision) {
            if (!revision.is_array() || revision.size() != 3)
                throw InvalidWire{};
            const auto file = Number(revision[0]);
            if (file >= Limits{}.maximumFiles)
                throw InvalidWire{};
            return {static_cast<std::uint32_t>(file), Number(revision[1]), Number(revision[2])};
        }

        /** @brief Validates the entire content-free record before exposing inert evidence. */
        SourceReviewAudit ReadSourceReviewPayload(const Json &payload, const std::uint64_t version) {
            if (version == WireVersion)
                Shape(payload, {"proposal", "decisions", "choices", "revisions"});
            else
                Shape(payload, {"proposal", "decisions", "choices", "revisions", "sequence", "disposition"});
            const auto &proposal = payload.at("proposal");
            const auto &choices = payload.at("choices");
            const auto &revisions = payload.at("revisions");
            if (Number(proposal.at("version")) != WireVersion || !choices.is_array() || choices.size() > Limits{}.maximumFiles ||
                !revisions.is_array() || revisions.size() > choices.size())
                throw InvalidWire{};
            SourceReviewAudit audit{AuditPayload(proposal), Digest(payload.at("decisions"))};
            for (const auto &file : choices)
                audit.choices.push_back(ReadSourceReviewChoices(file));
            for (const auto &revision : revisions)
                audit.revisions.push_back(ReadSourceReviewRevision(revision));
            if (version == SourceReviewAuditVersion) {
                audit.historySequence = Number(payload.at("sequence"));
                const auto disposition = Number(payload.at("disposition"));
                if (disposition > static_cast<std::uint64_t>(SourceReviewDisposition::Stale))
                    throw InvalidWire{};
                audit.disposition = static_cast<SourceReviewDisposition>(disposition);
            }
            if (!SourceReviewAuditValid(audit) || !SourceReviewOutcomeValid(audit))
                throw InvalidWire{};
            return audit;
        }
    }  // namespace

    /** @copydoc EncodeRecovery */
    Result<std::string> EncodeRecovery(const ChangeSet &proposal, const Limits &limits) {
        if (!limits.IsValid())
            return Fail<std::string>(Errors::Bounds);
        try {
            auto payload = Payload(proposal);
            const auto canonical = payload.dump();
            const auto digest = ComputeSha256(std::as_bytes(std::span{canonical}));
            auto encoded = Json{{"format", "horo.source-proposal"},
                                {"version", WireVersion},
                                {"payload", std::move(payload)},
                                {"commitment", FormatSha256(digest)}}
                               .dump();
            if (encoded.size() > limits.maximumWireBytes)
                return Fail<std::string>(Errors::Bounds);
            return Result<std::string>::Success(std::move(encoded));
        } catch (const std::bad_alloc &) {
            return Fail<std::string>(Errors::Bounds);
        }
    }

    /** @copydoc DecodeRecovery */
    Result<ChangeSet> DecodeRecovery(std::string_view text, const ProjectPolicy &policy, const Limits &limits) {
        if (!limits.IsValid() || text.size() > limits.maximumWireBytes)
            return Fail<ChangeSet>(Errors::Bounds);
        try {
            const auto root = Parse(text);
            Shape(root, {"format", "version", "payload", "commitment"});
            if (Number(root.at("version")) != WireVersion)
                return Fail<ChangeSet>(Errors::UnsupportedVersion);
            const auto &body = RecoveryPayload(root);
            if (!RecoveryPolicyMatches(body, policy))
                return Fail<ChangeSet>(Errors::PolicyDenied);
            auto draft = RecoveryDraft(body, limits);
            if (draft.HasError())
                return Result<ChangeSet>::Failure(draft.ErrorValue());
            return ChangeSet::Capture(policy, std::move(draft).Value(), limits);
        } catch (const std::bad_alloc &) {
            return Fail<ChangeSet>(Errors::Bounds);
        } catch (const InvalidWire &) {
            return Fail<ChangeSet>(Errors::Malformed);
        } catch (const Json::exception &) {
            return Fail<ChangeSet>(Errors::Malformed);
        }
    }

    /** @copydoc DecodeStructuredPatch */
    Result<ChangeSet> DecodeStructuredPatch(const std::string_view text, const ProjectPolicy &policy, ProposalIdentity identity,
                                            const std::span<const SourceDocumentSnapshot> sources, const Limits &limits) {
        if (!limits.IsValid() || text.size() > limits.maximumWireBytes || sources.size() > Limits{}.maximumFiles)
            return Fail<ChangeSet>(Errors::Bounds);
        try {
            // Each hunk object accounts for at most eight JSON parser events.
            const auto events = 16U + limits.maximumFiles * (24U + 8U * limits.maximumHunksPerFile);
            const auto root = Parse(text, events);
            Shape(root, {"version", "files"});
            if (Number(root.at("version")) != WireVersion)
                return Fail<ChangeSet>(Errors::UnsupportedVersion);
            const auto &files = root.at("files");
            if (!files.is_array())
                return Fail<ChangeSet>(Errors::Malformed);
            if (files.size() > limits.maximumFiles)
                return Fail<ChangeSet>(Errors::Bounds);
            Draft draft{std::move(identity.id), identity.revision, std::move(identity.provenance)};
            draft.files.reserve(files.size());
            std::vector<const SourceDocumentSnapshot *> used;
            used.reserve(files.size());
            for (const auto &file : files) {
                auto parsed = StructuredOperation(file, policy, sources, limits, used);
                if (parsed.HasError())
                    return Result<ChangeSet>::Failure(parsed.ErrorValue());
                draft.files.push_back(std::move(parsed).Value());
            }
            auto proposal = ChangeSet::Capture(policy, std::move(draft), limits);
            if (proposal.HasValue() && std::ranges::any_of(used, [](const auto *source) {
                return !source->IsCurrent();
            }))
                return Fail<ChangeSet>(Errors::StaleBase);
            return proposal;
        } catch (const std::bad_alloc &) {
            return Fail<ChangeSet>(Errors::Bounds);
        } catch (const WireBudgetExceeded &) {
            return Fail<ChangeSet>(Errors::Bounds);
        } catch (const InvalidWire &) {
            return Result<ChangeSet>::Failure(MakeError(Errors::Malformed, "Structured patch does not match the operation schema"));
        } catch (const Json::parse_error &error) {
            return Result<ChangeSet>::Failure(MakeError(Errors::Malformed, "Structured patch near byte " + std::to_string(error.byte)));
        } catch (const Json::exception &) {
            return Fail<ChangeSet>(Errors::Malformed);
        }
    }

    /** @copydoc EncodeAudit */
    Result<std::string> EncodeAudit(const AuditRecord &record) {
        if (!AuditValid(record))
            return Fail<std::string>(Errors::Invalid);
        try {
            return Result<std::string>::Success(AuditPayload(record).dump());
        } catch (const std::bad_alloc &) {
            return Fail<std::string>(Errors::Bounds);
        }
    }

    /** @copydoc DecodeAudit */
    Result<AuditRecord> DecodeAudit(std::string_view text) {
        if (text.size() > 4096)
            return Fail<AuditRecord>(Errors::Bounds);
        try {
            const auto root = Parse(text);
            if (root.contains("version") && Number(root.at("version")) != WireVersion)
                return Fail<AuditRecord>(Errors::UnsupportedVersion);
            auto result = AuditPayload(root);
            if (!AuditValid(result))
                throw InvalidWire{};
            return Result<AuditRecord>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Fail<AuditRecord>(Errors::Bounds);
        } catch (const InvalidWire &) {
            return Fail<AuditRecord>(Errors::Malformed);
        } catch (const Json::exception &) {
            return Fail<AuditRecord>(Errors::Malformed);
        }
    }

    /** @copydoc EncodeSourceReviewAudit */
    Result<std::string> EncodeSourceReviewAudit(const SourceReviewAudit &audit) {
        if (!SourceReviewAuditValid(audit) || !SourceReviewOutcomeValid(audit))
            return Fail<std::string>(Errors::Invalid);
        try {
            auto payload = SourceReviewPayload(audit);
            const auto canonical = payload.dump();
            auto encoded = Json{{"format", SourceReviewAuditFormat},
                                {"version", SourceReviewAuditVersion},
                                {"payload", std::move(payload)},
                                {"commitment", FormatSha256(ComputeSha256(std::as_bytes(std::span{canonical})))}}
                               .dump();
            if (encoded.size() > MaximumSourceReviewAuditBytes)
                return Fail<std::string>(Errors::Bounds);
            return Result<std::string>::Success(std::move(encoded));
        } catch (const std::bad_alloc &) {
            return Fail<std::string>(Errors::Bounds);
        }
    }

    /** @copydoc DecodeSourceReviewAudit */
    Result<SourceReviewAudit> DecodeSourceReviewAudit(std::string_view text) {
        if (text.size() > MaximumSourceReviewAuditBytes)
            return Fail<SourceReviewAudit>(Errors::Bounds);
        try {
            const auto root = Parse(text, SourceReviewAuditEvents);
            Shape(root, {"format", "version", "payload", "commitment"});
            const auto version = Number(root.at("version"));
            if (version != WireVersion && version != SourceReviewAuditVersion)
                return Fail<SourceReviewAudit>(Errors::UnsupportedVersion);
            if (String(root.at("format"), 64) != SourceReviewAuditFormat)
                throw InvalidWire{};
            const auto &payload = root.at("payload");
            const auto canonical = payload.dump();
            if (Digest(root.at("commitment")) != ComputeSha256(std::as_bytes(std::span{canonical})))
                throw InvalidWire{};
            return Result<SourceReviewAudit>::Success(ReadSourceReviewPayload(payload, version));
        } catch (const WireBudgetExceeded &) {
            return Fail<SourceReviewAudit>(Errors::Bounds);
        } catch (const std::bad_alloc &) {
            return Fail<SourceReviewAudit>(Errors::Bounds);
        } catch (const InvalidWire &) {
            return Fail<SourceReviewAudit>(Errors::Malformed);
        } catch (const Json::exception &) {
            return Fail<SourceReviewAudit>(Errors::Malformed);
        }
    }
}  // namespace Horo::Editor::Proposals
