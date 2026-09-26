#include "Horo/Cli/CliDispatcher.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Platform/ExternalProcess.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace Horo::Cli {
    namespace {
        constexpr CliContractVersion Contract{1, 2, 0};

        [[nodiscard]] CliCommandDescriptor Descriptor(std::vector<CliCapabilityId> capabilities = {{"horo.project.read"}}) {
            CliCommandDescriptor descriptor;
            descriptor.path = {{"project", "inspect"}};
            descriptor.summary = "Inspect a project.";
            descriptor.requiredCapabilities = std::move(capabilities);
            descriptor.output = {.id = "horo.cli.project-inspection",
                                 .version = 1,
                                 .formats = CliOutputFormat::Human | CliOutputFormat::Json};
            descriptor.interactive = CliInteractivePolicy::Forbidden;
            descriptor.hosts = CliHostAvailability::HoroEngine;
            descriptor.ownerId = "horo.project";
            descriptor.origin = CliCommandOrigin::BuiltIn;
            descriptor.contractVersion = Contract;
            descriptor.sideEffects = CliSideEffectPolicy::ReadsState;
            descriptor.cancellation = CliCancellationPolicy::Cooperative;
            descriptor.timeout = {.defaultMilliseconds = 1'000, .maximumMilliseconds = 5'000};
            descriptor.stdinPolicy = CliStdinPolicy::None;
            return descriptor;
        }

        [[nodiscard]] CliCommandRegistry Registry(const CliCommandDescriptor &descriptor) {
            auto created = CliCommandRegistry::Create(std::span{&descriptor, 1},
                                                      {.activeHost = CliHostKind::HoroEngine,
                                                       .supportedContractVersion = Contract,
                                                       .grantedCapabilities = {{"horo.project.read"}, {"horo.project.write"}}});
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        [[nodiscard]] ConfigurationSnapshot Configuration() {
            ConfigurationSchema schema;
            REQUIRE(schema
                        .Register({.key = SettingKey{"cli.test.enabled"},
                                   .type = SettingValueType::Boolean,
                                   .defaultValue = true,
                                   .scope = SettingScope::Invocation,
                                   .reloadPolicy = ReloadPolicy::NextOperation,
                                   .sensitivity = SettingSensitivity::Public})
                        .HasValue());
            REQUIRE(schema.Seal().HasValue());
            auto resolved = ConfigurationResolver::Resolve(schema, {}, 17);
            REQUIRE(resolved.HasValue());
            return std::move(resolved).Value();
        }

        [[nodiscard]] CliCommandRequest Request() {
            return {.command = {{"project", "inspect"}}};
        }

        struct UseCaseState final {
            std::size_t calls{};
            std::size_t adapterDestructions{};
            bool fail{};
            bool bindInvalidCorrelation{};
            bool overReport{};
            bool oversizedResult{};
            bool observedRead{};
            bool observedWrite{};
            ConfigurationRevision revision{};
            CancellationSource *cancelDuringExecution{};
            CliInvocationStopController *interruptDuringExecution{};
            std::uint64_t delayMilliseconds{};
            bool acknowledgeStop{};
            IExternalProcessRunner *processRunner{};
        };

        class IProjectInspectionUseCase {
        public:
            virtual ~IProjectInspectionUseCase() = default;
            [[nodiscard]] virtual Result<std::string> Inspect() = 0;
        };

        class ProjectInspectionUseCase final : public IProjectInspectionUseCase {
        public:
            explicit ProjectInspectionUseCase(UseCaseState &state) : state_(state) {}

            [[nodiscard]] Result<std::string> Inspect() override {
                ++state_.calls;
                if (state_.fail)
                    return Result<std::string>::Failure(MakeError(CliErrors::ParseFailed));
                return Result<std::string>::Success("shared-result");
            }

        private:
            UseCaseState &state_;
        };

        class ProjectInspectionAdapter final : public ICliCommandAdapter {
        public:
            ProjectInspectionAdapter(CliCommandDescriptor descriptor, IProjectInspectionUseCase &useCase, UseCaseState &state)
                : descriptor_(std::move(descriptor)), useCase_(useCase), state_(state) {}

            ~ProjectInspectionAdapter() override {
                ++state_.adapterDestructions;
            }

            [[nodiscard]] const CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            [[nodiscard]] Result<CliCommandResult> Execute(const CliCommandRequest &, CliExecutionContext &context) override {
                state_.observedRead = context.HasCapability({"horo.project.read"});
                state_.observedWrite = context.HasCapability({"horo.project.write"});
                state_.revision = context.Configuration().Revision();

                if (state_.bindInvalidCorrelation) {
                    static_cast<void>(context.BindOperation({0}));
                } else {
                    REQUIRE(context.BindOperation({41}).HasValue());
                    REQUIRE(context.BindJob({73}).HasValue());
                }

                const std::size_t reports = state_.overReport ? 2 : 1;
                for (std::size_t index = 0; index < reports; ++index)
                    static_cast<void>(context.ReportProgress({.phase = "inspect", .completion = 0.5F, .message = "Working"}));

                auto inspected = useCase_.Inspect();
                if (inspected.HasError())
                    return Result<CliCommandResult>::Failure(inspected.ErrorValue());
                if (state_.cancelDuringExecution != nullptr)
                    state_.cancelDuringExecution->RequestCancellation();
                if (state_.interruptDuringExecution != nullptr)
                    state_.interruptDuringExecution->Interrupt();
                if (state_.delayMilliseconds != 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(state_.delayMilliseconds));
                if (state_.acknowledgeStop && context.IsStopRequested())
                    return Result<CliCommandResult>::Failure(MakeError(CliErrors::ExecutionCancelled));
                if (state_.processRunner != nullptr) {
                    ExternalProcessRequest process{.executable = "test-child", .timeout = std::chrono::seconds{10}};
                    if (const auto remaining = context.RemainingTimeout(); remaining.has_value())
                        process.timeout = std::min(process.timeout, *remaining);
                    process.forceCancellation = context.ForceCancellation();
                    auto child = state_.processRunner->Run(process, context.Cancellation());
                    if (child.HasError())
                        return Result<CliCommandResult>::Failure(child.ErrorValue());
                }
                std::string value = std::move(inspected).Value();
                if (state_.oversizedResult)
                    value = "oversized";
                return Result<CliCommandResult>::Success({.fields = {{.name = "result", .value = std::move(value)}}});
            }

        private:
            CliCommandDescriptor descriptor_;
            IProjectInspectionUseCase &useCase_;
            UseCaseState &state_;
        };

        class RecordingProcessRunner final : public IExternalProcessRunner {
        public:
            [[nodiscard]] Result<ExternalProcessResult> Run(const ExternalProcessRequest &request,
                                                            const CancellationToken &cancellation) override {
                ++calls;
                timeout = request.timeout;
                cooperative = cancellation;
                forced = request.forceCancellation;
                return Result<ExternalProcessResult>::Success({.reason = ProcessTerminationReason::Exited});
            }

            std::size_t calls{};
            std::chrono::milliseconds timeout{};
            CancellationToken cooperative;
            CancellationToken forced;
        };

        [[nodiscard]] Result<CliDispatcher> MakeDispatcher(
            UseCaseState &state, IProjectInspectionUseCase &useCase, CliCommandDescriptor descriptor = Descriptor(),
            std::vector<CliCapabilityId> bound = {{"horo.project.read"}},
            CliDispatchPolicy policy = {.activeHost = CliHostKind::HoroEngine,
                                        .maximumSideEffects = CliSideEffectPolicy::ReadsState,
                                        .grantedCapabilities = {{"horo.project.read"}}}) {
            CliCommandRegistry registry = Registry(descriptor);
            std::vector<CliCommandAdapterRegistration> registrations;
            registrations.push_back(
                {.adapter = std::make_unique<ProjectInspectionAdapter>(descriptor, useCase, state), .capabilities = std::move(bound)});
            return CliDispatcher::Create(std::move(registry), std::move(registrations), std::move(policy));
        }

        [[nodiscard]] CliInvocationContext Invocation(const ConfigurationSnapshot &configuration, CliProgressMailbox *progress = nullptr) {
            return {.invocation = {11},
                    .configuration = &configuration,
                    .progress = progress,
                    .project = CliSafeProjectContext{"project-123"}};
        }

        [[nodiscard]] CliDispatcher RequiredDispatcher(UseCaseState &state, IProjectInspectionUseCase &useCase) {
            auto created = MakeDispatcher(state, useCase);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        struct DispatcherFixture final {
            UseCaseState state;
            ProjectInspectionUseCase useCase{state};
            CliDispatcher dispatcher{RequiredDispatcher(state, useCase)};
            ConfigurationSnapshot configuration{Configuration()};
        };

        void RequireError(const CliTerminalResult &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.Outcome().HasError());
            CHECK(result.Outcome().ErrorValue().domain.Value() == "horo.cli");
            CHECK(result.Outcome().ErrorValue().code.Value() == descriptor.code.Value());
        }
    }  // namespace

    TEST_CASE("CLI dispatch exposes only declared capabilities and preserves complete correlation") {
        DispatcherFixture fixture;
        CliProgressMailbox progress;

        const CliTerminalResult terminal = fixture.dispatcher.Dispatch(Request(), Invocation(fixture.configuration, &progress));

        REQUIRE(terminal.Outcome().HasValue());
        CHECK(fixture.state.calls == 1);
        CHECK(fixture.state.observedRead);
        CHECK_FALSE(fixture.state.observedWrite);
        CHECK(fixture.state.revision == 17);
        const auto update = progress.Take();
        REQUIRE(update.has_value());
        CHECK(update->phase == "inspect");
        CHECK(terminal.Correlation().invocation == CliInvocationId{11});
        CHECK(terminal.Correlation().operation == CliOperationId{41});
        CHECK(terminal.Correlation().job == CliJobId{73});
        REQUIRE(terminal.Correlation().project.has_value());
        CHECK(terminal.Correlation().project->identity == "project-123");
    }

    TEST_CASE("CLI adapter calls the same application use case as GUI and MCP adapters") {
        UseCaseState state;
        ProjectInspectionUseCase useCase(state);
        auto gui = [&useCase] {
            return useCase.Inspect();
        };
        auto mcp = [&useCase] {
            return useCase.Inspect();
        };
        REQUIRE(gui().Value() == "shared-result");
        REQUIRE(mcp().Value() == "shared-result");

        auto created = MakeDispatcher(state, useCase);
        REQUIRE(created.HasValue());
        const ConfigurationSnapshot configuration = Configuration();
        const CliTerminalResult cli = std::move(created).Value().Dispatch(Request(), Invocation(configuration));

        REQUIRE(cli.Outcome().HasValue());
        CHECK(std::get<std::string>(cli.Outcome().Value().fields.front().value) == "shared-result");
        CHECK(state.calls == 3);
    }

    TEST_CASE("CLI dispatcher rejects missing unauthorized and mismatched bindings before domain mutation") {
        UseCaseState state;
        ProjectInspectionUseCase useCase(state);

        auto missing = MakeDispatcher(state, useCase, Descriptor(), {});
        REQUIRE(missing.HasError());
        CHECK(missing.ErrorValue().code.Value() == CliErrors::DispatchRegistrationInvalid.code.Value());

        auto unauthorized = MakeDispatcher(state, useCase, Descriptor(), {{"horo.project.read"}},
                                           {.activeHost = CliHostKind::HoroEngine,
                                            .maximumSideEffects = CliSideEffectPolicy::ReadsState,
                                            .grantedCapabilities = {}});
        REQUIRE(unauthorized.HasError());
        CHECK(unauthorized.ErrorValue().code.Value() == CliErrors::CapabilityUnauthorized.code.Value());

        CliCommandDescriptor mismatched = Descriptor();
        CliCommandDescriptor adapterDescriptor = mismatched;
        adapterDescriptor.ownerId = "horo.other";
        CliCommandRegistry registry = Registry(mismatched);
        std::vector<CliCommandAdapterRegistration> registrations;
        registrations.push_back({.adapter = std::make_unique<ProjectInspectionAdapter>(adapterDescriptor, useCase, state),
                                 .capabilities = {{"horo.project.read"}}});
        auto mismatch = CliDispatcher::Create(std::move(registry), std::move(registrations),
                                              {.activeHost = CliHostKind::HoroEngine,
                                               .maximumSideEffects = CliSideEffectPolicy::ReadsState,
                                               .grantedCapabilities = {{"horo.project.read"}}});
        REQUIRE(mismatch.HasError());
        CHECK(mismatch.ErrorValue().code.Value() == CliErrors::DispatchRegistrationInvalid.code.Value());
        CHECK(state.calls == 0);
    }

    TEST_CASE("CLI dispatcher validates host side-effect and capability policy before activation") {
        UseCaseState state;
        ProjectInspectionUseCase useCase(state);

        auto sideEffects = MakeDispatcher(state, useCase, Descriptor(), {{"horo.project.read"}},
                                          {.activeHost = CliHostKind::HoroEngine,
                                           .maximumSideEffects = CliSideEffectPolicy::None,
                                           .grantedCapabilities = {{"horo.project.read"}}});
        REQUIRE(sideEffects.HasError());
        CHECK(sideEffects.ErrorValue().code.Value() == CliErrors::SideEffectUnauthorized.code.Value());

        auto duplicateGrant = MakeDispatcher(state, useCase, Descriptor(), {{"horo.project.read"}},
                                             {.activeHost = CliHostKind::HoroEngine,
                                              .maximumSideEffects = CliSideEffectPolicy::ReadsState,
                                              .grantedCapabilities = {{"horo.project.read"}, {"horo.project.read"}}});
        REQUIRE(duplicateGrant.HasError());
        CHECK(duplicateGrant.ErrorValue().code.Value() == CliErrors::DispatchRegistrationInvalid.code.Value());
        CHECK(state.calls == 0);
    }

    TEST_CASE("CLI dispatcher rejects invocation authority failures before application execution") {
        DispatcherFixture fixture;

        CliInvocationContext invalid = Invocation(fixture.configuration);
        invalid.invocation = {};
        RequireError(fixture.dispatcher.Dispatch(Request(), invalid), CliErrors::ExecutionContextInvalid);

        invalid = Invocation(fixture.configuration);
        invalid.configuration = nullptr;
        RequireError(fixture.dispatcher.Dispatch(Request(), invalid), CliErrors::ExecutionContextInvalid);

        invalid = Invocation(fixture.configuration);
        invalid.timeoutMilliseconds = 5'001;
        RequireError(fixture.dispatcher.Dispatch(Request(), invalid), CliErrors::ExecutionContextInvalid);

        invalid = Invocation(fixture.configuration);
        invalid.project = CliSafeProjectContext{"   "};
        RequireError(fixture.dispatcher.Dispatch(Request(), invalid), CliErrors::ExecutionContextInvalid);
        CHECK(fixture.state.calls == 0);
    }

    TEST_CASE("CLI dispatcher preserves cancellation and application failures with correlation") {
        DispatcherFixture fixture;

        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CliInvocationContext invocation = Invocation(fixture.configuration);
        invocation.cancellation = cancelled.Token();
        const CliTerminalResult preCancelled = fixture.dispatcher.Dispatch(Request(), invocation);
        RequireError(preCancelled, CliErrors::ExecutionCancelled);
        CHECK(fixture.state.calls == 0);
        CHECK(preCancelled.Correlation().invocation == CliInvocationId{11});

        fixture.state.fail = true;
        const CliTerminalResult failed = fixture.dispatcher.Dispatch(Request(), Invocation(fixture.configuration));
        RequireError(failed, CliErrors::ParseFailed);
        CHECK(failed.Correlation().operation == CliOperationId{41});
        CHECK(failed.Correlation().job == CliJobId{73});

        fixture.state.fail = false;
        CancellationSource duringExecution;
        fixture.state.cancelDuringExecution = &duringExecution;
        invocation = Invocation(fixture.configuration);
        invocation.cancellation = duringExecution.Token();
        const CliTerminalResult completedBeforeAcknowledgement = fixture.dispatcher.Dispatch(Request(), invocation);
        REQUIRE(completedBeforeAcknowledgement.Outcome().HasValue());
        CHECK(completedBeforeAcknowledgement.Correlation().operation == CliOperationId{41});

        fixture.state.acknowledgeStop = true;
        CancellationSource acknowledgedCancellation;
        fixture.state.cancelDuringExecution = &acknowledgedCancellation;
        invocation.cancellation = acknowledgedCancellation.Token();
        const CliTerminalResult cancelledDuringExecution = fixture.dispatcher.Dispatch(Request(), invocation);
        RequireError(cancelledDuringExecution, CliErrors::ExecutionCancelled);
    }

    TEST_CASE("CLI dispatcher applies the descriptor deadline to terminal production") {
        UseCaseState state;
        state.delayMilliseconds = 5;
        state.acknowledgeStop = true;
        ProjectInspectionUseCase useCase(state);
        CliCommandDescriptor descriptor = Descriptor();
        descriptor.timeout = {.defaultMilliseconds = 1, .maximumMilliseconds = 10};
        auto created = MakeDispatcher(state, useCase, std::move(descriptor));
        REQUIRE(created.HasValue());
        CliDispatcher dispatcher = std::move(created).Value();
        const ConfigurationSnapshot configuration = Configuration();

        const CliTerminalResult timedOut = dispatcher.Dispatch(Request(), Invocation(configuration));
        RequireError(timedOut, CliErrors::ExecutionTimedOut);
        CHECK(timedOut.Correlation().operation == CliOperationId{41});
        CHECK(timedOut.Correlation().job == CliJobId{73});

        state.acknowledgeStop = false;
        const CliTerminalResult completed = dispatcher.Dispatch(Request(), Invocation(configuration));
        REQUIRE(completed.Outcome().HasValue());

        state.acknowledgeStop = true;
        CliInvocationStopController interrupted{std::chrono::seconds{5}};
        state.interruptDuringExecution = &interrupted;
        CliInvocationContext invocation = Invocation(configuration);
        invocation.cancellation = interrupted.Token();
        invocation.stopControl = &interrupted;
        const CliTerminalResult cancelled = dispatcher.Dispatch(Request(), invocation);
        RequireError(cancelled, CliErrors::ExecutionCancelled);
    }

    TEST_CASE("CLI process adapter caps its timeout and propagates both stop tokens") {
        DispatcherFixture fixture;
        RecordingProcessRunner runner;
        fixture.state.processRunner = &runner;
        CliInvocationStopController stop{std::chrono::seconds{5}};
        CliInvocationContext invocation = Invocation(fixture.configuration);
        invocation.cancellation = stop.Token();
        invocation.forceCancellation = stop.ForceToken();
        invocation.stopControl = &stop;
        const CliTerminalResult result = fixture.dispatcher.Dispatch(Request(), invocation);
        REQUIRE(result.Outcome().HasValue());
        REQUIRE(runner.calls == 1);
        CHECK(runner.timeout > std::chrono::milliseconds::zero());
        CHECK(runner.timeout <= std::chrono::seconds{1});
        stop.Interrupt();
        CHECK(runner.cooperative.IsCancellationRequested());
        CHECK_FALSE(runner.forced.IsCancellationRequested());
        stop.Interrupt();
        CHECK(runner.forced.IsCancellationRequested());
    }

    TEST_CASE("CLI dispatcher enforces progress result and correlation bounds") {
        UseCaseState state;
        ProjectInspectionUseCase useCase(state);
        CliDispatchPolicy policy{.activeHost = CliHostKind::HoroEngine,
                                 .maximumSideEffects = CliSideEffectPolicy::ReadsState,
                                 .grantedCapabilities = {{"horo.project.read"}},
                                 .limits = {.maximumAdapters = 1,
                                            .maximumProgressEvents = 1,
                                            .maximumProgressTextBytes = 16,
                                            .maximumResultFields = 1,
                                            .maximumResultTextBytes = 6,
                                            .maximumProjectIdentityBytes = 32}};
        auto created = MakeDispatcher(state, useCase, Descriptor(), {{"horo.project.read"}}, policy);
        REQUIRE(created.HasValue());
        CliDispatcher dispatcher = std::move(created).Value();
        const ConfigurationSnapshot configuration = Configuration();

        state.overReport = true;
        RequireError(dispatcher.Dispatch(Request(), Invocation(configuration)), CliErrors::ExecutionCapacityExceeded);
        state.overReport = false;
        state.oversizedResult = true;
        RequireError(dispatcher.Dispatch(Request(), Invocation(configuration)), CliErrors::ExecutionCapacityExceeded);
        state.oversizedResult = false;
        state.bindInvalidCorrelation = true;
        RequireError(dispatcher.Dispatch(Request(), Invocation(configuration)), CliErrors::ExecutionContextInvalid);
    }

    TEST_CASE("CLI dispatcher owns adapters and releases them before injected services") {
        UseCaseState state;
        ProjectInspectionUseCase useCase(state);
        {
            auto created = MakeDispatcher(state, useCase);
            REQUIRE(created.HasValue());
            CliDispatcher dispatcher = std::move(created).Value();
            CHECK(state.adapterDestructions == 0);
        }
        CHECK(state.adapterDestructions == 1);
    }

    TEST_CASE("CLI dispatcher reports unknown and unbound commands without invoking a use case") {
        UseCaseState state;
        ProjectInspectionUseCase useCase(state);
        const CliCommandDescriptor descriptor = Descriptor();
        CliCommandRegistry registry = Registry(descriptor);
        auto empty = CliDispatcher::Create(std::move(registry), {},
                                           {.activeHost = CliHostKind::HoroEngine,
                                            .maximumSideEffects = CliSideEffectPolicy::ReadsState,
                                            .grantedCapabilities = {{"horo.project.read"}}});
        REQUIRE(empty.HasValue());
        CliDispatcher dispatcher = std::move(empty).Value();
        const ConfigurationSnapshot configuration = Configuration();
        RequireError(dispatcher.Dispatch(Request(), Invocation(configuration)), CliErrors::CommandUnavailable);

        CliCommandRequest unknown{.command = {{"not", "registered"}}};
        RequireError(dispatcher.Dispatch(unknown, Invocation(configuration)), CliErrors::CommandUnknown);
        CHECK(state.calls == 0);
    }
}  // namespace Horo::Cli
