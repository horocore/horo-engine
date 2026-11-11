#include "ScriptModuleReferenceService.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <limits>

namespace Horo::Extensions {
    namespace {
        ScriptCallResult Failure(std::string code, std::string message) {
            return ScriptCallResult::Failure({.domain = "horo.reference", .code = std::move(code), .message = std::move(message)});
        }

        ScriptCallResult Sum(std::span<const ScriptValue> arguments) {
            const auto a = *arguments[0].AsSignedInteger();
            const auto b = arguments.size() > 1 ? *arguments[1].AsSignedInteger() : std::int64_t{1};
            if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) || (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
                return Failure("horo.reference.range", "Signed sum exceeds int64.");
            return ScriptCallResult::Success({ScriptValue{a + b}});
        }

        Result<ScriptCallResult> Digest(std::span<const std::byte> bytes, const BackendServiceCallContext &context) {
            std::uint64_t value = 14695981039346656037ULL;
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                if (index % 256 == 0 && context.IsCancellationRequested())
                    return Result<ScriptCallResult>::Failure(MakeError(ExtensionErrors::ScriptInvocationCancelled));
                value = (value ^ std::to_integer<std::uint8_t>(bytes[index])) * 1099511628211ULL;
            }
            return Result<ScriptCallResult>::Success(ScriptCallResult::Success({ScriptValue{value}}));
        }
    }  // namespace

    Result<void> ScriptModuleReferenceService::Validate(const ScriptInvocationRequest &request) const {
        if (!descriptors_ || !request.descriptors || request.descriptors->Fingerprint() != descriptors_->Fingerprint())
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationProviderRevoked));
        const auto *api = descriptors_->Find(request.apiId);
        if (!api)
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        const auto function = std::ranges::find(api->functions, request.functionId, &ScriptExportFunctionDescriptor::id);
        if (function == api->functions.end())
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        auto valid = ValidateScriptArguments(*function, request.arguments, *api);
        return valid;
    }

    Result<ScriptCallResult> ScriptModuleReferenceService::Invoke(const ScriptInvocationRequest &request,
                                                                  const BackendServiceCallContext &context) const {
        using Output = Result<ScriptCallResult>;
        if (closed_.load(std::memory_order_acquire) || context.IsCancellationRequested())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationCancelled));
        if (auto valid = Validate(request); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        if (request.functionId == "echo")
            return Output::Success(ScriptCallResult::Success(request.arguments));
        if (request.functionId == "sum")
            return Output::Success(Sum(request.arguments));
        if (request.functionId == "digest")
            return Digest(request.arguments[0].AsBytes(), context);
        if (request.functionId == "apirevision")
            return Output::Success(ScriptCallResult::Success({ScriptValue{std::string{"1.1.0"}}}));
        return Output::Success(Failure("horo.reference.rejected", std::string{request.arguments[0].AsString()}));
    }
}  // namespace Horo::Extensions
