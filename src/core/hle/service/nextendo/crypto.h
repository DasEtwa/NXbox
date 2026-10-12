// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

namespace Service::Nextendo {

inline std::string Base64Url(std::string_view bytes) {
    std::string output(4 * ((bytes.size() + 2) / 3), '\0');
    if (!bytes.empty()) {
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                        reinterpret_cast<const unsigned char*>(bytes.data()),
                        static_cast<int>(bytes.size()));
    }
    for (char& c : output) {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
    }
    while (!output.empty() && output.back() == '=')
        output.pop_back();
    return output;
}

inline std::string RandomUrl(std::size_t count) {
    std::string bytes(count, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), static_cast<int>(count)) != 1) {
        throw std::runtime_error("Nextendo random generation failed");
    }
    return Base64Url(bytes);
}

inline std::string PkceChallenge(std::string_view verifier) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned size = 0;
    if (EVP_Digest(verifier.data(), verifier.size(), digest, &size, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("Nextendo digest failed");
    }
    return Base64Url({reinterpret_cast<const char*>(digest), size});
}

// An independently implemented RS256 envelope. No Nintendo identity material is accepted.
// An operator-issued Nextendo key is needed for titles checking the published JWKS.
class IdTokenSigner {
public:
    explicit IdTokenSigner(std::string_view nextendo_pem = {}) {
        if (!nextendo_pem.empty()) {
            std::unique_ptr<BIO, decltype(&BIO_free)> input(
                BIO_new_mem_buf(nextendo_pem.data(), static_cast<int>(nextendo_pem.size())),
                BIO_free);
            if (input)
                key_.reset(PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr));
        } else {
            std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
                EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
            EVP_PKEY* generated = nullptr;
            if (context && EVP_PKEY_keygen_init(context.get()) > 0 &&
                EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) > 0 &&
                EVP_PKEY_keygen(context.get(), &generated) > 0)
                key_.reset(generated);
        }
        if (!key_ || EVP_PKEY_base_id(key_.get()) != EVP_PKEY_RSA ||
            EVP_PKEY_bits(key_.get()) < 2048) {
            throw std::runtime_error("Nextendo signing key unavailable");
        }
    }

    std::string Sign(std::string_view claims) const {
        const std::string data =
            Base64Url(R"({"alg":"RS256","kid":"nextendo-baas-key-1","typ":"id_token"})") + '.' +
            Base64Url(claims);
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                        EVP_MD_CTX_free);
        std::size_t size = 0;
        if (!context ||
            EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key_.get()) <= 0 ||
            EVP_DigestSign(context.get(), nullptr, &size,
                           reinterpret_cast<const unsigned char*>(data.data()), data.size()) <= 0) {
            return {};
        }
        std::string signature(size, '\0');
        if (EVP_DigestSign(context.get(), reinterpret_cast<unsigned char*>(signature.data()), &size,
                           reinterpret_cast<const unsigned char*>(data.data()), data.size()) <= 0) {
            return {};
        }
        signature.resize(size);
        return data + '.' + Base64Url(signature);
    }

private:
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key_{nullptr, EVP_PKEY_free};
};

} // namespace Service::Nextendo
