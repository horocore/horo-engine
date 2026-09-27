#include "Horo/Mcp/McpToolRegistry.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

namespace Horo::Mcp {
    namespace {
        constexpr std::size_t MaximumTools = 256;
        constexpr std::size_t MaximumSchemaBytes = 16U << 10U;
        constexpr std::size_t MaximumSchemaNodes = 2048;
        constexpr std::size_t MaximumSchemaDepth = 24;

        /** @brief Accepts JSON's signed and unsigned representations of nonnegative size bounds. */
        [[nodiscard]] bool ValidSizeBound(const nlohmann::json &bound) {
            return bound.is_number_integer() && (bound.is_number_unsigned() || bound.get<std::int64_t>() >= 0);
        }

        /** @brief Accepts only bounded stable identities, not paths or provider-native names. */
        [[nodiscard]] bool ValidIdentity(const std::string_view value) noexcept {
            if (value.empty() || value.size() > 128 || value.front() < 'a' || value.front() > 'z')
                return false;
            return std::ranges::all_of(value, [](const unsigned char character) {
                return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '.' ||
                       character == '_' || character == '-';
            });
        }

        /** @brief Applies a finite structural budget before serialization or recursive schema checks. */
        [[nodiscard]] bool WithinBudget(const nlohmann::json &value, const std::size_t maximumBytes, const std::size_t maximumDepth,
                                        const std::size_t maximumNodes, const std::size_t depth, std::size_t &nodes, std::size_t &bytes) {
            if (depth > maximumDepth || ++nodes > maximumNodes)
                return false;
            if (value.is_string()) {
                const auto &text = value.get_ref<const std::string &>();
                if (text.size() > maximumBytes - bytes)
                    return false;
                bytes += text.size();
            } else if (value.is_number_float() && !std::isfinite(value.get<double>())) {
                return false;
            } else if (value.is_object()) {
                for (auto it = value.begin(); it != value.end(); ++it) {
                    if (it.key().size() > maximumBytes - bytes)
                        return false;
                    bytes += it.key().size();
                    if (!WithinBudget(it.value(), maximumBytes, maximumDepth, maximumNodes, depth + 1, nodes, bytes))
                        return false;
                }
            } else if (value.is_array()) {
                for (const auto &member : value) {
                    if (!WithinBudget(member, maximumBytes, maximumDepth, maximumNodes, depth + 1, nodes, bytes))
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool MatchesSchema(const nlohmann::json &schema, const nlohmann::json &value);

        /** @brief Rejects schemas outside the deliberately supported closed JSON Schema subset. */
        [[nodiscard]] bool ValidSchemaShape(const nlohmann::json &schema, const std::size_t depth, std::size_t &nodes) {
            if (!schema.is_object() || depth > MaximumSchemaDepth || ++nodes > MaximumSchemaNodes)
                return false;
            static constexpr std::string_view keywords[]{"type",      "properties", "required", "additionalProperties",
                                                         "items",     "enum",       "minItems", "maxItems",
                                                         "minLength", "maxLength",  "minimum",  "maximum"};
            for (auto it = schema.begin(); it != schema.end(); ++it) {
                if (std::ranges::find(keywords, it.key()) == std::end(keywords))
                    return false;
            }
            const auto type = schema.find("type");
            if (type == schema.end() || !type->is_string())
                return false;
            const std::string &kind = type->get_ref<const std::string &>();
            if (kind != "object" && kind != "array" && kind != "string" && kind != "integer" && kind != "number" && kind != "boolean" &&
                kind != "null")
                return false;
            if (const auto properties = schema.find("properties"); properties != schema.end()) {
                if (kind != "object" || !properties->is_object())
                    return false;
                for (auto it = properties->begin(); it != properties->end(); ++it) {
                    if (it.key().empty() || it.key().size() > 128 || !IsValidUtf8ScalarSequence(it.key()) ||
                        !ValidSchemaShape(it.value(), depth + 1, nodes))
                        return false;
                }
            }
            if (const auto required = schema.find("required"); required != schema.end()) {
                if (kind != "object" || !required->is_array())
                    return false;
                std::vector<std::string> seen;
                for (const auto &name : *required) {
                    if (!name.is_string() || name.get_ref<const std::string &>().empty() ||
                        std::ranges::find(seen, name.get_ref<const std::string &>()) != seen.end())
                        return false;
                    seen.push_back(name.get<std::string>());
                }
            }
            if (const auto additional = schema.find("additionalProperties");
                additional != schema.end() && (kind != "object" || !additional->is_boolean()))
                return false;
            if (const auto items = schema.find("items");
                items != schema.end() && (kind != "array" || !ValidSchemaShape(*items, depth + 1, nodes)))
                return false;
            if (const auto choices = schema.find("enum"); choices != schema.end() && (!choices->is_array() || choices->empty()))
                return false;
            for (const std::string_view key : {"minItems", "maxItems", "minLength", "maxLength"}) {
                const auto bound = schema.find(std::string{key});
                if (bound != schema.end() &&
                    (!ValidSizeBound(*bound) || ((key == "minItems" || key == "maxItems") ? kind != "array" : kind != "string")))
                    return false;
            }
            for (const std::string_view key : {"minimum", "maximum"}) {
                const auto bound = schema.find(std::string{key});
                if (bound != schema.end() && (!bound->is_number() || (kind != "number" && kind != "integer")))
                    return false;
            }
            for (const auto [lower, upper] :
                 {std::pair{"minItems", "maxItems"}, std::pair{"minLength", "maxLength"}, std::pair{"minimum", "maximum"}}) {
                if (schema.contains(lower) && schema.contains(upper) && schema[lower] > schema[upper])
                    return false;
            }
            if (const auto choices = schema.find("enum"); choices != schema.end()) {
                nlohmann::json withoutEnum = schema;
                withoutEnum.erase("enum");
                for (auto it = choices->begin(); it != choices->end(); ++it) {
                    if (std::find(choices->begin(), it, *it) != it || !MatchesSchema(withoutEnum, *it))
                        return false;
                }
            }
            return true;
        }

        /** @brief Validates one value against the accepted non-referencing schema subset. */
        [[nodiscard]] bool MatchesSchema(const nlohmann::json &schema, const nlohmann::json &value) {
            const std::string &kind = schema["type"].get_ref<const std::string &>();
            const bool typeMatches = kind == "object"    ? value.is_object()
                                     : kind == "array"   ? value.is_array()
                                     : kind == "string"  ? value.is_string()
                                     : kind == "integer" ? value.is_number_integer()
                                     : kind == "number"  ? value.is_number()
                                     : kind == "boolean" ? value.is_boolean()
                                                         : value.is_null();
            if (!typeMatches || (schema.contains("enum") && std::ranges::find(schema["enum"], value) == schema["enum"].end()))
                return false;
            if (kind == "object") {
                if (schema.contains("required")) {
                    for (const auto &name : schema["required"]) {
                        if (!value.contains(name.get_ref<const std::string &>()))
                            return false;
                    }
                }
                const auto properties = schema.find("properties");
                for (auto it = value.begin(); it != value.end(); ++it) {
                    const auto property = properties == schema.end() ? nlohmann::json::const_iterator{} : properties->find(it.key());
                    if (properties != schema.end() && property != properties->end()) {
                        if (!MatchesSchema(*property, it.value()))
                            return false;
                    } else if (schema.value("additionalProperties", true) == false) {
                        return false;
                    }
                }
            } else if (kind == "array") {
                if (value.size() < schema.value("minItems", std::size_t{0}) ||
                    value.size() > schema.value("maxItems", std::numeric_limits<std::size_t>::max()))
                    return false;
                if (const auto items = schema.find("items"); items != schema.end()) {
                    for (const auto &member : value) {
                        if (!MatchesSchema(*items, member))
                            return false;
                    }
                }
            } else if (kind == "string") {
                const auto size = value.get_ref<const std::string &>().size();
                if (size < schema.value("minLength", std::size_t{0}) ||
                    size > schema.value("maxLength", std::numeric_limits<std::size_t>::max()))
                    return false;
            } else if (kind == "integer" || kind == "number") {
                if ((schema.contains("minimum") && value < schema["minimum"]) || (schema.contains("maximum") && value > schema["maximum"]))
                    return false;
            }
            return true;
        }

        /** @brief Validates both structural and serialized byte budgets. */
        [[nodiscard]] bool BoundedValue(const nlohmann::json &value, const std::size_t maximumBytes, const McpToolBounds &bounds) {
            std::size_t nodes{};
            std::size_t bytes{};
            if (!WithinBudget(value, maximumBytes, bounds.maximumDepth, bounds.maximumNodes, 0, nodes, bytes))
                return false;
            try {
                return value.dump().size() <= maximumBytes;
            } catch (const nlohmann::json::exception &) {
                return false;
            }
        }

        /** @brief Checks descriptor metadata without invoking or registering its adapter. */
        [[nodiscard]] bool ValidDescriptor(const McpToolDescriptor &descriptor) {
            if (!ValidIdentity(descriptor.id.value) || descriptor.version.major == 0 || descriptor.description.empty() ||
                descriptor.description.size() > 1024 || !IsValidUtf8ScalarSequence(descriptor.description) ||
                descriptor.effect > McpToolEffect::Mutation || descriptor.requiredCapabilities.size() > 64 ||
                descriptor.bounds.maximumInputBytes == 0 || descriptor.bounds.maximumInputBytes > (1U << 20U) ||
                descriptor.bounds.maximumResultBytes == 0 || descriptor.bounds.maximumResultBytes > (1U << 20U) ||
                descriptor.bounds.maximumDepth == 0 || descriptor.bounds.maximumDepth > 64 || descriptor.bounds.maximumNodes == 0 ||
                descriptor.bounds.maximumNodes > 65536)
                return false;
            std::vector<std::string> unique;
            for (const std::string &capability : descriptor.requiredCapabilities) {
                if (!ValidIdentity(capability) || std::ranges::find(unique, capability) != unique.end())
                    return false;
                unique.push_back(capability);
            }
            std::size_t nodes{};
            if (!BoundedValue(descriptor.inputSchema, MaximumSchemaBytes,
                              {.maximumDepth = MaximumSchemaDepth, .maximumNodes = MaximumSchemaNodes}) ||
                !ValidSchemaShape(descriptor.inputSchema, 0, nodes))
                return false;
            nodes = 0;
            return BoundedValue(descriptor.outputSchema, MaximumSchemaBytes,
                                {.maximumDepth = MaximumSchemaDepth, .maximumNodes = MaximumSchemaNodes}) &&
                   ValidSchemaShape(descriptor.outputSchema, 0, nodes);
        }

        /** @brief Checks only host-granted capability identities; trust and approval belong to later policy. */
        [[nodiscard]] bool Granted(const McpToolDescriptor &descriptor, const std::span<const std::string> capabilities) {
            return std::ranges::all_of(descriptor.requiredCapabilities, [&capabilities](const std::string &required) {
                return std::ranges::find(capabilities, required) != capabilities.end();
            });
        }
    }  // namespace

    McpToolSnapshot::McpToolSnapshot(const std::uint64_t generation, std::vector<McpToolRegistration> entries)
        : generation_(generation), entries_(std::move(entries)) {}

    /** @copydoc McpToolSnapshot::Generation */
    std::uint64_t McpToolSnapshot::Generation() const noexcept {
        return generation_;
    }

    /** @copydoc McpToolSnapshot::Discover */
    std::vector<McpToolDescriptor> McpToolSnapshot::Discover(const std::span<const std::string> capabilities) const {
        std::vector<McpToolDescriptor> visible;
        for (const auto &entry : entries_) {
            if (Granted(entry.descriptor, capabilities))
                visible.push_back(entry.descriptor);
        }
        return visible;
    }

    /** @copydoc McpToolSnapshot::Invoke */
    Result<nlohmann::json> McpToolSnapshot::Invoke(const McpToolId &id, const nlohmann::json &arguments,
                                                   const McpRequestContext &context) const {
        const auto found = std::ranges::lower_bound(entries_, id.value, {}, [](const McpToolRegistration &entry) -> const std::string & {
            return entry.descriptor.id.value;
        });
        if (found == entries_.end() || found->descriptor.id != id)
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::ToolUnavailable));
        if (!Granted(found->descriptor, context.capabilities))
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::ToolCapabilityUnavailable));
        if (!BoundedValue(arguments, found->descriptor.bounds.maximumInputBytes, found->descriptor.bounds) ||
            !MatchesSchema(found->descriptor.inputSchema, arguments))
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::ToolInputInvalid));
        try {
            auto outcome = found->adapter->Invoke(arguments, context);
            if (outcome.HasValue() &&
                (!BoundedValue(outcome.Value(), found->descriptor.bounds.maximumResultBytes, found->descriptor.bounds) ||
                 !MatchesSchema(found->descriptor.outputSchema, outcome.Value())))
                return Result<nlohmann::json>::Failure(MakeError(McpErrors::ToolOutputInvalid));
            return outcome;
        } catch (...) {
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::ControllerFailed));
        }
    }

    McpToolRegistry::McpToolRegistry() : current_(std::shared_ptr<const McpToolSnapshot>(new McpToolSnapshot(0, {}))) {}

    /** @copydoc McpToolRegistry::Publish */
    Result<std::uint64_t> McpToolRegistry::Publish(std::vector<McpToolRegistration> entries,
                                                   const std::span<const std::string> availableCapabilities) {
        if (entries.size() > MaximumTools)
            return Result<std::uint64_t>::Failure(MakeError(McpErrors::ToolDescriptorInvalid));
        std::vector<std::string_view> capabilities;
        capabilities.reserve(availableCapabilities.size());
        for (const std::string &capability : availableCapabilities) {
            if (!ValidIdentity(capability) || std::ranges::find(capabilities, capability) != capabilities.end())
                return Result<std::uint64_t>::Failure(MakeError(McpErrors::ToolDescriptorInvalid));
            capabilities.push_back(capability);
        }
        for (const auto &entry : entries) {
            if (entry.adapter == nullptr || !ValidDescriptor(entry.descriptor))
                return Result<std::uint64_t>::Failure(MakeError(McpErrors::ToolDescriptorInvalid));
            if (!Granted(entry.descriptor, availableCapabilities))
                return Result<std::uint64_t>::Failure(MakeError(McpErrors::ToolCapabilityUnavailable));
        }
        for (auto &entry : entries)
            std::ranges::sort(entry.descriptor.requiredCapabilities);
        std::ranges::sort(entries, {}, [](const McpToolRegistration &entry) -> const std::string & {
            return entry.descriptor.id.value;
        });
        for (std::size_t index = 1; index < entries.size(); ++index) {
            if (entries[index - 1].descriptor.id == entries[index].descriptor.id)
                return Result<std::uint64_t>::Failure(MakeError(McpErrors::ToolDuplicate));
        }
        std::lock_guard lock{mutex_};
        if (current_->Generation() == std::numeric_limits<std::uint64_t>::max())
            return Result<std::uint64_t>::Failure(MakeError(McpErrors::ToolIncompatible));
        for (const auto &entry : entries) {
            const auto old = std::ranges::lower_bound(current_->entries_, entry.descriptor.id.value, {},
                                                      [](const McpToolRegistration &candidate) -> const std::string & {
                return candidate.descriptor.id.value;
            });
            if (old != current_->entries_.end() && old->descriptor.id == entry.descriptor.id &&
                (entry.descriptor.version.major != old->descriptor.version.major || entry.descriptor.version < old->descriptor.version ||
                 entry.descriptor.effect != old->descriptor.effect ||
                 entry.descriptor.requiredCapabilities != old->descriptor.requiredCapabilities ||
                 entry.descriptor.inputSchema != old->descriptor.inputSchema ||
                 entry.descriptor.outputSchema != old->descriptor.outputSchema))
                return Result<std::uint64_t>::Failure(MakeError(McpErrors::ToolIncompatible));
        }
        const auto generation = current_->Generation() + 1;
        current_ = std::shared_ptr<const McpToolSnapshot>(new McpToolSnapshot(generation, std::move(entries)));
        return Result<std::uint64_t>::Success(generation);
    }

    /** @copydoc McpToolRegistry::Read */
    std::shared_ptr<const McpToolSnapshot> McpToolRegistry::Read() const {
        std::lock_guard lock{mutex_};
        return current_;
    }
}  // namespace Horo::Mcp
