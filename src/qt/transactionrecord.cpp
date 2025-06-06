// Copyright (c) 2011-2015 The Bitcoin Core developers
// Copyright (c) 2014-2019 The Dash Core developers
// Copyright (c) 2020 The Memeium developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "transactionrecord.h"

#include "assets/assets.h"
#include "base58.h"
#include "consensus/consensus.h"
#include "core_io.h"
#include "timedata.h"
#include "validation.h"
#include "wallet/wallet.h"

#include "privatesend/privatesend.h"

#include <stdint.h>


/* Return positive answer if transaction should be shown in list.
 */
bool TransactionRecord::showTransaction(const CWalletTx& wtx)
{
    // There are currently no cases where we hide transactions, but
    // we may want to use this in the future for things like RBF.
    return true;
}

/*
 * Decompose CWallet transaction to model transaction records.
 */
QList<TransactionRecord> TransactionRecord::decomposeTransaction(const CWallet* wallet, const CWalletTx& wtx)
{
    QList<TransactionRecord> parts;
    int64_t nTime = wtx.GetTxTime();
    CAmount nCredit = wtx.GetCredit(ISMINE_ALL);
    CAmount nDebit = wtx.GetDebit(ISMINE_ALL);
    CAmount nNet = nCredit - nDebit;
    uint256 hash = wtx.GetHash();
    std::map<std::string, std::string> mapValue = wtx.mapValue;

    if (nNet >= 0 || wtx.IsCoinBase()) {
        //
        // Credit
        //
        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            const CTxOut& txout = wtx.tx->vout[i];
            isminetype mine = wallet->IsMine(txout);

            /** MMM START */
            if (txout.scriptPubKey.IsAssetScript() || txout.scriptPubKey.IsNullAssetTxDataScript() || txout.scriptPubKey.IsNullGlobalRestrictionAssetTxDataScript())
                continue;
            /** MMM END */

            if (mine) {
                TransactionRecord sub(hash, nTime);
                CTxDestination address;
                sub.idx = i; // vout index
                sub.credit = txout.nValue;
                sub.involvesWatchAddress = mine & ISMINE_WATCH_ONLY;
                if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*wallet, address)) {
                    // Received by Memeium Address
                    sub.type = TransactionRecord::RecvWithAddress;
                    sub.strAddress = CBitcoinAddress(address).ToString();
                } else {
                    // Received by IP connection (deprecated features), or a multisignature or other non-simple transaction
                    sub.type = TransactionRecord::RecvFromOther;
                    sub.strAddress = mapValue["from"];
                }
                if (wtx.IsCoinBase()) {
                    // Generated
                    sub.type = TransactionRecord::Generated;
                }

                sub.address.SetString(sub.strAddress);
                sub.txDest = sub.address.Get();
                parts.append(sub);
            }
        }
    } else {
        bool fAllFromMeDenom = true;
        int nFromMe = 0;
        bool involvesWatchAddress = false;
        isminetype fAllFromMe = ISMINE_SPENDABLE;
        for (const CTxIn& txin : wtx.tx->vin) {
            if (wallet->IsMine(txin)) {
                fAllFromMeDenom = fAllFromMeDenom && wallet->IsDenominated(txin.prevout);
                nFromMe++;
            }
            isminetype mine = wallet->IsMine(txin);
            if (mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if (fAllFromMe > mine) fAllFromMe = mine;
        }

        isminetype fAllToMe = ISMINE_SPENDABLE;
        bool fAllToMeDenom = true;
        int nToMe = 0;
        for (const CTxOut& txout : wtx.tx->vout) {
            /** MMM START */
            if (txout.scriptPubKey.IsAssetScript() || txout.scriptPubKey.IsNullAssetTxDataScript() || txout.scriptPubKey.IsNullGlobalRestrictionAssetTxDataScript())
                continue;
            /** MMM END */

            if (wallet->IsMine(txout)) {
                fAllToMeDenom = fAllToMeDenom && CPrivateSend::IsDenominatedAmount(txout.nValue);
                nToMe++;
            }
            isminetype mine = wallet->IsMine(txout);
            if (mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if (fAllToMe > mine) fAllToMe = mine;
        }

        if (fAllFromMeDenom && fAllToMeDenom && nFromMe * nToMe) {
            parts.append(TransactionRecord(hash, nTime, TransactionRecord::PrivateSendDenominate, "", -nDebit, nCredit));
            parts.last().involvesWatchAddress = false; // maybe pass to TransactionRecord as constructor argument
        } else if (fAllFromMe && fAllToMe) {
            // Payment to self
            // TODO: this section still not accurate but covers most cases,
            // might need some additional work however

            TransactionRecord sub(hash, nTime);
            // Payment to self by default
            sub.type = TransactionRecord::SendToSelf;
            sub.strAddress = "";

            if (mapValue["DS"] == "1") {
                sub.type = TransactionRecord::PrivateSend;
                CTxDestination address;
                if (ExtractDestination(wtx.tx->vout[0].scriptPubKey, address)) {
                    // Sent to Memeium Address
                    sub.strAddress = CBitcoinAddress(address).ToString();
                } else {
                    // Sent to IP, or other non-address transaction like OP_EVAL
                    sub.strAddress = mapValue["to"];
                }
            } else {
                sub.idx = parts.size();
                if (wtx.tx->vin.size() == 1 && wtx.tx->vout.size() == 1 && CPrivateSend::IsCollateralAmount(nDebit) && CPrivateSend::IsCollateralAmount(nCredit) && CPrivateSend::IsCollateralAmount(-nNet)) {
                    sub.type = TransactionRecord::PrivateSendCollateralPayment;
                } else {
                    for (const auto& txout : wtx.tx->vout) {
                        if (txout.nValue == CPrivateSend::GetMaxCollateralAmount()) {
                            sub.type = TransactionRecord::PrivateSendMakeCollaterals;
                            continue; // Keep looking, could be a part of PrivateSendCreateDenominations
                        } else if (CPrivateSend::IsDenominatedAmount(txout.nValue)) {
                            sub.type = TransactionRecord::PrivateSendCreateDenominations;
                            break; // Done, it's definitely a tx creating mixing denoms, no need to look any further
                        }
                    }
                }
            }

            CAmount nChange = wtx.GetChange();

            sub.debit = -(nDebit - nChange);
            sub.credit = nCredit - nChange;
            sub.address.SetString(sub.strAddress);
            sub.txDest = sub.address.Get();
            parts.append(sub);
            parts.last().involvesWatchAddress = involvesWatchAddress; // maybe pass to TransactionRecord as constructor argument
        } else if (fAllFromMe) {
            //
            // Debit
            //
            CAmount nTxFee = nDebit - wtx.tx->GetValueOut();

            bool fDone = false;
            if (wtx.tx->vin.size() == 1 && wtx.tx->vout.size() == 1 && CPrivateSend::IsCollateralAmount(nDebit) && nCredit == 0 // OP_RETURN
                && CPrivateSend::IsCollateralAmount(-nNet)) {
                TransactionRecord sub(hash, nTime);
                sub.idx = 0;
                sub.type = TransactionRecord::PrivateSendCollateralPayment;
                sub.debit = -nDebit;
                parts.append(sub);
                fDone = true;
            }

            for (unsigned int nOut = 0; nOut < wtx.tx->vout.size() && !fDone; nOut++) {
                const CTxOut& txout = wtx.tx->vout[nOut];

                /** MMM START */
                if (txout.scriptPubKey.IsAssetScript())
                    continue;
                /** MMM END */

                TransactionRecord sub(hash, nTime);
                sub.idx = nOut;
                sub.involvesWatchAddress = involvesWatchAddress;

                if (wallet->IsMine(txout)) {
                    // Ignore parts sent to self, as this is usually the change
                    // from a transaction sent back to our own address.
                    continue;
                }

                CTxDestination address;
                if (ExtractDestination(txout.scriptPubKey, address)) {
                    // Sent to Memeium Address
                    sub.type = TransactionRecord::SendToAddress;
                    sub.strAddress = CBitcoinAddress(address).ToString();
                } else {
                    // Sent to IP, or other non-address transaction like OP_EVAL
                    sub.type = TransactionRecord::SendToOther;
                    sub.strAddress = mapValue["to"];
                }

                if (mapValue["DS"] == "1") {
                    sub.type = TransactionRecord::PrivateSend;
                }

                CAmount nValue = txout.nValue;
                /* Add fee to first output */
                if (nTxFee > 0) {
                    nValue += nTxFee;
                    nTxFee = 0;
                }
                sub.debit = -nValue;

                sub.address.SetString(sub.strAddress);
                sub.txDest = sub.address.Get();

                parts.append(sub);
            }
        } else {
            //
            // Mixed debit transaction, can't break down payees
            //
            parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", nNet, 0));
            parts.last().involvesWatchAddress = involvesWatchAddress;
        }
    }

    /** MMM START */
    if (AreAssetsDeployed()) {
        CAmount nFee;
        std::string strSentAccount;
        std::list<COutputEntry> listReceived;
        std::list<COutputEntry> listSent;

        std::list<CAssetOutputEntry> listAssetsReceived;
        std::list<CAssetOutputEntry> listAssetsSent;

        wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, ISMINE_ALL, listAssetsReceived, listAssetsSent);

        // LogPrintf("TXID: %s: Rec: %d Sent: %d\n", wtx.GetHash().ToString(), listAssetsReceived.size(), listAssetsSent.size());

        if (listAssetsReceived.size() > 0) {
            for (const CAssetOutputEntry& data : listAssetsReceived) {
                TransactionRecord sub(hash, nTime);
                sub.idx = data.vout;

                const CTxOut& txout = wtx.tx->vout[sub.idx];
                isminetype mine = wallet->IsMine(txout);

                sub.address = EncodeDestination(data.destination);
                sub.txDest = sub.address.Get();
                sub.strAddress = EncodeDestination(data.destination);
                sub.assetName = data.assetName;
                sub.credit = data.nAmount;
                sub.involvesWatchAddress = mine & ISMINE_WATCH_ONLY;

                if (data.type == TX_NEW_ASSET)
                    sub.type = TransactionRecord::Issue;
                else if (data.type == TX_REISSUE_ASSET)
                    sub.type = TransactionRecord::Reissue;
                else if (data.type == TX_TRANSFER_ASSET)
                    sub.type = TransactionRecord::TransferFrom;
                else {
                    sub.type = TransactionRecord::Other;
                }

                sub.units = DEFAULT_UNITS;

                if (IsAssetNameAnOwner(sub.assetName))
                    sub.units = OWNER_UNITS;
                else if (CheckIssueDataTx(wtx.tx->vout[sub.idx])) {
                    CNewAsset asset;
                    std::string strAddress;
                    if (AssetFromTransaction(wtx, asset, strAddress))
                        sub.units = asset.units;
                } else {
                    CNewAsset asset;
                    if (passets->GetAssetMetaDataIfExists(sub.assetName, asset))
                        sub.units = asset.units;
                }

                parts.append(sub);
            }
        }

        if (listAssetsSent.size() > 0) {
            for (const CAssetOutputEntry& data : listAssetsSent) {
                TransactionRecord sub(hash, nTime);
                sub.idx = data.vout;
                sub.address = EncodeDestination(data.destination);
                sub.txDest = sub.address.Get();
                sub.strAddress = EncodeDestination(data.destination);
                sub.assetName = data.assetName;
                sub.credit = -data.nAmount;
                sub.involvesWatchAddress = false;

                if (data.type == TX_TRANSFER_ASSET)
                    sub.type = TransactionRecord::TransferTo;
                else
                    sub.type = TransactionRecord::Other;

                if (IsAssetNameAnOwner(sub.assetName))
                    sub.units = OWNER_UNITS;
                else if (CheckIssueDataTx(wtx.tx->vout[sub.idx])) {
                    CNewAsset asset;
                    std::string strAddress;
                    if (AssetFromTransaction(wtx, asset, strAddress))
                        sub.units = asset.units;
                } else {
                    CNewAsset asset;
                    if (passets->GetAssetMetaDataIfExists(sub.assetName, asset))
                        sub.units = asset.units;
                }

                parts.append(sub);
            }
        }
    }
    /** MMM END */

    return parts;
}

