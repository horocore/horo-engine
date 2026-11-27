#include "Horo/Foundation/Utf8.h"
#include "ProposedPatchInternal.h"

#include <algorithm>
#include <charconv>
#include <new>

namespace Horo::Editor::Proposals {
    namespace {
        struct PatchError final {
            const ErrorCodeDescriptor &code;
        };

        /** @brief Retains wire line views and removes only the selected transport delimiter. */
        class Lines final {
        public:
            explicit Lines(std::string_view text) : remaining_(text), crlf_(text.substr(0, text.find('\n')).ends_with('\r')) {}

            bool Empty() const noexcept {
                return remaining_.empty();
            }

            bool CrLf() const noexcept {
                return crlf_;
            }

            std::size_t LastLine() const noexcept {
                return lastLine_;
            }

            std::string_view Peek() const {
                const auto end = remaining_.find('\n');
                if (end == std::string_view::npos)
                    throw PatchError{Errors::Malformed};
                auto line = remaining_.substr(0, end);
                if (crlf_) {
                    if (!line.ends_with('\r'))
                        throw PatchError{Errors::Malformed};
                    line.remove_suffix(1);
                }
                return line;
            }

            std::string_view Take() {
                const auto line = Peek();
                remaining_.remove_prefix(line.size() + (crlf_ ? 2U : 1U));
                ++lastLine_;
                return line;
            }

        private:
            std::string_view remaining_;
            bool crlf_{};
            std::size_t lastLine_{};
        };

        struct Range final {
            std::size_t start{};
            std::size_t count{};
        };

        /** @brief Reads a non-negative decimal coordinate without signed or floating conversions. */
        std::size_t Number(std::string_view &text) {
            std::size_t value{};
            const auto end = text.data() + text.size();
            const auto result = std::from_chars(text.data(), end, value);
            if (result.ec != std::errc{})
                throw PatchError{Errors::Malformed};
            text.remove_prefix(static_cast<std::size_t>(result.ptr - text.data()));
            return value;
        }

        /** @brief Consumes one exact grammar delimiter. */
        void Require(std::string_view &text, const std::string_view prefix) {
            if (!text.starts_with(prefix))
                throw PatchError{Errors::Malformed};
            text.remove_prefix(prefix.size());
        }

        /** @brief Normalizes the unified format's zero-length insertion coordinate. */
        Range ReadRange(std::string_view &text) {
            const auto line = Number(text);
            std::size_t count{1};
            if (text.starts_with(',')) {
                text.remove_prefix(1);
                count = Number(text);
            }
            if (count && !line)
                throw PatchError{Errors::Malformed};
            return {count ? line - 1 : line, count};
        }

        /** @brief Admits only explicit portable names and the unified create/delete sentinel. */
        std::optional<SourceDocumentId> HeaderPath(std::string_view line, const std::string_view label, const std::string_view side) {
            Require(line, label);
            if (line == "/dev/null")
                return {};
            Require(line, side);
            auto path = SourceDocumentId::Parse(line);
            if (path.HasError())
                throw PatchError{Errors::Malformed};
            return std::move(path).Value();
        }

        /** @brief Git index hashes are checked as syntax and never accepted as revision authority. */
        bool Hex(std::string_view text) {
            return text.size() >= 7 && text.size() <= 64 && std::ranges::all_of(text, [](const char letter) {
                return (letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'f');
            });
        }

        /** @brief Validates optional Git text-diff metadata without accepting permission changes. */
        void ReadIndex(std::string_view line) {
            Require(line, "index ");
            const auto space = line.find(' ');
            if (space != std::string_view::npos && line.substr(space + 1) != "100644" && line.substr(space + 1) != "100755")
                throw PatchError{Errors::Malformed};
            line = line.substr(0, space);
            const auto separator = line.find("..");
            if (separator == std::string_view::npos || !Hex(line.substr(0, separator)) || !Hex(line.substr(separator + 2)))
                throw PatchError{Errors::Malformed};
        }

        struct Paths final {
            std::optional<SourceDocumentId> before;
            std::optional<SourceDocumentId> after;
        };

