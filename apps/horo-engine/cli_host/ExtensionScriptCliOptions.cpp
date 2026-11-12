#include "ExtensionScriptCliInternal.h"
#include "Horo/Cli/CliErrors.h"

namespace Horo::Application::Internal {
    namespace {
        template <typename T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(Cli::CliErrors::ParseFailed));
        }

        const Cli::CliResolvedValue *Option(const Cli::CliCommandRequest &request, const std::string_view name) {
            const auto found = std::ranges::find(request.options, name, &Cli::CliParsedOption::name);
            return found != request.options.end() && found->values.size() == 1 ? &found->values.front() : nullptr;
        }

        Result<std::string> String(const Cli::CliCommandRequest &request, const std::string_view name) {
            const auto *value = Option(request, name);
            const auto *text = value ? std::get_if<std::string>(&value->value) : nullptr;
            if (!text || text->empty() || text->size() > 256)
                return Invalid<std::string>();
            return Result<std::string>::Success(*text);
        }

        Result<std::filesystem::path> Path(const Cli::CliCommandRequest &request, const std::string_view name, const bool directory) {
            const auto *value = Option(request, name);
            const auto *path = value ? std::get_if<Cli::CliPathValue>(&value->value) : nullptr;
            if (!path || path->value.empty())
                return Invalid<std::filesystem::path>();
            const std::filesystem::path input{std::u8string{path->value.begin(), path->value.end()}};
            std::error_code error;
            const auto canonical = std::filesystem::canonical(input, error);
            if (error || !input.is_absolute() || std::filesystem::is_directory(canonical) != directory)
                return Invalid<std::filesystem::path>();
            return Result<std::filesystem::path>::Success(canonical);
        }

        bool Approved(const Cli::CliCommandRequest &request, const std::string_view name) {
            const auto *value = Option(request, name);
            const auto *flag = value ? std::get_if<bool>(&value->value) : nullptr;
            return flag && *flag;
        }

        bool ReferenceUpdate(const ExtensionScriptOptions &options, const Cli::CliCommandRequest &request) {
            return options.moduleOperation && Approved(request, "refresh-reference");
        }

        bool Consented(const Cli::CliCommandRequest &request) {
            return Approved(request, "approve-native") && Approved(request, "approve-script");
        }

        Result<void> RuntimeSelection(ExtensionScriptOptions &options, const Cli::CliCommandRequest &request) {
            if (!options.moduleOperation)
                return Result<void>::Success();
            auto runtime = String(request, "runtime");
            if (runtime.HasError())
                return Invalid<void>();
            if (runtime.Value() == "javascript")
                options.runtime = Extensions::ScriptModuleRuntimeKind::JavaScriptNg;
            else if (runtime.Value() != "lua")
                return Invalid<void>();
            return Result<void>::Success();
        }

        Result<void> Identities(ExtensionScriptOptions &options, const Cli::CliCommandRequest &request) {
            auto package = String(request, "extension-id");
            auto moduleId = String(request, "module");
            auto importId = options.moduleOperation ? Result<std::string>::Success({}) : String(request, "import");
            auto publisher = String(request, "publisher");
            if (package.HasError() || moduleId.HasError() || importId.HasError() || publisher.HasError())
                return Invalid<void>();
            options.package = std::move(package).Value();
            options.moduleId = std::move(moduleId).Value();
            options.importId = std::move(importId).Value();
            options.publisher = std::move(publisher).Value();
            return Result<void>::Success();
        }
    }  // namespace

    Result<ExtensionScriptOptions> ParseExtensionScriptOptions(const Cli::CliCommandRequest &request) {
        if (!Consented(request))
            return Invalid<ExtensionScriptOptions>();
        auto package = Path(request, "extension-root", true);
        auto trust = Path(request, "host-trust", false);
        auto temporary = Path(request, "temporary-root", true);
        auto script = String(request, "script");
        if (package.HasError() || trust.HasError() || temporary.HasError() || script.HasError())
            return Invalid<ExtensionScriptOptions>();
        ExtensionScriptOptions options{std::move(package).Value(), std::move(trust).Value(), std::move(temporary).Value(), script.Value()};
        options.moduleOperation = request.command.segments == std::vector<std::string>{"extension", "module-run"};
        options.refreshReference = ReferenceUpdate(options, request);
        if (auto runtime = RuntimeSelection(options, request); runtime.HasError())
            return Result<ExtensionScriptOptions>::Failure(runtime.ErrorValue());
        const auto extension = options.runtime == Extensions::ScriptModuleRuntimeKind::Lua54 ? ".lua" : ".js";
        if (options.script.extension() != extension)
            return Invalid<ExtensionScriptOptions>();
        if (auto ids = Identities(options, request); ids.HasError())
            return Result<ExtensionScriptOptions>::Failure(ids.ErrorValue());
        return Result<ExtensionScriptOptions>::Success(std::move(options));
    }
}  // namespace Horo::Application::Internal
