#include "../cooked/AnimationCookedFormat.h"
#include "AnimationAssetSourceInternal.h"

#include <ostream>
#include <streambuf>

namespace Horo::Animation::AssetSourceInternal {
    void Check(const AnimationCookedContext &context, const AnimationAssetSourceLimits &limits) {
        if (!context.accepting)
            Reject(AnimationAssetErrors::Unavailable);
        if (context.cancellation.IsCancellationRequested())
            Reject(AnimationAssetErrors::Cancelled);
        if (ValidateContext(context).HasError())
            Reject(AnimationAssetErrors::Capacity);
        if (limits.bytes == 0 || limits.bytes > 16U * 1024U * 1024U)
            Reject(AnimationAssetErrors::Capacity);
        if (limits.values == 0 || limits.values > 2U * 1024U * 1024U)
            Reject(AnimationAssetErrors::Capacity);
        if (limits.depth == 0 || limits.depth > 32)
            Reject(AnimationAssetErrors::Capacity);
    }

    namespace {
        struct ParseBudget final {
            const AnimationCookedContext &context;
            const AnimationAssetSourceLimits &limits;
            std::vector<std::set<std::string>> keys;
            std::uint64_t values{};

            void Charge(int depth, const Json &value) {
                if (context.cancellation.IsCancellationRequested())
                    Reject(AnimationAssetErrors::Cancelled);
                if (depth < 0 || static_cast<std::size_t>(depth) > limits.depth)
                    Reject(AnimationAssetErrors::Capacity);
                ++values;
                if (values > limits.values || values > context.limits.values || values > context.limits.ownedBytes / 128)
                    Reject(AnimationAssetErrors::Capacity);
                if (value.is_string() && value.get_ref<const std::string &>().size() > context.limits.stringBytes)
                    Reject(AnimationAssetErrors::Capacity);
            }

            bool Event(int depth, Json::parse_event_t event, Json &value) {
                Charge(depth, value);
                if (event == Json::parse_event_t::object_start)
                    keys.emplace_back();
                if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
                    Reject(AnimationAssetErrors::Malformed);
                if (event == Json::parse_event_t::object_end)
                    keys.pop_back();
                return true;
            }
        };

        class BoundedOutput final : public std::streambuf {
        public:
            BoundedOutput(const AnimationCookedContext &context, std::size_t maximum) : context_(context), maximum_(maximum) {}

            std::string Take() {
                return std::move(bytes_);
            }

        private:
            std::streamsize xsputn(const char *bytes, std::streamsize count) override {
                if (context_.cancellation.IsCancellationRequested())
                    Reject(AnimationAssetErrors::Cancelled);
                if (count < 0 || static_cast<std::uint64_t>(count) > maximum_ - bytes_.size())
                    Reject(AnimationAssetErrors::Capacity);
                bytes_.append(bytes, static_cast<std::size_t>(count));
                return count;
            }

            int_type overflow(int_type character) override {
                if (traits_type::eq_int_type(character, traits_type::eof()))
                    return traits_type::not_eof(character);
                const char byte = traits_type::to_char_type(character);
                xsputn(&byte, 1);
                return character;
            }

            const AnimationCookedContext &context_;
            std::size_t maximum_;
            std::string bytes_;
        };
    }  // namespace

    Json Parse(std::string_view source, const AnimationCookedContext &context, const AnimationAssetSourceLimits &limits) {
        Check(context, limits);
        if (source.empty())
            Reject(AnimationAssetErrors::Malformed);
        if (source.size() > limits.bytes)
            Reject(AnimationAssetErrors::Capacity);
        if (source.starts_with("\xEF\xBB\xBF") || source.find('\0') != std::string_view::npos)
            Reject(AnimationAssetErrors::Malformed);
        ParseBudget budget{context, limits, {}, 0};
        return Json::parse(source.begin(), source.end(), [&budget](int depth, Json::parse_event_t event, Json &value) {
            return budget.Event(depth, event, value);
        });
    }

    std::string Emit(const Json &value, const AnimationCookedContext &context, const AnimationAssetSourceLimits &limits) {
        BoundedOutput buffer(context, std::min<std::uint64_t>(limits.bytes, context.limits.ownedBytes / 2));
        std::ostream output(&buffer);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << value << '\n';
        output.flush();
        return buffer.Take();
    }
}  // namespace Horo::Animation::AssetSourceInternal
