// Copyright (c) 2018 The Dash Core developers
// Copyright (c) 2020 The Memeium developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MEMEIUM_CRYPTO_BLS_IES_H
#define MEMEIUM_CRYPTO_BLS_IES_H

#include "bls.h"
#include "streams.h"

class CBLSIESEncryptedBlob
{
public:
    CBLSPublicKey ephemeralPubKey;
    unsigned char iv[16];
    std::vector<unsigned char> data;

    bool valid{false};

public:
    ADD_SERIALIZE_METHODS

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        if (!ser_action.ForRead()) {
            assert(valid);
        } else {
            valid = false;
        }
        READWRITE(ephemeralPubKey);
        READWRITE(FLATDATA(iv));
        READWRITE(data);
        if (ser_action.ForRead()) {
            valid = true;
        }
    };

public:
    bool Encrypt(const CBLSPublicKey& peerPubKey, const void* data, size_t dataSize);
    bool Decrypt(const CBLSSecretKey& secretKey, CDataStream& decryptedDataRet) const;
};

template <typename Object>
class CBLSIESEncryptedObject : public CBLSIESEncryptedBlob
{
public:
    CBLSIESEncryptedObject()
    {
    }

    bool Encrypt(const CBLSPublicKey& peerPubKey, const Object& obj, int nVersion)
    {
        try {
            CDataStream ds(SER_NETWORK, nVersion);
            ds << obj;
            return CBLSIESEncryptedBlob::Encrypt(peerPubKey, ds.data(), ds.size());
        } catch (std::exception&) {
            return false;
        }
    }

    bool Decrypt(const CBLSSecretKey& secretKey, Object& objRet, int nVersion) const
    {
        CDataStream ds(SER_NETWORK, nVersion);
        if (!CBLSIESEncryptedBlob::Decrypt(secretKey, ds)) {
            return false;
        }
        try {
            ds >> objRet;
        } catch (std::exception& e) {
            return false;
        }
        return true;
    }
};

class CBLSIESMultiRecipientBlobs
{
public:
    typedef std::vector<unsigned char> Blob;
    typedef std::vector<Blob> BlobVector;

public:
    CBLSPublicKey ephemeralPubKey;
    uint256 ivSeed;
    BlobVector blobs;

    // Used while encrypting. Temporary and only in-memory
    CBLSSecretKey ephemeralSecretKey;
    std::vector<uint256> ivVector;

public:
    bool Encrypt(const std::vector<CBLSPublicKey>& recipients, const BlobVector& _blobs);

    void InitEncrypt(size_t count);
    bool Encrypt(size_t idx, const CBLSPublicKey& recipient, const Blob& blob);
    bool Decrypt(size_t idx, const CBLSSecretKey& sk, Blob& blobRet) const;

public:
    ADD_SERIALIZE_METHODS

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(ephemeralPubKey);
        READWRITE(ivSeed);
        READWRITE(blobs);
    }
};

template <typename Object>
class CBLSIESMultiRecipientObjects : public CBLSIESMultiRecipientBlobs
{
public:
    typedef std::vector<Object> ObjectVector;

public:
    bool Encrypt(const std::vector<CBLSPublicKey>& recipients, const ObjectVector& _objects, int nVersion)
    {
        BlobVector blobs;
        blobs.resize(_objects.size());

        try {
            CDataStream ds(SER_NETWORK, nVersion);
            for (size_t i = 0; i < _objects.size(); i++) {
                ds.clear();

                ds << _objects[i];
                blobs[i].assign(ds.begin(), ds.end());
            }
        } catch (std::exception&) {
            return false;
        }

        return CBLSIESMultiRecipientBlobs::Encrypt(recipients, blobs);
    }

    bool Encrypt(size_t idx, const CBLSPublicKey& recipient, const Object& obj, int nVersion)
    {
        CDataStream ds(SER_NETWORK, nVersion);
        ds << obj;
        Blob blob(ds.begin(), ds.end());
        return CBLSIESMultiRecipientBlobs::Encrypt(idx, recipient, blob);
    }

    bool Decrypt(size_t idx, const CBLSSecretKey& sk, Object& objectRet, int nVersion) const
    {
        Blob blob;
        if (!CBLSIESMultiRecipientBlobs::Decrypt(idx, sk, blob)) {
            return false;
        }

        try {
            CDataStream ds(blob, SER_NETWORK, nVersion);
            ds >> objectRet;
            return true;
        } catch (std::exception&) {
            return false;
        }
    }
};

#endif // MEMEIUM_CRYPTO_BLS_IES_H
