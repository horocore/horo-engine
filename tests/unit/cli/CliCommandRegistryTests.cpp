#include "Horo/Cli/CliCommandRegistry.h"
#include "Horo/Cli/CliErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace Horo::Cli {
    namespace {
        constexpr CliContractVersion CurrentContract{1, 2, 0};

        [[nodiscard]] CliOptionDescriptor Flag(std::string name, const char shortName) {
            return {.name = std::move(name),
                    .shortName = shortName,
                    .summary = "Enable the operation.",
                    .valueKind = CliOptionValueKind::Flag};
        }

        [[nodiscard]] CliCommandDescriptor Descriptor(std::vector<std::string> path,
                                                      const CliCommandOrigin origin = CliCommandOrigin::BuiltIn) {
            return {.path = {std::move(path)},
                    .summary = "Perform the operation.",
                    .options = {Flag("force", 'f'),
                                {.name = "mode",
                                 .shortName = 'm',
                                 .summary = "Select the operating mode.",
                                 .valueKind = CliOptionValueKind::Enumeration,
                                 .defaultValue = "fast",
                                 .enumerationValues = {"safe", "fast"}}},
                    .requiredCapabilities = {{"horo.project.read"}},
                    .output = {.id = "horo.cli.operation-result", .version = 1, .formats = CliOutputFormat::Human | CliOutputFormat::Json},
                    .interactive = CliInteractivePolicy::Forbidden,
                    .hosts = CliHostAvailability::HoroEngine,
                    .contractVersion = CurrentContract,
                    .sideEffects = CliSideEffectPolicy::ReadsState,
                    .cancellation = CliCancellationPolicy::Cooperative,
                    .timeout = {.defaultMilliseconds = 5'000, .maximumMilliseconds = 60'000},
                    .stdinPolicy = CliStdinPolicy::None,
                    .origin = origin,
                    .ownerId = origin == CliCommandOrigin::BuiltIn ? "horo.project" : "example.extension"};
        }

        [[nodiscard]] CliCommandRegistryPolicy Policy() {
            return {.activeHost = CliHostKind::HoroEngine,
                    .supportedContractVersion = CurrentContract,
                    .grantedCapabilities = {{"horo.project.read"}, {"horo.project.write"}}};
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().domain.Value() == "horo.cli");
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }

        template <typename Mutator> void RequireOptionSchemaError(Mutator mutator) {
            CliCommandDescriptor descriptor = Descriptor({"project", "validate"});
            mutator(descriptor);
            RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OptionSchemaIncompatible);
        }

        [[nodiscard]] CliCommandRegistry CreateRegistry(const std::span<const CliCommandDescriptor> descriptors) {
            auto created = CliCommandRegistry::Create(descriptors, Policy());
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }
    }  // namespace

    TEST_CASE("CLI registry admits built-in and contributed descriptors through one deterministic path", "[unit][cli][registry]") {
        const std::array descriptors{Descriptor({"project", "validate"}), Descriptor({"asset", "inspect"}, CliCommandOrigin::Contribution)};
        const CliCommandRegistry registry = CreateRegistry(descriptors);

        REQUIRE(registry.Commands().size() == 2);
        REQUIRE((registry.Commands()[0].path.segments == std::vector<std::string>{"asset", "inspect"}));
        REQUIRE(registry.Commands()[0].origin == CliCommandOrigin::Contribution);
        REQUIRE((registry.Commands()[1].path.segments == std::vector<std::string>{"project", "validate"}));
        REQUIRE(registry.Commands()[1].origin == CliCommandOrigin::BuiltIn);
        REQUIRE(registry.Commands()[0].options[0].name == "force");
        REQUIRE((registry.Commands()[0].options[1].enumerationValues == std::vector<std::string>{"fast", "safe"}));
    }

    TEST_CASE("CLI registry rejects duplicate command paths regardless of origin", "[unit][cli][registry]") {
        const std::array descriptors{Descriptor({"project", "validate"}),
                                     Descriptor({"project", "validate"}, CliCommandOrigin::Contribution)};
        RequireError(CliCommandRegistry::Create(descriptors, Policy()), CliErrors::CommandPathDuplicate);
    }

    TEST_CASE("CLI registry rejects duplicate long and short option names", "[unit][cli][registry]") {
        CliCommandDescriptor descriptor = Descriptor({"project", "validate"});
        descriptor.options.push_back(Flag("force", 'x'));
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OptionNameDuplicate);

        descriptor = Descriptor({"project", "validate"});
        descriptor.options.push_back(Flag("verbose", 'f'));
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OptionNameDuplicate);
    }

    TEST_CASE("CLI registry rejects incompatible option schema combinations", "[unit][cli][registry]") {
        RequireOptionSchemaError([](CliCommandDescriptor &descriptor) {
            descriptor.options[0].defaultValue = "true";
        });
        RequireOptionSchemaError([](CliCommandDescriptor &descriptor) {
            descriptor.options[0].sensitive = true;
        });
        RequireOptionSchemaError([](CliCommandDescriptor &descriptor) {
            descriptor.options[1].enumerationValues.clear();
        });
        RequireOptionSchemaError([](CliCommandDescriptor &descriptor) {
            descriptor.options[1].defaultValue = "unknown";
        });
        RequireOptionSchemaError([](CliCommandDescriptor &descriptor) {
            descriptor.options[1].enumerationValues = {"fast", "fast"};
        });
        RequireOptionSchemaError([](CliCommandDescriptor &descriptor) {
            descriptor.options[1].sensitive = true;
        });
        RequireOptionSchemaError([](CliCommandDescriptor &descriptor) {
            descriptor.options[1].valueKind = CliOptionValueKind::String;
            descriptor.options[1].enumerationValues.clear();
            descriptor.options[1].defaultValue = "unsafe\ndefault";
        });
    }

    TEST_CASE("CLI registry validates typed scalar defaults", "[unit][cli][registry]") {
        CliCommandDescriptor descriptor = Descriptor({"project", "validate"});
        descriptor.options.push_back({.name = "workers",
                                      .summary = "Set the worker count.",
                                      .valueKind = CliOptionValueKind::SignedInteger,
                                      .defaultValue = "four"});
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OptionSchemaIncompatible);

        descriptor.options.back().defaultValue = "4";
        descriptor.options.push_back({.name = "ratio",
                                      .summary = "Set a finite ratio.",
                                      .valueKind = CliOptionValueKind::FloatingPoint,
                                      .defaultValue = "infinity"});
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OptionSchemaIncompatible);

        descriptor.options.back().defaultValue = "0.5";
        REQUIRE(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()).HasValue());
    }

    TEST_CASE("CLI registry rejects unauthorized and malformed capabilities", "[unit][cli][registry]") {
        CliCommandDescriptor descriptor = Descriptor({"project", "validate"});
        descriptor.requiredCapabilities.push_back({"horo.project.delete"});
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::CapabilityUnauthorized);

        descriptor = Descriptor({"project", "validate"});
        descriptor.requiredCapabilities.push_back(descriptor.requiredCapabilities.front());
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::DescriptorInvalid);

        descriptor = Descriptor({"project", "validate"});
        descriptor.requiredCapabilities[0].value = "Project.Read";
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::DescriptorInvalid);
    }

    TEST_CASE("CLI registry rejects descriptors unavailable on the active host", "[unit][cli][registry]") {
        CliCommandDescriptor descriptor = Descriptor({"project", "validate"});
        descriptor.hosts = CliHostAvailability::HoroPak;
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::HostUnsupported);

        CliCommandRegistryPolicy horoPak = Policy();
        horoPak.activeHost = CliHostKind::HoroPak;
        descriptor = Descriptor({"package", "verify"});
        descriptor.hosts = CliHostAvailability::HoroPak;
        REQUIRE(CliCommandRegistry::Create(std::span{&descriptor, 1}, horoPak).HasValue());
    }

    TEST_CASE("CLI registry validates contract compatibility before publication", "[unit][cli][registry]") {
        CliCommandDescriptor descriptor = Descriptor({"project", "validate"});
        descriptor.contractVersion = {2, 0, 0};
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::ContractVersionIncompatible);

        descriptor.contractVersion = {1, 3, 0};
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::ContractVersionIncompatible);

        descriptor.contractVersion = {1, 2, 99};
        REQUIRE(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()).HasValue());

        descriptor.contractVersion = {1, 1, 99};
        REQUIRE(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()).HasValue());
    }

    TEST_CASE("CLI discovery and lookup expose only accepted descriptors in path order", "[unit][cli][registry]") {
        const std::array descriptors{Descriptor({"project", "restore"}), Descriptor({"asset", "cook"}), Descriptor({"project", "create"})};
        const CliCommandRegistry registry = CreateRegistry(descriptors);

        REQUIRE(registry.Find({{"project", "create"}}) != nullptr);
        REQUIRE(registry.Find({{"project"}}) == nullptr);
        REQUIRE(registry.Find({{"missing"}}) == nullptr);

        const auto projects = registry.Discover({{"project"}});
        REQUIRE(projects.size() == 2);
        REQUIRE(projects[0]->path.segments.back() == "create");
        REQUIRE(projects[1]->path.segments.back() == "restore");
        REQUIRE(registry.Discover().size() == 3);
        REQUIRE(registry.Discover({{"scene"}}).empty());

        REQUIRE((registry.DiscoverNextSegments() == std::vector<std::string_view>{"asset", "project"}));
        REQUIRE((registry.DiscoverNextSegments({{"project"}}) == std::vector<std::string_view>{"create", "restore"}));
        REQUIRE(registry.DiscoverNextSegments({{"project", "create"}}).empty());
        REQUIRE(registry.DiscoverNextSegments({{"missing"}}).empty());
    }

    TEST_CASE("CLI help is generated only from the accepted deterministic inventory", "[unit][cli][registry]") {
        CliCommandDescriptor project = Descriptor({"project", "validate"});
        project.output.formats = CliOutputFormat::Human | CliOutputFormat::Json | CliOutputFormat::JsonLines;
        const std::array descriptors{project, Descriptor({"asset", "cook"})};
        const CliCommandRegistry registry = CreateRegistry(descriptors);

        REQUIRE(registry.GenerateHelp("horo-engine") == "Usage: horo-engine <command> [options]\n\nCommands:\n"
                                                        "  asset cook  Perform the operation.\n"
                                                        "  project validate  Perform the operation.\n");
        REQUIRE(registry.GenerateHelp("horo-engine", {{"project", "validate"}}) ==
                "Usage: horo-engine project validate [options]\n\n"
                "Perform the operation.\n\n"
                "Options:\n"
                "  -f, --force  Enable the operation.\n"
                "  -m, --mode <fast|safe>  Select the operating mode. (default: fast)\n\n"
                "Common options:\n"
                "  --log-level <critical|debug|error|info|trace|warning>  Set the invocation log threshold.\n"
                "  --non-interactive  Disable terminal prompts.\n"
                "  -o, --output <human|json|jsonl>  Select the output encoding. (default: human)\n"
                "  -p, --project <path>  Select the project root.\n"
                "  --timeout-ms <integer>  Bound this invocation in milliseconds.\n\n"
                "Output: human, json, jsonl\n");
        REQUIRE(registry.GenerateHelp("horo-engine", {{"not", "accepted"}}).empty());
    }

    TEST_CASE("CLI registry output and ordering do not depend on contribution order", "[unit][cli][registry]") {
        const std::array forward{Descriptor({"project", "validate"}), Descriptor({"asset", "cook"})};
        const std::array reverse{forward[1], forward[0]};
        const CliCommandRegistry first = CreateRegistry(forward);
        const CliCommandRegistry second = CreateRegistry(reverse);

        REQUIRE(first.GenerateHelp("horo-engine") == second.GenerateHelp("horo-engine"));
        REQUIRE(first.Commands()[0].path == second.Commands()[0].path);
        REQUIRE(first.Commands()[1].path == second.Commands()[1].path);
    }

    TEST_CASE("CLI registry owns accepted metadata independently of contribution storage", "[unit][cli][registry]") {
        CliCommandDescriptor descriptor = Descriptor({"project", "validate"});
        CliCommandRegistry registry = CreateRegistry(std::span{&descriptor, 1});
        descriptor.path.segments[0] = "changed";
        descriptor.options[0].name = "changed";
        descriptor.requiredCapabilities[0].value = "changed.capability";

        REQUIRE(registry.Commands()[0].path.segments[0] == "project");
        REQUIRE(registry.Commands()[0].options[0].name == "force");
        REQUIRE(registry.Commands()[0].requiredCapabilities[0].value == "horo.project.read");
    }

    TEST_CASE("CLI registry enforces bounded metadata before copying contributions", "[unit][cli][registry]") {
        const std::array descriptors{Descriptor({"project", "validate"}), Descriptor({"asset", "cook"})};
        CliCommandRegistryPolicy policy = Policy();
        policy.limits.maximumCommands = 1;
        RequireError(CliCommandRegistry::Create(descriptors, policy), CliErrors::RegistryCapacityExceeded);

        policy = Policy();
        policy.limits.maximumOptionsPerCommand = 1;
        RequireError(CliCommandRegistry::Create(std::span{descriptors}.first(1), policy), CliErrors::DescriptorInvalid);

        policy = Policy();
        policy.limits.maximumPathSegments = 1;
        RequireError(CliCommandRegistry::Create(std::span{descriptors}.first(1), policy), CliErrors::DescriptorInvalid);

        CliCommandDescriptor descriptor = Descriptor({"project", "validate"});
        policy = Policy();
        policy.limits.maximumEnumerationValues = 1;
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, policy), CliErrors::RegistryCapacityExceeded);

        policy = Policy();
        policy.limits.maximumCommands = 0;
        RequireError(CliCommandRegistry::Create(std::span{descriptors}.first(1), policy), CliErrors::DescriptorInvalid);
    }

    TEST_CASE("CLI registry rejects malformed paths output and descriptor enums", "[unit][cli][registry]") {
        CliCommandDescriptor descriptor = Descriptor({"Project", "validate"});
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::DescriptorInvalid);

        descriptor = Descriptor({"project", "validate"});
        descriptor.output.formats = CliOutputFormat::Human;
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OutputSchemaIncompatible);

        descriptor = Descriptor({"project", "validate"});
        descriptor.output.version = 0;
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OutputSchemaIncompatible);

        descriptor = Descriptor({"project", "validate"});
        descriptor.output.id = "invalid";
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::DescriptorInvalid);

        descriptor = Descriptor({"project", "validate"});
        descriptor.interactive = static_cast<CliInteractivePolicy>(255);
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::DescriptorInvalid);

        descriptor = Descriptor({"project", "validate"});
        descriptor.summary = "unsafe\nsummary";
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::DescriptorInvalid);

        descriptor = Descriptor({"project", "validate"});
        descriptor.timeout.maximumMilliseconds = 1'000;
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::DescriptorInvalid);
    }

    TEST_CASE("CLI registry admits opt-in deadlines while preserving the disabled default", "[unit][cli][registry][timeout]") {
        auto descriptor = Descriptor({"project", "validate"});
        descriptor.timeout = {.maximumMilliseconds = 5000};
        const auto registry = CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy());
        REQUIRE(registry.HasValue());
        CHECK(registry.Value().Commands().front().timeout.defaultMilliseconds == 0);
        CHECK(registry.Value().Commands().front().timeout.maximumMilliseconds == 5000);
    }

    TEST_CASE("CLI registry policy validates host contract and capability grants", "[unit][cli][registry]") {
        const CliCommandDescriptor descriptor = Descriptor({"project", "validate"});
        CliCommandRegistryPolicy policy = Policy();
        policy.activeHost = static_cast<CliHostKind>(255);
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, policy), CliErrors::DescriptorInvalid);

        policy = Policy();
        policy.supportedContractVersion = {};
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, policy), CliErrors::DescriptorInvalid);

        policy = Policy();
        policy.grantedCapabilities.push_back(policy.grantedCapabilities.front());
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, policy), CliErrors::DescriptorInvalid);
    }

    TEST_CASE("CLI generated help reflects every typed option value kind", "[unit][cli][registry]") {
        CliCommandDescriptor descriptor = Descriptor({"project", "create"});
        descriptor.options =
            {{.name = "name", .summary = "Set the project name.", .valueKind = CliOptionValueKind::String, .required = true},
             {.name = "workers",
              .summary = "Set worker counts.",
              .valueKind = CliOptionValueKind::SignedInteger,
              .repeatable = true,
              .defaultValue = "4"},
             {.name = "ratio", .summary = "Set the ratio.", .valueKind = CliOptionValueKind::FloatingPoint, .defaultValue = "0.5"},
             {.name = "root", .summary = "Set the project root.", .valueKind = CliOptionValueKind::Path}};
        const CliCommandRegistry registry = CreateRegistry(std::span{&descriptor, 1});
        const std::string help = registry.GenerateHelp("horo-engine", descriptor.path);

        REQUIRE(help.find("--name <string>  Set the project name. (required)") != std::string::npos);
        REQUIRE(help.find("--workers <integer>  Set worker counts. (default: 4) (repeatable)") != std::string::npos);
        REQUIRE(help.find("--ratio <number>  Set the ratio. (default: 0.5)") != std::string::npos);
        REQUIRE(help.find("--root <path>  Set the project root.") != std::string::npos);
    }

    TEST_CASE("CLI registry rejects malformed option host and output enum metadata", "[unit][cli][registry]") {
        CliCommandDescriptor descriptor = Descriptor({"project", "validate"});
        descriptor.options[0].shortName = '!';
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::DescriptorInvalid);

        descriptor = Descriptor({"project", "validate"});
        descriptor.options[0].valueKind = static_cast<CliOptionValueKind>(255);
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::DescriptorInvalid);

        descriptor = Descriptor({"project", "validate"});
        descriptor.options[0].enumerationValues = {"unexpected"};
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OptionSchemaIncompatible);

        descriptor = Descriptor({"project", "validate"});
        descriptor.hosts = CliHostAvailability::None;
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::DescriptorInvalid);

        descriptor = Descriptor({"project", "validate"});
        descriptor.output.formats = static_cast<CliOutputFormat>(255);
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OutputSchemaIncompatible);
    }

    TEST_CASE("CLI validation failures retain unique stable typed identities", "[unit][cli][registry]") {
        const std::array
            errors{&CliErrors::DescriptorInvalid,           &CliErrors::RegistryCapacityExceeded, &CliErrors::CommandPathDuplicate,
                   &CliErrors::OptionNameDuplicate,         &CliErrors::OptionSchemaIncompatible, &CliErrors::OutputSchemaIncompatible,
                   &CliErrors::CapabilityUnauthorized,      &CliErrors::HostUnsupported,          &CliErrors::ContractVersionIncompatible,
                   &CliErrors::ParserPolicyInvalid,         &CliErrors::CommandUnknown,           &CliErrors::ParseFailed,
                   &CliErrors::InputModeUnsupported,        &CliErrors::InputCapacityExceeded,    &CliErrors::InteractiveInputUnavailable,
                   &CliErrors::DispatchRegistrationInvalid, &CliErrors::CommandUnavailable,       &CliErrors::SideEffectUnauthorized,
                   &CliErrors::ExecutionContextInvalid,     &CliErrors::ExecutionCancelled,       &CliErrors::ExecutionTimedOut,
                   &CliErrors::ExecutionCapacityExceeded};
        for (std::size_t current = 0; current < errors.size(); ++current) {
            REQUIRE(errors[current]->domain.Value() == "horo.cli");
            REQUIRE_FALSE(errors[current]->code.Value().empty());
            for (std::size_t prior = 0; prior < current; ++prior)
                REQUIRE(errors[current]->code.Value() != errors[prior]->code.Value());
        }
    }

    TEST_CASE("CLI registry validates parser ranges positionals common names and prompt alternatives", "[unit][cli][registry]") {
        CliCommandDescriptor descriptor = Descriptor({"project", "validate"});
        descriptor.options[1].numericRange.minimumInteger = 1;
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OptionSchemaIncompatible);

        descriptor = Descriptor({"project", "validate"});
        descriptor.options.push_back(
            {.name = "project", .summary = "Conflicts with shared project.", .valueKind = CliOptionValueKind::Path});
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OptionNameDuplicate);

        descriptor = Descriptor({"project", "validate"});
        descriptor.options.push_back(
            {.name = "timeout-ms", .summary = "Conflicts with the shared deadline.", .valueKind = CliOptionValueKind::SignedInteger});
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OptionNameDuplicate);

        descriptor = Descriptor({"project", "validate"});
        descriptor.positionals = {{.name = "password",
                                   .summary = "Credential input.",
                                   .valueKind = CliOptionValueKind::String,
                                   .required = true,
                                   .sensitive = true}};
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::OptionSchemaIncompatible);

        descriptor = Descriptor({"project", "validate"});
        descriptor.interactive = CliInteractivePolicy::Required;
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::DescriptorInvalid);

        descriptor.interactiveAlternativeOption = "missing";
        RequireError(CliCommandRegistry::Create(std::span{&descriptor, 1}, Policy()), CliErrors::DescriptorInvalid);
    }
}  // namespace Horo::Cli
