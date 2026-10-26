#pragma once

#include "AnimationGraphSourceInternal.h"

#include <type_traits>

namespace Horo::Animation::GraphSourceDetail {
    /** @brief Bounded canonical text sink; owns no JSON containers with allocating destruction paths.
     * Scalar spelling/escaping still uses the existing JSON implementation. Every temporary is a
     * primitive, whose destruction only releases storage. A failed append unwinds ordinary strings.
     */
    class SourceWriter final {
    public:
        SourceWriter(const AnimationGraphCompileContext &context, const AnimationGraphSourceLimits &limits)
            : context_(context), limits_(limits) {}

        /** @brief Writes a single named child and counts its key at the same depth as the source parser. */
        template <typename T> void Field(const std::string_view key, const T &value) {
            if (depth_ > limits_.depth || values_ >= limits_.jsonValues)
                Reject(AnimationErrors::GraphLimitExceeded);
            ++values_;
            Separator();
            Append(Json(std::string(key)).dump());
            Append(":");
            Value(value);
        }

        /** @brief Emits one object in explicit field order, without building a dynamic tree. */
        template <typename F> void Object(const F &fields) {
            Container('{', '}', fields);
        }

        /** @brief Emits an explicit sequence such as clips followed by the skeleton dependency. */
        template <typename F> void Array(const F &items) {
            Container('[', ']', items);
        }

        /** @brief Emits one sibling in an explicit array body. */
        template <typename T> void Item(const T &value) {
            Separator();
            Value(value);
        }

        /** @brief Emits a complete bounded range in its already-canonical order. */
        template <typename Range, typename F> void Array(const Range &range, const F &item) {
            Container('[', ']', [&] {
                for (const auto &value : range) {
                    Separator();
                    item(value);
                }
            });
        }

        /** @brief Emits one callback-produced container or a canonically spelled primitive. */
        template <typename T> void Value(const T &value) {
            if constexpr (std::is_invocable_v<T>)
                value();
            else {
                Count(1);
                if constexpr (std::is_same_v<T, std::nullptr_t>)
                    Append("null");
                else if constexpr (std::is_convertible_v<T, std::string_view>)
                    Append(Json(std::string(std::string_view(value))).dump());
                else if constexpr (std::is_same_v<T, float>)
                    Append(Json(value == 0.0F ? 0.0F : value).dump());
                else
                    Append(Json(value).dump());
            }
        }

        /** @brief Seals the sole source representation with its required trailing newline. */
        std::string Finish() {
            Append("\n");
            CheckAdmission(context_);
            return std::move(bytes_);
        }

    private:
        /** @brief Counts the same primitive/structured events as the source parser before append. */
        void Count(const std::size_t events) {
            CheckAdmission(context_);
            if (depth_ > limits_.depth || events > limits_.jsonValues - values_)
                Reject(AnimationErrors::GraphLimitExceeded);
            values_ += events;
        }

        /** @brief Enforces the encoded-byte ceiling before allocation or partial append. */
        void Append(const std::string_view text) {
            if (text.size() > limits_.bytes - bytes_.size())
                Reject(AnimationErrors::GraphLimitExceeded);
            bytes_.append(text);
        }

        /** @brief Delimits siblings without retaining heap-backed container metadata. */
        void Separator() {
            if (!first_)
                Append(",");
            first_ = false;
        }

        /** @brief Recursion follows the fixed schema, with each nested value checked before writing. */
        template <typename F> void Container(const char begin, const char end, const F &children) {
            Count(2);
            Append(std::string_view(&begin, 1));
            const bool parentFirst = first_;
            first_ = true;
            ++depth_;
            children();
            --depth_;
            first_ = parentFirst;
            Append(std::string_view(&end, 1));
        }

        const AnimationGraphCompileContext &context_;
        const AnimationGraphSourceLimits &limits_;
        std::string bytes_;
        std::size_t depth_{};
        std::size_t values_{};
        bool first_{true};
    };
}  // namespace Horo::Animation::GraphSourceDetail
