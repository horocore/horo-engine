#include "LspProtocol.h"

#include "OwnedJsonReader.h"

#include <algorithm>
#include <charconv>
#include <new>
#include <utility>

namespace Horo::Editor::Language {
    namespace {
        constexpr std::string_view HeaderEnd = "\r\n\r\n";
        constexpr std::size_t MaximumLspJsonDepth = 64;
        const ErrorDomainId LanguageDomain{"editor.language"};
        const ErrorCodeDescriptor InvalidStream{LanguageDomain,
                                                ErrorCode{"editor.language.invalid_stream"},
                                                ErrorSeverity::Error,
                                                "Invalid or truncated language server stream.",
                                                {}};
        const ErrorCodeDescriptor InvalidMessage{LanguageDomain,
                                                 ErrorCode{"editor.language.invalid_message"},
                                                 ErrorSeverity::Error,
                                                 "Invalid language server message.",
                                                 {}};
        const ErrorCodeDescriptor MessageLimit{LanguageDomain,
                                               ErrorCode{"editor.language.message_limit"},
                                               ErrorSeverity::Error,
                                               "Language server message exceeds available storage or framing limits.",
                                               {}};

        /** @brief Folds ASCII header field names without locale-dependent behavior. */
        char LowerAscii(const char value) noexcept {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
        }

        /** @brief Compares a wire token with its canonical lowercase spelling. */
        bool EqualHeaderToken(const std::string_view wire, const std::string_view lowercase) noexcept {
            return std::ranges::equal(wire, lowercase, {}, LowerAscii);
        }

        /** @brief Recognizes HTTP-style header names, including legal extension fields. */
        bool HeaderNameByte(const char value) noexcept {
            const auto lower = LowerAscii(value);
            return (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') ||
                   std::string_view{"!#$%&'*+-.^_`|~"}.find(value) != std::string_view::npos;
        }

        /** @brief Allows printable ASCII and horizontal tabs inside a header value. */
        bool HeaderValueByte(const char value) noexcept {
            return value == '\t' || (value >= ' ' && value <= '~');
        }

        /** @brief Strips horizontal whitespace around a header value. */
        std::string_view TrimHeaderValue(std::string_view value) noexcept {
            const auto first = value.find_first_not_of(" \t");
            if (first == std::string_view::npos)
                return {};
            return value.substr(first, value.find_last_not_of(" \t") - first + 1);
        }

        /** @brief Accepts UTF-8 and the historic LSP utf8 spelling in the optional MIME field. */
        bool SupportedContentType(const std::string_view value) noexcept {
            const auto separator = value.find(';');
            const auto mime = TrimHeaderValue(value.substr(0, separator));
            if (!EqualHeaderToken(mime, "application/vscode-jsonrpc") && !EqualHeaderToken(mime, "application/json"))
                return false;
            if (separator == std::string_view::npos)
                return true;
            const auto parameter = value.substr(separator + 1);
            const auto equal = parameter.find('=');
            if (equal == std::string_view::npos || !EqualHeaderToken(TrimHeaderValue(parameter.substr(0, equal)), "charset"))
                return false;
            auto charset = TrimHeaderValue(parameter.substr(equal + 1));
            if (charset.size() >= 2 && charset.front() == '"' && charset.back() == '"')
                charset = charset.substr(1, charset.size() - 2);
            return EqualHeaderToken(charset, "utf-8") || EqualHeaderToken(charset, "utf8");
        }

        /** @brief Parses a positive bounded decimal byte length without overflow or trailing tokens. */
        std::optional<std::size_t> ParseLength(const std::string_view value) noexcept {
            if (value.empty())
                return std::nullopt;
            std::size_t bytes{};
            const auto end = value.data() + value.size();
            const auto result = std::from_chars(value.data(), end, bytes);
            if (result.ec != std::errc{} || result.ptr != end || bytes == 0 || bytes > MaximumLspBodyBytes)
                return std::nullopt;
            return bytes;
        }

        /** @brief A validated header field borrowing the buffered ASCII line. */
        struct HeaderField final {
            std::string_view name;
            std::string_view value;
        };

        /** @brief Splits one header field and rejects invalid names or control bytes. */
        std::optional<HeaderField> ParseHeaderField(const std::string_view line) noexcept {
            const auto separator = line.find(':');
            if (separator == std::string_view::npos || separator == 0)
                return std::nullopt;
            const HeaderField field{line.substr(0, separator), line.substr(separator + 1)};
            if (!std::ranges::all_of(field.name, HeaderNameByte) || !std::ranges::all_of(field.value, HeaderValueByte))
                return std::nullopt;
            return field;
        }

        /** @brief Reads one header block and rejects ambiguous singleton fields. */
        std::optional<std::size_t> ReadContentLength(std::string_view block) noexcept {
            std::optional<std::size_t> bodyLength;
            bool hasContentType = false;
            while (!block.empty()) {
                const auto lineEnd = block.find("\r\n");
                const auto field = ParseHeaderField(block.substr(0, lineEnd));
                if (!field)
                    return std::nullopt;
                if (EqualHeaderToken(field->name, "content-length")) {
                    if (bodyLength)
                        return std::nullopt;
                    bodyLength = ParseLength(TrimHeaderValue(field->value));
                    if (!bodyLength)
                        return std::nullopt;
                } else if (EqualHeaderToken(field->name, "content-type")) {
                    if (hasContentType || !SupportedContentType(field->value))
                        return std::nullopt;
                    hasContentType = true;
                }
                block.remove_prefix(lineEnd == std::string_view::npos ? block.size() : lineEnd + 2);
            }
            return bodyLength;
        }

        /** @brief Validates an optional JSON-RPC ID on a method-bearing message. */
        bool ValidId(const nlohmann::json &id) noexcept {
            return id.is_string() || id.is_number_integer();
        }

        /** @brief Classifies a request or notification after validating its optional structured parameters. */
        std::optional<LspMessageKind> ClassifyCall(const nlohmann::json &message, const nlohmann::json &method) {
            if (!method.is_string() || method.get_ref<const std::string &>().empty() || message.contains("result") ||
                message.contains("error"))
                return std::nullopt;
            const auto params = message.find("params");
            if (params != message.end() && !params->is_array() && !params->is_object())
                return std::nullopt;
            const auto id = message.find("id");
            if (id == message.end())
                return LspMessageKind::Notification;
            if (!ValidId(*id))
                return std::nullopt;
            return LspMessageKind::Request;
        }

        /** @brief Validates the required fields of a JSON-RPC error response. */
        bool ValidError(const nlohmann::json &error) {
            if (!error.is_object())
                return false;
            const auto code = error.find("code");
            const auto message = error.find("message");
            return code != error.end() && code->is_number_integer() && message != error.end() && message->is_string();
        }

        /** @brief Classifies a response, including the null ID used for uncorrelated protocol errors. */
        std::optional<LspMessageKind> ClassifyResponse(const nlohmann::json &message) {
            const auto id = message.find("id");
            const auto error = message.find("error");
            if (id == message.end() || message.contains("params"))
                return std::nullopt;
            if (error == message.end()) {
                if (ValidId(*id) && message.contains("result"))
                    return LspMessageKind::Result;
            } else if (!message.contains("result") && (ValidId(*id) || id->is_null()) && ValidError(*error)) {
                return LspMessageKind::Error;
            }
            return std::nullopt;
        }

        /** @brief Validates the JSON-RPC 2.0 object envelope before permitting payload dispatch. */
        std::optional<LspMessageKind> Classify(const nlohmann::json &message) {
            if (!message.is_object())
                return std::nullopt;
            const auto version = message.find("jsonrpc");
            if (version == message.end() || *version != "2.0")
                return std::nullopt;
            const auto method = message.find("method");
            return method == message.end() ? ClassifyResponse(message) : ClassifyCall(message, *method);
        }
    }  // namespace

