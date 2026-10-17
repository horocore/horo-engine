#pragma once

/** @file AssetRegistryJson.h
 *  @brief Private completed-DOM ownership with preadmitted iterative cleanup storage.
 */

#include "Horo/Foundation/Assertions.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Horo::Assets::Detail {
    /** @brief Counts raw nesting outside quoted strings; canonical parsing remains the syntax authority.
     *  @param contents Already byte-bounded registry or sidecar input.
     *  @return Upper bound on simultaneously owned structured ancestors, including the root slot.
     *  @throws std::length_error When the input cannot represent one additional root slot.
     */
    inline std::size_t RegistryJsonScratchDepth(const std::string_view contents) {
        if (contents.size() == std::numeric_limits<std::size_t>::max())
            throw std::length_error("Registry JSON nesting storage exceeds addressable capacity.");
        std::size_t depth{};
        std::size_t maximum{};
        bool quoted{};
        bool escaped{};
        for (const char character : contents) {
            if (quoted) {
                if (escaped)
                    escaped = false;
                else if (character == '\\')
                    escaped = true;
                else if (character == '"')
                    quoted = false;
            } else if (character == '"') {
                quoted = true;
            } else if (character == '{' || character == '[') {
                maximum = std::max(maximum, ++depth);
            } else if ((character == '}' || character == ']') && depth != 0) {
                --depth;
            }
        }
        return maximum + 1;
    }

    /** @brief Applies the canonical JSON grammar and duplicate-key policy before any DOM is constructed.
     *  Duplicate keys are recorded without stopping SAX parsing: a subsequent syntax error retains
     *  its original precedence and parser diagnostic. No accepted nesting limit is added.
     */
    class RegistryJsonValidation final : public nlohmann::json_sax<nlohmann::json> {
    public:
        using Json = nlohmann::json;

        /** @brief Accepts a grammar-validated null without constructing a DOM value. */
        bool null() override {
            return true;
        }

        /** @brief Accepts a grammar-validated boolean without constructing a DOM value. */
        bool boolean(bool) override {
            return true;
        }

        /** @brief Accepts a grammar-validated signed number without changing numeric interpretation. */
        bool number_integer(Json::number_integer_t) override {
            return true;
        }

        /** @brief Accepts a grammar-validated unsigned number without changing numeric interpretation. */
        bool number_unsigned(Json::number_unsigned_t) override {
            return true;
        }

        /** @brief Retains canonical floating-point validation performed by the upstream lexer. */
        bool number_float(Json::number_float_t, const Json::string_t &) override {
            return true;
        }

        /** @brief Accepts a decoded string; only object keys require owned duplicate evidence. */
        bool string(Json::string_t &) override {
            return true;
        }

        /** @brief Satisfies the canonical SAX interface; JSON text never emits binary values. */
        bool binary(Json::binary_t &) override {
            return true;
        }

        /** @brief Starts one object-local key set without imposing a new depth limit. */
        bool start_object(std::size_t) override {
            frames_.push_back(std::make_unique<std::unordered_set<std::string>>());
            return true;
        }

        /** @brief Records duplicate keys while continuing complete syntax validation. */
        bool key(Json::string_t &value) override {
            duplicate_ = !frames_.back()->insert(value).second || duplicate_;
            return true;
        }

        /** @brief Releases exactly the completed object's duplicate evidence. */
        bool end_object() override {
            frames_.pop_back();
            return true;
        }

        /** @brief Tracks array nesting without assigning object-key authority to array values. */
        bool start_array(std::size_t) override {
            frames_.push_back(nullptr);
            return true;
        }

        /** @brief Releases the completed array frame. */
        bool end_array() override {
            return end_object();
        }

        /** @brief Preserves the canonical parser diagnostic before reporting malformed input. */
        bool parse_error(std::size_t, const std::string &, const Json::exception &error) override {
            error_ = error.what();
            return false;
        }

        /** @brief Reports whether any object repeated an exact decoded key. */
        [[nodiscard]] bool HasDuplicate() const noexcept {
            return duplicate_;
        }

        /** @brief Borrows the exact canonical syntax diagnostic after SAX rejection. */
        [[nodiscard]] const std::string &ErrorMessage() const noexcept {
            return error_;
        }

    private:
        std::vector<std::unique_ptr<std::unordered_set<std::string>>> frames_;
        bool duplicate_{};
        std::string error_;
    };

    /** @brief Releases admitted pointer storage without constructing checked-STL container proxies. */
    struct RegistryJsonScratchRelease final {
        std::size_t capacity;

        void operator()(nlohmann::json **nodes) const noexcept {
            std::allocator<nlohmann::json *>{}.deallocate(nodes, capacity);
        }
    };

    /** @brief Owns a completed DOM and preadmitted iterative cleanup scratch through every return and unwind.
     *  Scratch is allocated before parsing; node removal sees only already emptied children. No recursion,
     *  new scratch allocation, parser substitution or authored nesting restriction is introduced by cleanup.
     *  Upstream empty-container destruction/iterator proxies remain STL-specific: checked MSVC Debug
     *  is not claimed allocation-safe; exhaustive global-fault proof uses its consistent Release profile.
     */
    class RegistryJson final {
    public:
        using Json = nlohmann::json;

        /** @brief Reserves cleanup storage before any completed DOM can be acquired.
         *  @param contents Exact already byte-bounded input used by the canonical parser.
         *  @throws std::bad_alloc When the finite observed-depth scratch cannot be admitted.
         *  @throws std::length_error When scratch capacity cannot be represented.
         */
        explicit RegistryJson(const std::string_view contents)
            : capacity_(RegistryJsonScratchDepth(contents)),
              ancestors_(std::allocator<Json *>{}.allocate(capacity_), RegistryJsonScratchRelease{capacity_}) {
            std::uninitialized_value_construct_n(ancestors_.get(), capacity_);
        }

        /** @brief Transfers both the DOM and its preadmitted cleanup storage without allocating. */
        RegistryJson(RegistryJson &&other) noexcept
            : capacity_(std::exchange(other.capacity_, 0)), ancestors_(std::move(other.ancestors_)), value_(std::move(other.value_)) {}

        RegistryJson(const RegistryJson &) = delete;
        RegistryJson &operator=(const RegistryJson &) = delete;
        RegistryJson &operator=(RegistryJson &&) = delete;

        /** @brief Empties deepest children before upstream flattening destructors can allocate. */
        ~RegistryJson() noexcept {
            Json *current = &value_;
            std::size_t depth{};
            for (;;) {
                if (Json *child = StructuredChild(*current)) {
                    HORO_INVARIANT_MSG(depth < capacity_, "Registry JSON exceeds its preadmitted raw nesting envelope.");
                    ancestors_.get()[depth++] = current;
                    current = child;
                    continue;
                }
                current->clear();
                if (depth == 0)
                    return;
                current = ancestors_.get()[--depth];
                if (auto *parent = current->get_ptr<Json::object_t *>())
                    parent->erase(parent->begin());
                else
                    current->get_ptr<Json::array_t *>()->pop_back();
            }
        }

        /** @brief Borrows the canonical DOM within the admitted input-depth envelope. @return Mutable owned parser output. */
        [[nodiscard]] Json &Dom() noexcept {
            return value_;
        }

        /** @brief Borrows the completed canonical DOM without mutation. @return Immutable owned parser output. */
        [[nodiscard]] const Json &Dom() const noexcept {
            return value_;
        }

    private:
        /** @brief Selects one still-owned child without recursion, allocation or iterator ownership. */
        [[nodiscard]] static Json *StructuredChild(Json &value) noexcept {
            if (auto *object = value.get_ptr<Json::object_t *>(); object && !object->empty())
                return &object->begin()->second;
            if (auto *array = value.get_ptr<Json::array_t *>(); array && !array->empty())
                return &array->back();
            return nullptr;
        }

        std::size_t capacity_; /**< Raw nesting envelope; moved-from owners have zero capacity and a null DOM. */
        std::unique_ptr<Json *, RegistryJsonScratchRelease> ancestors_; /**< Admitted storage outlives the completed DOM. */
        Json value_; /**< Callers must not add deeper structured nodes than the admitted input. */
    };
}  // namespace Horo::Assets::Detail
