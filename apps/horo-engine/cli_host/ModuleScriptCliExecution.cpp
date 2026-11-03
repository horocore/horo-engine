#include "ExtensionScriptCliInternal.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "ScriptModuleHostServices.h"

#include <algorithm>
#include <thread>

namespace Horo::Application::Internal {
    namespace {
        Error Failure(const Extensions::ScriptError &error) {
            Error cause{.code = ErrorCode{error.code}, .domain = ErrorDomainId{error.domain}, .message = error.message};
            return WrapError(error.cancelled ? Extensions::ExtensionErrors::ScriptInvocationCancelled
                                             : Extensions::ExtensionErrors::InvocationFailed,
                             std::move(cause), "Module program returned a structured failed outcome.");
        }

        std::string Hex(std::span<const std::byte> bytes) {
            constexpr char alphabet[] = "0123456789abcdef";
            std::string text(bytes.size() * 2, '0');
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                const auto byte = std::to_integer<unsigned>(bytes[index]);
                text[index * 2] = alphabet[byte >> 4];
                text[index * 2 + 1] = alphabet[byte & 15];
            }
            return text;
        }

        Result<Cli::CliCommandResult> Present(const Extensions::ScriptModuleProgram &program, const Extensions::ScriptCallResult &result) {
            using Output = Result<Cli::CliCommandResult>;
            if (result.error)
                return Output::Failure(Failure(*result.error));
            auto encoded = Extensions::EncodeScriptCallResult(result);
            if (encoded.HasError())
                return Output::Failure(encoded.ErrorValue());
            constexpr std::size_t bytesPerChunk = 16U * 1024U;
            const auto &bytes = encoded.Value();
            const auto count = (bytes.size() + bytesPerChunk - 1) / bytesPerChunk;
            Cli::CliCommandResult output{
                .fields = {{"codec", std::string{"horo.script.call-result.v1"}},
                           {"extension", program.Manifest().id},
                           {"runtime", std::string{program.Runtime() == Extensions::ScriptModuleRuntimeKind::Lua54 ? "lua" : "javascript"}},
                           {"chunk-count", static_cast<std::int64_t>(count)}}};
            output.fields.reserve(output.fields.size() + count);
            for (std::size_t index = 0; index < count; ++index) {
                const auto part =
                    std::span{bytes}.subspan(index * bytesPerChunk, std::min(bytesPerChunk, bytes.size() - index * bytesPerChunk));
                output.fields.push_back({"result-" + std::to_string(index), Hex(part)});
            }
            return Output::Success(std::move(output));
        }
    }  // namespace

    Result<Cli::CliCommandResult> ExecuteCapturedModuleProgram(Extensions::ScriptModuleHostServices &host,
                                                               std::shared_ptr<const Extensions::ScriptModuleProgram> program,
                                                               Extensions::ScriptCapabilityScopeLease project,
                                                               Extensions::ScriptCapabilityScopeLease operation,
                                                               Cli::CliExecutionContext &context, bool refreshReference) {
        using Output = Result<Cli::CliCommandResult>;
        auto capabilities = Extensions::ScriptModuleRequestedCapabilities(*program);
        Extensions::ScriptModuleOperationPolicy policy{.projectCapabilities = capabilities,
                                                       .contextCapabilities = std::move(capabilities),
                                                       .scopes = {std::move(project), std::move(operation)},
                                                       .nativeConsent = true,
                                                       .scriptConsent = true};
        auto running = host.Begin(program, std::move(policy), context.Cancellation());
        if (running.HasError())
            return Output::Failure(running.ErrorValue());
        auto owner = std::move(running).Value();
        if (refreshReference) {
            auto updated = host.RefreshReference({1, 1, 0});
            if (updated.HasError())
                return Output::Failure(updated.ErrorValue());
        }
        while (!owner->ResultSnapshot()) {
            if (context.IsStopRequested()) {
                owner->Cancel();
                return Output::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
            }
            auto advanced = owner->Poll();
            if (advanced.HasError())
                return Output::Failure(advanced.ErrorValue());
            std::this_thread::yield();
        }
        auto result = owner->ResultSnapshot();
        owner->Cancel();
        return Present(*program, *result);
    }
}  // namespace Horo::Application::Internal
