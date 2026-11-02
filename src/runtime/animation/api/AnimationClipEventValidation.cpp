#include "AnimationClipEventsInternal.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <type_traits>

namespace Horo::Animation::Detail {
    namespace {
        /** @brief Stable clip-owned rejection does not expose untrusted payload names. */
        Result<void> Invalid(const std::string_view text) {
            return Result<void>::Failure(MakeError(AnimationErrors::ClipMalformed, std::string{text}));
        }

        /** @brief Validates finite value alternatives without lossy conversions or raw host layout. */
        bool Value(const AnimationEventValue &value) noexcept {
            return std::visit([]<typename T>(const T &item) {
                if constexpr (std::is_same_v<T, double>)
                    return std::isfinite(item);
                else if constexpr (std::is_same_v<T, Math::Vec3>)
                    return Math::IsFinite(item);
                else if constexpr (std::is_same_v<T, Assets::AssetId>)
                    return item.IsValid();
                else
                    return true;
            }, value);
        }

        /** @brief Field schemas preserve exact closed type and optional adjacent-owner allowed value set. */
        bool SchemaField(AnimationEventFieldSchema &field) {
            if (!field.id.IsValid() || field.kind >= AnimationEventValueKind::Count ||
                field.allowedUnsigned.size() > AnimationClipHardLimits::AllowedEventValues)
                return false;
            if (!field.allowedUnsigned.empty() && field.kind != AnimationEventValueKind::UnsignedInteger)
                return false;
            std::ranges::sort(field.allowedUnsigned);
            return std::adjacent_find(field.allowedUnsigned.begin(), field.allowedUnsigned.end()) == field.allowedUnsigned.end();
        }

        /** @brief Requires unique exact field IDs and finite per-schema storage before marker lookup. */
        bool Schema(AnimationEventSchema &schema) {
            if (!schema.type.IsValid() || schema.revision == 0 || schema.fields.size() > AnimationEventPayload::MaximumFields)
                return false;
            std::ranges::sort(schema.fields, {}, &AnimationEventFieldSchema::id);
            AnimationEventFieldId previous{};
            for (auto &field : schema.fields) {
                if (field.id == previous || !SchemaField(field))
                    return false;
                previous = field.id;
            }
            return true;
        }

        /** @brief One payload field must match exact schema type and closed allowed-value set. */
        bool PayloadField(const AnimationEventField &field, const AnimationEventSchema &schema) {
            const auto found = std::ranges::lower_bound(schema.fields, field.id, {}, &AnimationEventFieldSchema::id);
            if (!field.id.IsValid() || found == schema.fields.end() || found->id != field.id)
                return false;
            if (field.value.index() != static_cast<std::size_t>(found->kind) || !Value(field.value))
                return false;
            return found->allowedUnsigned.empty() ||
                   std::ranges::binary_search(found->allowedUnsigned, std::get<std::uint64_t>(field.value));
        }

        /** @brief Required fields and unused fixed storage cannot hide unvalidated values. */
        bool CompletePayload(const AnimationEventPayload &payload, const AnimationEventSchema &schema) {
            const auto values = std::span{payload.fields}.first(payload.count);
            for (const auto &field : schema.fields)
                if (field.required && std::ranges::find(values, field.id, &AnimationEventField::id) == values.end())
                    return false;
            for (std::size_t i = payload.count; i < payload.fields.size(); ++i)
                if (payload.fields[i] != AnimationEventField{})
                    return false;
            return true;
        }

        /** @brief Canonical fixed field map contains each exact declared field at most once. */
        bool Payload(AnimationClipEvent &event, const AnimationEventSchema &schema) {
            auto &payload = event.payload;
            if (payload.count > AnimationEventPayload::MaximumFields)
                return false;
            auto values = std::span{payload.fields}.first(payload.count);
            std::ranges::sort(values, {}, &AnimationEventField::id);
            AnimationEventFieldId previous{};
            for (const auto &field : values) {
                if (field.id == previous || !PayloadField(field, schema))
                    return false;
                previous = field.id;
            }
            return CompletePayload(payload, schema);
        }

        /** @brief Names are bounded immutable metadata, never parsed by a hot consumer. */
        bool Name(const std::string &text) noexcept {
            if (text.empty() || text.size() > AnimationClipHardLimits::EventNameBytes)
                return false;
            return std::ranges::all_of(text, [](unsigned char c) {
                return c >= 32 && c < 127;
            });
        }

        /** @brief Closed clip-local point/window range admits exact endpoints without overflowing duration arithmetic. */
        bool Timing(const AnimationClipEvent &event, const AnimationClipDescriptor &clip) noexcept {
            if (event.time.ticks < 0 || event.time.ticks > clip.duration.ticks)
                return false;
            return !event.duration || (event.duration->ticks > 0 && event.duration->ticks <= clip.duration.ticks - event.time.ticks);
        }
    }  // namespace

    namespace {
        /** @brief Canonical exact schema revisions are unique before event payload resolution. */
        bool Schemas(AnimationClipData &data) {
            std::ranges::sort(data.eventSchemas, [](const auto &a, const auto &b) {
                return std::tie(a.type, a.revision) < std::tie(b.type, b.revision);
            });
            std::pair<AnimationEventTypeId, std::uint32_t> previous{};
            for (auto &schema : data.eventSchemas) {
                const std::pair key{schema.type, schema.revision};
                if (key == previous || !Schema(schema))
                    return false;
                previous = key;
            }
            return true;
        }

        /** @brief Marker identity and exact schema bind remain immutable with this clip publication. */
        bool Events(AnimationClipData &data) {
            std::set<AnimationEventMarkerId> markers;
            for (auto &event : data.events) {
                if (!event.id.IsValid() || !markers.insert(event.id).second || !Name(event.name) || !Timing(event, data.descriptor))
                    return false;
                const auto schema = std::ranges::find_if(data.eventSchemas, [&](const auto &value) {
                    return value.type == event.type && value.revision == event.schemaRevision;
                });
                if (schema == data.eventSchemas.end() || !Payload(event, *schema))
                    return false;
            }
            return true;
        }

        /** @brief Bounds all authored metadata before canonical sorting or temporary allocation. */
        bool MetadataFits(const AnimationClipData &data, const AnimationClipLimits &limits) noexcept {
            return limits.maximumEvents <= AnimationClipHardLimits::Events &&
                   limits.maximumEventSchemas <= AnimationClipHardLimits::EventSchemas && data.events.size() <= limits.maximumEvents &&
                   data.eventSchemas.size() <= limits.maximumEventSchemas;
        }
    }  // namespace

    /** @copydoc ValidateClipEvents */
    Result<void> ValidateClipEvents(AnimationClipData &data, const AnimationClipLimits &limits) {
        if (!MetadataFits(data, limits))
            return Result<void>::Failure(MakeError(AnimationErrors::ClipLimitExceeded));
        if (!Schemas(data))
            return Invalid("The event schema is malformed or duplicated.");
        if (!Events(data))
            return Invalid("The event marker range or typed payload does not match its exact immutable schema.");
        std::ranges::sort(data.events, [](const auto &a, const auto &b) {
            return std::tie(a.time, a.id) < std::tie(b.time, b.id);
        });
        return Result<void>::Success();
    }
}  // namespace Horo::Animation::Detail
