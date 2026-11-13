#include "editor/source_editor/SourceSyntax.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Locale-independent ASCII identifier admission; UTF-8 bytes remain readable ordinary text. */
        [[nodiscard]] bool IdentifierStart(const char c) noexcept {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        /** @brief ASCII digit admission without signed-char locale hazards. */
        [[nodiscard]] bool Digit(const char c) noexcept {
            return c >= '0' && c <= '9';
        }

        /** @brief Tests a word against a bounded, space-delimited language vocabulary. */
        [[nodiscard]] bool WordIn(const std::string_view words, const std::string_view word) noexcept {
            std::size_t start = 0;
            while (start < words.size()) {
                const auto end = words.find(' ', start);
                if (words.substr(start, end == std::string_view::npos ? end : end - start) == word)
                    return true;
                if (end == std::string_view::npos)
                    break;
                start = end + 1;
            }
            return false;
        }

        /** @brief Classifies reserved words independently for script, shader, native, and configuration text. */
        [[nodiscard]] SourceSyntaxRole WordRole(const SourceLanguage language, const std::string_view word) noexcept {
            using enum SourceLanguage;
            using enum SourceSyntaxRole;

            struct Vocabulary {
                SourceLanguage language;
                std::string_view words;
                SourceSyntaxRole role;
            };

            constexpr std::array vocabularies{
                Vocabulary{HoroScript,
                           "and break do else elseif end false for function goto if in local nil not or repeat return then true until "
                           "while",
                           Keyword},
                Vocabulary{Cpp,
                           "alignas alignof asm auto break case catch class co_await co_return co_yield concept const consteval constexpr "
                           "constinit const_cast continue default delete do dynamic_cast else enum explicit export extern false for friend "
                           "goto if inline mutable namespace new noexcept nullptr operator private protected public register "
                           "reinterpret_cast requires return sizeof static static_assert static_cast struct switch template this "
                           "thread_local throw true try typedef typeid typename union using virtual volatile while",
                           Keyword},
                Vocabulary{Cpp, "bool char char8_t char16_t char32_t double float int long short signed unsigned void wchar_t", Type},
                Vocabulary{Shader, "cbuffer discard groupshared in inout layout out precision register sampler shared uniform varying",
                           Keyword},
                Vocabulary{Shader,
                           "float2 float3 float4 float2x2 float3x3 float4x4 half half2 half3 half4 int2 int3 int4 uint uint2 uint3 uint4 "
                           "vec2 vec3 vec4 mat2 mat3 mat4 sampler2D Texture2D SamplerState",
                           Type},
                Vocabulary{Json, "true false null", Keyword},
            };
            const auto family = language == Toml || language == Yaml ? Json : language;
            for (const auto &vocabulary : vocabularies) {
                const bool sharedNative = language == Shader && vocabulary.language == Cpp;
                if ((vocabulary.language == family || sharedNative) && WordIn(vocabulary.words, word))
                    return vocabulary.role;
            }
            return Text;
        }

        /** @brief Scans one escaped single-line string, returning no end for incomplete lexical input. */
        [[nodiscard]] std::size_t QuotedEnd(const std::string_view text, std::size_t position, const char quote) noexcept {
            while (position < text.size()) {
                const char c = text[position++];
                if (c == quote)
                    return position;
                if (c == '\n' || c == '\r')
                    return std::string_view::npos;
                if (c == '\\' && position < text.size())
                    ++position;
            }
            return std::string_view::npos;
        }

        /** @brief Matches one bounded Lua closing delimiter. */
        [[nodiscard]] bool LuaClose(const std::string_view text, const std::size_t cursor, const std::string_view delimiter) noexcept {
            const auto equals = delimiter.size() - 2;
            return text[cursor] == ']' && text.size() - cursor >= delimiter.size() &&
                   text.substr(cursor + 1, equals) == delimiter.substr(1, equals) && text[cursor + equals + 1] == ']';
        }

        /** @brief Finds the end of Lua long-bracket strings/comments, including arbitrary equal-sign levels. */
        [[nodiscard]] std::size_t LuaBracketEnd(const std::string_view text, const std::size_t begin) noexcept {
            auto cursor = begin + 1;
            while (cursor < text.size() && text[cursor] == '=')
                ++cursor;
            if (cursor >= text.size() || text[cursor] != '[')
                return begin;
            const auto equals = cursor - begin - 1;
            if (equals > 16)
                return std::string_view::npos;
            const auto delimiter = text.substr(begin, equals + 2);
            for (++cursor; cursor < text.size(); ++cursor) {
                if (LuaClose(text, cursor, delimiter))
                    return cursor + equals + 2;
            }
            return std::string_view::npos;
        }

        /** @brief Lexical scanner with one cursor and bounded append-only storage. */
        class Scanner {
        public:
            Scanner(const SourceLanguage language, const std::string_view text, const SourceSyntaxLimits limits)
                : language_(language), text_(text), limits_(limits) {}

            [[nodiscard]] SourceSyntax Run() {
                SourceSyntax result{.language = language_};
                while (cursor_ < text_.size()) {
                    const auto begin = cursor_;
                    const auto [end, role] = Next();
                    if (end == std::string_view::npos)
                        return {.language = language_, .fallback = SourceSyntaxFallback::Malformed};
                    cursor_ = end;
                    if (role == SourceSyntaxRole::Text)
                        continue;
                    if (result.spans.size() >= limits_.maximumSpans)
                        return {.language = language_, .fallback = SourceSyntaxFallback::BudgetExceeded};
                    result.spans.emplace_back(begin, end, role);
                }
                return result;
            }

        private:
            using Token = std::pair<std::size_t, SourceSyntaxRole>;

            /** @brief Recognizes native and Lua line/block comments without consuming string contents. */
            [[nodiscard]] std::optional<Token> Comment() const {
                const auto tail = text_.substr(cursor_);
                if (Native() && tail.starts_with("/*"))
                    return Token{DelimitedEnd("*/", cursor_ + 2), SourceSyntaxRole::Comment};
                if (language_ == SourceLanguage::Xml && tail.starts_with("<!--"))
                    return Token{DelimitedEnd("-->", cursor_ + 4), SourceSyntaxRole::Comment};
                if (const auto script = ScriptComment())
                    return script;
                if (Native() && tail.starts_with("//"))
                    return Token{LineEnd(), SourceSyntaxRole::Comment};
                if (ConfigComment())
                    return Token{LineEnd(), SourceSyntaxRole::Comment};
                return std::nullopt;
            }

            /** @brief Handles Lua long comments before the ordinary line-comment path. */
            [[nodiscard]] std::optional<Token> ScriptComment() const noexcept {
                const auto tail = text_.substr(cursor_);
                if (language_ != SourceLanguage::HoroScript || !tail.starts_with("--"))
                    return std::nullopt;
                if (tail.size() > 2 && tail[2] == '[') {
                    const auto end = LuaBracketEnd(text_, cursor_ + 2);
                    if (end != cursor_ + 2)
                        return Token{end, SourceSyntaxRole::Comment};
                }
                return Token{LineEnd(), SourceSyntaxRole::Comment};
            }

            /** @brief Classifies native lexical families once per dispatch. */
            [[nodiscard]] bool Native() const noexcept {
                return language_ == SourceLanguage::Cpp || language_ == SourceLanguage::Shader;
            }

            /** @brief Configuration hash comments are independent of native preprocessor directives. */
            [[nodiscard]] bool ConfigComment() const noexcept {
                using enum SourceLanguage;

                if (language_ == Ini && text_[cursor_] == ';')
                    return true;
                return text_[cursor_] == '#' && (language_ == Toml || language_ == Yaml || language_ == Ini || language_ == CMake);
            }

            /** @brief Scans bounded-delimiter native raw strings without treating embedded comments as syntax. */
            [[nodiscard]] Token RawString() const noexcept {
                const auto delimiterEnd = text_.find('(', cursor_ + 2);
                if (delimiterEnd == std::string_view::npos || delimiterEnd - cursor_ > 18)
                    return {std::string_view::npos, SourceSyntaxRole::Text};
                const auto delimiter = text_.substr(cursor_ + 2, delimiterEnd - cursor_ - 2);
                for (auto end = delimiterEnd + 1; end < text_.size(); ++end) {
                    if (text_[end] != ')' || text_.substr(end + 1, delimiter.size()) != delimiter)
                        continue;
                    const auto quote = end + delimiter.size() + 1;
                    if (quote < text_.size() && text_[quote] == '"')
                        return {quote + 1, SourceSyntaxRole::String};
                }
                return {std::string_view::npos, SourceSyntaxRole::Text};
            }

            /** @brief Resolves quoted values and JSON property names; unclosed input falls back as a whole. */
            [[nodiscard]] Token String() const noexcept {
                const char quote = text_[cursor_];
                if (quote == '\'' && language_ == SourceLanguage::Json)
                    return {std::string_view::npos, SourceSyntaxRole::Text};
                if (const auto tail = text_.substr(cursor_); TripleString(tail))
                    return {DelimitedEnd(tail.substr(0, 3), cursor_ + 3), SourceSyntaxRole::String};
                const auto end = QuotedEnd(text_, cursor_ + 1, quote);
                if (end == std::string_view::npos)
                    return {end, SourceSyntaxRole::Text};
                const auto next = text_.find_first_not_of(" \t\r", end);
                const bool property = language_ == SourceLanguage::Json && next != std::string_view::npos && text_[next] == ':';
                return {end, property ? SourceSyntaxRole::Property : SourceSyntaxRole::String};
            }

            /** @brief Finds identifier roles without treating language words inside strings/comments as syntax. */
            [[nodiscard]] Token Word() const noexcept {
                auto end = cursor_ + 1;
                while (end < text_.size() && (IdentifierStart(text_[end]) || Digit(text_[end])))
                    ++end;
                const auto next = text_.find_first_not_of(" \t", end);
                auto role = WordRole(language_, text_.substr(cursor_, end - cursor_));
                if (IsProperty(next))
                    role = SourceSyntaxRole::Property;
                if (XmlTag())
                    role = SourceSyntaxRole::Keyword;
                if (language_ == SourceLanguage::CMake && next != std::string_view::npos && text_[next] == '(')
                    role = SourceSyntaxRole::Keyword;
                return {end, role};
            }

            /** @brief Native XML tag syntax is independent of ordinary content words. */
            [[nodiscard]] bool XmlTag() const noexcept {
                return language_ == SourceLanguage::Xml && cursor_ > 0 && (text_[cursor_ - 1] == '<' || text_[cursor_ - 1] == '/');
            }

            /** @brief Recognizes multiline configuration quotes without changing lexical string ownership. */
            [[nodiscard]] bool TripleString(const std::string_view tail) const noexcept {
                const bool supported = language_ == SourceLanguage::Toml || language_ == SourceLanguage::Yaml;
                return supported && tail.size() >= 3 && tail[1] == tail[0] && tail[2] == tail[0];
            }

            /** @brief Recognizes unquoted configuration keys independently of their values. */
            [[nodiscard]] bool IsProperty(const std::size_t next) const noexcept {
                using enum SourceLanguage;

                if (next == std::string_view::npos)
                    return false;
                if (language_ == Yaml)
                    return text_[next] == ':';
                return text_[next] == '=' && (language_ == Ini || language_ == Toml);
            }

            /** @brief Recognizes long-bracket strings separately from ordinary array/index operators. */
            [[nodiscard]] std::optional<Token> LongString() const noexcept {
                if (language_ != SourceLanguage::HoroScript || text_[cursor_] != '[')
                    return std::nullopt;
                const auto end = LuaBracketEnd(text_, cursor_);
                if (end == cursor_)
                    return std::nullopt;
                return Token{end, SourceSyntaxRole::String};
            }

            /** @brief Dispatches one lexical unit; every ordinary byte advances without interpreting UTF-8 interiors. */
            [[nodiscard]] Token Next() const {
                if (const auto token = Comment())
                    return *token;
                if (const auto token = LongString())
                    return *token;
                const char c = text_[cursor_];
                if (const auto token = NativeSpecial())
                    return *token;
                if (c == '"' || c == '\'')
                    return String();
                if (Digit(c))
                    return Number();
                if (IdentifierStart(c))
                    return Word();
                return {cursor_ + 1, SourceSyntaxRole::Text};
            }

            /** @brief Handles native preprocessor and raw-string forms before ordinary identifiers. */
            [[nodiscard]] std::optional<Token> NativeSpecial() const noexcept {
                if (!Native())
                    return std::nullopt;
                if (text_[cursor_] == '#')
                    return Token{LineEnd(), SourceSyntaxRole::Directive};
                if (text_.substr(cursor_).starts_with("R\""))
                    return RawString();
                return std::nullopt;
            }

            /** @brief Consumes a numeric literal without absorbing adjacent arithmetic operators. */
            [[nodiscard]] Token Number() const noexcept {
                auto end = cursor_ + 1;
                while (end < text_.size() && NumericContinuation(end))
                    ++end;
                return {end, SourceSyntaxRole::Number};
            }

            /** @brief Retains numeric separators and exponent signs without absorbing arithmetic expressions. */
            [[nodiscard]] bool NumericContinuation(const std::size_t offset) const noexcept {
                const char c = text_[offset];
                if (IdentifierStart(c) || Digit(c) || c == '.')
                    return true;
                if (c == '\'' && Native())
                    return offset + 1 < text_.size() && Digit(text_[offset + 1]);
                if (c != '+' && c != '-')
                    return false;
                return WordIn("e E p P", text_.substr(offset - 1, 1));
            }

            [[nodiscard]] std::size_t LineEnd() const noexcept {
                const auto end = text_.find_first_of("\r\n", cursor_);
                return end == std::string_view::npos ? text_.size() : end;
            }

            [[nodiscard]] std::size_t DelimitedEnd(const std::string_view delimiter, const std::size_t start) const noexcept {
                const auto end = text_.find(delimiter, start);
                return end == std::string_view::npos ? end : end + delimiter.size();
            }

            SourceLanguage language_;
            std::string_view text_;
            SourceSyntaxLimits limits_;
            std::size_t cursor_{};
        };
    }  // namespace

    /** @copydoc SourceSyntax::RoleAt */
    SourceSyntaxRole SourceSyntax::RoleAt(const std::size_t offset) const noexcept {
        const auto found = std::ranges::upper_bound(spans, offset, {}, &SourceSyntaxSpan::begin);
        if (found == spans.begin())
            return SourceSyntaxRole::Text;
        const auto &span = *std::prev(found);
        return offset < span.end ? span.role : SourceSyntaxRole::Text;
    }

    /** @copydoc ClassifySourceLanguage */
    SourceLanguage ClassifySourceLanguage(const std::string_view path) noexcept {
        using enum SourceLanguage;

        const auto separator = path.find_last_of("/\\");
        const auto name = path.substr(separator == std::string_view::npos ? 0 : separator + 1);
        if (name == "CMakeLists.txt")
            return CMake;
        const auto dot = name.rfind('.');
        if (dot == std::string_view::npos || name.size() - dot > 16)
            return PlainText;
        std::array<char, 16> extension{};
        const auto size = name.size() - dot;
        for (std::size_t index = 0; index < size; ++index) {
            const char c = name[dot + index];
            extension[index] = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        }
        const std::string_view ext{extension.data(), size};
        constexpr std::array extensions{
            std::pair{".c .cc .cpp .cxx .h .hh .hpp .hxx .inl .ipp .tpp", Cpp},
            std::pair{".horo_script .lua", HoroScript},
            std::pair{".glsl .hlsl .metal .shader .vert .frag .geom .comp .tesc .tese", Shader},
            std::pair{".json", Json},
            std::pair{".toml", Toml},
            std::pair{".yaml .yml", Yaml},
            std::pair{".ini .cfg", Ini},
            std::pair{".xml", Xml},
            std::pair{".cmake", CMake},
        };
        for (const auto &[suffixes, language] : extensions) {
            if (WordIn(suffixes, ext))
                return language;
        }
        return PlainText;
    }

    /** @copydoc TokenizeSource */
    SourceSyntax TokenizeSource(const SourceLanguage language, const std::string_view text, const SourceSyntaxLimits limits) {
        using enum SourceSyntaxFallback;

        if (language == SourceLanguage::PlainText)
            return {.language = language, .fallback = UnknownLanguage};
        if (text.size() > limits.maximumBytes)
            return {.language = language, .fallback = BudgetExceeded};
        std::size_t lineBytes = 0;
        for (const char c : text) {
            lineBytes = c == '\n' ? 0 : lineBytes + 1;
            if (lineBytes > limits.maximumLineBytes)
                return {.language = language, .fallback = BudgetExceeded};
        }
        return Scanner{language, text, limits}.Run();
    }

    /** @copydoc PlanSourceLanguageNewline */
    Result<SourceEditingPlan> PlanSourceLanguageNewline(const SourceDocumentSnapshot &source, const SourceEditingView &view) {
        auto plain = PlanSourceNewline(source, view);
        if (plain.HasError())
            return plain;
        const auto language = ClassifySourceLanguage(source.Identity().key.source.Value());
        if (language == SourceLanguage::PlainText)
            return plain;
        const auto caret = std::min(view.anchor, view.caret);
        const auto prefix = source.Text().substr(0, caret);
        const auto lineBreak = prefix.find_last_of("\r\n");
        const auto lineStart = lineBreak == std::string_view::npos ? 0 : lineBreak + 1;
        const auto last = prefix.find_last_not_of(" \t\r\n");
        if (last == std::string_view::npos || last < lineStart)
            return plain;
        const auto syntax = TokenizeSource(language, prefix);
        if (syntax.fallback != SourceSyntaxFallback::None || syntax.RoleAt(last) == SourceSyntaxRole::Comment ||
            syntax.RoleAt(last) == SourceSyntaxRole::String)
            return plain;
        bool opensBlock = prefix[last] == '{' || prefix[last] == '[' || prefix[last] == '(';
        if (language == SourceLanguage::HoroScript) {
            for (const auto &span : syntax.spans) {
                if (span.begin < lineStart || span.role != SourceSyntaxRole::Keyword)
                    continue;
                const auto token = prefix.substr(span.begin, span.end - span.begin);
                if (token == "function" || token == "then" || token == "do" || token == "repeat" || token == "else")
                    opensBlock = true;
            }
        }
        if (!opensBlock)
            return plain;
        auto insertion = plain.Value().insert + "    ";
        const auto innerEnd = caret + insertion.size();
        const auto tail = source.Text().substr(std::max(view.anchor, view.caret));
        const auto next = tail.find_first_not_of(" \t");
        if (next != std::string_view::npos && (tail[next] == '}' || tail[next] == ']' || tail[next] == ')'))
            insertion += plain.Value().insert;
        auto plan = PlanSourceReplacement(source, view, insertion);
        if (plan.HasError())
            return plan;
        auto completed = std::move(plan).Value();
        completed.after.anchor = completed.after.caret = innerEnd;
        return Result<SourceEditingPlan>::Success(std::move(completed));
    }
}  // namespace Horo::Editor
