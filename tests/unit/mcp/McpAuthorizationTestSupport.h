#pragma once

#include "Horo/Mcp/McpAuthorization.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Mcp::Test {
    /** @brief Deterministic test entropy, never linked into production composition. */
    class Entropy final : public Security::SecureRandomSource {
    public:
        Result<void> Fill(std::span<std::byte> output) override {
            const auto sequence = sequence_.fetch_add(1);
            std::fill(output.begin(), output.end(), std::byte{0});
            for (std::size_t i = 0; i < std::min(output.size(), sizeof(sequence)); ++i)
                output[i] = static_cast<std::byte>((sequence >> (i * 8U)) & 0xffU);
            return Result<void>::Success();
        }

    private:
        std::atomic<std::uint64_t> sequence_{1};
    };

    inline std::shared_ptr<McpAuthorization> Authorization() {
        static const auto policy = [] {
            auto result = std::make_shared<McpAuthorization>(std::make_shared<Entropy>());
            REQUIRE(result->SetTrust({}, 1, true).HasValue());
            REQUIRE(result->SetTrust("first", 1, true).HasValue());
            REQUIRE(result->SetTrust("project-one", 3, true).HasValue());
            return result;
        }();
        return policy;
    }

    inline McpSessionAdmission Authenticate(McpSessionAdmission admission,
                                            const std::shared_ptr<McpAuthorization> &policy = Authorization()) {
        auto credential = policy->IssueCredential(admission, std::chrono::hours{1});
        REQUIRE(credential.HasValue());
        auto principal = policy->Authenticate(admission, std::move(credential).Value());
        REQUIRE(principal.HasValue());
        admission.authority = std::move(principal).Value();
        return admission;
    }

    inline McpRequestContext Context(McpSessionAdmission admission = {.clientIdentity = "test-client"}) {
        admission = Authenticate(std::move(admission));
        McpRequestContext context;
        context.session = {1, 1};
        context.clientIdentity = admission.clientIdentity;
        context.capabilities = admission.capabilities;
        context.projectIdentity = admission.projectIdentity;
        context.authorizationRevision = admission.authorizationRevision;
        context.registryRevision = admission.registryRevision;
        context.deadline = std::chrono::steady_clock::now() + std::chrono::hours{1};
        context.authority = admission.authority;
        context.authorization = Authorization();
        context.requestIdentity = 1;
        return context;
    }
}  // namespace Horo::Mcp::Test