        /** @brief Reads one file's original and result identities and checks any Git envelope against them. */
        Paths FilePaths(Lines &lines) {
            std::string_view git, mode;
            if (lines.Peek().starts_with("diff --git ")) {
                git = lines.Take();
                const auto next = lines.Peek();
                if (next.starts_with("new file mode ") || next.starts_with("deleted file mode "))
                    mode = lines.Take();
                if (lines.Peek().starts_with("index "))
                    ReadIndex(lines.Take());
            }
            Paths paths{HeaderPath(lines.Take(), "--- ", "a/"), HeaderPath(lines.Take(), "+++ ", "b/")};
            if ((!paths.before && !paths.after) || (paths.before && paths.after && *paths.before != *paths.after))
                throw PatchError{Errors::Malformed};
            const auto &path = (paths.before ? *paths.before : *paths.after).Value();
            if (!git.empty() && git != "diff --git a/" + path + " b/" + path)
                throw PatchError{Errors::Malformed};
            if (!mode.empty() && !(mode == "new file mode 100644" && !paths.before) &&
                !(mode == "deleted file mode 100644" && !paths.after))
                throw PatchError{Errors::Malformed};
            return paths;
        }

        /** @brief Tracks monotonic coordinates and exact source bytes for one file. */
        struct FileCursor final {
            std::string_view source;
            std::size_t offset{};
            std::size_t oldLine{};
            std::size_t newLine{};
            bool newEnded{};
            bool crlfAddition{};
        };

        /** @brief Advances one source line once, preserving both the bytes and its exact ending. */
        std::string_view TakeSourceLine(FileCursor &file) {
            if (file.offset == file.source.size())
                throw PatchError{Errors::StaleBase};
            const auto rest = file.source.substr(file.offset);
            const auto end = rest.find('\n');
            const auto line = rest.substr(0, end == std::string_view::npos ? end : end + 1);
            file.offset += line.size();
            ++file.oldLine;
            return line;
        }

        /** @brief Checks complete context or a removed line against the offered source. */
        void CheckSourceLine(std::string_view actual, const std::string_view payload, const bool newline, const bool crlf) {
            if (actual.ends_with('\n') != newline)
                throw PatchError{Errors::StaleBase};
            if (newline) {
                actual.remove_suffix(1);
                if (crlf && actual.ends_with('\r'))
                    actual.remove_suffix(1);
            }
            if (actual != payload)
                throw PatchError{Errors::StaleBase};
        }

        /** @brief Appends complete source bytes within the configured per-hunk budget. */
        void Append(std::string &text, const std::string_view bytes, const Limits &limits) {
            if (bytes.size() > limits.maximumTextBytes - text.size())
                throw PatchError{Errors::Bounds};
            text.append(bytes);
        }

        /** @brief Advances unchanged ranges and confirms that both headers name the same insertion point. */
        void Seek(FileCursor &file, const Range before, const Range after) {
            if (file.newEnded || before.start < file.oldLine || after.start < file.newLine ||
                before.start - file.oldLine != after.start - file.newLine)
                throw PatchError{Errors::Malformed};
            while (file.oldLine < before.start) {
                if (!TakeSourceLine(file).ends_with('\n'))
                    throw PatchError{Errors::Malformed};
            }
            file.newLine = after.start;
        }

        /** @brief Reads one hunk, comparing context and counting its declared source and result lines. */
        TextHunk ReadHunk(Lines &lines, FileCursor &file, const Limits &limits) {
            auto header = lines.Take();
            Require(header, "@@ -");
            auto before = ReadRange(header);
            Require(header, " +");
            auto after = ReadRange(header);
            Require(header, " @@");
            if ((!header.empty() && !header.starts_with(' ')) || (!before.count && !after.count))
                throw PatchError{Errors::Malformed};
            Seek(file, before, after);
            TextHunk hunk{file.offset};
            while (before.count || after.count) {
                const auto line = lines.Take();
                if (line.empty() || (line.front() != ' ' && line.front() != '+' && line.front() != '-'))
                    throw PatchError{Errors::Malformed};
                const auto payload = line.substr(1);
                const bool newline = lines.Empty() || lines.Peek() != "\\ No newline at end of file";
                if (!newline)
                    static_cast<void>(lines.Take());
                std::string_view original;
                if (line.front() != '+') {
                    if (!before.count)
                        throw PatchError{Errors::Malformed};
                    --before.count;
                    original = TakeSourceLine(file);
                    CheckSourceLine(original, payload, newline, lines.CrLf());
                    Append(hunk.before, original, limits);
                }
                if (line.front() != '-') {
                    if (!after.count || file.newEnded)
                        throw PatchError{Errors::Malformed};
                    --after.count;
                    ++file.newLine;
                    file.newEnded = !newline;
                    Append(hunk.after, line.front() == ' ' ? original : payload, limits);
                    if (line.front() == '+' && newline)
                        Append(hunk.after, file.crlfAddition ? "\r\n" : "\n", limits);
                }
            }
            return hunk;
        }

