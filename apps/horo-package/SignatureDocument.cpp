#include "SignatureDocument.h"

#include "HostSignatureDocument.h"

#include <utility>

namespace Horo::PackageCommand {
    Outcome LoadSignature(const std::filesystem::path &path, Security::DetachedSignatureEnvelope &envelope) {
        std::vector<std::byte> bytes;
        if (const auto read = ReadBounded(path, Application::Internal::MaximumHostSignatureDocumentBytes, bytes); !read.success)
            return read;
        auto decoded = Application::Internal::DecodeDetachedSignatureDocument(bytes);
        if (decoded.HasError())
            return Failure(decoded.ErrorValue().code.Value(), decoded.ErrorValue().message);
        envelope = std::move(decoded).Value();
        return Success();
    }

    Outcome LoadTrust(const std::filesystem::path &path, Packages::PackagePublisherVerificationPolicy &policy) {
        std::vector<std::byte> bytes;
        if (const auto read = ReadBounded(path, Application::Internal::MaximumHostSignatureDocumentBytes, bytes); !read.success)
            return read;
        auto decoded = Application::Internal::DecodePublisherTrustDocument(bytes, policy);
        if (decoded.HasError())
            return Failure(decoded.ErrorValue().code.Value(), decoded.ErrorValue().message);
        policy = std::move(decoded).Value();
        return Success();
    }

    std::string EncodeSignature(const Security::DetachedSignatureEnvelope &envelope) {
        return Application::Internal::EncodeDetachedSignatureDocument(envelope);
    }
}  // namespace Horo::PackageCommand
