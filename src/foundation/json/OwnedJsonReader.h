#pragma once
/** @file OwnedJsonReader.h
 * @brief Allocation-safe DOM ownership adapter over the SAME pinned parser and canonical duplicate/depth validation.
 */
#include "CanonicalJsonWriter.h"
#include "JsonUtils.h"

#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::JsonEncoding::Detail {
    /** @brief Staged children remain move-only owners until every destination container slot exists. */
    class OwnedJsonSax final : public nlohmann::json_sax<nlohmann::json> {
        using Json = nlohmann::json;

        /** @brief Noexcept-movable staging prevents structured JSON copies during vector growth. */
        struct Child final {
            Child(std::string key, OwnedJsonValue value) noexcept : key(std::move(key)), value(std::move(value)) {}

            Child(Child &&) noexcept = default;
            Child &operator=(Child &&) noexcept = default;
            Child(const Child &) = delete;
            Child &operator=(const Child &) = delete;
            std::string key;
            OwnedJsonValue value;
        };

        /** @brief Frames own children rather than attaching them to a potentially reallocating object/vector. */
        struct Frame final {
            explicit Frame(bool object) noexcept : object(object) {}

            Frame(Frame &&) noexcept = default;
            Frame &operator=(Frame &&) noexcept = default;
            Frame(const Frame &) = delete;
            Frame &operator=(const Frame &) = delete;
            bool object{};
            bool pendingKey{};
            std::string key;
            std::vector<Child> children;
        };

        static_assert(std::is_nothrow_move_constructible_v<Child> && std::is_nothrow_move_assignable_v<Child>);
        static_assert(std::is_nothrow_move_constructible_v<Frame> && std::is_nothrow_move_assignable_v<Frame>);

        /** @brief Input bytes already bound token count; this bound cannot reject a syntactically valid in-budget JSON value. */
        bool Charge() noexcept {
            return nodes_++ < maximumNodes_;
        }

        /** @brief Retains the complete child while staging allocation may fail. */
        bool Adopt(OwnedJsonValue value) {
            if (frames_.empty()) {
                if (complete_)
                    return false;
                root_.Get().swap(value.Get());
                complete_ = true;
                return true;
            }
            auto &parent = frames_.back();
            if (parent.object && !parent.pendingKey)
                return false;
            parent.children.emplace_back(std::move(parent.key), std::move(value));
            parent.pendingKey = false;
            return true;
        }

        /** @brief Creates ALL null array slots before any structured child is swapped into the final container. */
        static OwnedJsonValue Array(Frame &frame) {
            OwnedJsonValue result{Json(Json::value_t::array)};
            auto &values = result.Get().get_ref<Json::array_t &>();
            values.resize(frame.children.size());
            for (std::size_t index = 0; index < values.size(); ++index)
                values[index].swap(frame.children[index].value.Get());
            return result;
        }

        /** @brief Creates ALL null object slots first; ordered-map relocation can only copy null JSON values.
         * Staging retains original key event order and every structured child through all potentially allocating operations.
         */
        static OwnedJsonValue Object(Frame &frame) {
            OwnedJsonValue result{Json(Json::value_t::object)};
            auto &values = result.Get().get_ref<Json::object_t &>();
            for (const auto &child : frame.children)
                values.emplace(child.key, Json());
            for (auto &child : frame.children)
                values.find(child.key)->second.swap(child.value.Get());
            return result;
        }

        /** @brief Finalizes only a complete container and moves its still-owned value into its parent staging. */
        bool Close(bool object) {
            if (frames_.empty() || frames_.back().object != object || frames_.back().pendingKey)
                return false;
            auto frame = std::move(frames_.back());
            frames_.pop_back();
            auto value = object ? Object(frame) : Array(frame);
            return Adopt(std::move(value));
        }

        /** @brief Bounded container entry never creates a native partially owned structured DOM. */
        bool Open(bool object) {
            if (!Charge() || frames_.size() >= maximumDepth_)
                return false;
            frames_.emplace_back(object);
            return true;
        }

        /** @brief Scalar JSON temporaries never own structured children and retire without allocation. */
        template <typename Value> bool Scalar(Value value) {
            return Charge() && Adopt(OwnedJsonValue{Json(value)});
        }

    public:
        /** @brief Captures actual input-derived token and validated depth bounds without allocating. */
        OwnedJsonSax(std::size_t maximumNodes, std::size_t maximumDepth) noexcept
            : maximumNodes_(maximumNodes), maximumDepth_(maximumDepth) {}

        /** @brief Returns a complete root only after strict SAX consumption; all failure cleanup remains owned. */
        [[nodiscard]] std::optional<OwnedJsonValue> Take() {
            if (!complete_ || !frames_.empty())
                return std::nullopt;
            return std::optional<OwnedJsonValue>{std::move(root_)};
        }

        /** @brief Preserves the pinned parser's null DOM type. */
        bool null() override {
            return Scalar(nullptr);
        }

        /** @brief Preserves the pinned parser's boolean DOM type. */
        bool boolean(bool value) override {
            return Scalar(value);
        }

        /** @brief Preserves signed integer classification and its exact value. */
        bool number_integer(number_integer_t value) override {
            return Scalar(value);
        }

        /** @brief Preserves unsigned integer classification without narrowing high bits. */
        bool number_unsigned(number_unsigned_t value) override {
            return Scalar(value);
        }

        /** @brief Preserves the pinned parser's floating conversion and negative-zero representation. */
        bool number_float(number_float_t value, const string_t &) override {
            return Scalar(value);
        }

        /** @brief Transfers lexer-owned text only into an already-owned scalar, then owned child staging. */
        bool string(string_t &value) override {
            if (!Charge())
                return false;
            OwnedJsonValue result{Json(Json::value_t::string)};
            result.Get().get_ref<Json::string_t &>().swap(value);
            return Adopt(std::move(result));
        }

        /** @brief JSON text cannot declare a binary DOM alternative. */
        bool binary(binary_t &) override {
            return false;
        }

        /** @brief The actual parser's object event selects the final DOM container kind. */
        bool start_object(std::size_t) override {
            return Open(true);
        }

        /** @brief Transfers the exact decoded key without changing object event order. */
        bool key(string_t &value) override {
            if (!Charge() || frames_.empty() || !frames_.back().object || frames_.back().pendingKey)
                return false;
            auto &frame = frames_.back();
            frame.key.clear();
            frame.key.swap(value);
            frame.pendingKey = true;
            return true;
        }

        /** @brief Publishes an object to its parent staging only after all null destination slots exist. */
        bool end_object() override {
            return Close(true);
        }

        /** @brief The actual parser's array event preserves sequence event order. */
        bool start_array(std::size_t) override {
            return Open(false);
        }

        /** @brief Publishes an array to its parent staging only after all null destination slots exist. */
        bool end_array() override {
            return Close(false);
        }

        /** @brief Syntax rejection exposes no partially built DOM and does not replace parser rules. */
        bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &) override {
            return false;
        }

    private:
        std::size_t maximumNodes_{};
        std::size_t maximumDepth_{};
        std::size_t nodes_{};
        bool complete_{};
        std::vector<Frame> frames_;
        OwnedJsonValue root_;
    };

    /** @brief Builds a DOM only after SAME-parser syntax/key/depth validation, retaining every partial structured value.
     * @param bytes Unchanged source bytes bounded by the owning codec.
     * @param maximumBytes Actual owning codec byte bound. @param maximumDepth Actual existing parser depth policy.
     * @return Complete owned DOM or rejection; allocation exceptions leave no unowned structured cleanup.
     * @throws std::bad_alloc When bounded lexer/guard/staging/DOM storage cannot be allocated.
     */
    inline std::optional<OwnedJsonValue> ReadOwnedJson(std::string_view bytes, std::size_t maximumBytes, std::size_t maximumDepth) {
        if (bytes.size() > maximumBytes || !maximumDepth)
            return std::nullopt;
        Foundation::JsonParseGuard guard{maximumDepth};
        if (!guard.Validate(bytes.begin(), bytes.end()))
            return std::nullopt;
        OwnedJsonSax builder{bytes.size(), maximumDepth};
        if (!nlohmann::json::sax_parse(bytes.begin(), bytes.end(), &builder))
            return std::nullopt;
        return builder.Take();
    }
}  // namespace Horo::JsonEncoding::Detail