        /** @brief Converts one complete admitted file section into a neutral operation. */
        FileOperation ReadFile(Lines &lines, const ProjectPolicy &policy, std::span<const SourceDocumentSnapshot> sources,
                               const Limits &limits, std::vector<const SourceDocumentSnapshot *> &used) {
            auto paths = FilePaths(lines);
            const auto &path = paths.before ? *paths.before : *paths.after;
            if (!policy.Allows(path))
                throw PatchError{Errors::PolicyDenied};
            FileCursor file;
            std::optional<FileBase> base;
            if (paths.before) {
                const auto *source = PatchDetail::ResolveBase(path, sources);
                if (!source)
                    throw PatchError{Errors::StaleBase};
                auto captured = FileBase::Capture(*source);
                if (captured.HasError())
                    throw PatchError{Errors::Invalid};
                base.emplace(std::move(captured).Value());
                if (base->bytes > limits.maximumTextBytes)
                    throw PatchError{Errors::Bounds};
                file.source = source->Text();
                file.offset = base->utf8Bom ? 3U : 0U;
                const auto firstEnd = file.source.find('\n');
                file.crlfAddition = lines.CrLf() && firstEnd != std::string_view::npos && firstEnd && file.source[firstEnd - 1] == '\r';
                used.push_back(source);
            }
            std::vector<TextHunk> hunks;
            while (!lines.Empty() && lines.Peek().starts_with("@@ ")) {
                if (hunks.size() == limits.maximumHunksPerFile)
                    throw PatchError{Errors::Bounds};
                auto hunk = ReadHunk(lines, file, limits);
                if (hunk.before == hunk.after || (!paths.after && !hunk.after.empty()))
                    throw PatchError{Errors::Malformed};
                hunks.push_back(std::move(hunk));
            }
            if (file.newEnded && file.offset != file.source.size())
                throw PatchError{Errors::Malformed};
            if (!base) {
                std::string text;
                for (const auto &hunk : hunks)
                    Append(text, hunk.after, limits);
                return CreateFile{std::move(*paths.after), std::move(text)};
            }
            if (!paths.after) {
                if (file.offset != file.source.size() || (!hunks.empty() && hunks.front().offset != (base->utf8Bom ? 3U : 0U)))
                    throw PatchError{Errors::Malformed};
                std::size_t removed = base->utf8Bom ? 3U : 0U;
                for (const auto &hunk : hunks)
                    removed += hunk.before.size();
                if (removed != file.source.size())
                    throw PatchError{Errors::Malformed};
                return DeleteFile{std::move(*base)};
            }
            return EditFile{std::move(*base), std::move(hunks)};
        }
    }  // namespace

    /** @copydoc DecodeUnifiedPatch */
    Result<ChangeSet> DecodeUnifiedPatch(const std::string_view text, const ProjectPolicy &policy, ProposalIdentity identity,
                                         const std::span<const SourceDocumentSnapshot> sources, const Limits &limits) {
        using Output = Result<ChangeSet>;
        if (!limits.IsValid() || text.size() > limits.maximumWireBytes || sources.size() > Limits{}.maximumFiles)
            return Output::Failure(MakeError(Errors::Bounds));
        if (!IsValidUtf8ScalarSequence(text) || std::ranges::any_of(text, [](const unsigned char byte) {
            return byte == 127 || (byte < 32 && byte != '\t' && byte != '\r' && byte != '\n' && byte != '\f');
        }))
            return Output::Failure(MakeError(Errors::Malformed));
        Lines lines{text};
        try {
            Draft draft{std::move(identity.id), identity.revision, std::move(identity.provenance)};
            std::vector<const SourceDocumentSnapshot *> used;
            while (!lines.Empty()) {
                if (draft.files.size() == limits.maximumFiles)
                    return Output::Failure(MakeError(Errors::Bounds));
                draft.files.push_back(ReadFile(lines, policy, sources, limits, used));
            }
            auto proposal = ChangeSet::Capture(policy, std::move(draft), limits);
            if (proposal.HasValue() && std::ranges::any_of(used, [](const auto *source) {
                return !source->IsCurrent();
            }))
                return Output::Failure(MakeError(Errors::StaleBase));
            return proposal;
        } catch (const PatchError &error) {
            return Output::Failure(
                MakeError(error.code, "Unified patch near line " + std::to_string(std::max(std::size_t{1}, lines.LastLine()))));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(Errors::Bounds));
        }
    }
}  // namespace Horo::Editor::Proposals
