#include "Horo/Cli/CliCommandRegistry.h"
#include "Horo/Cli/CliOptionParser.h"

#include <algorithm>

namespace Horo::Cli {
    namespace {
        /** @brief Formats admitted command segments for human usage text. */
        [[nodiscard]] std::string JoinPath(const CommandPath &path) {
            std::string result;
            for (const std::string &segment : path.segments) {
                if (!result.empty())
                    result.push_back(' ');
                result.append(segment);
            }
            return result;
        }

        /** @brief Presents only the descriptor-declared output formats. */
        void AppendFormats(std::string &help, const CliOutputFormat formats) {
            using enum CliOutputFormat;
            help.append("Output: human");
            if ((formats & Json) != None)
                help.append(", json");
            if ((formats & JsonLines) != None)
                help.append(", jsonl");
            help.push_back('\n');
        }

        /** @brief Presents the typed option value grammar. */
        void AppendOptionValue(std::string &help, const CliOptionDescriptor &option) {
            using enum CliOptionValueKind;
            switch (option.valueKind) {
                case Flag:
                    return;
                case String:
                    help.append(" <string>");
                    return;
                case SignedInteger:
                    help.append(" <integer>");
                    return;
                case FloatingPoint:
                    help.append(" <number>");
                    return;
                case Path:
                    help.append(" <path>");
                    return;
                case Enumeration:
                    help.append(" <");
                    for (std::size_t index = 0; index < option.enumerationValues.size(); ++index) {
                        if (index > 0)
                            help.push_back('|');
                        help.append(option.enumerationValues[index]);
                    }
                    help.push_back('>');
                    return;
            }
        }

        /** @brief Presents admitted source and repetition policy without sensitive defaults. */
        void AppendOptionPolicy(std::string &help, const CliOptionDescriptor &option) {
            if (option.required)
                help.append(" (required)");
            else if (option.defaultValue.has_value() && !option.sensitive)
                help.append(" (default: ").append(*option.defaultValue).push_back(')');
            if (option.repeatable)
                help.append(" (repeatable)");
        }

        /** @brief Projects option metadata into human help lines. */
        void AppendOptions(std::string &help, const std::span<const CliOptionDescriptor> options) {
            for (const CliOptionDescriptor &option : options) {
                help.append("  ");
                if (option.shortName.has_value()) {
                    help.push_back('-');
                    help.push_back(*option.shortName);
                    help.append(", ");
                }
                help.append("--").append(option.name);
                AppendOptionValue(help, option);
                help.append("  ").append(option.summary);
                AppendOptionPolicy(help, option);
                help.push_back('\n');
            }
        }

        /** @brief Matches a discovery prefix against admitted command metadata. */
        [[nodiscard]] bool HasPrefix(const CommandPath &path, const CommandPath &prefix) noexcept {
            return prefix.segments.size() <= path.segments.size() &&
                   std::ranges::equal(prefix.segments, std::span{path.segments}.first(prefix.segments.size()));
        }

    }  // namespace

    /** @copydoc CliCommandRegistry::Discover */
    std::vector<const CliCommandDescriptor *> CliCommandRegistry::Discover(const CommandPath &prefix) const {
        std::vector<const CliCommandDescriptor *> discovered;
        discovered.reserve(commands_.size());
        for (const CliCommandDescriptor &descriptor : commands_) {
            if (HasPrefix(descriptor.path, prefix))
                discovered.push_back(&descriptor);
        }
        return discovered;
    }

    /** @copydoc CliCommandRegistry::DiscoverNextSegments */
    std::vector<std::string_view> CliCommandRegistry::DiscoverNextSegments(const CommandPath &prefix) const {
        std::vector<std::string_view> segments;
        for (const CliCommandDescriptor &descriptor : commands_) {
            if (!HasPrefix(descriptor.path, prefix) || descriptor.path.segments.size() == prefix.segments.size())
                continue;
            const std::string_view candidate = descriptor.path.segments[prefix.segments.size()];
            if (std::ranges::find(segments, candidate) == segments.end())
                segments.push_back(candidate);
        }
        std::ranges::sort(segments);
        return segments;
    }

    /** @copydoc CliCommandRegistry::GenerateHelp */
    std::string CliCommandRegistry::GenerateHelp(const std::string_view programName, const CommandPath &path) const {
        std::string help;
        if (path.segments.empty()) {
            help.append("Usage: ").append(programName).append(" <command> [options]\n\nCommands:\n");
            for (const CliCommandDescriptor &descriptor : commands_)
                help.append("  ").append(JoinPath(descriptor.path)).append("  ").append(descriptor.summary).push_back('\n');
            return help;
        }

        const CliCommandDescriptor *descriptor = Find(path);
        if (!descriptor)
            return {};
        help.append("Usage: ").append(programName).push_back(' ');
        help.append(JoinPath(path));
        if (!descriptor->options.empty())
            help.append(" [options]");
        for (const CliPositionalDescriptor &positional : descriptor->positionals) {
            help.append(positional.required ? " <" : " [").append(positional.name);
            if (positional.repeatable)
                help.append("...");
            help.push_back(positional.required ? '>' : ']');
        }
        help.append("\n\n").append(descriptor->summary).push_back('\n');
        if (!descriptor->options.empty()) {
            help.append("\nOptions:\n");
            AppendOptions(help, descriptor->options);
        }
        help.append("\nCommon options:\n");
        AppendOptions(help, CliOptionParser::CommonOptions());
        help.push_back('\n');
        AppendFormats(help, descriptor->output.formats);
        return help;
    }
}  // namespace Horo::Cli
