/**
 *  Copyright (C) 2021 FISCO BCOS.
 *  SPDX-License-Identifier: Apache-2.0
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * @brief implementation for hsm sm2 signature
 * @file HsmSM2Crypto.cpp
 * @date 2022.10.07
 * @author lucasli
 */
#include "hsm-crypto/hsm/CryptoProvider.h"
#include "hsm-crypto/hsm/SDFCryptoProvider.h"
#include <bcos-crypto/signature/Exceptions.h>
#include <bcos-crypto/signature/codec/SignatureDataWithPub.h>
#include <bcos-crypto/signature/hsmSM2/HsmSM2Crypto.h>
#include <bcos-crypto/signature/hsmSM2/HsmSM2KeyPair.h>
#include <algorithm>

using namespace bcos;
using namespace bcos::crypto;
using namespace hsm;

#define SDR_OK 0x0
#define SDR_BASE 0x01000000
#define SDR_VERIFYERR (SDR_BASE + 0x0000000E)

std::shared_ptr<bytes> HsmSM2Crypto::sign(
    const KeyPairInterface& _keyPair, const HashType& _hash, bool _signatureWithPub) const
{
    auto& hsmKeyPair = dynamic_cast<const HsmSM2KeyPair&>(_keyPair);
    auto beginSignTime = utcTimeUs();
    Key key = Key();
    if (hsmKeyPair.isInternalKey())
    {
        auto pwdConstPtr =
            std::make_shared<bytes>(hsmKeyPair.password().begin(), hsmKeyPair.password().end());
        key = Key(hsmKeyPair.keyIndex(), pwdConstPtr);
    }
    else
    {
        auto privKey =
            std::make_shared<const std::vector<byte>>((byte*)hsmKeyPair.secretKey()->constData(),
                (byte*)hsmKeyPair.secretKey()->constData() + 32);
        key.setPrivateKey(privKey);
        CRYPTO_LOG(DEBUG) << "[HSMSignature::key] is external key ";
    }
    std::shared_ptr<bytes> signatureData = std::make_shared<bytes>(64);

    // According to the SM2 standard
    // step 1 : calculate M' = Za || M
    // step 2 : e = H(M')
    // step 3 : signature = Sign(e)
    // get provider
    auto step1BeginTime = utcTimeUs();
    auto pubKey =
        std::make_shared<const std::vector<byte>>((byte*)hsmKeyPair.publicKey()->constData(),
            (byte*)hsmKeyPair.publicKey()->constData() + 64);
    key.setPublicKey(pubKey);
    auto step1Time = utcTimeUs() - step1BeginTime;
    if (c_fileLogLevel <= DEBUG)
    {
        CRYPTO_LOG(DEBUG) << "[HSMSignature::sign] step 1:calculate M' = Za || M success"
                          << LOG_KV("step1Cost", step1Time);
    }
    // step 2 : e = H(M')
    unsigned char hashResult[HSM_SM3_DIGEST_LENGTH];
    unsigned int uiHashResultLen;
    auto step2BeginTime = utcTimeUs();
    unsigned int code = m_provider.Hash(&key, hsm::SM3, _hash.data(), HSM_SM3_DIGEST_LENGTH,
        (unsigned char*)hashResult, &uiHashResultLen);
    auto step2Time = utcTimeUs() - step2BeginTime;
    if (c_fileLogLevel <= DEBUG)
    {
        CRYPTO_LOG(DEBUG) << "[HSMSignature::sign] step 2:calculate e = H(M') success"
                          << LOG_KV("step2Cost", step2Time);
    }
    if (code != SDR_OK)
    {
        CRYPTO_LOG(ERROR) << "[HSMSignature::sign] ERROR of compute H(M')"
                          << LOG_KV("message", m_provider.GetErrorMessage(code));
        return nullptr;
    }

    // step 3 : signature = Sign(e)
    unsigned int signLen;
    auto step3BeginTime = utcTimeUs();
    code = m_provider.Sign(
        key, hsm::SM2, (const unsigned char*)hashResult, 32, signatureData->data(), &signLen);
    if (code != SDR_OK)
    {
        CRYPTO_LOG(ERROR) << "[HSMSignature::sign] ERROR of Sign"
                          << LOG_KV("error", m_provider.GetErrorMessage(code));
        return nullptr;
    }
    auto step3Time = utcTimeUs() - step3BeginTime;
    if (c_fileLogLevel <= DEBUG)
    {
        CRYPTO_LOG(DEBUG) << "[HSMSignature::sign] step 3:signature = Sign(e) success"
                          << LOG_KV("step3Cost", step3Time);
    }

    // append the public key
    if (_signatureWithPub)
    {
        signatureData->insert(signatureData->end(), hsmKeyPair.publicKey()->mutableData(),
            hsmKeyPair.publicKey()->mutableData() + hsmKeyPair.publicKey()->size());
    }
    CRYPTO_LOG(INFO)
        << "[HSMSignature::sign] sign success" << LOG_KV("total cost", utcTimeUs() - beginSignTime)
        << LOG_KV("step1Time Proportion",
               static_cast<float>(step1Time) / static_cast<float>(utcTimeUs() - beginSignTime))
        << LOG_KV("step2Time Proportion",
               static_cast<float>(step2Time) / static_cast<float>(utcTimeUs() - beginSignTime))
        << LOG_KV("step3Time Proportion", static_cast<float>(utcTimeUs() - step3BeginTime) /
                                              static_cast<float>(utcTimeUs() - beginSignTime))
        << LOG_KV("otherTime Proportion",
               static_cast<float>(utcTimeUs() - beginSignTime - step1Time - step2Time - step3Time) /
                   static_cast<float>(utcTimeUs() - beginSignTime));
    return signatureData;
}

