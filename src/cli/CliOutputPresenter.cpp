#include "Horo/Cli/CliOutputPresenter.h"

#include "Horo/Cli/CliErrors.h"
#include "Horo/Foundation/Utf8.h"

#include <cmath>
#include <format>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace Horo::Cli {
    namespace {
        using Json = nlohmann::json;

        /** @brief Returns a canonical dotted command identity without interpreting application data. */
        std::string CommandName(const CommandPath &path) {
            std::string name;
            for (const auto &segment : path.segments) {
                if (!name.empty())
                    name += '.';
                name += segment;
            }
            return name;
        }

        /** @brief Removes terminal control bytes from human text. */
        std::string HumanText(std::string text) {
            for (char &byte : text)
                if (static_cast<unsigned char>(byte) < 32U || static_cast<unsigned char>(byte) == 127U)
                    byte = ' ';
            return text;
        }

        /** @brief Admits bounded scalar values independent of field identity. */
        bool ValidValue(const CliResultValue &value) {
            if (const auto *number = std::get_if<double>(&value))
                return std::isfinite(*number);
            if (const auto *text = std::get_if<std::string>(&value))
                return text->size() <= 65536 && IsValidUtf8ScalarSequence(*text);
            return true;
        }

        /** @brief Claims each bounded UTF-8 identity once within its record collection. */
        bool ClaimName(const std::string_view name, std::set<std::string_view, std::less<>> &names) {
            return !name.empty() && name.size() <= 128 && IsValidUtf8ScalarSequence(name) && names.insert(name).second;
        }

        /** @brief Validates flat typed result fields before any output is committed. */
        bool ValidFields(const CliCommandResult &result) {
            if (result.fields.size() > 1024 || result.partialFailures.size() > 1024)
                return false;
            std::set<std::string_view, std::less<>> names;
            for (const auto &field : result.fields)
                if (!ClaimName(field.name, names) || !ValidValue(field.value))
                    return false;
            names.clear();
            for (const auto &failure : result.partialFailures)
                if (!ClaimName(failure.item, names))
                    return false;
            return true;
        }

        /** @brief Admits the bounded scalar progress payload before either encoding is written. */
        bool ValidProgress(const CliProgressEvent &event) {
            const bool fraction = std::isfinite(event.completion) && event.completion >= 0 && event.completion <= 1;
            const bool phase = !event.phase.empty() && event.phase.size() <= 4096 && IsValidUtf8ScalarSequence(event.phase);
            const bool message = event.message.size() <= 4096 && IsValidUtf8ScalarSequence(event.message);
            return fraction && phase && message;
        }

        /** @brief Version-one result envelope shared by single JSON and JSONL terminal records. */
        Json Envelope(const CliCommandDescriptor &descriptor, const CliExecutionCorrelation &correlation) {
            Json metadata = Json::object();
            if (correlation.operation)
                metadata["operationId"] = correlation.operation->value;
            if (correlation.job)
                metadata["jobId"] = correlation.job->value;
            if (correlation.project)
                metadata["projectId"] = correlation.project->identity;
            return {{"schemaVersion", 1},
                    {"command", CommandName(descriptor.path)},
                    {"invocationId", std::format("cli-{}", correlation.invocation.value)},
                    {"outputSchema", {{"id", descriptor.output.id}, {"version", descriptor.output.version}}},
                    {"status", "succeeded"},
                    {"exitCode", 0},
                    {"result", Json::object()},
                    {"diagnostics", Json::array()},
                    {"error", nullptr},
                    {"metadata", std::move(metadata)}};
        }

        /** @brief Checks correlation before untrusted text can enter an envelope. */
        bool ValidCorrelation(const CliExecutionCorrelation &correlation, const CliInvocationId invocation) {
            return correlation.invocation == invocation && invocation.IsValid() &&
                   (!correlation.operation || correlation.operation->IsValid()) && (!correlation.job || correlation.job->IsValid()) &&
                   (!correlation.project ||
                    (correlation.project->identity.size() <= 256 && IsValidUtf8ScalarSequence(correlation.project->identity)));
        }

        /** @brief Projects admitted scalar values and canonical partial failures without committing output. */
        std::optional<Error> ProjectResult(Json &envelope, const CliCommandResult &result, const Hosts::ErrorTranslator &translator,
                                           CliPresentationOutcome &outcome) {
            if (!ValidFields(result))
                return MakeError(CliErrors::ExecutionContextInvalid);
            for (const auto &field : result.fields)
                std::visit([&envelope, &field](const auto &value) {
                    envelope["result"][field.name] = value;
                }, field.value);
            if (result.partialFailures.empty())
                return std::nullopt;
            envelope["status"] = "partial_failure";
            envelope["partialFailures"] = Json::array();
            for (const auto &item : result.partialFailures) {
                const auto translated = translator.Translate(item.error);
                if (!translated)
                    return item.error;
                envelope["partialFailures"].push_back({{"item", item.item}, {"error", Json::parse(translated->Json())}});
                if (outcome.exitCode == 0)
                    outcome.exitCode = static_cast<int>(translated->Category());
            }
            return std::nullopt;
        }

        /** @brief Fails closed on missing mappings while retaining the complete original typed error. */
        void ProjectFailure(Json &envelope, const Error &failure, const Hosts::ErrorTranslator &translator, const bool hostFailure,
                            CliPresentationOutcome &outcome) {
            auto translated = translator.Translate(failure);
            if (!translated) {
                outcome.rejectedError = failure;
                translated = translator.Translate(MakeError(CliErrors::HostFailure));
                outcome.exitCode = 1;
            } else
                outcome.exitCode = hostFailure ? 1 : static_cast<int>(translated->Category());
            envelope["status"] = "failed";
            envelope["result"] = nullptr;
            envelope.erase("partialFailures");
            if (translated) {
                envelope["error"] = Json::parse(translated->Json());
                envelope["diagnostics"] = envelope["error"]["diagnostics"];
            }
        }

        /** @brief Checks both borrowed streams after the completed record is committed. */
        bool OutputFailed(const std::ostream &output, const std::ostream &diagnostics) {
            return !output || !diagnostics;
        }

        /** @brief Writes human presentation without letting control text decorate result streams. */
        void WriteHuman(const Json &envelope, std::ostream &output, std::ostream &diagnostics) {
            if (envelope["status"] == "failed") {
                diagnostics << HumanText(envelope["error"].is_null() ? "CLI host failure" : envelope["error"]["message"].get<std::string>())
                            << '\n';
                return;
            }
            output << envelope["command"].get<std::string>() << ": " << envelope["status"].get<std::string>() << '\n';
            for (const auto &[name, value] : envelope["result"].items())
                output << HumanText(name) << ": " << HumanText(value.dump()) << '\n';
            if (envelope.contains("partialFailures"))
                for (const auto &failure : envelope["partialFailures"])
                    diagnostics << HumanText(failure["item"].get<std::string>()) << ": "
                                << HumanText(failure["error"]["message"].get<std::string>()) << '\n';
        }
    }  // namespace

    /** @copydoc CliOutputPresenter::CliOutputPresenter */
    CliOutputPresenter::CliOutputPresenter(CliCommandDescriptor descriptor, const CliInvocationId invocation,
                                           Hosts::ErrorTranslator translator, std::ostream &output, std::ostream &diagnostics,
                                           const CliProgressOutputMode mode, const bool terminal)
        : descriptor_(std::move(descriptor)), invocation_(invocation), translator_(std::move(translator)), output_(&output),
          diagnostics_(&diagnostics), mode_(mode), terminal_(terminal) {}

    /** @copydoc CliOutputPresenter::Progress */
    Result<void> CliOutputPresenter::Progress(const CliProgressEvent &event) {
        if (completed_ || !ValidProgress(event)) {
            progressRejected_ = true;
            return Result<void>::Failure(MakeError(CliErrors::ExecutionContextInvalid));
        }
        if (mode_ == CliProgressOutputMode::Json)
            return Result<void>::Success();
        if (mode_ == CliProgressOutputMode::JsonLines) {
            if (!descriptor_.output.progressRecords) {
                progressRejected_ = true;
                return Result<void>::Failure(MakeError(CliErrors::OutputSchemaIncompatible));
            }
            *output_ << Json{{"schemaVersion", 1},
                             {"recordVersion", 1},
                             {"type", "progress"},
                             {"command", CommandName(descriptor_.path)},
                             {"invocationId", std::format("cli-{}", invocation_.value)},
                             {"phase", event.phase},
                             {"completion", event.completion},
                             {"message", event.message}}
                            .dump()
                     << '\n';
        } else {
            if (terminal_)
                *diagnostics_ << '\r';
            *diagnostics_ << HumanText(event.phase) << ' ' << static_cast<int>(event.completion * 100) << "% " << HumanText(event.message)
                          << '\n';
        }
        return Result<void>::Success();
    }

    /** @copydoc CliOutputPresenter::Complete */
    CliPresentationOutcome CliOutputPresenter::Complete(const CliTerminalResult &terminal) {
        return Present(terminal, false);
    }

    /** @copydoc CliOutputPresenter::HostFailure */
    CliPresentationOutcome CliOutputPresenter::HostFailure() {
        return Present(CliTerminalResult::Failure({.invocation = invocation_}, MakeError(CliErrors::HostFailure)), true);
    }

    /** @copydoc CliOutputPresenter::Present */
    CliPresentationOutcome CliOutputPresenter::Present(const CliTerminalResult &terminal, const bool hostFailure) {
        if (completed_)
            return outcome_;
        const auto &correlation = terminal.Correlation();
        const bool validCorrelation = ValidCorrelation(correlation, invocation_);
        Json envelope = Envelope(descriptor_, validCorrelation ? correlation : CliExecutionCorrelation{.invocation = invocation_});
        std::optional<Error> failure;
        if (const bool invalidContext = !validCorrelation || progressRejected_; invalidContext)
            failure = MakeError(CliErrors::ExecutionContextInvalid);
        else if (terminal.Outcome().HasError())
            failure = terminal.Outcome().ErrorValue();
        else
            failure = ProjectResult(envelope, terminal.Outcome().Value(), translator_, outcome_);
        if (failure)
            ProjectFailure(envelope, *failure, translator_, hostFailure, outcome_);
        envelope["exitCode"] = outcome_.exitCode;
        if (mode_ == CliProgressOutputMode::JsonLines) {
            envelope["type"] = "result";
            envelope["recordVersion"] = 1;
        }
        const std::string document = envelope.dump();
        completed_ = true;
        if (mode_ != CliProgressOutputMode::Human)
            *output_ << document << '\n';
        else
            WriteHuman(envelope, *output_, *diagnostics_);
        if (OutputFailed(*output_, *diagnostics_))
            outcome_.exitCode = 1;
        return outcome_;
    }
}  // namespace Horo::Cli