void TransactionRecord::updateStatus(const CWalletTx& wtx, int chainLockHeight)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(wtx.GetWallet()->cs_wallet);
    // Determine transaction status

    // Find the block the tx is in
    CBlockIndex* pindex = nullptr;
    BlockMap::iterator mi = mapBlockIndex.find(wtx.hashBlock);
    if (mi != mapBlockIndex.end())
        pindex = (*mi).second;

    // Sort order, unrecorded transactions sort to the top
    status.sortKey = strprintf("%010d-%01d-%010u-%03d",
        (pindex ? pindex->nHeight : std::numeric_limits<int>::max()),
        (wtx.IsCoinBase() ? 1 : 0),
        wtx.nTimeReceived,
        idx);
    status.countsForBalance = wtx.IsTrusted() && !(wtx.GetBlocksToMaturity() > 0);
    status.depth = wtx.GetDepthInMainChain();
    status.cur_num_blocks = chainActive.Height();
    status.cachedChainLockHeight = chainLockHeight;

    bool oldLockedByChainLocks = status.lockedByChainLocks;
    if (!status.lockedByChainLocks) {
        status.lockedByChainLocks = wtx.IsChainLocked();
    }

    auto addrBookIt = wtx.GetWallet()->mapAddressBook.find(this->txDest);
    if (addrBookIt == wtx.GetWallet()->mapAddressBook.end()) {
        status.label = "";
    } else {
        status.label = QString::fromStdString(addrBookIt->second.name);
    }

    if (!CheckFinalTx(wtx)) {
        if (wtx.tx->nLockTime < LOCKTIME_THRESHOLD) {
            status.status = TransactionStatus::OpenUntilBlock;
            status.open_for = wtx.tx->nLockTime - chainActive.Height();
        } else {
            status.status = TransactionStatus::OpenUntilDate;
            status.open_for = wtx.tx->nLockTime;
        }
    }
    // For generated transactions, determine maturity
    else if (type == TransactionRecord::Generated) {
        if (wtx.GetBlocksToMaturity() > 0) {
            status.status = TransactionStatus::Immature;

            if (wtx.IsInMainChain()) {
                status.matures_in = wtx.GetBlocksToMaturity();

                // Check if the block was requested by anyone
                if (GetAdjustedTime() - wtx.nTimeReceived > 2 * 60 && wtx.GetRequestCount() == 0)
                    status.status = TransactionStatus::MaturesWarning;
            } else {
                status.status = TransactionStatus::NotAccepted;
            }
        } else {
            status.status = TransactionStatus::Confirmed;
        }
    } else {
        // The IsLockedByInstantSend call is quite expensive, so we only do it when a state change is actually possible.
        if (status.lockedByChainLocks) {
            if (oldLockedByChainLocks != status.lockedByChainLocks) {
                status.lockedByInstantSend = wtx.IsLockedByInstantSend();
            } else {
                status.lockedByInstantSend = false;
            }
        } else if (!status.lockedByInstantSend) {
            status.lockedByInstantSend = wtx.IsLockedByInstantSend();
        }

        if (status.depth < 0) {
            status.status = TransactionStatus::Conflicted;
        } else if (GetAdjustedTime() - wtx.nTimeReceived > 2 * 60 && wtx.GetRequestCount() == 0) {
            status.status = TransactionStatus::Offline;
        } else if (status.depth == 0) {
            status.status = TransactionStatus::Unconfirmed;
            if (wtx.isAbandoned())
                status.status = TransactionStatus::Abandoned;
        } else if (status.depth < RecommendedNumConfirmations && !status.lockedByChainLocks) {
            status.status = TransactionStatus::Confirming;
        } else {
            status.status = TransactionStatus::Confirmed;
        }
    }
    status.needsUpdate = false;
}

bool TransactionRecord::statusUpdateNeeded(int chainLockHeight)
{
    AssertLockHeld(cs_main);
    return status.cur_num_blocks != chainActive.Height() || status.needsUpdate || (!status.lockedByChainLocks && status.cachedChainLockHeight != chainLockHeight);
}

QString TransactionRecord::getTxID() const
{
    return QString::fromStdString(hash.ToString());
}

int TransactionRecord::getOutputIndex() const
{
    return idx;
}