bool HsmSM2Crypto::verify(std::shared_ptr<bytes const> _pubKeyBytes, const HashType& _hash,
    bytesConstRef _signatureData) const
{
    return verify(
        std::make_shared<KeyImpl>(HSM_SM2_PUBLIC_KEY_LEN, _pubKeyBytes), _hash, _signatureData);
}

bool HsmSM2Crypto::verify(
    PublicPtr _pubKey, const HashType& _hash, bytesConstRef _signatureData) const
{
    // get provider
    auto beginVerifyTime = utcTimeUs();
    // parse input
    Key key = Key();
    auto pubKey = std::make_shared<const std::vector<byte>>(
        (byte*)_pubKey->constData(), (byte*)_pubKey->constData() + 64);
    key.setPublicKey(pubKey);
    bool verifyResult = false;

    // Get Z

    auto verifyBeginTime = utcTimeUs();
    unsigned int code = m_provider.Verify(key, hsm::SM2, (const unsigned char*)_hash.data(),
        HSM_SM3_DIGEST_LENGTH, _signatureData.data(), 64, &verifyResult);
    if (code != SDR_OK)
    {
        CRYPTO_LOG(ERROR) << "[HSMSignature::verify] ERROR of Verify"
                          << LOG_KV("error", m_provider.GetErrorMessage(code));
        return false;
    }
    if (c_fileLogLevel <= DEBUG)
    {
        CRYPTO_LOG(DEBUG) << "[HSMSignature::verify] Verify success"
                          << LOG_KV("HSM verify Cost", utcTimeUs() - verifyBeginTime);
    }

    CRYPTO_LOG(INFO) << "[HSMSignature::verify] verify success"
                     << LOG_KV("total cost", utcTimeUs() - beginVerifyTime)
                     << LOG_KV("verify cost", utcTimeUs() - verifyBeginTime);
    return true;
}

PublicPtr HsmSM2Crypto::recover(const HashType& _hash, bytesConstRef _signData) const
{
    auto signatureStruct = std::make_shared<SignatureDataWithPub>(_signData);
    auto hsmSM2Pub = std::make_shared<KeyImpl>(HSM_SM2_PUBLIC_KEY_LEN, signatureStruct->pub());
    if (verify(hsmSM2Pub, _hash, _signData))
    {
        return hsmSM2Pub;
    }
    BOOST_THROW_EXCEPTION(InvalidSignature() << errinfo_comment(
                              "invalid signature: hsm sm2 recover public key failed, msgHash : " +
                              _hash.hex() + ", signature:" + *toHexString(_signData)));
}

std::pair<bool, bytes> HsmSM2Crypto::recoverAddress(Hash::Ptr _hashImpl, bytesConstRef _input) const
{
    struct
    {
        HashType hash;
        h512 pub;
        h256 r;
        h256 s;
    } in;
    memcpy(&in, _input.data(),
        std::min<size_t>(_input.size(), sizeof(in)));  // TODO: Fix this! memcpy on non-pod type!
    // verify the signature
    auto signatureData = std::make_shared<SignatureDataWithPub>(in.r, in.s, in.pub.ref());
    try
    {
        auto encodedData = signatureData->encode();
        auto hsmSM2Pub = std::make_shared<KeyImpl>(HSM_SM2_PUBLIC_KEY_LEN, signatureData->pub());
        if (verify(hsmSM2Pub, in.hash, bytesConstRef(encodedData->data(), encodedData->size())))
        {
            auto address = calculateAddress(_hashImpl, hsmSM2Pub);
            return {true, address.asBytes()};
        }
    }
    catch (const std::exception& e)
    {
        CRYPTO_LOG(WARNING) << LOG_DESC("Hsm SM2 recoverAddress failed")
                            << LOG_KV("error", boost::diagnostic_information(e));
    }
    return {false, {}};
}

KeyPairInterface::UniquePtr HsmSM2Crypto::generateKeyPair() const
{
    return m_keyPairFactory->generateKeyPair();
}

KeyPairInterface::UniquePtr HsmSM2Crypto::createKeyPair(SecretPtr _secretKey) const
{
    return m_keyPairFactory->createKeyPair(_secretKey);
}

KeyPairInterface::UniquePtr HsmSM2Crypto::createKeyPair(
    unsigned int _keyIndex, std::string _password)
{
    return dynamic_pointer_cast<HsmSM2KeyPairFactory>(m_keyPairFactory)
        ->createKeyPair(_keyIndex, _password);
}