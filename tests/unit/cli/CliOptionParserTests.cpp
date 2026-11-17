#include "Horo/Cli/CliErrors.h"
#include "Horo/Cli/CliOptionParser.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Horo::Cli {
    namespace {
        constexpr CliContractVersion CurrentContract{1, 2, 0};

        class TestPathNormalizer final : public ICliPathNormalizer {
        public:
            [[nodiscard]] Result<std::string> Normalize(const std::string_view value) const override {
                ++calls;
                if (fail)
                    return Result<std::string>::Failure(MakeError(CliErrors::ParseFailed));
                return Result<std::string>::Success("normalized/" + std::string{value});
            }

            mutable std::size_t calls{};
            bool fail{};
        };

        class TestInputReader final : public ICliInputReader {
        public:
            [[nodiscard]] Result<std::vector<std::byte>> Read(const std::size_t maximumBytes) override {
                ++calls;
                observedLimit = maximumBytes;
                if (fail)
                    return Result<std::vector<std::byte>>::Failure(MakeError(CliErrors::ParseFailed));
                return Result<std::vector<std::byte>>::Success(payload);
            }

            std::vector<std::byte> payload;
            std::size_t calls{};
            std::size_t observedLimit{};
            bool fail{};
        };

        [[nodiscard]] std::vector<std::byte> Bytes(const std::string_view text) {
            const auto *begin = reinterpret_cast<const std::byte *>(text.data());
            return {begin, begin + text.size()};
        }

        [[nodiscard]] CliCommandDescriptor Descriptor() {
            CliCommandDescriptor descriptor;
            descriptor.path = {{"project", "validate"}};
            descriptor.summary = "Validate a project.";
            descriptor.options =
                {{.name = "count",
                  .shortName = 'c',
                  .summary = "Set the count.",
                  .valueKind = CliOptionValueKind::SignedInteger,
                  .required = true,
                  .numericRange = {.minimumInteger = 1, .maximumInteger = 8}},
                 {.name = "mode",
                  .shortName = 'm',
                  .summary = "Set validation mode.",
                  .valueKind = CliOptionValueKind::Enumeration,
                  .defaultValue = "safe",
                  .enumerationValues = {"fast", "safe"}},
                 {.name = "tag", .shortName = 't', .summary = "Add a tag.", .valueKind = CliOptionValueKind::String, .repeatable = true}};
            descriptor.requiredCapabilities = {{"horo.project.read"}};
            descriptor.output = {.id = "horo.cli.validation-result",
                                 .version = 1,
                                 .formats = CliOutputFormat::Human | CliOutputFormat::Json};
            descriptor.interactive = CliInteractivePolicy::Forbidden;
            descriptor.hosts = CliHostAvailability::HoroEngine;
            descriptor.contractVersion = CurrentContract;
            descriptor.sideEffects = CliSideEffectPolicy::ReadsState;
            descriptor.cancellation = CliCancellationPolicy::Cooperative;
            descriptor.timeout = {.defaultMilliseconds = 1'000, .maximumMilliseconds = 5'000};
            descriptor.stdinPolicy = CliStdinPolicy::None;
            descriptor.origin = CliCommandOrigin::BuiltIn;
            descriptor.ownerId = "horo.project";
            return descriptor;
        }

        [[nodiscard]] CliCommandRegistry Registry(CliCommandDescriptor descriptor = Descriptor()) {
            const CliCommandRegistryPolicy policy{.activeHost = CliHostKind::HoroEngine,
                                                  .supportedContractVersion = CurrentContract,
                                                  .grantedCapabilities = {{"horo.project.read"}}};
            auto created = CliCommandRegistry::Create(std::span{&descriptor, 1}, policy);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        [[nodiscard]] const CliParsedOption *FindOption(const CliCommandRequest &request, const std::string_view name) {
            const auto found = std::ranges::find(request.options, name, &CliParsedOption::name);
            return found == request.options.end() ? nullptr : std::to_address(found);
        }

        void RequireError(const Result<CliCommandRequest> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().domain.Value() == "horo.cli");
            REQUIRE(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        [[nodiscard]] ConfigurationSnapshot Configuration(const ConfigurationSourceMap &environment = {},
                                                          const ConfigurationSourceMap &user = {}) {
            ConfigurationSchema schema;
            const ConfigurationSourcePolicy sources{.allowedSources =
                                                        ConfigurationSourceMask::Invocation | ConfigurationSourceMask::Environment |
                                                        ConfigurationSourceMask::Session | ConfigurationSourceMask::Project |
                                                        ConfigurationSourceMask::User | ConfigurationSourceMask::PackagedProfile};
            REQUIRE(schema
                        .Register({.key = SettingKey{"cli.output.format"},
                                   .type = SettingValueType::String,
                                   .defaultValue = std::string{"human"},
                                   .scope = SettingScope::Invocation,
                                   .reloadPolicy = ReloadPolicy::NextOperation,
                                   .sensitivity = SettingSensitivity::Public,
                                   .sourcePolicy = sources})
                        .HasValue());
            REQUIRE(schema
                        .Register({.key = SettingKey{"project.root"},
                                   .type = SettingValueType::String,
                                   .defaultValue = std::string{"default-project"},
                                   .scope = SettingScope::Invocation,
                                   .reloadPolicy = ReloadPolicy::NextOperation,
                                   .sensitivity = SettingSensitivity::Public,
                                   .sourcePolicy = sources})
                        .HasValue());
            REQUIRE(schema.Seal().HasValue());
            ConfigurationResolutionRequest request{.environment = environment, .user = user};
            auto resolved = ConfigurationResolver::Resolve(schema, request, 9);
            REQUIRE(resolved.HasValue());
            return std::move(resolved).Value();
        }
    }  // namespace

    TEST_CASE("CLI parser resolves command options defaults and repeatable values", "[unit][cli][parser]") {
        const CliCommandRegistry registry = Registry();
        TestPathNormalizer paths;
        const auto arguments = std::to_array<std::string_view>({"project", "validate", "--count=4", "-t", "alpha", "--tag", "beta"});
        auto parsed = CliOptionParser::Parse(arguments, registry, {.pathNormalizer = &paths});

        REQUIRE(parsed.HasValue());
        REQUIRE((parsed.Value().command.segments == std::vector<std::string>{"project", "validate"}));
        REQUIRE(std::get<std::int64_t>(FindOption(parsed.Value(), "count")->values[0].value) == 4);
        REQUIRE(std::get<std::string>(FindOption(parsed.Value(), "mode")->values[0].value) == "safe");
        REQUIRE(FindOption(parsed.Value(), "mode")->values[0].source == CliValueSource::DescriptorDefault);
        REQUIRE(FindOption(parsed.Value(), "tag")->values.size() == 2);
    }

    TEST_CASE("CLI parser aggregates unknown missing invalid and duplicate option diagnostics", "[unit][cli][parser]") {
        const CliCommandRegistry registry = Registry();
        TestInputReader input;
        const auto arguments =
            std::to_array<std::string_view>({"project", "validate", "--unknown", "--count", "99", "--count", "2", "--mode"});
        auto parsed = CliOptionParser::Parse(arguments, registry, {.inputReader = &input});

        RequireError(parsed, CliErrors::ParseFailed);
        REQUIRE(parsed.ErrorValue().diagnostics.size() == 5);
        REQUIRE(parsed.ErrorValue().diagnostics[0].code.Value() == "cli.option_unknown");
        REQUIRE(input.calls == 0);
    }

    TEST_CASE("CLI parser applies invocation over resolved configuration and retains provenance", "[unit][cli][parser]") {
        ConfigurationSourceMap environment;
        environment.try_emplace(SettingKey{"cli.output.format"}, ConfigurationInputValue{.value = std::string{"json"}});
        const ConfigurationSnapshot snapshot = Configuration(environment);
        const CliCommandRegistry registry = Registry();
        TestPathNormalizer paths;

        const auto configuredArguments = std::to_array<std::string_view>({"project", "validate", "--count", "2"});
        auto configured = CliOptionParser::Parse(configuredArguments, registry, {.configuration = &snapshot, .pathNormalizer = &paths});
        REQUIRE(configured.HasValue());
        REQUIRE(std::get<std::string>(FindOption(configured.Value(), "output")->values[0].value) == "json");
        REQUIRE(FindOption(configured.Value(), "output")->values[0].source == CliValueSource::Configuration);
        REQUIRE(FindOption(configured.Value(), "output")->values[0].configurationSource == ConfigurationSource::Environment);

        const auto invocationArguments = std::to_array<std::string_view>({"project", "validate", "--count", "2", "--output=jsonl"});
        auto invocation = CliOptionParser::Parse(invocationArguments, registry, {.configuration = &snapshot, .pathNormalizer = &paths});
        REQUIRE(invocation.HasValue());
        REQUIRE(std::get<std::string>(FindOption(invocation.Value(), "output")->values[0].value) == "jsonl");
        REQUIRE(FindOption(invocation.Value(), "output")->values[0].source == CliValueSource::Invocation);
    }

    TEST_CASE("CLI parser normalizes option configuration and positional paths through one adapter", "[unit][cli][parser]") {
        CliCommandDescriptor descriptor = Descriptor();
        descriptor.positionals = {{.name = "input", .summary = "Input path.", .valueKind = CliOptionValueKind::Path, .required = true}};
        const CliCommandRegistry registry = Registry(std::move(descriptor));
        const ConfigurationSnapshot snapshot = Configuration();
        TestPathNormalizer paths;
        const auto arguments = std::to_array<std::string_view>({"project", "validate", "--count", "2", "--", "-relative/path"});
        auto parsed = CliOptionParser::Parse(arguments, registry, {.configuration = &snapshot, .pathNormalizer = &paths});

        REQUIRE(parsed.HasValue());
        REQUIRE(std::get<CliPathValue>(FindOption(parsed.Value(), "project")->values[0].value).value == "normalized/default-project");
        REQUIRE(std::get<CliPathValue>(parsed.Value().positionals[0].value).value == "normalized/-relative/path");
        REQUIRE(paths.calls == 2);
    }

    TEST_CASE("CLI parser never reads inherited stdin unless a supported mode is selected", "[unit][cli][parser]") {
        const CliCommandRegistry registry = Registry();
        TestInputReader input;
        input.payload = Bytes("{\"ok\":true}");
        const auto arguments = std::to_array<std::string_view>({"project", "validate", "--count", "2"});

        REQUIRE(CliOptionParser::Parse(arguments, registry, {.inputReader = &input}).HasValue());
        REQUIRE(input.calls == 0);
        auto selected =
            CliOptionParser::Parse(arguments, registry,
                                   {.inputReader = &input, .stdinSelected = true, .selectedStdinPolicy = CliStdinPolicy::JsonDocument});
        RequireError(selected, CliErrors::InputModeUnsupported);
        REQUIRE(input.calls == 0);
    }

    TEST_CASE("CLI parser admits only explicit bounded JSON document input", "[unit][cli][parser]") {
        CliCommandDescriptor descriptor = Descriptor();
        descriptor.stdinPolicy = CliStdinPolicy::JsonDocument;
        const CliCommandRegistry registry = Registry(std::move(descriptor));
        TestInputReader input;
        input.payload = Bytes("{\"ok\":true}");
        const auto arguments = std::to_array<std::string_view>({"project", "validate", "--count", "2"});
        CliParserContext context{.inputReader = &input, .stdinSelected = true, .selectedStdinPolicy = CliStdinPolicy::JsonDocument};

        auto parsed = CliOptionParser::Parse(arguments, registry, context, {.maximumInputBytes = 32});
        REQUIRE(parsed.HasValue());
        REQUIRE(input.calls == 1);
        REQUIRE(input.observedLimit == 32);
        REQUIRE(parsed.Value().input->bytes == input.payload);

        input.payload = Bytes("not-json");
        auto malformed = CliOptionParser::Parse(arguments, registry, context);
        RequireError(malformed, CliErrors::ParseFailed);
        REQUIRE(malformed.ErrorValue().diagnostics.size() == 1);
        REQUIRE(malformed.ErrorValue().diagnostics[0].code.Value() == "cli.stdin_malformed");
        REQUIRE(malformed.ErrorValue().diagnostics[0].location.source == "stdin");

        input.fail = true;
        auto readFailure = CliOptionParser::Parse(arguments, registry, context);
        RequireError(readFailure, CliErrors::ParseFailed);
        REQUIRE(readFailure.ErrorValue().cause);
        REQUIRE(readFailure.ErrorValue().cause.Get()->code.Value() == CliErrors::ParseFailed.code.Value());
    }

    TEST_CASE("CLI parser validates JSONL and binary modes independently", "[unit][cli][parser]") {
        CliCommandDescriptor descriptor = Descriptor();
        descriptor.stdinPolicy = CliStdinPolicy::JsonLines;
        CliCommandRegistry registry = Registry(descriptor);
        TestInputReader input;
        input.payload = Bytes("{\"id\":1}\n{\"id\":2}\n");
        const auto arguments = std::to_array<std::string_view>({"project", "validate", "--count", "2"});
        CliParserContext context{.inputReader = &input, .stdinSelected = true, .selectedStdinPolicy = CliStdinPolicy::JsonLines};
        REQUIRE(CliOptionParser::Parse(arguments, registry, context).HasValue());

        input.payload = Bytes("{\"id\":1}\n\n");
        RequireError(CliOptionParser::Parse(arguments, registry, context), CliErrors::ParseFailed);

        descriptor.stdinPolicy = CliStdinPolicy::BinaryStream;
        registry = Registry(descriptor);
        input.payload = {std::byte{0}, std::byte{255}};
        context.selectedStdinPolicy = CliStdinPolicy::BinaryStream;
        REQUIRE(CliOptionParser::Parse(arguments, registry, context).HasValue());
        input.payload.resize(5);
        RequireError(CliOptionParser::Parse(arguments, registry, context, {.maximumInputBytes = 4}), CliErrors::InputCapacityExceeded);
    }

    TEST_CASE("CLI parser enforces required interaction alternatives without implicit prompts", "[unit][cli][parser]") {
        CliCommandDescriptor descriptor = Descriptor();
        descriptor.interactive = CliInteractivePolicy::Required;
        descriptor.interactiveAlternativeOption = "mode";
        descriptor.options[1].defaultValue.reset();
        const CliCommandRegistry registry = Registry(std::move(descriptor));
        const auto missing = std::to_array<std::string_view>({"project", "validate", "--count", "2", "--non-interactive"});
        RequireError(CliOptionParser::Parse(missing, registry, {.terminalAvailable = true}), CliErrors::InteractiveInputUnavailable);

        const auto supplied =
            std::to_array<std::string_view>({"project", "validate", "--count", "2", "--non-interactive", "--mode", "fast"});
        auto parsed = CliOptionParser::Parse(supplied, registry, {.terminalAvailable = true});
        REQUIRE(parsed.HasValue());
        REQUIRE_FALSE(parsed.Value().interactive);
    }

    TEST_CASE("CLI parser bounds argv diagnostics and unknown commands before retaining input", "[unit][cli][parser]") {
        const CliCommandRegistry registry = Registry();
        const auto unknown = std::to_array<std::string_view>({"scene", "validate"});
        RequireError(CliOptionParser::Parse(unknown, registry), CliErrors::CommandUnknown);

        const auto oversized = std::to_array<std::string_view>({"project", "validate", "--count", "12345"});
        RequireError(CliOptionParser::Parse(oversized, registry, {}, {.maximumArgumentBytes = 4}), CliErrors::InputCapacityExceeded);

        const auto noisy = std::to_array<std::string_view>({"project", "validate", "--bad-a", "--bad-b", "--bad-c"});
        auto parsed = CliOptionParser::Parse(noisy, registry, {}, {.maximumDiagnostics = 2});
        RequireError(parsed, CliErrors::ParseFailed);
        REQUIRE(parsed.ErrorValue().diagnostics.size() == 2);
    }

    TEST_CASE("CLI parser rejects sensitive argv before normalization or stdin reads", "[unit][cli][parser][security]") {
        auto descriptor = Descriptor();
        descriptor.options.push_back({.name = "credential",
                                      .shortName = 's',
                                      .summary = "Protected credential input.",
                                      .valueKind = CliOptionValueKind::Path,
                                      .sensitive = true});
        descriptor.stdinPolicy = CliStdinPolicy::JsonDocument;
        const auto registry = Registry(descriptor);
        TestPathNormalizer paths;
        TestInputReader input;
        input.payload = Bytes("{}");
        const std::array<std::vector<std::string_view>, 4> cases{{{"--credential", "fake-cli-secret-731"},
                                                                  {"--credential=fake-cli-secret-731"},
                                                                  {"-s", "fake-cli-secret-731"},
                                                                  {"-s=fake-cli-secret-731"}}};
        for (const auto &suffix : cases) {
            std::vector<std::string_view> arguments{"project", "validate", "--count", "2"};
            arguments.insert(arguments.end(), suffix.begin(), suffix.end());
            const auto parsed = CliOptionParser::Parse(arguments, registry,
                                                       {.pathNormalizer = &paths,
                                                        .inputReader = &input,
                                                        .stdinSelected = true,
                                                        .selectedStdinPolicy = CliStdinPolicy::JsonDocument});
            RequireError(parsed, CliErrors::ParseFailed);
            REQUIRE(parsed.ErrorValue().diagnostics.size() == 1);
            REQUIRE(parsed.ErrorValue().diagnostics.front().code.Value() == "cli.sensitive_argument_forbidden");
            REQUIRE(parsed.ErrorValue().diagnostics.front().message.find("fake-cli-secret-731") == std::string::npos);
        }
        REQUIRE(paths.calls == 0);
        REQUIRE(input.calls == 0);
    }

    TEST_CASE("CLI shared options have one deterministic backend-neutral schema", "[unit][cli][parser]") {
        const auto common = CliOptionParser::CommonOptions();
        REQUIRE(common.size() == 5);
        REQUIRE(common[0].name == "log-level");
        REQUIRE(common[1].name == "non-interactive");
        REQUIRE(common[2].name == "output");
        REQUIRE(common[3].name == "project");
        REQUIRE(common[4].name == "timeout-ms");
        REQUIRE(common[2].configurationKey == "cli.output.format");
    }

    TEST_CASE("CLI parser admits only supported positive bounded invocation timeouts before stdin", "[unit][cli][parser][timeout]") {
        auto descriptor = Descriptor();
        descriptor.stdinPolicy = CliStdinPolicy::JsonDocument;
        const auto registry = Registry(descriptor);
        TestInputReader input;
        input.payload = Bytes("{}");
        const auto accepted = std::to_array<std::string_view>({"project", "validate", "--count", "2", "--timeout-ms", "250"});
        REQUIRE(CliOptionParser::Parse(accepted, registry).HasValue());
        for (const auto milliseconds : {"0", "-1", "5001", "86400001", "9223372036854775807"}) {
            const std::array<std::string_view, 6> arguments{"project", "validate", "--count", "2", "--timeout-ms", milliseconds};
            RequireError(CliOptionParser::Parse(arguments, registry,
                                                {.inputReader = &input,
                                                 .stdinSelected = true,
                                                 .selectedStdinPolicy = CliStdinPolicy::JsonDocument}),
                         CliErrors::ParseFailed);
        }
        CHECK(input.calls == 0);
        descriptor.timeout = {};
        RequireError(CliOptionParser::Parse(accepted, Registry(descriptor)), CliErrors::ParseFailed);
    }
}  // namespace Horo::Cli
