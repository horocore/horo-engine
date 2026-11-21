#include "LspLifecycle.h"

#include <new>

namespace Horo::Editor::Language {
    namespace {
        constexpr std::int32_t InitializeId = -1;  // Document request IDs are positive.
        constexpr std::int32_t ShutdownId = -2;
        const ErrorDomainId LifecycleDomain{"editor.language"};
        const ErrorCodeDescriptor InvalidState{LifecycleDomain,
                                               ErrorCode{"editor.language.lifecycle_state"},
                                               ErrorSeverity::Error,
                                               "Language server lifecycle transition is unavailable.",
                                               {}};
        const ErrorCodeDescriptor HandshakeFailure{LifecycleDomain,
                                                   ErrorCode{"editor.language.handshake"},
                                                   ErrorSeverity::Error,
                                                   "Language server returned an invalid handshake response.",
                                                   {}};
        const ErrorCodeDescriptor HandshakeTimeout{LifecycleDomain,
                                                   ErrorCode{"editor.language.handshake_timeout"},
                                                   ErrorSeverity::Error,
                                                   "Language server did not finish the handshake in time.",
                                                   {}};
        const ErrorCodeDescriptor InvalidParameters{LifecycleDomain,
                                                    ErrorCode{"editor.language.lifecycle_parameters"},
                                                    ErrorSeverity::Error,
                                                    "Language server startup parameters are invalid or exceed their size limit.",
                                                    {}};

        /** @brief Encodes an internally constructed bounded lifecycle notification or request. */
        std::string Frame(const nlohmann::json &message) {
            auto body = message.dump();
            return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        }

        /** @brief Uses an integer reserved by the opening or closing handshake, never a feature request ID. */
        bool IsResponseTo(const LspMessage &message, const std::int32_t expectedId) {
            if (message.kind != LspMessageKind::Result && message.kind != LspMessageKind::Error)
                return false;
            const auto &id = message.envelope.Get().at("id");
            return id.is_number_integer() && !id.is_number_unsigned() && id.get<std::int64_t>() == expectedId;
        }

        /** @brief Checks the minimum server capabilities and the negotiated position encoding. */
        bool ValidInitializeResult(const nlohmann::json &result) {
            const auto capabilities = result.find("capabilities");
            if (capabilities == result.end() || !capabilities->is_object())
                return false;
            const auto encoding = capabilities->find("positionEncoding");
            return encoding == capabilities->end() || *encoding == "utf-16";
        }

        /** @brief Produces one handshake response before the caller observes the next state. */
        Result<LspLifecycleDispatch> SendNotification(const std::string_view method, const bool emptyParams) {
            try {
                nlohmann::json message{{"jsonrpc", "2.0"}, {"method", method}};
                if (emptyParams)
                    message["params"] = nlohmann::json::object();
                return Result<LspLifecycleDispatch>::Success({true, Frame(message)});
            } catch (const std::bad_alloc &) {
                return Result<LspLifecycleDispatch>::Failure(MakeError(InvalidParameters));
            }
        }

        /** @brief Declines the one startup prompt a server may issue before feature dispatch is ready. */
        Result<LspLifecycleDispatch> DeclineStartupPrompt(const LspMessage &message) {
            try {
                const auto &id = message.envelope.Get().at("id");
                return Result<LspLifecycleDispatch>::Success({true, Frame({{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}})});
            } catch (const std::bad_alloc &) {
                return Result<LspLifecycleDispatch>::Failure(MakeError(InvalidParameters));
            }
        }
    }  // namespace

