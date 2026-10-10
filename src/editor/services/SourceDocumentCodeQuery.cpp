#include "Horo/Editor/SourceDocumentCodeQuery.h"

#include <thread>

namespace Horo::Editor {
    /** @copydoc MakeSourceDocumentCodeQueryProviders */
    Application::CodeQueryProviders MakeSourceDocumentCodeQueryProviders(std::shared_ptr<const SourceDocumentService> documents,
                                                                         std::shared_ptr<const DocumentIdentityRegistry> identities,
                                                                         std::string projectIdentity,
                                                                         const std::uint64_t projectGeneration) {
        Application::CodeQueryProviders providers;
        if (!documents || !identities)
            return providers;
        providers.existingText =
            [documents = std::move(documents), identities = std::move(identities), owner = std::this_thread::get_id(),
             identity = std::move(projectIdentity),
             projectGeneration](const std::string_view path,
                                const Application::CodeQueryContext &context) -> Result<std::optional<Application::CodeQueryTextSnapshot>> {
            using Observation = std::optional<Application::CodeQueryTextSnapshot>;
            if (std::this_thread::get_id() != owner)
                return Result<Observation>::Failure(MakeError(SourceDocumentErrors::WrongThread));
            if (context.projectIdentity != identity || context.projectGeneration != projectGeneration)
                return Result<Observation>::Failure(MakeError(Application::CodeQueryErrors::Stale));
            if (auto stop = Platform::CheckProjectReadContext(context); stop.HasError())
                return Result<Observation>::Failure(stop.ErrorValue());
            auto source = SourceDocumentId::Parse(path);
            if (source.HasError())
                return Result<Observation>::Failure(source.ErrorValue());
            const auto existing = identities->Find(DocumentOpenKey{DocumentKind::Source, std::move(source).Value()});
            if (!existing)
                return Result<Observation>::Success(std::nullopt);
            auto snapshot = documents->Snapshot(existing->instance);
            if (snapshot.HasError())
                return Result<Observation>::Failure(snapshot.ErrorValue());
            if (snapshot.Value().Identity() != *existing || snapshot.Value().Revision() == 0)
                return Result<Observation>::Failure(MakeError(Application::CodeQueryErrors::Stale));
            if (snapshot.Value().Text().size() > (1U << 20U))
                return Result<Observation>::Failure(MakeError(Application::CodeQueryErrors::Capacity));
            auto bytes = std::make_shared<const std::string>(snapshot.Value().Text());
            const std::string revision =
                "source:" + std::to_string(existing->instance.Value()) + ':' + std::to_string(snapshot.Value().Revision());
            if (auto stop = Platform::CheckProjectReadContext(context); stop.HasError())
                return Result<Observation>::Failure(stop.ErrorValue());
            return Result<Observation>::Success(
                Application::CodeQueryTextSnapshot{identity, projectGeneration, std::string{path}, revision, std::move(bytes)});
        };
        return providers;
    }
}  // namespace Horo::Editor
