#include "HoroScriptSyntax.h"

#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Foundation/Utf8.h"

#include <charconv>
#include <cstdlib>
#include <memory>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Horo::Editor::Language {
    namespace {
        constexpr std::size_t MaximumSyntaxSourceBytes = 256 * 1024;
        constexpr std::size_t MaximumSyntaxMemoryBytes = 4 * 1024 * 1024;
        constexpr std::size_t MaximumDiagnosticBytes = 4096;
        constexpr std::string_view ChunkName = "horo-source";

        /** @brief Charges the complete parser state to one private allocation budget. */
        void *AllocateSyntaxMemory(void *context, void *pointer, std::size_t previous, const std::size_t bytes) noexcept {
            auto &used = *static_cast<std::size_t *>(context);
            const auto retained = used - (pointer ? previous : 0);
            if (bytes == 0) {
                std::free(pointer);
                used = retained;
                return nullptr;
            }
            if (bytes > MaximumSyntaxMemoryBytes - retained)
                return nullptr;
            if (auto *memory = std::realloc(pointer, bytes)) {
                used = retained + bytes;
                return memory;
            }
            return nullptr;
        }

        /** @brief Projects a Lua logical line into its text bytes, including mixed CR/LF pairs. */
        LanguageByteSpan SyntaxLine(const std::string_view text, std::size_t line, std::size_t offset) {
            while (line > 1) {
                const auto newline = text.find_first_of("\r\n", offset);
                if (newline == std::string_view::npos)
                    return {text.size(), text.size()};
                offset = newline + 1;
                if (offset < text.size() && (text[offset] == '\n' || text[offset] == '\r') && text[offset] != text[newline])
                    ++offset;
                --line;
            }
            const auto end = text.find_first_of("\r\n", offset);
            return {offset, end == std::string_view::npos ? text.size() : end};
        }

        /** @brief Trims parser detail to a valid marker string at a UTF-8 boundary. */
        std::string_view MarkerMessage(std::string_view message) {
            if (message.size() > MaximumDiagnosticBytes) {
                auto end = MaximumDiagnosticBytes;
                while (end > 0 && (static_cast<unsigned char>(message[end]) & 0xC0U) == 0x80U)
                    --end;
                message = message.substr(0, end);
            }
            return IsValidUtf8ScalarSequence(message) ? message : std::string_view{};
        }

        /** @brief Decodes the line from Lua's fixed chunk label without treating file names as parser messages. */
        LanguageDiagnostic SyntaxDiagnostic(std::string_view message, const SourceDocumentSnapshot &source, const std::size_t bom) {
            const auto prefix = message.find(':');
            std::size_t line = 1;
            if (message.substr(0, prefix) == ChunkName && prefix != std::string_view::npos) {
                const auto digits = message.substr(prefix + 1);
                const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), line);
                if (parsed.ec == std::errc{} && parsed.ptr != digits.data() + digits.size() && *parsed.ptr == ':') {
                    message = digits.substr(static_cast<std::size_t>(parsed.ptr - digits.data()) + 1);
                    if (message.starts_with(' '))
                        message.remove_prefix(1);
                }
            }
            return {SyntaxLine(source.Text(), line, bom), LanguageSeverity::Error, std::string{MarkerMessage(message)}};
        }
    }  // namespace

    /** @copydoc CheckHoroScriptSyntax */
    Result<LanguageResult> CheckHoroScriptSyntax(const SourceDocumentSnapshot &source) {
        using Output = Result<LanguageResult>;
        if (source.Revision() == 0)
            return Output::Failure(MakeError(SourceDocumentErrors::Invalid));
        if (source.Text().size() > MaximumSyntaxSourceBytes)
            return Output::Failure(MakeError(SourceDocumentErrors::TooLarge));
        std::size_t used = 0;
        const std::unique_ptr<lua_State, decltype(&lua_close)> parser{lua_newstate(AllocateSyntaxMemory, &used), lua_close};
        if (!parser)
            return Output::Failure(MakeError(SourceDocumentErrors::TooLarge));
        const auto bom = source.Metadata().utf8Bom ? 3U : 0U;
        const auto text = source.Text().substr(bom);
        const int loaded = luaL_loadbufferx(parser.get(), text.data(), text.size(), "=horo-source", "t");
        LanguageDiagnostics diagnostics;
        if (loaded == LUA_ERRSYNTAX) {
            const auto *message = lua_tostring(parser.get(), -1);
            diagnostics.items.push_back(SyntaxDiagnostic(message ? message : "Invalid syntax.", source, bom));
        } else if (loaded != LUA_OK) {
            return Output::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
        return Output::Success({{SourceLanguageRequestKind::Diagnostics, source}, std::move(diagnostics)});
    }

    /** @copydoc HoroScriptSyntaxWorkspace::~HoroScriptSyntaxWorkspace */
    HoroScriptSyntaxWorkspace::~HoroScriptSyntaxWorkspace() {
        if (work_)
            static_cast<void>(work_->job.RequestCancel());
    }

    /** @copydoc HoroScriptSyntaxWorkspace::Start */
    void HoroScriptSyntaxWorkspace::Start(const JobSystem &jobs, const SourceDocumentSnapshot &source) {
        auto output = std::make_shared<std::optional<LanguageResult>>();
        auto submitted = jobs.SubmitResult({.priority = JobPriority::Background}, [source, output](const CancellationToken &cancel) {
            if (cancel.IsCancellationRequested())
                return Result<void>::Success();
            auto result = CheckHoroScriptSyntax(source);
            if (result.HasError())
                return Result<void>::Failure(std::move(result).ErrorValue());
            if (!cancel.IsCancellationRequested())
                output->emplace(std::move(result).Value());
            return Result<void>::Success();
        });
        if (submitted.HasValue()) {
            work_.emplace(Work{source, std::move(submitted).Value(), std::move(output)});
            attempted_ = source;
        } else {
            retryAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
        }
    }

    /** @copydoc HoroScriptSyntaxWorkspace::Admit */
    void HoroScriptSyntaxWorkspace::Admit(const JobSystem &jobs, const SourceFileOpenService &sources,
                                          const SourceDocumentSnapshot &source) {
        if (!source.IsCurrent()) {
            attempted_ = {};
            retryAt_ = {};
            return;
        }
        if (attempted_.SharesStateWith(source) || std::chrono::steady_clock::now() < retryAt_)
            return;
        const auto identity = source.Identity();
        const auto &utf8Path = identity.key.source.Value();
        const std::filesystem::path path{std::u8string{utf8Path.begin(), utf8Path.end()}};
        if (sources.Classify(path).kind == SourceFileKind::HoroScript)
            Start(jobs, source);
        else
            attempted_ = source;
    }

    /** @copydoc HoroScriptSyntaxWorkspace::Poll */
    std::optional<LanguageResult> HoroScriptSyntaxWorkspace::Poll(const JobSystem &jobs, const SourceFileOpenService &sources,
                                                                  const SourceDocumentSnapshot &source) {
        std::optional<LanguageResult> publication;
        if (work_) {
            const bool matches = source.IsCurrent() && work_->source.SharesStateWith(source);
            const auto job = work_->job.Snapshot();
            if (!job || !job->terminalResult) {
                if (!matches)
                    static_cast<void>(work_->job.RequestCancel());
                return std::nullopt;
            }
            if (matches && job->state == JobState::Succeeded)
                publication = std::move(*work_->result);
            if (job->state == JobState::Cancelled)
                attempted_ = {};
            work_.reset();
        }
        Admit(jobs, sources, source);
        return publication;
    }
}  // namespace Horo::Editor::Language
