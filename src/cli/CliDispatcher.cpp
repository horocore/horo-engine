#include "Horo/Cli/CliDispatcher.h"

#include "Horo/Cli/CliErrors.h"
#include "Horo/Foundation/String.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace Horo::Cli {
    namespace {
        [[nodiscard]] bool SameCapability(const CliCapabilityId &left, const CliCapabilityId &right) noexcept {
            return left.value == right.value;
        }

        [[nodiscard]] bool ContainsCapability(const std::span<const CliCapabilityId> capabilities,
                                              const CliCapabilityId &candidate) noexcept {
            return std::ranges::any_of(capabilities, [&candidate](const CliCapabilityId &item) {
                return SameCapability(item, candidate);
            });
        }

        [[nodiscard]] bool SameNumericRange(const CliNumericRange &left, const CliNumericRange &right) noexcept {
            return left.minimumInteger == right.minimumInteger && left.maximumInteger == right.maximumInteger &&
                   left.minimumNumber == right.minimumNumber && left.maximumNumber == right.maximumNumber;
        }

        [[nodiscard]] bool SameOptionIdentity(const CliOptionDescriptor &left, const CliOptionDescriptor &right) noexcept {
            return left.name == right.name && left.shortName == right.shortName && left.summary == right.summary;
        }

        [[nodiscard]] bool SameOptionShape(const CliOptionDescriptor &left, const CliOptionDescriptor &right) noexcept {
            return left.valueKind == right.valueKind && left.required == right.required && left.repeatable == right.repeatable &&
                   left.sensitive == right.sensitive;
        }

        [[nodiscard]] bool SameOptionValues(const CliOptionDescriptor &left, const CliOptionDescriptor &right) noexcept {
            return left.defaultValue == right.defaultValue && left.enumerationValues == right.enumerationValues &&
                   SameNumericRange(left.numericRange, right.numericRange) && left.configurationKey == right.configurationKey;
        }

        [[nodiscard]] bool SameOption(const CliOptionDescriptor &left, const CliOptionDescriptor &right) noexcept {
            return SameOptionIdentity(left, right) && SameOptionShape(left, right) && SameOptionValues(left, right);
        }

        [[nodiscard]] bool SamePositional(const CliPositionalDescriptor &left, const CliPositionalDescriptor &right) noexcept {
            return left.name == right.name && left.summary == right.summary && left.valueKind == right.valueKind &&
                   left.required == right.required && left.repeatable == right.repeatable && left.sensitive == right.sensitive &&
                   left.enumerationValues == right.enumerationValues && SameNumericRange(left.numericRange, right.numericRange);
        }

        template <typename T, typename Predicate>
        [[nodiscard]] bool SameVector(const std::vector<T> &left, const std::vector<T> &right, Predicate predicate) noexcept {
            return left.size() == right.size() && std::ranges::equal(left, right, predicate);
        }

        [[nodiscard]] bool SameDescriptorSchema(const CliCommandDescriptor &left, const CliCommandDescriptor &right) noexcept {
            return SameVector(left.options, right.options, SameOption) && SameVector(left.positionals, right.positionals, SamePositional) &&
                   SameVector(left.requiredCapabilities, right.requiredCapabilities, SameCapability) && left.output.id == right.output.id &&
                   left.output.version == right.output.version && left.output.formats == right.output.formats;
        }

        [[nodiscard]] bool SameDescriptorExecution(const CliCommandDescriptor &left, const CliCommandDescriptor &right) noexcept {
            return left.interactive == right.interactive && left.hosts == right.hosts && left.contractVersion == right.contractVersion &&
                   left.sideEffects == right.sideEffects && left.cancellation == right.cancellation && left.timeout == right.timeout &&
                   left.stdinPolicy == right.stdinPolicy && left.interactiveAlternativeOption == right.interactiveAlternativeOption;
        }

        [[nodiscard]] bool SameDescriptor(const CliCommandDescriptor &left, const CliCommandDescriptor &right) noexcept {
            return left.path == right.path && left.summary == right.summary && SameDescriptorSchema(left, right) &&
                   SameDescriptorExecution(left, right) && left.origin == right.origin && left.ownerId == right.ownerId;
        }

        [[nodiscard]] bool SameCapabilitySet(const std::vector<CliCapabilityId> &left, const std::vector<CliCapabilityId> &right) {
            if (left.size() != right.size())
                return false;
            return std::ranges::all_of(left, [&right](const CliCapabilityId &capability) {
                return std::ranges::count_if(right, [&capability](const CliCapabilityId &candidate) {
                    return SameCapability(candidate, capability);
                }) == 1;
            });
        }

        [[nodiscard]] bool ValidLimits(const CliExecutionLimits &limits) noexcept {
            return limits.maximumAdapters != 0 && limits.maximumProgressEvents != 0 && limits.maximumProgressTextBytes != 0 &&
                   limits.maximumResultFields != 0 && limits.maximumResultTextBytes != 0 && limits.maximumProjectIdentityBytes != 0;
        }

        [[nodiscard]] bool IsCapabilityCharacter(const char character) noexcept {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-';
        }

        [[nodiscard]] bool ValidCapabilityIdentity(const std::string_view value) noexcept {
            bool atTokenStart = true;
            bool foundSeparator = false;
            for (const char character : value) {
                if (character == '.') {
                    if (atTokenStart)
                        return false;
                    atTokenStart = true;
                    foundSeparator = true;
                    continue;
                }
                if ((atTokenStart && (character < 'a' || character > 'z')) || !IsCapabilityCharacter(character))
                    return false;
                atTokenStart = false;
            }
            return foundSeparator && !atTokenStart;
        }

        [[nodiscard]] CliHostAvailability HostBit(const CliHostKind host) noexcept {
            switch (host) {
                case CliHostKind::HoroEngine:
                    return CliHostAvailability::HoroEngine;
                case CliHostKind::HoroPak:
                    return CliHostAvailability::HoroPak;
            }
            return CliHostAvailability::None;
        }

        [[nodiscard]] bool ValidPolicy(const CliDispatchPolicy &policy) noexcept {
            if (!ValidLimits(policy.limits) || HostBit(policy.activeHost) == CliHostAvailability::None ||
                static_cast<std::uint8_t>(policy.maximumSideEffects) >
                    static_cast<std::uint8_t>(CliSideEffectPolicy::StartsExternalProcess))
                return false;
            for (std::size_t index = 0; index < policy.grantedCapabilities.size(); ++index) {
                if (!ValidCapabilityIdentity(policy.grantedCapabilities[index].value))
                    return false;
                for (std::size_t other = index + 1; other < policy.grantedCapabilities.size(); ++other) {
                    if (SameCapability(policy.grantedCapabilities[index], policy.grantedCapabilities[other]))
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool SideEffectsAdmitted(const CliSideEffectPolicy required, const CliSideEffectPolicy maximum) noexcept {
            return static_cast<std::uint8_t>(required) <= static_cast<std::uint8_t>(maximum);
        }

        [[nodiscard]] CliExecutionCorrelation InitialCorrelation(const CliInvocationContext &invocation) noexcept {
            return {.invocation = invocation.invocation};
        }

        [[nodiscard]] bool TextWithin(const std::string &value, const std::size_t maximumBytes) noexcept {
            return !Text::IsBlank(value) && value.size() <= maximumBytes;
        }

        [[nodiscard]] bool ValidInvocation(const CliInvocationContext &invocation, const CliExecutionLimits &limits) noexcept {
            if (!invocation.invocation.IsValid() || invocation.configuration == nullptr)
                return false;
            return !invocation.project.has_value() || TextWithin(invocation.project->identity, limits.maximumProjectIdentityBytes);
        }

        [[nodiscard]] Result<std::optional<std::chrono::steady_clock::time_point>> ResolveDeadline(
            const CliTimeoutPolicy &policy, const std::uint64_t requestedMilliseconds) {
            if (requestedMilliseconds != 0 && (policy.maximumMilliseconds == 0 || requestedMilliseconds > policy.maximumMilliseconds))
                return Result<std::optional<std::chrono::steady_clock::time_point>>::Failure(MakeError(CliErrors::ExecutionContextInvalid));

            const std::uint64_t timeout = requestedMilliseconds == 0 ? policy.defaultMilliseconds : requestedMilliseconds;
            if (timeout == 0)
                return Result<std::optional<std::chrono::steady_clock::time_point>>::Success(std::nullopt);
            return Result<std::optional<std::chrono::steady_clock::time_point>>::Success(std::chrono::steady_clock::now() +
                                                                                         std::chrono::milliseconds(timeout));
        }

        [[nodiscard]] Result<const CliCommandDescriptor *> ValidateRegistration(const CliCommandAdapterRegistration &registration,
                                                                                const CliCommandRegistry &registry,
                                                                                const CliDispatchPolicy &policy) {
            if (registration.adapter == nullptr)
                return Result<const CliCommandDescriptor *>::Failure(MakeError(CliErrors::DispatchRegistrationInvalid));

            const CliCommandDescriptor &adapterDescriptor = registration.adapter->GetDescriptor();
            const CliCommandDescriptor *accepted = registry.Find(adapterDescriptor.path);
            if (accepted == nullptr || !SameDescriptor(*accepted, adapterDescriptor) ||
                !SameCapabilitySet(accepted->requiredCapabilities, registration.capabilities))
                return Result<const CliCommandDescriptor *>::Failure(MakeError(CliErrors::DispatchRegistrationInvalid));
            if ((accepted->hosts & HostBit(policy.activeHost)) == CliHostAvailability::None)
                return Result<const CliCommandDescriptor *>::Failure(MakeError(CliErrors::HostUnsupported));
            if (!SideEffectsAdmitted(accepted->sideEffects, policy.maximumSideEffects))
                return Result<const CliCommandDescriptor *>::Failure(MakeError(CliErrors::SideEffectUnauthorized));
            if (std::ranges::any_of(registration.capabilities, [&policy](const CliCapabilityId &capability) {
                return !ContainsCapability(policy.grantedCapabilities, capability);
            }))
                return Result<const CliCommandDescriptor *>::Failure(MakeError(CliErrors::CapabilityUnauthorized));
            return Result<const CliCommandDescriptor *>::Success(accepted);
        }

        [[nodiscard]] std::optional<Error> ValidateResult(const CliCommandResult &result, const CliExecutionLimits &limits) {
            if (result.fields.size() > limits.maximumResultFields)
                return MakeError(CliErrors::ExecutionCapacityExceeded);

            std::unordered_set<std::string_view> names;
            names.reserve(result.fields.size());
            for (const CliResultField &field : result.fields) {
                if (!TextWithin(field.name, limits.maximumResultTextBytes) || !names.insert(field.name).second)
                    return MakeError(CliErrors::ExecutionContextInvalid);
                if (const auto *text = std::get_if<std::string>(&field.value);
                    text != nullptr && text->size() > limits.maximumResultTextBytes)
                    return MakeError(CliErrors::ExecutionCapacityExceeded);
                if (const auto *number = std::get_if<double>(&field.value); number != nullptr && !std::isfinite(*number))
                    return MakeError(CliErrors::ExecutionContextInvalid);
            }
            return std::nullopt;
        }
    }  // namespace

    /** @copydoc CliExecutionContext::CliExecutionContext */
    CliExecutionContext::CliExecutionContext(const CliInvocationContext &invocation, const std::span<const CliCapabilityId> capabilities,
                                             const CliExecutionLimits &limits,
                                             const std::optional<std::chrono::steady_clock::time_point> deadline) noexcept
        : configuration_(invocation.configuration), cancellation_(invocation.cancellation),
          forceCancellation_(invocation.forceCancellation), progress_(invocation.progress), capabilities_(capabilities), limits_(&limits),
          deadline_(deadline), correlation_(InitialCorrelation(invocation)) {
        correlation_.project = invocation.project;
    }

    /** @copydoc CliExecutionContext::Configuration */
    const ConfigurationSnapshot &CliExecutionContext::Configuration() const noexcept {
        return *configuration_;
    }

    /** @copydoc CliExecutionContext::Cancellation */
    const CancellationToken &CliExecutionContext::Cancellation() const noexcept {
        return cancellation_;
    }

    /** @copydoc CliExecutionContext::ForceCancellation */
    const CancellationToken &CliExecutionContext::ForceCancellation() const noexcept {
        return forceCancellation_;
    }

    /** @copydoc CliExecutionContext::RemainingTimeout */
    std::optional<std::chrono::milliseconds> CliExecutionContext::RemainingTimeout() const noexcept {
        if (!deadline_.has_value())
            return std::nullopt;
        const auto remaining = *deadline_ - std::chrono::steady_clock::now();
        return remaining <= std::chrono::steady_clock::duration::zero() ? std::chrono::milliseconds::zero()
                                                                        : std::chrono::ceil<std::chrono::milliseconds>(remaining);
    }

    /** @copydoc CliExecutionContext::GrantedCapabilities */
    std::span<const CliCapabilityId> CliExecutionContext::GrantedCapabilities() const noexcept {
        return capabilities_;
    }

    /** @copydoc CliExecutionContext::HasCapability */
    bool CliExecutionContext::HasCapability(const CliCapabilityId &capability) const noexcept {
        return ContainsCapability(capabilities_, capability);
    }

    /** @copydoc CliExecutionContext::IsStopRequested */
    bool CliExecutionContext::IsStopRequested() const noexcept {
        return cancellation_.IsCancellationRequested() || (deadline_.has_value() && std::chrono::steady_clock::now() >= *deadline_);
    }

    /** @copydoc CliExecutionContext::ReportProgress */
    Result<void> CliExecutionContext::ReportProgress(const CliProgressEvent &event) {
        if (progressCount_ >= limits_->maximumProgressEvents || event.phase.size() > limits_->maximumProgressTextBytes ||
            event.message.size() > limits_->maximumProgressTextBytes) {
            progressRejected_ = true;
            return Result<void>::Failure(MakeError(CliErrors::ExecutionCapacityExceeded));
        }
        if (!TextWithin(event.phase, limits_->maximumProgressTextBytes) || !std::isfinite(event.completion) || event.completion < 0.0F ||
            event.completion > 1.0F) {
            progressRejected_ = true;
            return Result<void>::Failure(MakeError(CliErrors::ExecutionContextInvalid));
        }
        ++progressCount_;
        if (progress_ != nullptr)
            progress_->Report(event);
        return Result<void>::Success();
    }

    /** @copydoc CliExecutionContext::BindOperation */
    Result<void> CliExecutionContext::BindOperation(const CliOperationId operation) {
        if (!operation.IsValid() || (correlation_.operation.has_value() && correlation_.operation != operation)) {
            correlationRejected_ = true;
            return Result<void>::Failure(MakeError(CliErrors::ExecutionContextInvalid));
        }
        correlation_.operation = operation;
        return Result<void>::Success();
    }

    /** @copydoc CliExecutionContext::BindJob */
    Result<void> CliExecutionContext::BindJob(const CliJobId job) {
        if (!job.IsValid() || (correlation_.job.has_value() && correlation_.job != job)) {
            correlationRejected_ = true;
            return Result<void>::Failure(MakeError(CliErrors::ExecutionContextInvalid));
        }
        correlation_.job = job;
        return Result<void>::Success();
    }

    /** @copydoc CliExecutionContext::Correlation */
    const CliExecutionCorrelation &CliExecutionContext::Correlation() const noexcept {
        return correlation_;
    }

    /** @copydoc CliTerminalResult::Success */
    CliTerminalResult CliTerminalResult::Success(CliExecutionCorrelation correlation, CliCommandResult result) {
        return CliTerminalResult(std::move(correlation), Result<CliCommandResult>::Success(std::move(result)));
    }

    /** @copydoc CliTerminalResult::Failure */
    CliTerminalResult CliTerminalResult::Failure(CliExecutionCorrelation correlation, Error error) {
        return CliTerminalResult(std::move(correlation), Result<CliCommandResult>::Failure(std::move(error)));
    }

    /** @copydoc CliTerminalResult::Correlation */
    const CliExecutionCorrelation &CliTerminalResult::Correlation() const noexcept {
        return correlation_;
    }

    /** @copydoc CliTerminalResult::Outcome */
    const Result<CliCommandResult> &CliTerminalResult::Outcome() const noexcept {
        return outcome_;
    }

    CliTerminalResult::CliTerminalResult(CliExecutionCorrelation correlation, Result<CliCommandResult> outcome)
        : correlation_(std::move(correlation)), outcome_(std::move(outcome)) {}

    /** @copydoc CliDispatcher::Create */
    Result<CliDispatcher> CliDispatcher::Create(CliCommandRegistry registry, std::vector<CliCommandAdapterRegistration> registrations,
                                                CliDispatchPolicy policy) {
        if (!ValidPolicy(policy) || registrations.size() > policy.limits.maximumAdapters)
            return Result<CliDispatcher>::Failure(MakeError(CliErrors::DispatchRegistrationInvalid));

        std::vector<Entry> entries;
        entries.reserve(registrations.size());
        for (CliCommandAdapterRegistration &registration : registrations) {
            Result<const CliCommandDescriptor *> validation = ValidateRegistration(registration, registry, policy);
            if (validation.HasError())
                return Result<CliDispatcher>::Failure(validation.ErrorValue());
            const CliCommandDescriptor *accepted = validation.Value();

            if (std::ranges::any_of(entries, [accepted](const Entry &entry) {
                return entry.descriptor->path == accepted->path;
            }))
                return Result<CliDispatcher>::Failure(MakeError(CliErrors::DispatchRegistrationInvalid));

            entries.push_back(
                {.descriptor = accepted, .adapter = std::move(registration.adapter), .capabilities = std::move(registration.capabilities)});
        }

        return Result<CliDispatcher>::Success(CliDispatcher(std::move(registry), std::move(entries), std::move(policy)));
    }

    /** @copydoc CliDispatcher::Dispatch */
    CliTerminalResult CliDispatcher::Dispatch(const CliCommandRequest &request, const CliInvocationContext &invocation) {
        CliExecutionCorrelation initial = InitialCorrelation(invocation);
        const CliCommandDescriptor *descriptor = registry_.Find(request.command);
        if (descriptor == nullptr)
            return CliTerminalResult::Failure(std::move(initial), MakeError(CliErrors::CommandUnknown));

        const auto entry = std::ranges::find_if(entries_, [descriptor](const Entry &candidate) {
            return candidate.descriptor->path == descriptor->path;
        });
        if (entry == entries_.end())
            return CliTerminalResult::Failure(std::move(initial), MakeError(CliErrors::CommandUnavailable));

        if (!ValidInvocation(invocation, policy_.limits))
            return CliTerminalResult::Failure(std::move(initial), MakeError(CliErrors::ExecutionContextInvalid));
        initial.project = invocation.project;

        if (invocation.cancellation.IsCancellationRequested())
            return CliTerminalResult::Failure(std::move(initial),
                                              MakeError(invocation.stopControl != nullptr &&
                                                                invocation.stopControl->Reason() == CliStopReason::TimedOut
                                                            ? CliErrors::ExecutionTimedOut
                                                            : CliErrors::ExecutionCancelled));

        Result<std::optional<std::chrono::steady_clock::time_point>> admittedDeadline =
            ResolveDeadline(descriptor->timeout, invocation.timeoutMilliseconds);
        if (admittedDeadline.HasError())
            return CliTerminalResult::Failure(std::move(initial), admittedDeadline.ErrorValue());
        const std::optional<std::chrono::steady_clock::time_point> deadline = std::move(admittedDeadline).Value();

        CliExecutionContext context(invocation, entry->capabilities, policy_.limits, deadline);
        Result<CliCommandResult> result = entry->adapter->Execute(request, context);
        return Finalize(invocation, context, deadline, std::move(result));
    }

    /** @copydoc CliDispatcher::Finalize */
    CliTerminalResult CliDispatcher::Finalize(const CliInvocationContext &invocation, const CliExecutionContext &context,
                                              const std::optional<std::chrono::steady_clock::time_point> &deadline,
                                              Result<CliCommandResult> result) const {
        CliExecutionCorrelation correlation = context.Correlation();

        if (context.correlationRejected_)
            return CliTerminalResult::Failure(std::move(correlation), MakeError(CliErrors::ExecutionContextInvalid));
        if (context.progressRejected_)
            return CliTerminalResult::Failure(std::move(correlation), MakeError(CliErrors::ExecutionCapacityExceeded));
        if (result.HasError()) {
            const Error &error = result.ErrorValue();
            const bool acknowledgedCancellation = error.domain.Value() == CliErrors::ExecutionCancelled.domain.Value() &&
                                                  error.code.Value() == CliErrors::ExecutionCancelled.code.Value();
            if (const bool timedOut = invocation.stopControl != nullptr
                                          ? invocation.stopControl->Reason() == CliStopReason::TimedOut
                                          : (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline);
                acknowledgedCancellation && timedOut)
                return CliTerminalResult::Failure(std::move(correlation), MakeError(CliErrors::ExecutionTimedOut));
            return CliTerminalResult::Failure(std::move(correlation), error);
        }
        if (const std::optional<Error> invalid = ValidateResult(result.Value(), policy_.limits); invalid.has_value())
            return CliTerminalResult::Failure(std::move(correlation), *invalid);
        return CliTerminalResult::Success(std::move(correlation), std::move(result).Value());
    }

    /** @copydoc CliDispatcher::Registry */
    const CliCommandRegistry &CliDispatcher::Registry() const noexcept {
        return registry_;
    }

    CliDispatcher::CliDispatcher(CliCommandRegistry registry, std::vector<Entry> entries, CliDispatchPolicy policy)
        : registry_(std::move(registry)), entries_(std::move(entries)), policy_(std::move(policy)) {}
}  // namespace Horo::Cli
