/*
 * TokenSign.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbrpc/TokenSign.h"
#include "flow/network.h"
#include "flow/serialize.h"
#include "flow/Arena.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/Platform.h"
#include "flow/Trace.h"
#include "flow/UnitTest.h"
#include <type_traits>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

namespace {

template <typename Func>
class ExitGuard {
	std::decay_t<Func> fn;

public:
	ExitGuard(Func&& fn) : fn(std::forward<Func>(fn)) {}

	~ExitGuard() { fn(); }
};

[[noreturn]] void traceAndThrow(const char* type) {
	auto te = TraceEvent(SevWarnAlways, type);
	te.suppressFor(60);
	if (auto err = ::ERR_get_error()) {
		char buf[256]{
			0,
		};
		::ERR_error_string_n(err, buf, sizeof(buf));
		te.detail("OpenSSLError", buf);
	}
	throw digital_signature_ops_error();
}

struct KeyPairRef {
	StringRef privateKey;
	StringRef publicKey;
};

Standalone<KeyPairRef> generateEcdsaKeyPair() {
	auto params = std::add_pointer_t<EVP_PKEY>();
	{
		auto pctx = ::EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
		ASSERT(pctx);
		auto ctxGuard = ExitGuard([pctx]() { ::EVP_PKEY_CTX_free(pctx); });
		ASSERT_LT(0, ::EVP_PKEY_paramgen_init(pctx));
		ASSERT_LT(0, ::EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1));
		ASSERT_LT(0, ::EVP_PKEY_paramgen(pctx, &params));
		ASSERT(params);
	}
	auto paramsGuard = ExitGuard([params]() { ::EVP_PKEY_free(params); });
	// keygen
	auto kctx = ::EVP_PKEY_CTX_new(params, nullptr);
	ASSERT(kctx);
	auto kctxGuard = ExitGuard([kctx]() { ::EVP_PKEY_CTX_free(kctx); });
	auto key = std::add_pointer_t<EVP_PKEY>();
	{
		ASSERT_LT(0, ::EVP_PKEY_keygen_init(kctx));
		ASSERT_LT(0, ::EVP_PKEY_keygen(kctx, &key));
	}
	ASSERT(key);
	auto keyGuard = ExitGuard([key]() { ::EVP_PKEY_free(key); });

	auto ret = Standalone<KeyPairRef>{};
	auto& arena = ret.arena();
	{
		auto len = 0;
		len = ::i2d_PrivateKey(key, nullptr);
		ASSERT_LT(0, len);
		auto buf = new (arena) uint8_t[len];
		auto out = std::add_pointer_t<uint8_t>(buf);
		len = ::i2d_PrivateKey(key, &out);
		ret.privateKey = StringRef(buf, len);
	}
	{
		auto len = 0;
		len = ::i2d_PUBKEY(key, nullptr);
		ASSERT_LT(0, len);
		auto buf = new (arena) uint8_t[len];
		auto out = std::add_pointer_t<uint8_t>(buf);
		len = ::i2d_PUBKEY(key, &out);
		ret.publicKey = StringRef(buf, len);
	}
	return ret;
}

} // namespace

Standalone<SignedAuthTokenRef> signToken(AuthTokenRef token, StringRef keyName, StringRef privateKeyDer) {
	auto ret = Standalone<SignedAuthTokenRef>{};
	auto arena = ret.arena();
	auto writer = ObjectWriter([&arena](size_t len) { return new (arena) uint8_t[len]; }, IncludeVersion());
	writer.serialize(token);
	auto tokenStr = writer.toStringRef();

	auto rawPrivKeyDer = privateKeyDer.begin();
	auto key = ::d2i_AutoPrivateKey(nullptr, &rawPrivKeyDer, privateKeyDer.size());
	if (!key) {
		traceAndThrow("SignTokenBadKey");
	}
	auto keyGuard = ExitGuard([key]() { ::EVP_PKEY_free(key); });
	auto mdctx = ::EVP_MD_CTX_create();
	if (!mdctx)
		traceAndThrow("SignTokenInitFail");
	auto mdctxGuard = ExitGuard([mdctx]() { ::EVP_MD_CTX_free(mdctx); });
	if (1 != ::EVP_DigestSignInit(mdctx, nullptr, ::EVP_sha256() /*Parameterize?*/, nullptr, key))
		traceAndThrow("SignTokenInitFail");
	if (1 != ::EVP_DigestSignUpdate(mdctx, tokenStr.begin(), tokenStr.size()))
		traceAndThrow("SignTokenUpdateFail");
	auto sigLen = size_t{};
	if (1 != ::EVP_DigestSignFinal(mdctx, nullptr, &sigLen)) // assess the length first
		traceAndThrow("SignTokenGetSigLenFail");
	auto sigBuf = new (arena) uint8_t[sigLen];
	if (1 != ::EVP_DigestSignFinal(mdctx, sigBuf, &sigLen))
		traceAndThrow("SignTokenFinalizeFail");
	ret.token = tokenStr;
	ret.signature = StringRef(sigBuf, sigLen);
	ret.keyName = StringRef(arena, keyName);
	return ret;
}