    /** @copydoc LspLifecycle::Start */
    Result<std::string> LspLifecycle::Start(const std::string_view rootUri, const std::optional<std::int32_t> processId,
                                            const Clock::time_point now) {
        if (state_ != LspLifecycleState::Idle)
            return Result<std::string>::Failure(MakeError(InvalidState));
        if (rootUri.size() > MaximumRootUriBytes || rootUri.find('\0') != std::string_view::npos || (processId && *processId <= 0))
            return Result<std::string>::Failure(MakeError(InvalidParameters));
        try {
            const nlohmann::json params{{"processId", processId ? nlohmann::json(*processId) : nlohmann::json(nullptr)},
                                        {"rootUri", rootUri.empty() ? nlohmann::json(nullptr) : nlohmann::json(rootUri)},
                                        {"capabilities", {{"general", {{"positionEncodings", nlohmann::json::array({"utf-16"})}}}}}};
            auto frame = Frame({{"jsonrpc", "2.0"}, {"id", InitializeId}, {"method", "initialize"}, {"params", params}});
            state_ = LspLifecycleState::Initializing;
            deadline_ = now + InitializeTimeout;
            return Result<std::string>::Success(std::move(frame));
        } catch (const nlohmann::json::exception &) {
            return Result<std::string>::Failure(MakeError(InvalidParameters));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(InvalidParameters));
        }
    }

    /** @copydoc LspLifecycle::Shutdown */
    Result<std::string> LspLifecycle::Shutdown(const Clock::time_point now) {
        if (state_ != LspLifecycleState::Ready)
            return Result<std::string>::Failure(MakeError(InvalidState));
        try {
            auto frame = Frame({{"jsonrpc", "2.0"}, {"id", ShutdownId}, {"method", "shutdown"}});
            state_ = LspLifecycleState::ShuttingDown;
            deadline_ = now + ShutdownTimeout;
            return Result<std::string>::Success(std::move(frame));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(InvalidParameters));
        }
    }

    /** @copydoc LspLifecycle::Receive */
    Result<LspLifecycleDispatch> LspLifecycle::Receive(const LspMessage &message, const Clock::time_point now) {
        if (Expire(now))
            return Result<LspLifecycleDispatch>::Failure(MakeError(HandshakeTimeout));
        if (state_ == LspLifecycleState::Initializing && message.kind == LspMessageKind::Request &&
            message.envelope.Get().at("method") == "window/showMessageRequest")
            return DeclineStartupPrompt(message);
        if (IsResponseTo(message, InitializeId)) {
            if (state_ == LspLifecycleState::Initializing)
                return HandleInitialize(message);
            return Result<LspLifecycleDispatch>::Success({true, std::nullopt});
        }
        if (IsResponseTo(message, ShutdownId)) {
            if (state_ == LspLifecycleState::ShuttingDown)
                return HandleShutdown(message, now);
            return Result<LspLifecycleDispatch>::Success({true, std::nullopt});
        }
        return Result<LspLifecycleDispatch>::Success({false, std::nullopt});
    }

    /** @copydoc LspLifecycle::HandleInitialize */
    Result<LspLifecycleDispatch> LspLifecycle::HandleInitialize(const LspMessage &message) {
        deadline_.reset();
        if (message.kind != LspMessageKind::Result || !ValidInitializeResult(message.envelope.Get().at("result"))) {
            state_ = LspLifecycleState::Failed;
            return Result<LspLifecycleDispatch>::Failure(MakeError(HandshakeFailure));
        }
        auto response = SendNotification("initialized", true);
        state_ = response.HasValue() ? LspLifecycleState::Ready : LspLifecycleState::Failed;
        return response;
    }

    /** @copydoc LspLifecycle::HandleShutdown */
    Result<LspLifecycleDispatch> LspLifecycle::HandleShutdown(const LspMessage &message, const Clock::time_point now) {
        deadline_.reset();
        if (message.kind != LspMessageKind::Result || !message.envelope.Get().at("result").is_null()) {
            state_ = LspLifecycleState::Failed;
            return Result<LspLifecycleDispatch>::Failure(MakeError(HandshakeFailure));
        }
        auto response = SendNotification("exit", false);
        state_ = response.HasValue() ? LspLifecycleState::Exiting : LspLifecycleState::Failed;
        if (response.HasValue())
            deadline_ = now + ShutdownTimeout;
        return response;
    }

    /** @copydoc LspLifecycle::Expire */
    bool LspLifecycle::Expire(const Clock::time_point now) noexcept {
        if (!deadline_ || now < *deadline_)
            return false;
        deadline_.reset();
        state_ = LspLifecycleState::Failed;
        return true;
    }

    /** @copydoc LspLifecycle::TransportStopped */
    void LspLifecycle::TransportStopped(const bool cleanExit) noexcept {
        deadline_.reset();
        if (state_ == LspLifecycleState::Stopped)
            return;
        state_ = cleanExit && state_ == LspLifecycleState::Exiting ? LspLifecycleState::Stopped : LspLifecycleState::Failed;
    }
}  // namespace Horo::Editor::Language
