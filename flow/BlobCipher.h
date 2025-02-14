/*
 * BlobCipher.h
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
#pragma once

#include "flow/network.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if (!defined(TLS_DISABLED))
#define ENCRYPTION_ENABLED 1
#else
#define ENCRYPTION_ENABLED 0
#endif

#if ENCRYPTION_ENABLED

#include "flow/Arena.h"
#include "flow/EncryptUtils.h"
#include "flow/FastRef.h"
#include "flow/flow.h"
#include "flow/genericactors.actor.h"

#include <openssl/aes.h>
#include <openssl/engine.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#define AES_256_KEY_LENGTH 32
#define AES_256_IV_LENGTH 16

// Encryption operations buffer management
// Approach limits number of copies needed during encryption or decryption operations.
// For encryption EncryptBuf is allocated using client supplied Arena and provided to AES library to capture
// the ciphertext. Similarly, on decryption EncryptBuf is allocated using client supplied Arena and provided
// to the AES library to capture decipher text and passed back to the clients. Given the object passed around
// is reference-counted, it gets freed once refrenceCount goes to 0.

class EncryptBuf : public ReferenceCounted<EncryptBuf>, NonCopyable {
public:
	EncryptBuf(int size, Arena& arena) : allocSize(size), logicalSize(size) {
		if (size > 0) {
			buffer = new (arena) uint8_t[size];
		} else {
			buffer = nullptr;
		}
	}

	int getLogicalSize() { return logicalSize; }
	void setLogicalSize(int value) {
		ASSERT(value <= allocSize);
		logicalSize = value;
	}
	uint8_t* begin() { return buffer; }

private:
	int allocSize;
	int logicalSize;
	uint8_t* buffer;
};

// BlobCipher Encryption header format
// This header is persisted along with encrypted buffer, it contains information necessary
// to assist decrypting the buffers to serve read requests.
//
// The total space overhead is 104 bytes.

#pragma pack(push, 1) // exact fit - no padding
typedef struct BlobCipherEncryptHeader {
	static constexpr int headerSize = 104;
	union {
		struct {
			uint8_t size; // reading first byte is sufficient to determine header
			              // length. ALWAYS THE FIRST HEADER ELEMENT.
			uint8_t headerVersion{};
			uint8_t encryptMode{};
			uint8_t authTokenMode{};
			uint8_t _reserved[4]{};
		} flags;
		uint64_t _padding{};
	};

	// Cipher text encryption information
	struct {
		// Encryption domain boundary identifier.
		EncryptCipherDomainId encryptDomainId{};
		// BaseCipher encryption key identifier
		EncryptCipherBaseKeyId baseCipherId{};
		// Random salt
		EncryptCipherRandomSalt salt{};
		// Initialization vector used to encrypt the payload.
		uint8_t iv[AES_256_IV_LENGTH];
	} cipherTextDetails;

	struct {
		// Encryption domainId for the header
		EncryptCipherDomainId encryptDomainId{};
		// BaseCipher encryption key identifier.
		EncryptCipherBaseKeyId baseCipherId{};
		// Random salt
		EncryptCipherRandomSalt salt{};
	} cipherHeaderDetails;

	// Encryption header is stored as plaintext on a persistent storage to assist reconstruction of cipher-key(s) for
	// reads. FIPS compliance recommendation is to leverage cryptographic digest mechanism to generate 'authentication
	// token' (crypto-secure) to protect against malicious tampering and/or bit rot/flip scenarios.

	union {
		// Encryption header support two modes of generation 'authentication tokens':
		// 1) SingleAuthTokenMode: the scheme generates single crypto-secrure auth token to protect {cipherText +
		// header} payload. Scheme is geared towards optimizing cost due to crypto-secure auth-token generation,
		// however, on decryption client needs to be read 'header' + 'encrypted-buffer' to validate the 'auth-token'.
		// The scheme is ideal for usecases where payload represented by the encryptionHeader is not large and it is
		// desirable to minimize CPU/latency penalty due to crypto-secure ops, such as: CommitProxies encrypted inline
		// transactions, StorageServer encrypting pages etc. 2) MultiAuthTokenMode: Scheme generates separate authTokens
		// for 'encrypted buffer' & 'encryption-header'. The scheme is ideal where payload represented by
		// encryptionHeader is large enough such that it is desirable to optimize cost of upfront reading full
		// 'encrypted buffer', compared to reading only encryptionHeader and ensuring its sanity; for instance:
		// backup-files.

		struct {
			// Cipher text authentication token
			uint8_t cipherTextAuthToken[AUTH_TOKEN_SIZE]{};
			uint8_t headerAuthToken[AUTH_TOKEN_SIZE]{};
		} multiAuthTokens;
		struct {
			uint8_t authToken[AUTH_TOKEN_SIZE]{};
			uint8_t _reserved[AUTH_TOKEN_SIZE]{};
		} singleAuthToken;
	};

	BlobCipherEncryptHeader() {}
} BlobCipherEncryptHeader;
#pragma pack(pop)

// Ensure no struct-packing issues
static_assert(sizeof(BlobCipherEncryptHeader) == BlobCipherEncryptHeader::headerSize,
              "BlobCipherEncryptHeader size mismatch");

// This interface is in-memory representation of CipherKey used for encryption/decryption information.
// It caches base encryption key properties as well as caches the 'derived encryption' key obtained by applying
// HMAC-SHA-256 derivation technique.

class BlobCipherKey : public ReferenceCounted<BlobCipherKey>, NonCopyable {
public:
	BlobCipherKey(const EncryptCipherDomainId& domainId,
	              const EncryptCipherBaseKeyId& baseCiphId,
	              const uint8_t* baseCiph,
	              int baseCiphLen);
	BlobCipherKey(const EncryptCipherDomainId& domainId,
	              const EncryptCipherBaseKeyId& baseCiphId,
	              const uint8_t* baseCiph,
	              int baseCiphLen,
	              const EncryptCipherRandomSalt& salt);

	uint8_t* data() const { return cipher.get(); }
	uint64_t getCreationTime() const { return creationTime; }
	EncryptCipherDomainId getDomainId() const { return encryptDomainId; }
	EncryptCipherRandomSalt getSalt() const { return randomSalt; }
	EncryptCipherBaseKeyId getBaseCipherId() const { return baseCipherId; }
	int getBaseCipherLen() const { return baseCipherLen; }
	uint8_t* rawCipher() const { return cipher.get(); }
	uint8_t* rawBaseCipher() const { return baseCipher.get(); }
	bool isEqual(const Reference<BlobCipherKey> toCompare) {
		return encryptDomainId == toCompare->getDomainId() && baseCipherId == toCompare->getBaseCipherId() &&
		       randomSalt == toCompare->getSalt() && baseCipherLen == toCompare->getBaseCipherLen() &&
		       memcmp(cipher.get(), toCompare->rawCipher(), AES_256_KEY_LENGTH) == 0 &&
		       memcmp(baseCipher.get(), toCompare->rawBaseCipher(), baseCipherLen) == 0;
	}
	void reset();

private:
	// Encryption domain boundary identifier
	EncryptCipherDomainId encryptDomainId;
	// Base encryption cipher key properties
	std::unique_ptr<uint8_t[]> baseCipher;
	int baseCipherLen;
	EncryptCipherBaseKeyId baseCipherId;
	// Random salt used for encryption cipher key derivation
	EncryptCipherRandomSalt randomSalt;
	// Creation timestamp for the derived encryption cipher key
	uint64_t creationTime;
	// Derived encryption cipher key
	std::unique_ptr<uint8_t[]> cipher;

	void initKey(const EncryptCipherDomainId& domainId,
	             const uint8_t* baseCiph,
	             int baseCiphLen,
	             const EncryptCipherBaseKeyId& baseCiphId,
	             const EncryptCipherRandomSalt& salt);
	void applyHmacSha256Derivation();
};

// This interface allows FDB processes participating in encryption to store and
// index recently used encyption cipher keys. FDB encryption has two dimensions:
// 1. Mapping on cipher encryption keys per "encryption domains"
// 2. Per encryption domain, the cipher keys are index using {baseCipherKeyId, salt} tuple.
//
// The design supports NIST recommendation of limiting lifetime of an encryption
// key. For details refer to:
// https://csrc.nist.gov/publications/detail/sp/800-57-part-1/rev-3/archive/2012-07-10
//
// Below gives a pictoral representation of in-memory datastructure implemented
// to index encryption keys:
//                  { encryptionDomain -> { {baseCipherId, salt} -> cipherKey } }
//
// Supported cache lookups schemes:
// 1. Lookup cipher based on { encryptionDomainId, baseCipherKeyId, salt } triplet.
// 2. Lookup latest cipher key for a given encryptionDomainId.
//
// Client is responsible to handle cache-miss usecase, the corrective operation
// might vary based on the calling process, for instance: EncryptKeyServer
// cache-miss shall invoke RPC to external Encryption Key Manager to fetch the
// required encryption key, however, CPs/SSs cache-miss would result in RPC to
// EncryptKeyServer to refresh the desired encryption key.

using BlobCipherKeyIdCacheKey = std::pair<EncryptCipherBaseKeyId, EncryptCipherRandomSalt>;
using BlobCipherKeyIdCacheKeyHash = boost::hash<BlobCipherKeyIdCacheKey>;
using BlobCipherKeyIdCacheMap =
    std::unordered_map<BlobCipherKeyIdCacheKey, Reference<BlobCipherKey>, BlobCipherKeyIdCacheKeyHash>;
using BlobCipherKeyIdCacheMapCItr =
    std::unordered_map<BlobCipherKeyIdCacheKey, Reference<BlobCipherKey>, BlobCipherKeyIdCacheKeyHash>::const_iterator;

struct BlobCipherKeyIdCache : ReferenceCounted<BlobCipherKeyIdCache> {
public:
	BlobCipherKeyIdCache();
	explicit BlobCipherKeyIdCache(EncryptCipherDomainId dId);

	BlobCipherKeyIdCacheKey getCacheKey(const EncryptCipherBaseKeyId& baseCipherId,
	                                    const EncryptCipherRandomSalt& salt);

	// API returns the last inserted cipherKey.
	// If none exists, 'encrypt_key_not_found' is thrown.

	Reference<BlobCipherKey> getLatestCipherKey();

	// API returns cipherKey corresponding to input 'baseCipherKeyId'.
	// If none exists, 'encrypt_key_not_found' is thrown.

	Reference<BlobCipherKey> getCipherByBaseCipherId(const EncryptCipherBaseKeyId& baseCipherKeyId,
	                                                 const EncryptCipherRandomSalt& salt);

	// API enables inserting base encryption cipher details to the BlobCipherKeyIdCache.
	// Given cipherKeys are immutable, attempting to re-insert same 'identical' cipherKey
	// is treated as a NOP (success), however, an attempt to update cipherKey would throw
	// 'encrypt_update_cipher' exception.
	//
	// API NOTE: Recommended usecase is to update encryption cipher-key is updated the external
	// keyManagementSolution to limit an encryption key lifetime

	void insertBaseCipherKey(const EncryptCipherBaseKeyId& baseCipherId, const uint8_t* baseCipher, int baseCipherLen);

	// API enables inserting base encryption cipher details to the BlobCipherKeyIdCache
	// Given cipherKeys are immutable, attempting to re-insert same 'identical' cipherKey
	// is treated as a NOP (success), however, an attempt to update cipherKey would throw
	// 'encrypt_update_cipher' exception.
	//
	// API NOTE: Recommended usecase is to update encryption cipher-key regeneration while performing
	// decryption. The encryptionheader would contain relevant details including: 'encryptDomainId',
	// 'baseCipherId' & 'salt'. The caller needs to fetch 'baseCipherKey' detail and re-populate KeyCache.
	// Also, the invocation will NOT update the latest cipher-key details.

	void insertBaseCipherKey(const EncryptCipherBaseKeyId& baseCipherId,
	                         const uint8_t* baseCipher,
	                         int baseCipherLen,
	                         const EncryptCipherRandomSalt& salt);

	// API cleanup the cache by dropping all cached cipherKeys
	void cleanup();

	// API returns list of all 'cached' cipherKeys
	std::vector<Reference<BlobCipherKey>> getAllCipherKeys();

private:
	EncryptCipherDomainId domainId;
	BlobCipherKeyIdCacheMap keyIdCache;
	EncryptCipherBaseKeyId latestBaseCipherKeyId;
	EncryptCipherRandomSalt latestRandomSalt;
};

using BlobCipherDomainCacheMap = std::unordered_map<EncryptCipherDomainId, Reference<BlobCipherKeyIdCache>>;

class BlobCipherKeyCache : NonCopyable, public ReferenceCounted<BlobCipherKeyCache> {
public:
	// Public visibility constructior ONLY to assist FlowSingleton instance creation.
	// API Note: Constructor is expected to be instantiated only in simulation mode.

	explicit BlobCipherKeyCache(bool ignored) { ASSERT(g_network->isSimulated()); }

	// Enable clients to insert base encryption cipher details to the BlobCipherKeyCache.
	// The cipherKeys are indexed using 'baseCipherId', given cipherKeys are immutable,
	// attempting to re-insert same 'identical' cipherKey is treated as a NOP (success),
	// however, an attempt to update cipherKey would throw 'encrypt_update_cipher' exception.
	//
	// API NOTE: Recommended usecase is to update encryption cipher-key is updated the external
	// keyManagementSolution to limit an encryption key lifetime

	void insertCipherKey(const EncryptCipherDomainId& domainId,
	                     const EncryptCipherBaseKeyId& baseCipherId,
	                     const uint8_t* baseCipher,
	                     int baseCipherLen);

	// Enable clients to insert base encryption cipher details to the BlobCipherKeyCache.
	// The cipherKeys are indexed using 'baseCipherId', given cipherKeys are immutable,
	// attempting to re-insert same 'identical' cipherKey is treated as a NOP (success),
	// however, an attempt to update cipherKey would throw 'encrypt_update_cipher' exception.
	//
	// API NOTE: Recommended usecase is to update encryption cipher-key regeneration while performing
	// decryption. The encryptionheader would contain relevant details including: 'encryptDomainId',
	// 'baseCipherId' & 'salt'. The caller needs to fetch 'baseCipherKey' detail and re-populate KeyCache.
	// Also, the invocation will NOT update the latest cipher-key details.

	void insertCipherKey(const EncryptCipherDomainId& domainId,
	                     const EncryptCipherBaseKeyId& baseCipherId,
	                     const uint8_t* baseCipher,
	                     int baseCipherLen,
	                     const EncryptCipherRandomSalt& salt);

	// API returns the last insert cipherKey for a given encryption domain Id.
	// If none exists, it would throw 'encrypt_key_not_found' exception.

	Reference<BlobCipherKey> getLatestCipherKey(const EncryptCipherDomainId& domainId);

	// API returns cipherKey corresponding to {encryptionDomainId, baseCipherId} tuple.
	// If none exists, it would throw 'encrypt_key_not_found' exception.

	Reference<BlobCipherKey> getCipherKey(const EncryptCipherDomainId& domainId,
	                                      const EncryptCipherBaseKeyId& baseCipherId,
	                                      const EncryptCipherRandomSalt& salt);

	// API returns point in time list of all 'cached' cipherKeys for a given encryption domainId.
	std::vector<Reference<BlobCipherKey>> getAllCiphers(const EncryptCipherDomainId& domainId);

	// API enables dropping all 'cached' cipherKeys for a given encryption domain Id.
	// Useful to cleanup cache if an encryption domain gets removed/destroyed etc.

	void resetEncryptDomainId(const EncryptCipherDomainId domainId);

	static Reference<BlobCipherKeyCache> getInstance() {
		if (g_network->isSimulated()) {
			return FlowSingleton<BlobCipherKeyCache>::getInstance(
			    []() { return makeReference<BlobCipherKeyCache>(g_network->isSimulated()); });
		} else {
			static BlobCipherKeyCache instance;
			return Reference<BlobCipherKeyCache>::addRef(&instance);
		}
	}

	// Ensures cached encryption key(s) (plaintext) never gets persisted as part
	// of FDB process/core dump.
	static void cleanup() noexcept;

private:
	BlobCipherDomainCacheMap domainCacheMap;

	BlobCipherKeyCache() {}
};

// This interface enables data block encryption. An invocation to encrypt() will
// do two things:
// 1) generate encrypted ciphertext for given plaintext input.
// 2) generate BlobCipherEncryptHeader (including the 'header authTokens') and persit for decryption on reads.

class EncryptBlobCipherAes265Ctr final : NonCopyable, public ReferenceCounted<EncryptBlobCipherAes265Ctr> {
public:
	static constexpr uint8_t ENCRYPT_HEADER_VERSION = 1;

	EncryptBlobCipherAes265Ctr(Reference<BlobCipherKey> tCipherKey,
	                           Reference<BlobCipherKey> hCipherKey,
	                           const uint8_t* iv,
	                           const int ivLen,
	                           const EncryptAuthTokenMode mode);
	~EncryptBlobCipherAes265Ctr();

	Reference<EncryptBuf> encrypt(const uint8_t* plaintext,
	                              const int plaintextLen,
	                              BlobCipherEncryptHeader* header,
	                              Arena&);

private:
	EVP_CIPHER_CTX* ctx;
	Reference<BlobCipherKey> textCipherKey;
	Reference<BlobCipherKey> headerCipherKey;
	EncryptAuthTokenMode authTokenMode;
	uint8_t iv[AES_256_IV_LENGTH];
};

// This interface enable data block decryption. An invocation to decrypt() would generate
// 'plaintext' for a given 'ciphertext' input, the caller needs to supply BlobCipherEncryptHeader.

class DecryptBlobCipherAes256Ctr final : NonCopyable, public ReferenceCounted<DecryptBlobCipherAes256Ctr> {
public:
	DecryptBlobCipherAes256Ctr(Reference<BlobCipherKey> tCipherKey,
	                           Reference<BlobCipherKey> hCipherKey,
	                           const uint8_t* iv);
	~DecryptBlobCipherAes256Ctr();

	Reference<EncryptBuf> decrypt(const uint8_t* ciphertext,
	                              const int ciphertextLen,
	                              const BlobCipherEncryptHeader& header,
	                              Arena&);

	// Enable caller to validate encryption header auth-token (if available) without needing to read the full encrypted
	// payload. The call is NOP unless header.flags.authTokenMode == ENCRYPT_HEADER_AUTH_TOKEN_MODE_MULTI.

	void verifyHeaderAuthToken(const BlobCipherEncryptHeader& header, Arena& arena);

private:
	EVP_CIPHER_CTX* ctx;
	Reference<BlobCipherKey> textCipherKey;
	Reference<BlobCipherKey> headerCipherKey;
	bool headerAuthTokenValidationDone;
	bool authTokensValidationDone;

	void verifyEncryptHeaderMetadata(const BlobCipherEncryptHeader& header);
	void verifyAuthTokens(const uint8_t* ciphertext,
	                      const int ciphertextLen,
	                      const BlobCipherEncryptHeader& header,
	                      uint8_t* buff,
	                      Arena& arena);
	void verifyHeaderSingleAuthToken(const uint8_t* ciphertext,
	                                 const int ciphertextLen,
	                                 const BlobCipherEncryptHeader& header,
	                                 uint8_t* buff,
	                                 Arena& arena);
	void verifyHeaderMultiAuthToken(const uint8_t* ciphertext,
	                                const int ciphertextLen,
	                                const BlobCipherEncryptHeader& header,
	                                uint8_t* buff,
	                                Arena& arena);
};

class HmacSha256DigestGen final : NonCopyable {
public:
	HmacSha256DigestGen(const unsigned char* key, size_t len);
	~HmacSha256DigestGen();
	HMAC_CTX* getCtx() const { return ctx; }
	StringRef digest(unsigned char const* data, size_t len, Arena&);

private:
	HMAC_CTX* ctx;
};

StringRef computeAuthToken(const uint8_t* payload,
                           const int payloadLen,
                           const uint8_t* key,
                           const int keyLen,
                           Arena& arena);

#endif // ENCRYPTION_ENABLED