bool verifyToken(SignedAuthTokenRef signedToken, StringRef publicKeyDer) {
	auto rawPubKeyDer = publicKeyDer.begin();
	auto key = ::d2i_PUBKEY(nullptr, &rawPubKeyDer, publicKeyDer.size());
	if (!key)
		traceAndThrow("VerifyTokenBadKey");
	auto keyGuard = ExitGuard([key]() { ::EVP_PKEY_free(key); });
	auto mdctx = ::EVP_MD_CTX_create();
	if (!mdctx)
		traceAndThrow("VerifyTokenInitFail");
	auto mdctxGuard = ExitGuard([mdctx]() { ::EVP_MD_CTX_free(mdctx); });
	if (1 != ::EVP_DigestVerifyInit(mdctx, nullptr, ::EVP_sha256(), nullptr, key))
		traceAndThrow("VerifyTokenInitFail");
	if (1 != ::EVP_DigestVerifyUpdate(mdctx, signedToken.token.begin(), signedToken.token.size()))
		traceAndThrow("VerifyTokenUpdateFail");
	if (1 != ::EVP_DigestVerifyFinal(mdctx, signedToken.signature.begin(), signedToken.signature.size())) {
		auto te = TraceEvent(SevInfo, "VerifyTokenFail");
		te.suppressFor(30);
		if (auto err = ::ERR_get_error()) {
			char buf[256]{
				0,
			};
			::ERR_error_string_n(err, buf, sizeof(buf));
			te.detail("OpenSSLError", buf);
		}
		return false;
	}
	return true;
}

void forceLinkTokenSignTests() {}

TEST_CASE("/fdbrpc/TokenSign") {
	const auto numIters = 100;
	for (auto i = 0; i < numIters; i++) {
		auto keyPair = generateEcdsaKeyPair();
		auto token = Standalone<AuthTokenRef>{};
		auto arena = token.arena();
		auto& rng = *deterministicRandom();
		token.expiresAt = timer_monotonic() * (0.5 + rng.random01());
		if (auto setIp = rng.randomInt(0, 3)) {
			if (setIp == 1) {
				token.ipAddress = IPAddress(rng.randomUInt32());
			} else {
				auto v6 = std::array<uint8_t, 16>{};
				for (auto& byte : v6)
					byte = rng.randomUInt32() & 255;
				token.ipAddress = IPAddress(v6);
			}
		}
		auto genRandomStringRef = [&arena, &rng]() {
			const auto len = rng.randomInt(1, 21);
			auto strRaw = new (arena) uint8_t[len];
			for (auto i = 0; i < len; i++)
				strRaw[i] = (uint8_t)rng.randomAlphaNumeric();
			return StringRef(strRaw, len);
		};
		const auto numTenants = rng.randomInt(0, 31);
		for (auto i = 0; i < numTenants; i++) {
			token.tenants.push_back(arena, genRandomStringRef());
		}
		auto keyName = genRandomStringRef();
		auto signedToken = signToken(token, keyName, keyPair.privateKey);
		const auto verifyExpectOk = verifyToken(signedToken, keyPair.publicKey);
		ASSERT(verifyExpectOk);
		// try tampering with signed token by adding one more tenant
		token.tenants.push_back(arena, genRandomStringRef());
		auto writer = ObjectWriter([&arena](size_t len) { return new (arena) uint8_t[len]; }, IncludeVersion());
		writer.serialize(token);
		signedToken.token = writer.toStringRef();
		const auto verifyExpectFail = verifyToken(signedToken, keyPair.publicKey);
		ASSERT(!verifyExpectFail);
	}
	printf("%d runs OK\n", numIters);
	return Void();
}
