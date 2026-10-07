#pragma once

/** @file PackageJsonGuard.h @brief Target-private bounded JSON admission shared by portable intent and lock decoders. */
#include <array>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace Horo::Packages::Detail {
    /** @brief Rejects duplicate keys, excessive nesting and oversized strings before either package model is constructed. */
    class PackageJsonGuard final {
    public:
        /** @brief Selects the owning codec's existing key ceiling; nesting and value ceilings remain common. */
        explicit PackageJsonGuard(const std::size_t maximumKeyBytes = 64U) : maximumKeyBytes_(maximumKeyBytes) {}

        /** @brief Admits one parser event; rejection remains sticky throughout parsing. */
        bool operator()(const int depth, const nlohmann::json::parse_event_t event, const nlohmann::json &value) {
            if (!AdmitDepth(depth))
                return false;
            const auto index = static_cast<std::size_t>(depth);
            switch (event) {
                case nlohmann::json::parse_event_t::object_start:
                    keys_[index + 1U].clear();
                    break;
                case nlohmann::json::parse_event_t::key:
                    valid_ = valid_ && AdmitKey(index, value);
                    break;
                case nlohmann::json::parse_event_t::value:
                    if (value.is_string())
                        valid_ = valid_ && value.get_ref<const std::string &>().size() <= 1024U;
                    break;
                default:
                    break;
            }
            return valid_;
        }

        /** @brief Reports whether all observed parser events were admissible. @return Sticky admission result. */
        [[nodiscard]] bool IsValid() const noexcept {
            return valid_;
        }

    private:
        /** @brief Applies the codec's key limit and per-object uniqueness before retaining a key. */
        bool AdmitKey(const std::size_t index, const nlohmann::json &value) {
            return value.is_string() && value.get_ref<const std::string &>().size() <= maximumKeyBytes_ &&
                   keys_[index].insert(value.get<std::string>()).second;
        }

        /** @brief Keeps indexing inside the shared depth bound before any event accesses key storage. */
        bool AdmitDepth(const int depth) {
            if (depth < 0 || static_cast<std::size_t>(depth) >= keys_.size() - 1U)
                valid_ = false;
            return valid_;
        }

        std::array<std::set<std::string, std::less<>>, 12> keys_;
        const std::size_t maximumKeyBytes_;
        bool valid_{true};
    };
}  // namespace Horo::Packages::Detail