    /** @copydoc LspFrameReader::ReadHeader */
    bool LspFrameReader::ReadHeader(std::string_view &input) {
        while (!input.empty() && header_.size() < MaximumLspHeaderBytes) {
            header_.push_back(input.front());
            input.remove_prefix(1);
            if (!header_.ends_with(HeaderEnd))
                continue;
            const auto length = ReadContentLength(std::string_view{header_}.substr(0, header_.size() - HeaderEnd.size()));
            if (!length)
                return false;
            bodyLength_ = *length;
            header_.clear();
            return true;
        }
        return header_.size() < MaximumLspHeaderBytes;
    }

    /** @copydoc LspFrameReader::Consume */
    Result<std::optional<std::string>> LspFrameReader::Consume(std::string_view &input) {
        using ReadResult = Result<std::optional<std::string>>;
        try {
            if (failed_ || (bodyLength_ == 0 && !ReadHeader(input))) {
                failed_ = true;
                return ReadResult::Failure(MakeError(InvalidStream));
            }
            if (bodyLength_ == 0)
                return ReadResult::Success(std::nullopt);
            const auto count = std::min(input.size(), bodyLength_ - body_.size());
            body_.append(input.substr(0, count));
            input.remove_prefix(count);
            if (body_.size() != bodyLength_)
                return ReadResult::Success(std::nullopt);
            bodyLength_ = 0;
            return ReadResult::Success(std::exchange(body_, {}));
        } catch (const std::bad_alloc &) {
            failed_ = true;
            return ReadResult::Failure(MakeError(MessageLimit));
        }
    }

    /** @copydoc LspFrameReader::EndOfStream */
    Result<void> LspFrameReader::EndOfStream() {
        if (failed_ || bodyLength_ != 0 || !header_.empty()) {
            failed_ = true;
            return Result<void>::Failure(MakeError(InvalidStream));
        }
        return Result<void>::Success();
    }

    /** @copydoc DecodeLspMessage */
    Result<LspMessage> DecodeLspMessage(const std::string_view body) {
        if (body.size() > MaximumLspBodyBytes)
            return Result<LspMessage>::Failure(MakeError(MessageLimit));
        if (body.find('\0') != std::string_view::npos)
            return Result<LspMessage>::Failure(MakeError(InvalidMessage));
        try {
            auto envelope = JsonEncoding::Detail::ReadOwnedJson(body, MaximumLspBodyBytes, MaximumLspJsonDepth);
            if (envelope) {
                const auto kind = Classify(envelope->Get());
                if (kind)
                    return Result<LspMessage>::Success({*kind, std::move(*envelope)});
            }
            return Result<LspMessage>::Failure(MakeError(InvalidMessage));
        } catch (const std::bad_alloc &) {
            return Result<LspMessage>::Failure(MakeError(MessageLimit));
        }
    }

    /** @copydoc EncodeLspMessage */
    Result<std::string> EncodeLspMessage(const nlohmann::json &message) {
        try {
            const auto body = message.dump();
            if (body.size() > MaximumLspBodyBytes)
                return Result<std::string>::Failure(MakeError(MessageLimit));
            auto frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            frame += body;
            return Result<std::string>::Success(std::move(frame));
        } catch (const nlohmann::json::exception &) {
            return Result<std::string>::Failure(MakeError(InvalidMessage));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(MessageLimit));
        }
    }
}  // namespace Horo::Editor::Language
