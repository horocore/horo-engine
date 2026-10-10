#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::JsonEncoding::Detail {
    /**
     * @brief Private canonical wire builder whose structural cleanup never allocates.
     * @details The supported Scene and Prefab schemas fix nesting depth. Ordinary vectors own structural
     * children; destruction neither flattens them into a heap-backed stack nor throws.
     * Only scalar spelling uses the pinned JSON codec, preserving its UTF-8 escaping
     * and floating-point representation without creating a JSON object/array tree.
     */
    template <bool SortedFields> class CanonicalJsonValue final {
    public:
        using array_t = std::vector<CanonicalJsonValue>;
        using object_t = std::vector<std::pair<std::string, CanonicalJsonValue>>;

        CanonicalJsonValue() noexcept = default;

        /** @brief Copies each complete alternative before publishing it into the recursive variant. */
        CanonicalJsonValue(const CanonicalJsonValue &other) {
            // GCC 13's recursive variant copy cleanup can visit a valueless destination after allocation failure.
            // Copy outside the destination variant; publishing the completed string/vector only moves owned storage.
            std::visit([this]<typename Item>(const Item &item) {
                Item copied{item};
                value_.template emplace<Item>(std::move(copied));
            }, other.value_);
        }

        /** @brief Transfers complete owned storage without allocation. */
        CanonicalJsonValue(CanonicalJsonValue &&) noexcept = default;

        /** @brief Preserves the destination if copying any nested child fails. */
        CanonicalJsonValue &operator=(const CanonicalJsonValue &other) {
            if (this != &other) {
                CanonicalJsonValue copied{other};
                *this = std::move(copied);
            }
            return *this;
        }

        /** @brief Replaces owned storage without allocation. */
        CanonicalJsonValue &operator=(CanonicalJsonValue &&) noexcept = default;

        // Schema initializer lists intentionally convert scalar fields into owned wire values.
        explicit(false) CanonicalJsonValue(std::nullptr_t) noexcept {}

        explicit(false) CanonicalJsonValue(const bool value) noexcept : value_(value) {}

        template <std::signed_integral Value>
        explicit(false) CanonicalJsonValue(const Value value) noexcept : value_(std::int64_t{value}) {}

        template <std::unsigned_integral Value>
        explicit(false) CanonicalJsonValue(const Value value) noexcept : value_(std::uint64_t{value}) {}

        template <std::floating_point Value>
        explicit(false) CanonicalJsonValue(const Value value) noexcept : value_(static_cast<double>(value)) {}

        explicit(false) CanonicalJsonValue(const std::string &value) : value_(value) {}

        explicit(false) CanonicalJsonValue(const std::string_view value) : value_(std::string{value}) {}

        explicit(false) CanonicalJsonValue(const char *value) : value_(std::string{value}) {}

        CanonicalJsonValue(std::initializer_list<typename object_t::value_type> fields) : value_(object_t{fields}) {
            if constexpr (SortedFields)
                std::ranges::sort(std::get<object_t>(value_), {}, &object_t::value_type::first);
        }

        /** @brief Creates an array with ordinary allocation-safe container cleanup. */
        [[nodiscard]] static CanonicalJsonValue array(std::initializer_list<CanonicalJsonValue> values = {}) {
            CanonicalJsonValue result;
            result.value_ = array_t{values};
            return result;
        }

        /** @brief Creates an empty object with the codec's explicit field-order policy. */
        [[nodiscard]] static CanonicalJsonValue object() {
            CanonicalJsonValue result;
            result.value_ = object_t{};
            return result;
        }

        /** @brief Reports whether an array has no children. */
        [[nodiscard]] bool empty() const {
            return std::get<array_t>(value_).empty();
        }

        /** @brief Bridges existing internal DOM consumers; never used by safe source serialization. */
        [[nodiscard]] nlohmann::json ToJson() const {
            return std::visit([]<typename Item>(const Item &item) {
                if constexpr (std::is_same_v<Item, array_t>) {
                    auto result = nlohmann::json::array();
                    for (const auto &child : item)
                        result.push_back(child.ToJson());
                    return result;
                } else if constexpr (std::is_same_v<Item, object_t>) {
                    auto result = nlohmann::json::object();
                    for (const auto &[name, child] : item)
                        result[name] = child.ToJson();
                    return result;
                } else
                    return nlohmann::json(item);
            }, value_);
        }

        /** @brief Borrows the typed structural container during synchronous encoding. */
        template <typename Container> [[nodiscard]] Container get_ref() {
            return std::get<std::remove_reference_t<Container>>(value_);
        }

        /** @brief Appends an owned array child; failed growth preserves existing children. */
        void push_back(CanonicalJsonValue value) {
            std::get<array_t>(value_).push_back(std::move(value));
        }

        /** @brief Finds or appends a field in explicit canonical insertion order. */
        CanonicalJsonValue &operator[](const std::string &name) {
            auto &fields = std::get<object_t>(value_);
            if (const auto found = std::ranges::find(fields, name, &object_t::value_type::first); found != fields.end())
                return found->second;
            if constexpr (SortedFields) {
                const auto position = std::ranges::lower_bound(fields, name, {}, &object_t::value_type::first);
                return fields.emplace(position, name, CanonicalJsonValue{})->second;
            } else
                return fields.emplace_back(name, CanonicalJsonValue{}).second;
        }

        /** @brief Produces the same two-space pretty wire as the pinned ordered JSON codec. */
        [[nodiscard]] std::string dump(const unsigned indentation) const {
            std::string bytes;
            Write(bytes, indentation, 0);
            return bytes;
        }

    private:
        using Value = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string, array_t, object_t>;

        /** @brief Writes indentation directly into output; partial output owns no unsafe cleanup. */
        static void Indent(std::string &bytes, const unsigned indentation, const unsigned depth) {
            bytes.append(indentation * depth, ' ');
        }

        /** @brief Writes a structural array with fixed comma/newline rules, including empty arrays. */
        static void WriteArray(std::string &bytes, const array_t &values, const unsigned indentation, const unsigned depth) {
            bytes += '[';
            for (std::size_t index = 0; index < values.size(); ++index) {
                bytes += index == 0 ? "\n" : ",\n";
                Indent(bytes, indentation, depth + 1);
                values[index].Write(bytes, indentation, depth + 1);
            }
            if (!values.empty()) {
                bytes += '\n';
                Indent(bytes, indentation, depth);
            }
            bytes += ']';
        }

        /** @brief Writes fields in authored canonical order without a JSON container temporary. */
        static void WriteObject(std::string &bytes, const object_t &fields, const unsigned indentation, const unsigned depth) {
            bytes += '{';
            for (std::size_t index = 0; index < fields.size(); ++index) {
                bytes += index == 0 ? "\n" : ",\n";
                Indent(bytes, indentation, depth + 1);
                WriteString(bytes, fields[index].first);
                bytes += ": ";
                fields[index].second.Write(bytes, indentation, depth + 1);
            }
            if (!fields.empty()) {
                bytes += '\n';
                Indent(bytes, indentation, depth);
            }
            bytes += '}';
        }

        /** @brief Establishes valid string storage before a fallible copy, preserving the pinned codec's escaping. */
        static void WriteString(std::string &bytes, const std::string_view value) {
            // Compatible-type construction sets the string tag before allocating in the pinned codec.
            // The typed constructor completes storage first, so failure never destroys a null string pointer.
            nlohmann::ordered_json scalar(nlohmann::ordered_json::value_t::string);
            scalar.get_ref<std::string &>() = value;
            bytes += scalar.dump();
        }

        /** @brief Delegates only non-structural scalar spelling to the existing canonical codec. */
        void Write(std::string &bytes, const unsigned indentation, const unsigned depth) const {
            std::visit([&]<typename Item>(const Item &item) {
                if constexpr (std::is_same_v<Item, array_t>)
                    WriteArray(bytes, item, indentation, depth);
                else if constexpr (std::is_same_v<Item, object_t>)
                    WriteObject(bytes, item, indentation, depth);
                else if constexpr (std::is_same_v<Item, std::string>)
                    WriteString(bytes, item);
                else
                    bytes += nlohmann::ordered_json(item).dump();
            }, value_);
        }

        Value value_{nullptr};
    };
}  // namespace Horo::JsonEncoding::Detail
