#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "JsonUtils.h"
#include "UiTemplateCodec.h"

#include <new>

namespace Horo::UiTemplates {
    namespace {
        using namespace Detail::Codec;

        /** @brief Encodes every aggregate policy bound; absence never silently restores defaults. */
        Json WriteLimits(const UiTemplateLimits &limits) {
            return Json::array({limits.maximumElements, limits.maximumDepth, limits.maximumInstances, limits.maximumParameters,
                                limits.maximumSlots, limits.maximumProperties, limits.maximumDependencies, limits.maximumPayloadBytes,
                                limits.maximumUndoEntries});
        }

        /** @brief Requires exact persisted admission policy before allocating decoded candidate vectors. */
        void CheckLimits(const Json &value, const UiTemplateLimits &limits) {
            const auto &tuple = Tuple(value, 9);
            UiTemplateLimits decoded{Integer<std::size_t>(tuple[0]), Integer<std::size_t>(tuple[1]), Integer<std::size_t>(tuple[2]),
                                     Integer<std::size_t>(tuple[3]), Integer<std::size_t>(tuple[4]), Integer<std::size_t>(tuple[5]),
                                     Integer<std::size_t>(tuple[6]), Integer<std::size_t>(tuple[7]), Integer<std::size_t>(tuple[8])};
            if (!decoded.IsValid() || decoded != limits)
                Reject();
        }

        /** @brief Preserves ordinary document parser ceilings under the aggregate admission policy. */
        Runtime::Ui::UiDocumentSerializationLimits OrdinaryLimits(const UiTemplateLimits &limits) {
            Runtime::Ui::UiDocumentSerializationLimits result;
            result.maximumSourceBytes = limits.maximumPayloadBytes;
            result.maximumElements = limits.maximumElements;
            return result;
        }

        /** @brief Rejects duplicate fields, excessive structural depth and node counts during DOM construction. */
        Json Parse(const std::string_view source) {
            Foundation::JsonParseGuard guard{256};
            std::size_t nodes = 0;
            auto callback = [&](const int depth, const Json::parse_event_t event, const Json &value) {
                if (++nodes > 262144 || !guard(depth, event, value))
                    Reject();
                return true;
            };
            return Json::parse(source, callback);
        }
    }  // namespace

    /** @copydoc SerializeTemplateDocument */
    Result<std::string> SerializeTemplateDocument(const UiTemplateDocumentSnapshot &snapshot) {
        try {
            const auto &limits = snapshot.Limits();
            const auto ordinary = Take(Runtime::Ui::SerializeUiDocument(snapshot.Source().ordinary, OrdinaryLimits(limits)));
            Json root{{"schema", 1},
                      {"identityAlgorithm", 1},
                      {"limits", WriteLimits(limits)},
                      {"ordinary", Parse(ordinary)},
                      {"instances", WriteRecords(snapshot.Source().instances)},
                      {"rich", WriteRich(snapshot.Source())}};
            auto bytes = root.dump();
            if (bytes.size() > limits.maximumPayloadBytes)
                return Result<std::string>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            return Result<std::string>::Success(std::move(bytes));
        } catch (const Failure &failure) {
            return Result<std::string>::Failure(failure.error);
        } catch (const Json::exception &) {
            return Result<std::string>::Failure(MakeError(UiErrors::TemplatePersistenceMalformed));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc DeserializeTemplateDocument */
    Result<std::unique_ptr<UiTemplateDocumentOwner>> DeserializeTemplateDocument(const std::string_view source,
                                                                                 const std::span<const UiTemplateCatalogLease> leases,
                                                                                 std::shared_ptr<const UiTemplateCatalog> authoringSchemas,
                                                                                 const UiTemplateLimits &limits) {
        using Output = Result<std::unique_ptr<UiTemplateDocumentOwner>>;
        try {
            if (!limits.IsValid() || source.size() > limits.maximumPayloadBytes)
                return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            if (source.empty() || !IsValidUtf8ScalarSequence(source))
                Reject();
            const auto root = Parse(source);
            Fields(root, {"schema", "identityAlgorithm", "limits", "ordinary", "instances", "rich"});
            if (Integer<std::uint32_t>(root.at("schema")) != 1 || Integer<std::uint32_t>(root.at("identityAlgorithm")) != 1)
                Reject();
            CheckLimits(root.at("limits"), limits);
            auto ordinary = Take(Runtime::Ui::DeserializeUiDocument(root.at("ordinary").dump(), OrdinaryLimits(limits)));
            UiTemplateAuthoredDocument candidate{std::move(ordinary), ReadRecords(root.at("instances"), limits)};
            ReadRich(root.at("rich"), candidate, limits);
            return UiTemplateDocumentOwner::Create(std::move(candidate), leases, std::move(authoringSchemas), limits);
        } catch (const Failure &failure) {
            return Output::Failure(failure.error);
        } catch (const Json::exception &) {
            return Output::Failure(MakeError(UiErrors::TemplatePersistenceMalformed));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }
}  // namespace Horo::UiTemplates
