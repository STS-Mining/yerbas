// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2020 The Dash Core developers
// Copyright (c) 2020 The Memeium developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "validation.h"

#include "arith_uint256.h"
#include "assets/assetdb.h"
#include "assets/assets.h"
#include "blockencodings.h"
#include "chain.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/tx_verify.h"
#include "consensus/validation.h"
#include "cuckoocache.h"
#include "fs.h"
#include "hash.h"
#include "init.h"
#include "policy/fees.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "reverse_iterator.h"
#include "script/script.h"
#include "script/sigcache.h"
#include "script/standard.h"
#include "spork.h"
#include "timedata.h"
#include "tinyformat.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "undo.h"
#include "util.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "validationinterface.h"
#include "versionbits.h"
#include "warnings.h"
#include <wallet/wallet.h>

#include "smartnode/smartnode-payments.h"
// #include "smartnode/smartnode-collaterals.h"

#include "evo/cbtx.h"
#include "evo/deterministicmns.h"
#include "evo/providertx.h"
#include "evo/specialtx.h"

#include "llmq/quorums_chainlocks.h"
#include "llmq/quorums_instantsend.h"

#include <atomic>
#include <sstream>

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/thread.hpp>

#if defined(NDEBUG)
#error "Memeium Core cannot be compiled without assertions."
#endif

/**
 * Global state
 */

CCriticalSection cs_main;

BlockMap mapBlockIndex;
PrevBlockMap mapPrevBlockIndex;
CChain chainActive;
CBlockIndex* pindexBestHeader = nullptr;
CWaitableCriticalSection csBestBlock;
CConditionVariable cvBlockChange;
int nScriptCheckThreads = 0;
std::atomic_bool fImporting(false);
bool fMessaging = true;
bool fReindex = false;
bool fTxIndex = true;
bool fAssetIndex = false;
bool fAddressIndex = false;
bool fTimestampIndex = false;
bool fSpentIndex = false;
bool fHavePruned = false;
bool fPruneMode = false;
bool fIsBareMultisigStd = DEFAULT_PERMIT_BAREMULTISIG;
bool fRequireStandard = true;
unsigned int nBytesPerSigOp = DEFAULT_BYTES_PER_SIGOP;
bool fCheckBlockIndex = false;
bool fCheckpointsEnabled = DEFAULT_CHECKPOINTS_ENABLED;
size_t nCoinCacheUsage = 5000 * 300;
uint64_t nPruneTarget = 0;
int64_t nMaxTipAge = DEFAULT_MAX_TIP_AGE;

std::atomic<bool> fDIP0001ActiveAtTip{false};
std::atomic<bool> fDIP0003ActiveAtTip{false};

uint256 hashAssumeValid;
arith_uint256 nMinimumChainWork;

CFeeRate minRelayTxFee = CFeeRate(DEFAULT_MIN_RELAY_TX_FEE);
CAmount maxTxFee = DEFAULT_TRANSACTION_MAXFEE;

bool fAssetUnitTest = false;

CBlockPolicyEstimator feeEstimator;
CTxMemPool mempool(&feeEstimator);

static void CheckBlockIndex(const Consensus::Params& consensusParams);

/** Constant stuff for coinbase transactions we create: */
CScript COINBASE_FLAGS;

const std::string strMessageMagic = "DarkCoin Signed Message:\n";

// Internal stuff
namespace
{

struct CBlockIndexWorkComparator {
    bool operator()(const CBlockIndex* pa, const CBlockIndex* pb) const
    {
        // First sort by most total work, ...
        if (pa->nChainWork > pb->nChainWork) return false;
        if (pa->nChainWork < pb->nChainWork) return true;

        // ... then by earliest time received, ...
        if (pa->nSequenceId < pb->nSequenceId) return false;
        if (pa->nSequenceId > pb->nSequenceId) return true;

        // Use pointer address as tie breaker (should only happen with blocks
        // loaded from disk, as those all have id 0).
        if (pa < pb) return false;
        if (pa > pb) return true;

        // Identical blocks.
        return false;
    }
};

CBlockIndex* pindexBestInvalid;

/**
 * The set of all CBlockIndex entries with BLOCK_VALID_TRANSACTIONS (for itself and all ancestors) and
 * as good as our current tip or better. Entries may be failed, though, and pruning nodes may be
 * missing the data for the block.
 */
std::set<CBlockIndex*, CBlockIndexWorkComparator> setBlockIndexCandidates;
/** All pairs A->B, where A (or one of its ancestors) misses transactions, but B has transactions.
 * Pruned nodes may have entries where B is missing data.
 */
std::multimap<CBlockIndex*, CBlockIndex*> mapBlocksUnlinked;

CCriticalSection cs_LastBlockFile;
std::vector<CBlockFileInfo> vinfoBlockFile;
int nLastBlockFile = 0;
/** Global flag to indicate we should check to see if there are
 *  block/undo files that should be deleted.  Set on startup
 *  or if we allocate more file space when we're in prune mode
 */
bool fCheckForPruning = false;

/**
 * Every received block is assigned a unique and increasing identifier, so we
 * know which one to give priority in case of a fork.
 */
CCriticalSection cs_nBlockSequenceId;
/** Blocks loaded from disk are assigned id 0, so start the counter at 1. */
int32_t nBlockSequenceId = 1;
/** Decreasing counter (used by subsequent preciousblock calls). */
int32_t nBlockReverseSequenceId = -1;
/** chainwork for the last block that preciousblock has been applied to. */
arith_uint256 nLastPreciousChainwork = 0;

/** In order to efficiently track invalidity of headers, we keep the set of
 * blocks which we tried to connect and found to be invalid here (ie which
 * were set to BLOCK_FAILED_VALID since the last restart). We can then
 * walk this set and check if a new header is a descendant of something in
 * this set, preventing us from having to walk mapBlockIndex when we try
 * to connect a bad block and fail.
 *
 * While this is more complicated than marking everything which descends
 * from an invalid block as invalid at the time we discover it to be
 * invalid, doing so would require walking all of mapBlockIndex to find all
 * descendants. Since this case should be very rare, keeping track of all
 * BLOCK_FAILED_VALID blocks in a set should be just fine and work just as
 * well.
 *
 * Because we alreardy walk mapBlockIndex in height-order at startup, we go
 * ahead and mark descendants of invalid blocks as FAILED_CHILD at that time,
 * instead of putting things in this set.
 */
std::set<CBlockIndex*> g_failed_blocks;

/** Dirty block index entries. */
std::set<CBlockIndex*> setDirtyBlockIndex;

/** Dirty block file entries. */
std::set<int> setDirtyFileInfo;
} // namespace

CBlockIndex* FindForkInGlobalIndex(const CChain& chain, const CBlockLocator& locator)
{
    // Find the first block the caller has in the main chain
    for (const uint256& hash : locator.vHave) {
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end()) {
            CBlockIndex* pindex = (*mi).second;
            if (chain.Contains(pindex))
                return pindex;
        }
    }
    return chain.Genesis();
}

CCoinsViewDB* pcoinsdbview = nullptr;
CCoinsViewCache* pcoinsTip = nullptr;
CBlockTreeDB* pblocktree = nullptr;

/* MMM ASSETS START */
CAssetsDB* passetsdb = nullptr;
CAssetsCache* passets = nullptr;
CLRUCache<std::string, CDatabasedAssetData>* passetsCache = nullptr;
CLRUCache<std::string, CMessage>* pMessagesCache = nullptr;
CLRUCache<std::string, int>* pMessageSubscribedChannelsCache = nullptr;
CLRUCache<std::string, int>* pMessagesSeenAddressCache = nullptr;
CMessageDB* pmessagedb = nullptr;
CMessageChannelDB* pmessagechanneldb = nullptr;
CMyRestrictedDB* pmyrestricteddb = nullptr;
CSnapshotRequestDB* pSnapshotRequestDb = nullptr;
CAssetSnapshotDB* pAssetSnapshotDb = nullptr;
CDistributeSnapshotRequestDB* pDistributeSnapshotDb = nullptr;

CLRUCache<std::string, CNullAssetTxVerifierString>* passetsVerifierCache = nullptr;
CLRUCache<std::string, int8_t>* passetsQualifierCache = nullptr;
CLRUCache<std::string, int8_t>* passetsRestrictionCache = nullptr;
CLRUCache<std::string, int8_t>* passetsGlobalRestrictionCache = nullptr;
CRestrictedDB* prestricteddb = nullptr;
/* MMM ASSETS END*/

enum FlushStateMode {
    FLUSH_STATE_NONE,
    FLUSH_STATE_IF_NEEDED,
    FLUSH_STATE_PERIODIC,
    FLUSH_STATE_ALWAYS
};

// See definition for documentation
static bool FlushStateToDisk(const CChainParams& chainParams, CValidationState& state, FlushStateMode mode, int nManualPruneHeight = 0);
static void FindFilesToPruneManual(std::set<int>& setFilesToPrune, int nManualPruneHeight);
static void FindFilesToPrune(std::set<int>& setFilesToPrune, uint64_t nPruneAfterHeight);
bool CheckInputs(const CTransaction& tx, CValidationState& state, const CCoinsViewCache& inputs, bool fScriptChecks, unsigned int flags, bool cacheSigStore, bool cacheFullScriptStore, PrecomputedTransactionData& txdata, std::vector<CScriptCheck>* pvChecks = nullptr);
static FILE* OpenUndoFile(const CDiskBlockPos& pos, bool fReadOnly = false);

bool CheckFinalTx(const CTransaction& tx, int flags)
{
    AssertLockHeld(cs_main);

    // By convention a negative value for flags indicates that the
    // current network-enforced consensus rules should be used. In
    // a future soft-fork scenario that would mean checking which
    // rules would be enforced for the next block and setting the
    // appropriate flags. At the present time no soft-forks are
    // scheduled, so no flags are set.
    flags = std::max(flags, 0);

    // CheckFinalTx() uses chainActive.Height()+1 to evaluate
    // nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a
    // transaction can be part of the *next* block, we need to call
    // IsFinalTx() with one more than chainActive.Height().
    const int nBlockHeight = chainActive.Height() + 1;

    // BIP113 will require that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // IsFinalTx() if LOCKTIME_MEDIAN_TIME_PAST is set.
    const int64_t nBlockTime = (flags & LOCKTIME_MEDIAN_TIME_PAST)
                                   ? chainActive.Tip()->GetMedianTimePast()
                                   : GetAdjustedTime();

    return IsFinalTx(tx, nBlockHeight, nBlockTime);
}

bool TestLockPointValidity(const LockPoints* lp)
{
    AssertLockHeld(cs_main);
    assert(lp);
    // If there are relative lock times then the maxInputBlock will be set
    // If there are no relative lock times, the LockPoints don't depend on the chain
    if (lp->maxInputBlock) {
        // Check whether chainActive is an extension of the block at which the LockPoints
        // calculation was valid.  If not LockPoints are no longer valid
        if (!chainActive.Contains(lp->maxInputBlock)) {
            return false;
        }
    }

    // LockPoints still valid
    return true;
}

bool CheckSequenceLocks(const CTransaction& tx, int flags, LockPoints* lp, bool useExistingLockPoints)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(mempool.cs);

    CBlockIndex* tip = chainActive.Tip();
    CBlockIndex index;
    index.pprev = tip;
    // CheckSequenceLocks() uses chainActive.Height()+1 to evaluate
    // height based locks because when SequenceLocks() is called within
    // ConnectBlock(), the height of the block *being*
    // evaluated is what is used.
    // Thus if we want to know if a transaction can be part of the
    // *next* block, we need to use one more than chainActive.Height()
    index.nHeight = tip->nHeight + 1;

    std::pair<int, int64_t> lockPair;
    if (useExistingLockPoints) {
        assert(lp);
        lockPair.first = lp->height;
        lockPair.second = lp->time;
    } else {
        // pcoinsTip contains the UTXO set for chainActive.Tip()
        CCoinsViewMemPool viewMemPool(pcoinsTip, mempool);
        std::vector<int> prevheights;
        prevheights.resize(tx.vin.size());
        for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
            const CTxIn& txin = tx.vin[txinIndex];
            Coin coin;
            if (!viewMemPool.GetCoin(txin.prevout, coin)) {
                return error("%s: Missing input", __func__);
            }
            if (coin.nHeight == MEMPOOL_HEIGHT) {
                // Assume all mempool transaction confirm in the next block
                prevheights[txinIndex] = tip->nHeight + 1;
            } else {
                prevheights[txinIndex] = coin.nHeight;
            }
        }
        lockPair = CalculateSequenceLocks(tx, flags, &prevheights, index);
        if (lp) {
            lp->height = lockPair.first;
            lp->time = lockPair.second;
            // Also store the hash of the block with the highest height of
            // all the blocks which have sequence locked prevouts.
            // This hash needs to still be on the chain
            // for these LockPoint calculations to be valid
            // Note: It is impossible to correctly calculate a maxInputBlock
            // if any of the sequence locked inputs depend on unconfirmed txs,
            // except in the special case where the relative lock time/height
            // is 0, which is equivalent to no sequence lock. Since we assume
            // input height of tip+1 for mempool txs and test the resulting
            // lockPair from CalculateSequenceLocks against tip+1.  We know
            // EvaluateSequenceLocks will fail if there was a non-zero sequence
            // lock on a mempool input, so we can use the return value of
            // CheckSequenceLocks to indicate the LockPoints validity
            int maxInputHeight = 0;
            for (int height : prevheights) {
                // Can ignore mempool inputs since we'll fail if they had non-zero locks
                if (height != tip->nHeight + 1) {
                    maxInputHeight = std::max(maxInputHeight, height);
                }
            }
            lp->maxInputBlock = tip->GetAncestor(maxInputHeight);
        }
    }
    return EvaluateSequenceLocks(index, lockPair);
}

bool GetUTXOCoin(const COutPoint& outpoint, Coin& coin)
{
    LOCK(cs_main);
    if (!pcoinsTip->GetCoin(outpoint, coin))
        return false;
    if (coin.IsSpent())
        return false;
    return true;
}

int GetUTXOHeight(const COutPoint& outpoint)
{
    // -1 means UTXO is yet unknown or already spent
    Coin coin;
    return GetUTXOCoin(outpoint, coin) ? coin.nHeight : -1;
}

int GetUTXOConfirmations(const COutPoint& outpoint)
{
    // -1 means UTXO is yet unknown or already spent
    LOCK(cs_main);
    int nPrevoutHeight = GetUTXOHeight(outpoint);
    return (nPrevoutHeight > -1 && chainActive.Tip()) ? chainActive.Height() - nPrevoutHeight + 1 : -1;
}


bool ContextualCheckTransaction(const CTransaction& tx, CValidationState& state, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int nHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;
    bool fDIP0001Active_context = consensusParams.DIP0001Enabled;
    bool fDIP0003Active_context = consensusParams.DIP0003Enabled;

    if (fDIP0003Active_context) {
        // check version 3 transaction types
        if (tx.nVersion >= 3) {
            if (tx.nType != TRANSACTION_NORMAL &&
                tx.nType != TRANSACTION_PROVIDER_REGISTER &&
                tx.nType != TRANSACTION_PROVIDER_UPDATE_SERVICE &&
                tx.nType != TRANSACTION_PROVIDER_UPDATE_REGISTRAR &&
                tx.nType != TRANSACTION_PROVIDER_UPDATE_REVOKE &&
                tx.nType != TRANSACTION_COINBASE &&
                tx.nType != TRANSACTION_QUORUM_COMMITMENT &&
                tx.nType != TRANSACTION_ASSET_REGISTER &&
                tx.nType != TRANSACTION_ASSET_REISUE) {
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-type");
            }
            if (tx.IsCoinBase() && tx.nType != TRANSACTION_COINBASE)
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-cb-type");
        } else if (tx.nType != TRANSACTION_NORMAL) {
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-type");
        }
    }

    // Size limits
    if (fDIP0001Active_context && ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION) > MAX_STANDARD_TX_SIZE)
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-oversize");

    return true;
}

// Returns the script flags which should be checked for a given block
static unsigned int GetBlockScriptFlags(const CBlockIndex* pindex, const Consensus::Params& chainparams);

static void LimitMempoolSize(CTxMemPool& pool, size_t limit, unsigned long age)
{
    int expired = pool.Expire(GetTime() - age);
    if (expired != 0) {
        LogPrint(BCLog::MEMPOOL, "Expired %i transactions from the memory pool\n", expired);
    }

    std::vector<COutPoint> vNoSpendsRemaining;
    pool.TrimToSize(limit, &vNoSpendsRemaining);
    for (const COutPoint& removed : vNoSpendsRemaining)
        pcoinsTip->Uncache(removed);
}

/** Convert CValidationState to a human-readable message for logging */
std::string FormatStateMessage(const CValidationState& state)
{
    return strprintf("%s%s (code %i)",
        state.GetRejectReason(),
        state.GetDebugMessage().empty() ? "" : ", " + state.GetDebugMessage(),
        state.GetRejectCode());
}

static bool IsCurrentForFeeEstimation()
{
    AssertLockHeld(cs_main);
    if (IsInitialBlockDownload())
        return false;
    if (chainActive.Tip()->GetBlockTime() < (GetTime() - MAX_FEE_ESTIMATION_TIP_AGE))
        return false;
    if (chainActive.Height() < pindexBestHeader->nHeight - 1)
        return false;
    return true;
}

/* Make mempool consistent after a reorg, by re-adding or recursively erasing
 * disconnected block transactions from the mempool, and also removing any
 * other transactions from the mempool that are no longer valid given the new
 * tip/height.
 *
 * Note: we assume that disconnectpool only contains transactions that are NOT
 * confirmed in the current chain nor already in the mempool (otherwise,
 * in-mempool descendants of such transactions would be removed).
 *
 * Passing fAddToMempool=false will skip trying to add the transactions back,
 * and instead just erase from the mempool as needed.
 */

void UpdateMempoolForReorg(DisconnectedBlockTransactions& disconnectpool, bool fAddToMempool)
{
    AssertLockHeld(cs_main);
    std::vector<uint256> vHashUpdate;
    // disconnectpool's insertion_order index sorts the entries from
    // oldest to newest, but the oldest entry will be the last tx from the
    // latest mined block that was disconnected.
    // Iterate disconnectpool in reverse, so that we add transactions
    // back to the mempool starting with the earliest transaction that had
    // been previously seen in a block.
    auto it = disconnectpool.queuedTx.get<insertion_order>().rbegin();
    while (it != disconnectpool.queuedTx.get<insertion_order>().rend()) {
        // ignore validation errors in resurrected transactions
        CValidationState stateDummy;
        if (!fAddToMempool || (*it)->IsCoinBase() || !AcceptToMemoryPool(mempool, stateDummy, *it, false, nullptr, true)) {
            // If the transaction doesn't make it in to the mempool, remove any
            // transactions that depend on it (which would now be orphans).
            mempool.removeRecursive(**it, MemPoolRemovalReason::REORG);
        } else if (mempool.exists((*it)->GetHash())) {
            vHashUpdate.push_back((*it)->GetHash());
        }
        ++it;
    }
    disconnectpool.queuedTx.clear();
    // AcceptToMemoryPool/addUnchecked all assume that new mempool entries have
    // no in-mempool children, which is generally not true when adding
    // previously-confirmed transactions back to the mempool.
    // UpdateTransactionsFromBlock finds descendants of any transactions in
    // the disconnectpool that were added back and cleans up the mempool state.
    mempool.UpdateTransactionsFromBlock(vHashUpdate);

    // We also need to remove any now-immature transactions
    mempool.removeForReorg(pcoinsTip, chainActive.Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);
    // Re-limit mempool size, in case we added any transactions
    LimitMempoolSize(mempool, gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000, gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
}

// Used to avoid mempool polluting consensus critical paths if CCoinsViewMempool
// were somehow broken and returning the wrong scriptPubKeys
static bool CheckInputsFromMempoolAndCache(const CTransaction& tx, CValidationState& state, const CCoinsViewCache& view, CTxMemPool& pool, unsigned int flags, bool cacheSigStore, PrecomputedTransactionData& txdata)
{
    AssertLockHeld(cs_main);

    // pool.cs should be locked already, but go ahead and re-take the lock here
    // to enforce that mempool doesn't change between when we check the view
    // and when we actually call through to CheckInputs
    LOCK(pool.cs);

    assert(!tx.IsCoinBase());
    for (const CTxIn& txin : tx.vin) {
        const Coin& coin = view.AccessCoin(txin.prevout);

        // At this point we haven't actually checked if the coins are all
        // available (or shouldn't assume we have, since CheckInputs does).
        // So we just return failure if the inputs are not available here,
        // and then only have to check equivalence for available inputs.
        if (coin.IsSpent()) return false;

        const CTransactionRef& txFrom = pool.get(txin.prevout.hash);
        if (txFrom) {
            assert(txFrom->GetHash() == txin.prevout.hash);
            assert(txFrom->vout.size() > txin.prevout.n);
            assert(txFrom->vout[txin.prevout.n] == coin.out);
        } else {
            const Coin& coinFromDisk = pcoinsTip->AccessCoin(txin.prevout);
            assert(!coinFromDisk.IsSpent());
            assert(coinFromDisk.out == coin.out);
        }
    }

    return CheckInputs(tx, state, view, true, flags, cacheSigStore, true, txdata);
}

static bool AcceptToMemoryPoolWorker(const CChainParams& chainparams, CTxMemPool& pool, CValidationState& state, const CTransactionRef& ptx, bool fLimitFree, bool* pfMissingInputs, int64_t nAcceptTime, bool fOverrideMempoolLimit, const CAmount& nAbsurdFee, std::vector<COutPoint>& coins_to_uncache, bool fDryRun)
{
    const CTransaction& tx = *ptx;
    const uint256 hash = tx.GetHash();
    std::vector<std::pair<std::string, uint256>> vReissueAssets;

    AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;
    // skip foudner check here
    if (!CheckTransaction(tx, state, 0, 0))
        return false; // state filled in by CheckTransaction

    if (!ContextualCheckTransaction(tx, state, chainparams.GetConsensus(), chainActive.Tip()))
        return error("%s: ContextualCheckTransaction: %s, %s", __func__, hash.ToString(), FormatStateMessage(state));

    if (tx.nVersion == 3 && tx.nType == TRANSACTION_QUORUM_COMMITMENT) {
        // quorum commitment is not allowed outside of blocks
        return state.DoS(100, false, REJECT_INVALID, "qc-not-allowed");
    }

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, false, REJECT_INVALID, "coinbase");

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    std::string reason;
    if (fRequireStandard && !IsStandardTx(tx, reason))
        return state.DoS(0, false, REJECT_NONSTANDARD, reason);

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    if (!CheckFinalTx(tx, STANDARD_LOCKTIME_VERIFY_FLAGS))
        return state.DoS(0, false, REJECT_NONSTANDARD, "non-final");

    // is it already in the memory pool?
    if (pool.exists(hash)) {
        return state.Invalid(false, REJECT_DUPLICATE, "txn-already-in-mempool");
    }

    llmq::CInstantSendLockPtr conflictLock = llmq::quorumInstantSendManager->GetConflictingLock(tx);
    if (conflictLock) {
        CTransactionRef txConflict;
        uint256 hashBlock;
        if (GetTransaction(conflictLock->txid, txConflict, chainparams.GetConsensus(), hashBlock)) {
            GetMainSignals().NotifyInstantSendDoubleSpendAttempt(tx, *txConflict);
        }
        return state.DoS(10, error("AcceptToMemoryPool : Transaction %s conflicts with locked TX %s", hash.ToString(), conflictLock->txid.ToString()),
            REJECT_INVALID, "tx-txlock-conflict");
    }

    bool isAssetsactive = Params().IsAssetsActive(chainActive.Tip());
    // Check for conflicts with in-memory transactions
    {
        LOCK(pool.cs); // protect pool.mapNextTx
        for (const CTxIn& txin : tx.vin) {
            auto itConflicting = pool.mapNextTx.find(txin.prevout);
            if (itConflicting != pool.mapNextTx.end()) {
                const CTransaction* ptxConflicting = itConflicting->second;

                // Transaction conflicts with mempool and RBF doesn't exist in Memeium
                return state.Invalid(false, REJECT_DUPLICATE, "txn-mempool-conflict");
            }
        }
    }

    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        LockPoints lp;
        {
            LOCK(pool.cs);
            CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
            view.SetBackend(viewMemPool);

            // do all inputs exist?
            for (const CTxIn txin : tx.vin) {
                if (!pcoinsTip->HaveCoinInCache(txin.prevout)) {
                    coins_to_uncache.push_back(txin.prevout);
                }
                if (!view.HaveCoin(txin.prevout)) {
                    // Are inputs missing because we already have the tx?
                    for (size_t out = 0; out < tx.vout.size(); out++) {
                        // Optimistically just do efficient check of cache for outputs
                        if (pcoinsTip->HaveCoinInCache(COutPoint(hash, out))) {
                            return state.Invalid(false, REJECT_DUPLICATE, "txn-already-known");
                        }
                    }
                    // Otherwise assume this might be an orphan tx for which we just haven't seen parents yet
                    if (pfMissingInputs) {
                        *pfMissingInputs = true;
                    }
                    return false; // fMissingInputs and !state.IsInvalid() is used to detect this condition, don't set state.Invalid()
                }
            }

            // Bring the best block into scope
            view.GetBestBlock();

            // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
            view.SetBackend(dummy);

            // Only accept BIP68 sequence locked transactions that can be mined in the next
            // block; we don't want our mempool filled up with transactions that can't
            // be mined yet.
            // Must keep pool.cs for this unless we change CheckSequenceLocks to take a
            // CoinsViewCache instead of create its own
            if (!CheckSequenceLocks(tx, STANDARD_LOCKTIME_VERIFY_FLAGS, &lp))
                return state.DoS(0, false, REJECT_NONSTANDARD, "non-BIP68-final");

        } // end LOCK(pool.cs)

        CAmount nFees = 0;
        CAmount specialTxFees = 0;
        if (!Consensus::CheckTxInputs(tx, state, view, GetSpendHeight(view), nFees, specialTxFees, true)) {
            return error("%s: Consensus::CheckTxInputs: %s, %s", __func__, tx.GetHash().ToString(), FormatStateMessage(state));
        }

        /** MMM START START */
        if (!isAssetsactive) {
            for (auto out : tx.vout)
                if (out.scriptPubKey.IsAssetScript())
                    return state.DoS(100, error("%s : Received Block with tx that contained an asset when assets wasn't active", __func__), REJECT_INVALID, "bad-txns-assets-not-active");
        } else {
            if (!Consensus::CheckTxAssets(tx, state, view, GetCurrentAssetCache(), true, vReissueAssets, isAssetsactive)) {
                return error("%s: Consensus::CheckTxAssets: %s, %s", __func__, tx.GetHash().ToString(),
                    FormatStateMessage(state));
            }
        }
        /** MMM ASSETS END */

        // Check for non-standard pay-to-script-hash in inputs
        if (fRequireStandard && !AreInputsStandard(tx, view))
            return state.Invalid(false, REJECT_NONSTANDARD, "bad-txns-nonstandard-inputs");

        unsigned int nSigOps = GetTransactionSigOpCount(tx, view, STANDARD_SCRIPT_VERIFY_FLAGS);

        // nModifiedFees includes any fee deltas from PrioritiseTransaction
        CAmount nModifiedFees = nFees;
        pool.ApplyDelta(hash, nModifiedFees);

        // Keep track of transactions that spend a coinbase, which we re-scan
        // during reorgs to ensure COINBASE_MATURITY is still met.
        bool fSpendsCoinbase = false;
        for (const CTxIn& txin : tx.vin) {
            const Coin& coin = view.AccessCoin(txin.prevout);
            if (coin.IsCoinBase()) {
                fSpendsCoinbase = true;
                break;
            }
        }

        CTxMemPoolEntry entry(ptx, nFees, specialTxFees, nAcceptTime, chainActive.Height(),
            fSpendsCoinbase, nSigOps, lp);
        unsigned int nSize = entry.GetTxSize();

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_STANDARD_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        if ((nSigOps > MAX_STANDARD_TX_SIGOPS) || (nBytesPerSigOp && nSigOps > nSize / nBytesPerSigOp))
            return state.DoS(0, false, REJECT_NONSTANDARD, "bad-txns-too-many-sigops", false,
                strprintf("%d", nSigOps));

        CAmount mempoolRejectFee = pool.GetMinFee(gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000).GetFee(nSize);
        if (mempoolRejectFee > 0 && nModifiedFees < mempoolRejectFee) {
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "mempool min fee not met", false, strprintf("%d < %d", nFees, mempoolRejectFee));
        }

        // No transactions are allowed below minRelayTxFee except from disconnected blocks
        if (fLimitFree && nModifiedFees < ::minRelayTxFee.GetFee(nSize)) {
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "min relay fee not met");
        }

        if (nAbsurdFee && nFees > nAbsurdFee)
            return state.Invalid(false,
                REJECT_HIGHFEE, "absurdly-high-fee",
                strprintf("%d > %d", nFees, nAbsurdFee));

        // Calculate in-mempool ancestors, up to a limit.
        CTxMemPool::setEntries setAncestors;
        size_t nLimitAncestors = gArgs.GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT);
        size_t nLimitAncestorSize = gArgs.GetArg("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT) * 1000;
        size_t nLimitDescendants = gArgs.GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT);
        size_t nLimitDescendantSize = gArgs.GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT) * 1000;
        std::string errString;
        if (!pool.CalculateMemPoolAncestors(entry, setAncestors, nLimitAncestors, nLimitAncestorSize, nLimitDescendants, nLimitDescendantSize, errString)) {
            return state.DoS(0, false, REJECT_NONSTANDARD, "too-long-mempool-chain", false, errString);
        }

        // check special TXs after all the other checks. If we'd do this before the other checks, we might end up
        // DoS scoring a node for non-critical errors, e.g. duplicate keys because a TX is received that was already
        // mined
        if (!CheckSpecialTx(tx, chainActive.Tip(), state))
            return false;

        if (pool.existsProviderTxConflict(tx)) {
            return state.DoS(0, false, REJECT_DUPLICATE, "protx-dup");
        }

        // If we aren't going to actually accept it but just were verifying it, we are fine already
        if (fDryRun) return true;

        constexpr unsigned int scriptVerifyFlags = STANDARD_SCRIPT_VERIFY_FLAGS;

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        PrecomputedTransactionData txdata(tx);
        if (!CheckInputs(tx, state, view, true, STANDARD_SCRIPT_VERIFY_FLAGS, true, false, txdata))
            return false; // state filled in by CheckInputs

        // Check again against the current block tip's script verification
        // flags to cache our script execution flags. This is, of course,
        // useless if the next block has different script flags from the
        // previous one, but because the cache tracks script flags for us it
        // will auto-invalidate and we'll just have a few blocks of extra
        // misses on soft-fork activation.
        //
        // This is also useful in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks (using TestBlockValidity), however allowing such
        // transactions into the mempool can be exploited as a DoS attack.
        unsigned int currentBlockScriptVerifyFlags = GetBlockScriptFlags(chainActive.Tip(), chainparams.GetConsensus());
        if (!CheckInputsFromMempoolAndCache(tx, state, view, pool, currentBlockScriptVerifyFlags, true, txdata)) {
            return error("%s: BUG! PLEASE REPORT THIS! CheckInputs failed against latest-block but not STANDARD flags %s, %s",
                __func__, hash.ToString(), FormatStateMessage(state));
        }

        // This transaction should only count for fee estimation if the
        // node is not behind, and the transaction is not dependent on any other
        // transactions in the mempool. Also ignore 0-fee txes.
        bool validForFeeEstimation = (nFees != 0) && IsCurrentForFeeEstimation() && pool.HasNoInputsOf(tx);

        // Store transaction in memory
        pool.addUnchecked(hash, entry, setAncestors, validForFeeEstimation);

        // Add memory address index
        if (fAddressIndex) {
            pool.addAddressIndex(entry, view);
        }

        // Add memory spent index
        if (fSpentIndex) {
            pool.addSpentIndex(entry, view);
        }

        // trim mempool and check if tx was trimmed
        if (!fOverrideMempoolLimit) {
            LimitMempoolSize(pool, gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000, gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
            if (!pool.exists(hash))
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "mempool full");
        }

        for (auto out : vReissueAssets) {
            mapReissuedAssets.insert(out);
            mapReissuedTx.insert(std::make_pair(out.second, out.first));
        }

        if (Params().IsAssetsActive(chainActive.Tip())) {
            for (auto out : tx.vout) {
                if (out.scriptPubKey.IsAssetScript()) {
                    CAssetOutputEntry data;
                    if (!GetAssetData(out.scriptPubKey, data))
                        continue;
                    if (data.type == TX_NEW_ASSET) {
                        pool.mapAssetToHash[data.assetName] = hash;
                        pool.mapHashToAsset[hash] = data.assetName;
                    }
                }
            }
        }
    }

    if (!fDryRun)
        GetMainSignals().TransactionAddedToMempool(ptx, nAcceptTime);

    return true;
}

/** (try to) add transaction to memory pool with a specified acceptance time **/
static bool AcceptToMemoryPoolWithTime(const CChainParams& chainparams, CTxMemPool& pool, CValidationState& state, const CTransactionRef& tx, bool fLimitFree, bool* pfMissingInputs, int64_t nAcceptTime, bool fOverrideMempoolLimit, const CAmount nAbsurdFee, bool fDryRun)
{
    std::vector<COutPoint> coins_to_uncache;
    bool res = AcceptToMemoryPoolWorker(chainparams, pool, state, tx, fLimitFree, pfMissingInputs, nAcceptTime, fOverrideMempoolLimit, nAbsurdFee, coins_to_uncache, fDryRun);
    if (!res || fDryRun) {
        if (!res) LogPrint(BCLog::MEMPOOL, "%s: %s %s (%s)\n", __func__, tx->GetHash().ToString(), state.GetRejectReason(), state.GetDebugMessage());
        for (const COutPoint& hashTx : coins_to_uncache)
            pcoinsTip->Uncache(hashTx);
    }
    // After we've (potentially) uncached entries, ensure our coins cache is still within its size limits
    CValidationState stateDummy;
    FlushStateToDisk(chainparams, stateDummy, FLUSH_STATE_PERIODIC);
    return res;
}

bool AcceptToMemoryPool(CTxMemPool& pool, CValidationState& state, const CTransactionRef& tx, bool fLimitFree, bool* pfMissingInputs, bool fOverrideMempoolLimit, const CAmount nAbsurdFee, bool fDryRun)
{
    const CChainParams& chainparams = Params();
    return AcceptToMemoryPoolWithTime(chainparams, pool, state, tx, fLimitFree, pfMissingInputs, GetTime(), fOverrideMempoolLimit, nAbsurdFee, fDryRun);
}

bool GetTimestampIndex(const unsigned int& high, const unsigned int& low, std::vector<uint256>& hashes)
{
    if (!fTimestampIndex)
        return error("Timestamp index not enabled");

    if (!pblocktree->ReadTimestampIndex(high, low, hashes))
        return error("Unable to get hashes for timestamps");

    return true;
}

bool GetSpentIndex(CSpentIndexKey& key, CSpentIndexValue& value)
{
    if (!fSpentIndex)
        return false;

    if (mempool.getSpentIndex(key, value))
        return true;

    if (!pblocktree->ReadSpentIndex(key, value))
        return false;

    return true;
}

bool GetAddressIndex(uint160 addressHash, int type, std::vector<std::pair<CAddressIndexKey, CAmount>>& addressIndex, int start, int end)
{
    if (!fAddressIndex)
        return error("address index not enabled");

    if (!pblocktree->ReadAddressIndex(addressHash, type, addressIndex, start, end))
        return error("unable to get txids for address");

    return true;
}

bool GetAddressUnspent(uint160 addressHash, int type, std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>>& unspentOutputs)
{
    if (!fAddressIndex)
        return error("address index not enabled");

    if (!pblocktree->ReadAddressUnspentIndex(addressHash, type, unspentOutputs))
        return error("unable to get txids for address");

    return true;
}

/** Return transaction in txOut, and if it was found inside a block, its hash is placed in hashBlock */
bool GetTransaction(const uint256& hash, CTransactionRef& txOut, const Consensus::Params& consensusParams, uint256& hashBlock, bool fAllowSlow)
{
    CBlockIndex* pindexSlow = nullptr;

    LOCK(cs_main);

    CTransactionRef ptx = mempool.get(hash);
    if (ptx) {
        txOut = ptx;
        return true;
    }

    if (fTxIndex) {
        CDiskTxPos postx;
        if (pblocktree->ReadTxIndex(hash, postx)) {
            CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
            if (file.IsNull())
                return error("%s: OpenBlockFile failed", __func__);
            CBlockHeader header;
            try {
                file >> header;
                fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
                file >> txOut;
            } catch (const std::exception& e) {
                return error("%s: Deserialize or I/O error - %s", __func__, e.what());
            }
            hashBlock = header.GetHash();
            if (txOut->GetHash() != hash)
                return error("%s: txid mismatch", __func__);
            return true;
        }

        // transaction not found in index, nothing more can be done
        return false;
    }

    if (fAllowSlow) { // use coin database to locate block that contains transaction, and scan it
        const Coin& coin = AccessByTxid(*pcoinsTip, hash);
        if (!coin.IsSpent()) pindexSlow = chainActive[coin.nHeight];
    }

    if (pindexSlow) {
        CBlock block;
        if (ReadBlockFromDisk(block, pindexSlow, consensusParams)) {
            for (const auto& tx : block.vtx) {
                if (tx->GetHash() == hash) {
                    txOut = tx;
                    hashBlock = pindexSlow->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}


//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

static bool WriteBlockToDisk(const CBlock& block, CDiskBlockPos& pos, const CMessageHeader::MessageStartChars& messageStart)
{
    // Open history file to append
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("WriteBlockToDisk: OpenBlockFile failed");

    // Write index header
    unsigned int nSize = GetSerializeSize(fileout, block);
    fileout << FLATDATA(messageStart) << nSize;

    // Write block
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("WriteBlockToDisk: ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool CheckPOW(const CBlock& block, const Consensus::Params& consensusParams)
{
    if (!CheckProofOfWork(block.GetPOWHash(), block.nBits, consensusParams)) {
        LogPrintf("CheckPOW: CheckProofOfWork failed for %s, retesting without POW cache\n", block.GetHash().ToString());
        // Retest without POW cache in case cache was corrupted:
        return CheckProofOfWork(block.GetPOWHash(false), block.nBits, consensusParams);
    }
    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CDiskBlockPos& pos, const Consensus::Params& consensusParams)
{
    block.SetNull();

    // Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("ReadBlockFromDisk: OpenBlockFile failed for %s", pos.ToString());

    // Read block
    try {
        filein >> block;
    } catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s at %s", __func__, e.what(), pos.ToString());
    }

    // Check the header
    if (!CheckPOW(block, consensusParams))
        return error("ReadBlockFromDisk: Errors in block header at %s", pos.ToString());

    return true;
}


bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex, const Consensus::Params& consensusParams)
{
    if (!ReadBlockFromDisk(block, pindex->GetBlockPos(), consensusParams))
        return false;
    if (block.GetHash() != pindex->GetBlockHash())
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*): GetHash() doesn't match index for %s at %s",
            pindex->ToString(), pindex->GetBlockPos().ToString());
    return true;
}

double ConvertBitsToDouble(unsigned int nBits)
{
    int nShift = (nBits >> 24) & 0xff;

    double dDiff = (double)0x0000ffff / (double)(nBits & 0x00ffffff);

    while (nShift < 29) {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29) {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

/*
NOTE:   unlike bitcoin we are using PREVIOUS block height here,
        might be a good idea to change this to use prev bits
        but current height to avoid confusion.
*/
CAmount GetBlockSubsidy(int nPrevBits, int nPrevHeight, const Consensus::Params& consensusParams, bool fSuperblockPartOnly)
{
    double nSubsidy = 100;       // (declaring the reward variable and its original/default amount)
    const short owlings = 21262; // amount of blocks between 2 owlings
    int multiplier;              // integer number of owlings
    int tempHeight;              // number of blocks since last anchor
    if (nPrevHeight < 420) {
        nSubsidy = 4.20;
    } else if ((nPrevHeight > 1000000) && (nPrevHeight < 2000000)) {
        nSubsidy = 80;
    } else if ((nPrevHeight > 2000000) && (nPrevHeight < 3000000)) {
        nSubsidy = 70;
    } else if ((nPrevHeight > 3000000) && (nPrevHeight < 4000000)) {
        nSubsidy = 50;
    } else if ((nPrevHeight > 4000000) && (nPrevHeight < 5000000)) {
        nSubsidy = 40;
    } else if ((nPrevHeight > 5000000) && (nPrevHeight < 6000000)) {
        nSubsidy = 20;
    } else if ((nPrevHeight > 6000000) && (nPrevHeight < 7000000)) {
        nSubsidy = 10;
    } else if ((nPrevHeight > 7000000) && (nPrevHeight < 8000000)) {
        nSubsidy = 9;
    } else if ((nPrevHeight > 8000000) && (nPrevHeight < 9000000)) {
        nSubsidy = 8;
    } else if ((nPrevHeight > 9000000) && (nPrevHeight < 10000000)) {
        nSubsidy = 7;
    } else if ((nPrevHeight > 10000000) && (nPrevHeight < 11000000)) {
        nSubsidy = 6;
    } else if ((nPrevHeight > 11000000) && (nPrevHeight < 12000000)) {
        nSubsidy = 5;
    } else if ((nPrevHeight > 12000000) && (nPrevHeight < 13000000)) {
        nSubsidy = 4.20;
    } else if ((nPrevHeight > 13000000) && (nPrevHeight < 14000000)) {
        nSubsidy = 3;
    } else if ((nPrevHeight > 14000000) && (nPrevHeight < 15000000)) {
        nSubsidy = 2;
    } else if ((nPrevHeight > 15000000) && (nPrevHeight < 16000000)) {
        nSubsidy = 1;
    } else if (nPrevHeight > 16000000) {
        nSubsidy = .420;
    }
    return nSubsidy * COIN;
}

CAmount GetSmartnodePayment(int nHeight, CAmount blockValue, CAmount specialTxFees)
{
    size_t mnCount = 0;
    if (chainActive.Tip() != nullptr) { // fix empty list when -checklevel = 4
        mnCount = deterministicMNManager->GetListForBlock(chainActive[nHeight - 1]).GetAllMNsCount();
    }
    if (mnCount >= 10) {
        int percentage = Params().GetConsensus().nCollaterals.getRewardPercentage(nHeight);
        CAmount specialFeeReward = specialTxFees * Params().GetConsensus().nAssetsRewardShare.smartnode;
        return blockValue * percentage / 100 + specialFeeReward;
    } else {
        return 0;
    }
}

bool IsInitialBlockDownload()
{
    // Once this function has returned false, it must remain false.
    static std::atomic<bool> latchToFalse{false};
    // Optimization: pre-test latch before taking the lock.
    if (latchToFalse.load(std::memory_order_relaxed))
        return false;

    LOCK(cs_main);
    if (latchToFalse.load(std::memory_order_relaxed))
        return false;
    if (fImporting || fReindex)
        return true;
    const CChainParams& chainParams = Params();
    if (chainActive.Tip() == nullptr)
        return true;
    if (chainActive.Tip()->nChainWork < nMinimumChainWork)
        return true;
    if (chainActive.Tip()->GetBlockTime() < (GetTime() - nMaxTipAge))
        return true;
    LogPrintf("Leaving InitialBlockDownload (latching to false)\n");
    latchToFalse.store(true, std::memory_order_relaxed);
    return false;
}

CBlockIndex *pindexBestForkTip = nullptr, *pindexBestForkBase = nullptr;

static void AlertNotify(const std::string& strMessage)
{
    uiInterface.NotifyAlertChanged();
    std::string strCmd = gArgs.GetArg("-alertnotify", "");
    if (strCmd.empty()) return;

    // Alert text should be plain ascii coming from a trusted source, but to
    // be safe we first strip anything not in safeChars, then add single quotes around
    // the whole string before passing it to the shell:
    std::string singleQuote("'");
    std::string safeStatus = SanitizeString(strMessage);
    safeStatus = singleQuote + safeStatus + singleQuote;
    boost::replace_all(strCmd, "%s", safeStatus);

    boost::thread t(runCommand, strCmd); // thread runs free
}

static void CheckForkWarningConditions()
{
    AssertLockHeld(cs_main);
    // Before we get past initial download, we cannot reliably alert about forks
    // (we assume we don't get stuck on a fork before finishing our initial sync)
    if (IsInitialBlockDownload())
        return;

    // If our best fork is no longer within 72 blocks (+/- 3 hours if no one mines it)
    // of our head, drop it
    if (pindexBestForkTip && chainActive.Height() - pindexBestForkTip->nHeight >= 72)
        pindexBestForkTip = nullptr;

    if (pindexBestForkTip || (pindexBestInvalid && pindexBestInvalid->nChainWork > chainActive.Tip()->nChainWork + (GetBlockProof(*chainActive.Tip()) * 6))) {
        if (!GetfLargeWorkForkFound() && pindexBestForkBase) {
            if (pindexBestForkBase->phashBlock) {
                std::string warning = std::string("'Warning: Large-work fork detected, forking after block ") +
                                      pindexBestForkBase->phashBlock->ToString() + std::string("'");
                AlertNotify(warning);
            }
        }
        if (pindexBestForkTip && pindexBestForkBase) {
            if (pindexBestForkBase->phashBlock) {
                LogPrintf("%s: Warning: Large valid fork found\n  forking the chain at height %d (%s)\n  lasting to height %d (%s).\nChain state database corruption likely.\n", __func__,
                    pindexBestForkBase->nHeight, pindexBestForkBase->phashBlock->ToString(),
                    pindexBestForkTip->nHeight, pindexBestForkTip->phashBlock->ToString());
                SetfLargeWorkForkFound(true);
            }
        } else {
            if (pindexBestInvalid->nHeight > chainActive.Height() + 6)
                LogPrintf("%s: Warning: Found invalid chain at least ~6 blocks longer than our best chain.\nChain state database corruption likely.\n", __func__);
            else
                LogPrintf("%s: Warning: Found invalid chain which has higher work (at least ~6 blocks worth of work) than our best chain.\nChain state database corruption likely.\n", __func__);
            SetfLargeWorkInvalidChainFound(true);
        }
    } else {
        SetfLargeWorkForkFound(false);
        SetfLargeWorkInvalidChainFound(false);
    }
}

static void CheckForkWarningConditionsOnNewFork(CBlockIndex* pindexNewForkTip)
{
    AssertLockHeld(cs_main);
    // If we are on a fork that is sufficiently large, set a warning flag
    CBlockIndex* pfork = pindexNewForkTip;
    CBlockIndex* plonger = chainActive.Tip();
    while (pfork && pfork != plonger) {
        while (plonger && plonger->nHeight > pfork->nHeight)
            plonger = plonger->pprev;
        if (pfork == plonger)
            break;
        pfork = pfork->pprev;
    }

    // We define a condition where we should warn the user about as a fork of at least 7 blocks
    // with a tip within 72 blocks (+/- 3 hours if no one mines it) of ours
    // or a chain that is entirely longer than ours and invalid (note that this should be detected by both)
    // We use 7 blocks rather arbitrarily as it represents just under 10% of sustained network
    // hash rate operating on the fork.
    // We define it this way because it allows us to only store the highest fork tip (+ base) which meets
    // the 7-block condition and from this always have the most-likely-to-cause-warning fork
    if (pfork && (!pindexBestForkTip || pindexNewForkTip->nHeight > pindexBestForkTip->nHeight) &&
        pindexNewForkTip->nChainWork - pfork->nChainWork > (GetBlockProof(*pfork) * 7) &&
        chainActive.Height() - pindexNewForkTip->nHeight < 72) {
        pindexBestForkTip = pindexNewForkTip;
        pindexBestForkBase = pfork;
    }

    CheckForkWarningConditions();
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (!pindexBestInvalid || pindexNew->nChainWork > pindexBestInvalid->nChainWork)
        pindexBestInvalid = pindexNew;

    LogPrintf("%s: invalid block=%s  height=%d  log2_work=%.8f  date=%s\n", __func__,
        pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
        log(pindexNew->nChainWork.getdouble()) / log(2.0), DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexNew->GetBlockTime()));
    CBlockIndex* tip = chainActive.Tip();
    assert(tip);
    LogPrintf("%s:  current best=%s  height=%d  log2_work=%.8f  date=%s\n", __func__,
        tip->GetBlockHash().ToString(), chainActive.Height(), log(tip->nChainWork.getdouble()) / log(2.0),
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", tip->GetBlockTime()));
    CheckForkWarningConditions();
}

void static InvalidBlockFound(CBlockIndex* pindex, const CValidationState& state)
{
    if (!state.CorruptionPossible()) {
        pindex->nStatus |= BLOCK_FAILED_VALID;
        g_failed_blocks.insert(pindex);
        setDirtyBlockIndex.insert(pindex);
        setBlockIndexCandidates.erase(pindex);
        InvalidChainFound(pindex);
    }
}

void UpdateCoins(const CTransaction& tx, CCoinsViewCache& inputs, CTxUndo& txundo, int nHeight, uint256 blockHash, CAssetsCache* assetCache, std::pair<std::string, CBlockAssetUndo>* undoAssetData)
{
    // mark inputs spent
    if (!tx.IsCoinBase()) {
        txundo.vprevout.reserve(tx.vin.size());
        for (const CTxIn& txin : tx.vin) {
            txundo.vprevout.emplace_back();
            bool is_spent = inputs.SpendCoin(txin.prevout, &txundo.vprevout.back(), assetCache);
            assert(is_spent);
        }
    }
    // add outputs
    AddCoins(inputs, tx, nHeight, blockHash, false, assetCache, undoAssetData); /** MMM START */ /* Pass assetCache into function */ /** MMM END */
}

void UpdateCoins(const CTransaction& tx, CCoinsViewCache& inputs, CTxUndo& txundo, int nHeight)
{
    UpdateCoins(tx, inputs, txundo, nHeight, uint256());
}

bool CScriptCheck::operator()()
{
    const CScript& scriptSig = ptxTo->vin[nIn].scriptSig;
    PrecomputedTransactionData txdata(*ptxTo);
    return VerifyScript(scriptSig, scriptPubKey, nFlags, CachingTransactionSignatureChecker(ptxTo, nIn, amount, txdata, cacheStore), &error);
}

int GetSpendHeight(const CCoinsViewCache& inputs)
{
    LOCK(cs_main);
    CBlockIndex* pindexPrev = mapBlockIndex.find(inputs.GetBestBlock())->second;
    return pindexPrev->nHeight + 1;
}


static CuckooCache::cache<uint256, SignatureCacheHasher> scriptExecutionCache;
static uint256 scriptExecutionCacheNonce(GetRandHash());

void InitScriptExecutionCache()
{
    // nMaxCacheSize is unsigned. If -maxsigcachesize is set to zero,
    // setup_bytes creates the minimum possible cache (2 elements).
    size_t nMaxCacheSize = std::min(std::max((int64_t)0, gArgs.GetArg("-maxsigcachesize", DEFAULT_MAX_SIG_CACHE_SIZE) / 2), MAX_MAX_SIG_CACHE_SIZE) * ((size_t)1 << 20);
    size_t nElems = scriptExecutionCache.setup_bytes(nMaxCacheSize);
    LogPrintf("Using %zu MiB out of %zu/2 requested for script execution cache, able to store %zu elements\n",
        (nElems * sizeof(uint256)) >> 20, (nMaxCacheSize * 2) >> 20, nElems);
}

/**
 * Check whether all inputs of this transaction are valid (no double spends, scripts & sigs, amounts)
 * This does not modify the UTXO set.
 *
 * If pvChecks is not nullptr, script checks are pushed onto it instead of being performed inline. Any
 * script checks which are not necessary (eg due to script execution cache hits) are, obviously,
 * not pushed onto pvChecks/run.
 *
 * Setting cacheSigStore/cacheFullScriptStore to false will remove elements from the corresponding cache
 * which are matched. This is useful for checking blocks where we will likely never need the cache
 * entry again.
 *
 * Non-static (and re-declared) in src/test/txvalidationcache_tests.cpp
 */
bool CheckInputs(const CTransaction& tx, CValidationState& state, const CCoinsViewCache& inputs, bool fScriptChecks, unsigned int flags, bool cacheSigStore, bool cacheFullScriptStore, PrecomputedTransactionData& txdata, std::vector<CScriptCheck>* pvChecks)
{
    if (!tx.IsCoinBase()) {
        if (pvChecks)
            pvChecks->reserve(tx.vin.size());

        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.

        // Skip script verification when connecting blocks under the
        // assumevalid block. Assuming the assumevalid block is valid this
        // is safe because block merkle hashes are still computed and checked,
        // Of course, if an assumed valid block is invalid due to false scriptSigs
        // this optimization would allow an invalid chain to be accepted.
        if (fScriptChecks) {
            // First check if script executions have been cached with the same
            // flags. Note that this assumes that the inputs provided are
            // correct (ie that the transaction hash which is in tx's prevouts
            // properly commits to the scriptPubKey in the inputs view of that
            // transaction).
            uint256 hashCacheEntry;
            // We only use the first 19 bytes of nonce to avoid a second SHA
            // round - giving us 19 + 32 + 4 = 55 bytes (+ 8 + 1 = 64)
            static_assert(55 - sizeof(flags) - 32 >= 128 / 8, "Want at least 128 bits of nonce for script execution cache");
            CSHA256().Write(scriptExecutionCacheNonce.begin(), 55 - sizeof(flags) - 32).Write(tx.GetHash().begin(), 32).Write((unsigned char*)&flags, sizeof(flags)).Finalize(hashCacheEntry.begin());
            AssertLockHeld(cs_main); // TODO: Remove this requirement by making CuckooCache not require external locks
            if (scriptExecutionCache.contains(hashCacheEntry, !cacheFullScriptStore)) {
                return true;
            }

            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                const COutPoint& prevout = tx.vin[i].prevout;
                const Coin& coin = inputs.AccessCoin(prevout);
                assert(!coin.IsSpent());

                // We very carefully only pass in things to CScriptCheck which
                // are clearly committed to by tx' witness hash. This provides
                // a sanity check that our caching is not introducing consensus
                // failures through additional data in, eg, the coins being
                // spent being checked as a part of CScriptCheck.
                const CScript& scriptPubKey = coin.out.scriptPubKey;
                const CAmount amount = coin.out.nValue;

                // Verify signature
                CScriptCheck check(scriptPubKey, amount, tx, i, flags, cacheSigStore, &txdata);
                if (pvChecks) {
                    pvChecks->push_back(CScriptCheck());
                    check.swap(pvChecks->back());
                } else if (!check()) {
                    if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                        // Check whether the failure was caused by a
                        // non-mandatory script verification check, such as
                        // non-standard DER encodings or non-null dummy
                        // arguments; if so, don't trigger DoS protection to
                        // avoid splitting the network between upgraded and
                        // non-upgraded nodes.
                        CScriptCheck check2(scriptPubKey, amount, tx, i,
                            flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, cacheSigStore, &txdata);
                        if (check2())
                            return state.Invalid(false, REJECT_NONSTANDARD, strprintf("non-mandatory-script-verify-flag (%s)", ScriptErrorString(check.GetScriptError())));
                    }
                    // Failures of other flags indicate a transaction that is
                    // invalid in new blocks, e.g. an invalid P2SH. We DoS ban
                    // such nodes as they are not following the protocol. That
                    // said during an upgrade careful thought should be taken
                    // as to the correct behavior - we may want to continue
                    // peering with non-upgraded nodes even after soft-fork
                    // super-majority signaling has occurred.
                    return state.DoS(100, false, REJECT_INVALID, strprintf("mandatory-script-verify-flag-failed (%s)", ScriptErrorString(check.GetScriptError())));
                }
            }

            if (cacheFullScriptStore && !pvChecks) {
                // We executed all of the provided scripts, and were told to
                // cache the result. Do so now.
                scriptExecutionCache.insert(hashCacheEntry);
            }
        }
    }

    return true;
}

namespace
{

bool UndoWriteToDisk(const CBlockUndo& blockundo, CDiskBlockPos& pos, const uint256& hashBlock, const CMessageHeader::MessageStartChars& messageStart)
{
    // Open history file to append
    CAutoFile fileout(OpenUndoFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s: OpenUndoFile failed", __func__);

    // Write index header
    unsigned int nSize = GetSerializeSize(fileout, blockundo);
    fileout << FLATDATA(messageStart) << nSize;

    // Write undo data
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("%s: ftell failed", __func__);
    pos.nPos = (unsigned int)fileOutPos;
    fileout << blockundo;

    // calculate & write checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << blockundo;
    fileout << hasher.GetHash();

    return true;
}

bool UndoReadFromDisk(CBlockUndo& blockundo, const CDiskBlockPos& pos, const uint256& hashBlock)
{
    // Open history file to read
    CAutoFile filein(OpenUndoFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: OpenUndoFile failed", __func__);

    // Read block
    uint256 hashChecksum;
    CHashVerifier<CAutoFile> verifier(&filein); // We need a CHashVerifier as reserializing may lose data
    try {
        verifier << hashBlock;
        verifier >> blockundo;
        filein >> hashChecksum;
    } catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    // Verify checksum
    if (hashChecksum != verifier.GetHash())
        return error("%s: Checksum mismatch", __func__);

    return true;
}

/** Abort with a message */
bool AbortNode(const std::string& strMessage, const std::string& userMessage = "")
{
    SetMiscWarning(strMessage);
    LogPrintf("*** %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occurred, see debug.log for details") : userMessage,
        "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

bool AbortNode(CValidationState& state, const std::string& strMessage, const std::string& userMessage = "")
{
    AbortNode(strMessage, userMessage);
    return state.Error(strMessage);
}

} // namespace

enum DisconnectResult {
    DISCONNECT_OK,      // All good.
    DISCONNECT_UNCLEAN, // Rolled back, but UTXO set was inconsistent with block.
    DISCONNECT_FAILED   // Something else went wrong.
};

/**
 * Restore the UTXO in a Coin at a given COutPoint
 * @param undo The Coin to be restored.
 * @param view The coins view to which to apply the changes.
 * @param out The out point that corresponds to the tx input.
 * @return A DisconnectResult as an int
 */
int ApplyTxInUndo(Coin&& undo, CCoinsViewCache& view, const COutPoint& out)
{
    bool fClean = true;

    if (view.HaveCoin(out)) fClean = false; // overwriting transaction output

    if (undo.nHeight == 0) {
        // Missing undo metadata (height and coinbase). Older versions included this
        // information only in undo records for the last spend of a transactions'
        // outputs. This implies that it must be present for some other output of the same tx.
        const Coin& alternate = AccessByTxid(view, out.hash);
        if (!alternate.IsSpent()) {
            undo.nHeight = alternate.nHeight;
            undo.fCoinBase = alternate.fCoinBase;
        } else {
            return DISCONNECT_FAILED; // adding output for transaction without known metadata
        }
    }
    // The potential_overwrite parameter to AddCoin is only allowed to be false if we know for
    // sure that the coin did not already exist in the cache. As we have queried for that above
    // using HaveCoin, we don't need to guess. When fClean is false, a coin already existed and
    // it is an overwrite.
    view.AddCoin(out, std::move(undo), !fClean);

    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}

/** Undo the effects of this block (with given index) on the UTXO set represented by coins.
 *  When FAILED is returned, view is left in an indeterminate state. */
static DisconnectResult DisconnectBlock(const CBlock& block, const CBlockIndex* pindex, CCoinsViewCache& view, CAssetsCache* assetsCache = nullptr, bool ignoreAddressIndex = false, bool databaseMessaging = true)
{
    bool fDIP0003Active = Params().GetConsensus().DIP0003Enabled;
    bool isAssetsactive = Params().IsAssetsActive(chainActive.Tip());
    bool fHasBestBlock = evoDb->VerifyBestBlock(pindex->GetBlockHash());

    if (fDIP0003Active && !fHasBestBlock) {
        // Nodes that upgraded after DIP3 activation will have to reindex to ensure evodb consistency
        AbortNode("Found EvoDB inconsistency, you must reindex to continue");
        return DISCONNECT_FAILED;
    }

    bool fClean = true;

    CBlockUndo blockUndo;
    CDiskBlockPos pos = pindex->GetUndoPos();
    if (pos.IsNull()) {
        error("DisconnectBlock(): no undo data available");
        return DISCONNECT_FAILED;
    }
    if (!UndoReadFromDisk(blockUndo, pos, pindex->pprev->GetBlockHash())) {
        error("DisconnectBlock(): failure reading undo data");
        return DISCONNECT_FAILED;
    }

    if (blockUndo.vtxundo.size() + 1 != block.vtx.size()) {
        error("DisconnectBlock(): block and undo data inconsistent");
        return DISCONNECT_FAILED;
    }

    std::vector<std::pair<std::string, CBlockAssetUndo>> vUndoData;
    if (!passetsdb->ReadBlockUndoAssetData(block.GetHash(), vUndoData)) {
        error("DisconnectBlock(): block asset undo data inconsistent");
        return DISCONNECT_FAILED;
    }

    std::vector<std::pair<CAddressIndexKey, CAmount>> addressIndex;
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> addressUnspentIndex;
    std::vector<std::pair<CSpentIndexKey, CSpentIndexValue>> spentIndex;

    if (!UndoSpecialTxsInBlock(block, pindex)) {
        return DISCONNECT_FAILED;
    }

    // undo transactions in reverse order
    CAssetsCache tempCache(*assetsCache);
    for (int i = block.vtx.size() - 1; i >= 0; i--) {
        const CTransaction& tx = *(block.vtx[i]);
        uint256 hash = tx.GetHash();
        bool is_coinbase = tx.IsCoinBase();
        std::vector<int> vAssetTxIndex;
        std::vector<int> vNullAssetTxIndex;

        if (fAddressIndex) {
            for (unsigned int k = tx.vout.size(); k-- > 0;) {
                const CTxOut& out = tx.vout[k];

                if (out.scriptPubKey.IsPayToScriptHash()) {
                    std::vector<unsigned char> hashBytes(out.scriptPubKey.begin() + 2, out.scriptPubKey.begin() + 22);

                    // undo receiving activity
                    addressIndex.push_back(std::make_pair(CAddressIndexKey(2, uint160(hashBytes), pindex->nHeight, i, hash, k, false), out.nValue));

                    // undo unspent index
                    addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(2, uint160(hashBytes), hash, k), CAddressUnspentValue()));

                } else if (out.scriptPubKey.IsPayToPublicKeyHash()) {
                    std::vector<unsigned char> hashBytes(out.scriptPubKey.begin() + 3, out.scriptPubKey.begin() + 23);

                    // undo receiving activity
                    addressIndex.push_back(std::make_pair(CAddressIndexKey(1, uint160(hashBytes), pindex->nHeight, i, hash, k, false), out.nValue));

                    // undo unspent index
                    addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(1, uint160(hashBytes), hash, k), CAddressUnspentValue()));

                } else if (out.scriptPubKey.IsPayToPublicKey()) {
                    uint160 hashBytes(Hash160(out.scriptPubKey.begin() + 1, out.scriptPubKey.end() - 1));
                    addressIndex.push_back(std::make_pair(CAddressIndexKey(1, hashBytes, pindex->nHeight, i, hash, k, false), out.nValue));
                    addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(1, hashBytes, hash, k), CAddressUnspentValue()));
                } else {
                    /** MMM ASSETS START */
                    if (isAssetsactive) {
                        std::string assetName;
                        CAmount assetAmount;
                        uint160 hashBytes;

                        if (ParseAssetScript(out.scriptPubKey, hashBytes, assetName, assetAmount)) {
                            //                            std::cout << "ConnectBlock(): pushing assets onto addressIndex: " << "1" << ", " << hashBytes.GetHex() << ", " << assetName << ", " << pindex->nHeight
                            //                                      << ", " << i << ", " << hash.GetHex() << ", " << k << ", " << "true" << ", " << assetAmount << std::endl;

                            // undo receiving activity
                            addressIndex.push_back(std::make_pair(
                                CAddressIndexKey(1, uint160(hashBytes), assetName, pindex->nHeight, i, hash, k,
                                    false),
                                assetAmount));

                            // undo unspent index
                            addressUnspentIndex.push_back(
                                std::make_pair(CAddressUnspentKey(1, uint160(hashBytes), assetName, hash, k),
                                    CAddressUnspentValue()));
                        } else {
                            continue;
                        }
                    }
                    /** MMM ASSETS END */
                }
            }
        }

        // Check that all outputs are available and match the outputs in the block itself
        // exactly.
        int indexOfRestrictedAssetVerifierString = -1;
        for (size_t o = 0; o < tx.vout.size(); o++) {
            if (!tx.vout[o].scriptPubKey.IsUnspendable()) {
                COutPoint out(hash, o);
                Coin coin;
                bool is_spent = view.SpendCoin(out, &coin, &tempCache);
                if (!is_spent || tx.vout[o] != coin.out || pindex->nHeight != coin.nHeight || is_coinbase != coin.fCoinBase) {
                    fClean = false; // transaction output mismatch
                }
                /** MMM ASSETS START */
                if (isAssetsactive) {
                    if (assetsCache) {
                        if (IsScriptTransferAsset(tx.vout[o].scriptPubKey))
                            vAssetTxIndex.emplace_back(o);
                    }
                }
            } else {
                if (isAssetsactive) {
                    if (assetsCache) {
                        if (tx.vout[o].scriptPubKey.IsNullAsset()) {
                            if (tx.vout[o].scriptPubKey.IsNullAssetVerifierTxDataScript()) {
                                indexOfRestrictedAssetVerifierString = o;
                            } else {
                                vNullAssetTxIndex.emplace_back(o);
                            }
                        }
                    }
                }
            }
        }
        /** MMM START */
        if (isAssetsactive) {
            if (assetsCache) {
                if (tx.IsNewAsset()) {
                    // Remove the newly created asset
                    CNewAsset asset;
                    std::string strAddress;
                    if (!AssetFromTransaction(tx, asset, strAddress)) {
                        error("%s : Failed to get asset from transaction. TXID : %s", __func__, tx.GetHash().GetHex());
                        return DISCONNECT_FAILED;
                    }
                    if (assetsCache->ContainsAsset(asset)) {
                        if (!assetsCache->RemoveNewAsset(asset, strAddress)) {
                            error("%s : Failed to Remove Asset. Asset Name : %s", __func__, asset.strName);
                            return DISCONNECT_FAILED;
                        }
                    }

                    // Get the owner from the transaction and remove it
                    std::string ownerName;
                    std::string ownerAddress;
                    if (!OwnerFromTransaction(tx, ownerName, ownerAddress)) {
                        error("%s : Failed to get owner from transaction. TXID : %s", __func__, tx.GetHash().GetHex());
                        return DISCONNECT_FAILED;
                    }

                    if (!assetsCache->RemoveOwnerAsset(ownerName, ownerAddress)) {
                        error("%s : Failed to Remove Owner from transaction. TXID : %s", __func__, tx.GetHash().GetHex());
                        return DISCONNECT_FAILED;
                    }
                } else if (tx.IsReissueAsset()) {
                    CReissueAsset reissue;
                    std::string strAddress;

                    if (!ReissueAssetFromTransaction(tx, reissue, strAddress)) {
                        error("%s : Failed to get reissue asset from transaction. TXID : %s", __func__,
                            tx.GetHash().GetHex());
                        return DISCONNECT_FAILED;
                    }

                    if (assetsCache->ContainsAsset(reissue.strName)) {
                        if (!assetsCache->RemoveReissueAsset(reissue, strAddress,
                                COutPoint(tx.GetHash(), tx.vout.size() - 1),
                                vUndoData)) {
                            error("%s : Failed to Undo Reissue Asset. Asset Name : %s", __func__, reissue.strName);
                            return DISCONNECT_FAILED;
                        }
                    }
                } else if (tx.IsNewUniqueAsset()) {
                    for (int n = 0; n < (int)tx.vout.size(); n++) {
                        auto out = tx.vout[n];
                        CNewAsset asset;
                        std::string strAddress;

                        if (IsScriptNewUniqueAsset(out.scriptPubKey)) {
                            if (!AssetFromScript(out.scriptPubKey, asset, strAddress)) {
                                error("%s : Failed to get unique asset from transaction. TXID : %s, vout: %s", __func__,
                                    tx.GetHash().GetHex(), n);
                                return DISCONNECT_FAILED;
                            }

                            if (assetsCache->ContainsAsset(asset.strName)) {
                                if (!assetsCache->RemoveNewAsset(asset, strAddress)) {
                                    error("%s : Failed to Undo Unique Asset. Asset Name : %s", __func__, asset.strName);
                                    return DISCONNECT_FAILED;
                                }
                            }
                        }
                    }
                } else if (tx.IsNewMsgChannelAsset()) {
                    CNewAsset asset;
                    std::string strAddress;

                    if (!MsgChannelAssetFromTransaction(tx, asset, strAddress)) {
                        error("%s : Failed to get msgchannel asset from transaction. TXID : %s", __func__,
                            tx.GetHash().GetHex());
                        return DISCONNECT_FAILED;
                    }

                    if (assetsCache->ContainsAsset(asset.strName)) {
                        if (!assetsCache->RemoveNewAsset(asset, strAddress)) {
                            error("%s : Failed to Undo Msg Channel Asset. Asset Name : %s", __func__, asset.strName);
                            return DISCONNECT_FAILED;
                        }
                    }
                } else if (tx.IsNewQualifierAsset()) {
                    CNewAsset asset;
                    std::string strAddress;

                    if (!QualifierAssetFromTransaction(tx, asset, strAddress)) {
                        error("%s : Failed to get qualifier asset from transaction. TXID : %s", __func__,
                            tx.GetHash().GetHex());
                        return DISCONNECT_FAILED;
                    }

                    if (assetsCache->ContainsAsset(asset.strName)) {
                        if (!assetsCache->RemoveNewAsset(asset, strAddress)) {
                            error("%s : Failed to Undo Qualifier Asset. Asset Name : %s", __func__, asset.strName);
                            return DISCONNECT_FAILED;
                        }
                    }
                } else if (tx.IsNewRestrictedAsset()) {
                    CNewAsset asset;
                    std::string strAddress;

                    if (!RestrictedAssetFromTransaction(tx, asset, strAddress)) {
                        error("%s : Failed to get restricted asset from transaction. TXID : %s", __func__,
                            tx.GetHash().GetHex());
                        return DISCONNECT_FAILED;
                    }

                    if (assetsCache->ContainsAsset(asset.strName)) {
                        if (!assetsCache->RemoveNewAsset(asset, strAddress)) {
                            error("%s : Failed to Undo Restricted Asset. Asset Name : %s", __func__, asset.strName);
                            return DISCONNECT_FAILED;
                        }
                    }

                    if (indexOfRestrictedAssetVerifierString < 0) {
                        error("%s : Failed to find the restricted asset verifier string index from trasaction. TxID : %s", __func__, tx.GetHash().GetHex());
                        return DISCONNECT_FAILED;
                    }

                    CNullAssetTxVerifierString verifier;
                    if (!AssetNullVerifierDataFromScript(tx.vout[indexOfRestrictedAssetVerifierString].scriptPubKey, verifier)) {
                        error("%s : Failed to get the restricted asset verifier string from trasaction. TxID : %s", __func__, tx.GetHash().GetHex());
                        return DISCONNECT_FAILED;
                    }

                    if (!assetsCache->RemoveRestrictedVerifier(asset.strName, verifier.verifier_string)) {
                        error("%s : Failed to Remove Restricted Verifier from transaction. TXID : %s", __func__, tx.GetHash().GetHex());
                        return DISCONNECT_FAILED;
                    }
                }

                for (auto index : vAssetTxIndex) {
                    CAssetTransfer transfer;
                    std::string strAddress;
                    if (!TransferAssetFromScript(tx.vout[index].scriptPubKey, transfer, strAddress)) {
                        error("%s : Failed to get transfer asset from transaction. CTxOut : %s", __func__,
                            tx.vout[index].ToString());
                        return DISCONNECT_FAILED;
                    }

                    COutPoint out(hash, index);
                    if (!assetsCache->RemoveTransfer(transfer, strAddress, out)) {
                        error("%s : Failed to Remove the transfer of an asset. Asset Name : %s, COutPoint : %s",
                            __func__,
                            transfer.strName, out.ToString());
                        return DISCONNECT_FAILED;
                    }

                    // Undo messages
                    if (fMessaging && databaseMessaging && !transfer.message.empty() &&
                        (IsAssetNameAnOwner(transfer.strName) || IsAssetNameAnMsgChannel(transfer.strName))) {
                        LOCK(cs_messaging);
                        if (IsChannelSubscribed(transfer.strName)) {
                            OrphanMessage(COutPoint(hash, index));
                        }
                    }
                }

                // Because of the strict rules for allowing the null asset tx types into a transaction.
                // We know that if these are in a transaction, that they are valid null asset tx, and can be reversed
                for (auto index : vNullAssetTxIndex) {
                    CScript script = tx.vout[index].scriptPubKey;

                    if (script.IsNullAssetTxDataScript()) {
                        CNullAssetTxData data;
                        std::string address;
                        if (!AssetNullDataFromScript(script, data, address)) {
                            error("%s : Failed to get null asset data from transaction. CTxOut : %s", __func__,
                                tx.vout[index].ToString());
                            return DISCONNECT_FAILED;
                        }

                        AssetType type;
                        IsAssetNameValid(data.asset_name, type);

                        // Handle adding qualifiers to addresses
                        if (type == AssetType::QUALIFIER || type == AssetType::SUB_QUALIFIER) {
                            if (!assetsCache->RemoveQualifierAddress(data.asset_name, address, data.flag ? QualifierType::ADD_QUALIFIER : QualifierType::REMOVE_QUALIFIER)) {
                                error("%s : Failed to remove qualifier from address, Qualifier : %s, Flag Removing : %d, Address : %s",
                                    __func__, data.asset_name, data.flag, address);
                                return DISCONNECT_FAILED;
                            }
                            // Handle adding restrictions to addresses
                        } else if (type == AssetType::RESTRICTED) {
                            if (!assetsCache->RemoveRestrictedAddress(data.asset_name, address, data.flag ? RestrictedType::FREEZE_ADDRESS : RestrictedType::UNFREEZE_ADDRESS)) {
                                error("%s : Failed to remove restriction from address, Restriction : %s, Flag Removing : %d, Address : %s",
                                    __func__, data.asset_name, data.flag, address);
                                return DISCONNECT_FAILED;
                            }
                        }
                    } else if (script.IsNullGlobalRestrictionAssetTxDataScript()) {
                        CNullAssetTxData data;
                        std::string address;
                        if (!GlobalAssetNullDataFromScript(script, data)) {
                            error("%s : Failed to get global null asset data from transaction. CTxOut : %s", __func__,
                                tx.vout[index].ToString());
                            return DISCONNECT_FAILED;
                        }

                        if (!assetsCache->RemoveGlobalRestricted(data.asset_name, data.flag ? RestrictedType::GLOBAL_FREEZE : RestrictedType::GLOBAL_UNFREEZE)) {
                            error("%s : Failed to remove global restriction from cache. Asset Name: %s, Flag Removing %d", __func__, data.asset_name, data.flag);
                            return DISCONNECT_FAILED;
                        }
                    } else if (script.IsNullAssetVerifierTxDataScript()) {
                        // These are handled in the undo restricted asset issuance, and restricted asset reissuance
                        continue;
                    }
                }
            }
        }
        /** MMM END */

        // restore inputs
        if (i > 0) { // not coinbases
            CTxUndo& txundo = blockUndo.vtxundo[i - 1];
            if (txundo.vprevout.size() != tx.vin.size()) {
                error("DisconnectBlock(): transaction and undo data inconsistent");
                return DISCONNECT_FAILED;
            }
            for (unsigned int j = tx.vin.size(); j-- > 0;) {
                const COutPoint& out = tx.vin[j].prevout;
                int undoHeight = txundo.vprevout[j].nHeight;
                int res = ApplyTxInUndo(std::move(txundo.vprevout[j]), view, out);
                if (res == DISCONNECT_FAILED) return DISCONNECT_FAILED;
                fClean = fClean && res != DISCONNECT_UNCLEAN;

                const CTxIn input = tx.vin[j];

                if (fSpentIndex) {
                    // undo and delete the spent index
                    spentIndex.push_back(std::make_pair(CSpentIndexKey(input.prevout.hash, input.prevout.n), CSpentIndexValue()));
                }

                if (fAddressIndex) {
                    const Coin& coin = view.AccessCoin(tx.vin[j].prevout);
                    const CTxOut& prevout = coin.out;
                    if (prevout.scriptPubKey.IsPayToScriptHash()) {
                        std::vector<unsigned char> hashBytes(prevout.scriptPubKey.begin() + 2, prevout.scriptPubKey.begin() + 22);

                        // undo spending activity
                        addressIndex.push_back(std::make_pair(CAddressIndexKey(2, uint160(hashBytes), pindex->nHeight, i, hash, j, true), prevout.nValue * -1));

                        // restore unspent index
                        addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(2, uint160(hashBytes), input.prevout.hash, input.prevout.n), CAddressUnspentValue(prevout.nValue, prevout.scriptPubKey, undoHeight)));


                    } else if (prevout.scriptPubKey.IsPayToPublicKeyHash()) {
                        std::vector<unsigned char> hashBytes(prevout.scriptPubKey.begin() + 3, prevout.scriptPubKey.begin() + 23);

                        // undo spending activity
                        addressIndex.push_back(std::make_pair(CAddressIndexKey(1, uint160(hashBytes), pindex->nHeight, i, hash, j, true), prevout.nValue * -1));

                        // restore unspent index
                        addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(1, uint160(hashBytes), input.prevout.hash, input.prevout.n), CAddressUnspentValue(prevout.nValue, prevout.scriptPubKey, undoHeight)));

                    } else if (prevout.scriptPubKey.IsPayToPublicKey()) {
                        uint160 hashBytes(Hash160(prevout.scriptPubKey.begin() + 1, prevout.scriptPubKey.end() - 1));
                        addressIndex.push_back(std::make_pair(CAddressIndexKey(1, hashBytes, pindex->nHeight, i, hash, j, false), prevout.nValue));
                        addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(1, hashBytes, hash, j), CAddressUnspentValue()));
                    } else {
                        /** MMM ASSETS START */
                        if (isAssetsactive) {
                            std::string assetName;
                            CAmount assetAmount;
                            uint160 hashBytes;

                            if (ParseAssetScript(prevout.scriptPubKey, hashBytes, assetName, assetAmount)) {
                                //                                std::cout << "ConnectBlock(): pushing assets onto addressIndex: " << "1" << ", " << hashBytes.GetHex() << ", " << assetName << ", " << pindex->nHeight
                                //                                          << ", " << i << ", " << hash.GetHex() << ", " << j << ", " << "true" << ", " << assetAmount * -1 << std::endl;

                                // undo spending activity
                                addressIndex.push_back(std::make_pair(
                                    CAddressIndexKey(1, uint160(hashBytes), assetName, pindex->nHeight, i, hash, j,
                                        true),
                                    assetAmount * -1));

                                // restore unspent index
                                addressUnspentIndex.push_back(std::make_pair(
                                    CAddressUnspentKey(1, uint160(hashBytes), assetName, input.prevout.hash,
                                        input.prevout.n),
                                    CAddressUnspentValue(assetAmount, prevout.scriptPubKey, undoHeight)));
                            } else {
                                continue;
                            }
                        }
                        /** MMM ASSETS END */
                    }
                }
            }
            // At this point, all of txundo.vprevout should have been moved out.
        }
    }

    // move best block pointer to prevout block
    view.SetBestBlock(pindex->pprev->GetBlockHash());

    if (fSpentIndex) {
        if (!pblocktree->UpdateSpentIndex(spentIndex)) {
            AbortNode("Failed to delete spent index");
            return DISCONNECT_FAILED;
        }
    }

    if (fAddressIndex) {
        if (!pblocktree->EraseAddressIndex(addressIndex)) {
            AbortNode("Failed to delete address index");
            return DISCONNECT_FAILED;
        }
        if (!pblocktree->UpdateAddressUnspentIndex(addressUnspentIndex)) {
            AbortNode("Failed to write address unspent index");
            return DISCONNECT_FAILED;
        }
    }

    evoDb->WriteBestBlock(pindex->pprev->GetBlockHash());

    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}

void static FlushBlockFile(bool fFinalize = false)
{
    LOCK(cs_LastBlockFile);

    CDiskBlockPos posOld(nLastBlockFile, 0);

    FILE* fileOld = OpenBlockFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }

    fileOld = OpenUndoFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nUndoSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }
}

static bool FindUndoPos(CValidationState& state, int nFile, CDiskBlockPos& pos, unsigned int nAddSize);

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);

void ThreadScriptCheck()
{
    RenameThread("memeium-scriptch");
    scriptcheckqueue.Thread();
}

// Protected by cs_main
VersionBitsCache versionbitscache;

int32_t ComputeBlockVersion(const CBlockIndex* pindexPrev, const Consensus::Params& params, bool fCheckSmartnodesUpgraded)
{
    LOCK(cs_main);
    int32_t nVersion = VERSIONBITS_TOP_BITS;

    for (int i = 0; i < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; i++) {
        Consensus::DeploymentPos pos = Consensus::DeploymentPos(i);
        ThresholdState state = VersionBitsState(pindexPrev, params, pos, versionbitscache);
        const struct VBDeploymentInfo& vbinfo = VersionBitsDeploymentInfo[pos];
        if (vbinfo.check_mn_protocol && state == THRESHOLD_STARTED && fCheckSmartnodesUpgraded) {
            // TODO implement new logic for MN upgrade checks (e.g. with LLMQ based feature/version voting)
        }
        if (state == THRESHOLD_LOCKED_IN || state == THRESHOLD_STARTED) {
            nVersion |= VersionBitsMask(params, (Consensus::DeploymentPos)i);
        }
    }

    return nVersion;
}

bool GetBlockHash(uint256& hashRet, int nBlockHeight)
{
    LOCK(cs_main);
    if (chainActive.Tip() == nullptr) return false;
    if (nBlockHeight < -1 || nBlockHeight > chainActive.Height()) return false;
    if (nBlockHeight == -1) nBlockHeight = chainActive.Height();
    hashRet = chainActive[nBlockHeight]->GetBlockHash();
    return true;
}

/**
 * Threshold condition checker that triggers when unknown versionbits are seen on the network.
 */
class WarningBitsConditionChecker : public AbstractThresholdConditionChecker
{
private:
    int bit;

public:
    WarningBitsConditionChecker(int bitIn) :
        bit(bitIn) {}

    int64_t BeginTime(const Consensus::Params& params) const override { return 0; }
    int64_t EndTime(const Consensus::Params& params) const override { return std::numeric_limits<int64_t>::max(); }
    int Period(const Consensus::Params& params) const override { return params.nMinerConfirmationWindow; }
    int Threshold(const Consensus::Params& params) const override { return params.nRuleChangeActivationThreshold; }

    bool Condition(const CBlockIndex* pindex, const Consensus::Params& params) const override
    {
        return ((pindex->nVersion & VERSIONBITS_TOP_MASK) == VERSIONBITS_TOP_BITS) &&
               ((pindex->nVersion >> bit) & 1) != 0 &&
               ((ComputeBlockVersion(pindex->pprev, params) >> bit) & 1) == 0;
    }
};

// Protected by cs_main
static ThresholdConditionCache warningcache[VERSIONBITS_NUM_BITS];

static unsigned int GetBlockScriptFlags(const CBlockIndex* pindex, const Consensus::Params& consensusparams)
{
    AssertLockHeld(cs_main);

    // BIP16 didn't become active until Apr 1 2012
    int64_t nBIP16SwitchTime = 1333238400;
    bool fStrictPayToScriptHash = (pindex->GetBlockTime() >= nBIP16SwitchTime);

    unsigned int flags = fStrictPayToScriptHash ? SCRIPT_VERIFY_P2SH : SCRIPT_VERIFY_NONE;

    // Start enforcing the DERSIG (BIP66) rule
    if (consensusparams.BIP66Enabled) {
        flags |= SCRIPT_VERIFY_DERSIG;
    }

    // Start enforcing CHECKLOCKTIMEVERIFY (BIP65) rule
    if (consensusparams.BIP65Enabled) {
        flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    }

    // Start enforcing BIP68 (sequence locks) and BIP112 (CHECKSEQUENCEVERIFY) using versionbits logic.
    if (consensusparams.BIPCSVEnabled) {
        flags |= SCRIPT_VERIFY_CHECKSEQUENCEVERIFY;
    }

    // Start enforcing BIP147 (NULLDUMMY) rule using versionbits logic.
    if (consensusparams.BIP147Enabled) {
        flags |= SCRIPT_VERIFY_NULLDUMMY;
    }

    return flags;
}


static int64_t nTimeCheck = 0;
static int64_t nTimeForks = 0;
static int64_t nTimeVerify = 0;
static int64_t nTimeISFilter = 0;
static int64_t nTimeSubsidy = 0;
static int64_t nTimeValueValid = 0;
static int64_t nTimePayeeValid = 0;
static int64_t nTimeProcessSpecial = 0;
static int64_t nTimeMemeiumSpecific = 0;
static int64_t nTimeConnect = 0;
static int64_t nTimeIndex = 0;
static int64_t nTimeCallbacks = 0;
static int64_t nTimeTotal = 0;

/** Apply the effects of this block (with given index) on the UTXO set represented by coins.
 *  Validity checks that depend on the UTXO set are also done; ConnectBlock()
 *  can fail if those validity checks fail (among other reasons). */
static bool ConnectBlock(const CBlock& block, CValidationState& state, CBlockIndex* pindex, CCoinsViewCache& view, const CChainParams& chainparams, CAssetsCache* assetsCache = nullptr, bool fJustCheck = false)
{
    AssertLockHeld(cs_main);
    assert(pindex);
    // pindex->phashBlock can be null if called by CreateNewBlock/TestBlockValidity
    assert((pindex->phashBlock == nullptr) ||
           (*pindex->phashBlock == block.GetHash()));
    int64_t nTimeStart = GetTimeMicros();

    // Check it again in case a previous version let a bad block in
    if (!CheckBlock(block, state, chainparams.GetConsensus(), pindex->nHeight, !fJustCheck, !fJustCheck))
        return error("%s: Consensus::CheckBlock: %s", __func__, FormatStateMessage(state));

    if (pindex->pprev && pindex->phashBlock && llmq::chainLocksHandler->HasConflictingChainLock(pindex->nHeight, pindex->GetBlockHash())) {
        return state.DoS(10, error("%s: conflicting with chainlock", __func__), REJECT_INVALID, "bad-chainlock");
    }

    // verify that the view's current state corresponds to the previous block
    uint256 hashPrevBlock = pindex->pprev == nullptr ? uint256() : pindex->pprev->GetBlockHash();
    assert(hashPrevBlock == view.GetBestBlock());

    if (pindex->pprev) {
        bool fDIP0003Active = chainparams.GetConsensus().DIP0003Enabled;
        bool fHasBestBlock = evoDb->VerifyBestBlock(pindex->pprev->GetBlockHash());

        if (fDIP0003Active && !fHasBestBlock) {
            // Nodes that upgraded after DIP3 activation will have to reindex to ensure evodb consistency
            return AbortNode(state, "Found EvoDB inconsistency, you must reindex to continue");
        }
    }

    // Special case for the genesis block, skipping connection of its transactions
    // (its coinbase is unspendable)
    if (block.GetHash() == chainparams.GetConsensus().hashGenesisBlock) {
        if (!fJustCheck)
            view.SetBestBlock(pindex->GetBlockHash());
        return true;
    }

    bool fScriptChecks = true;
    if (!hashAssumeValid.IsNull()) {
        // We've been configured with the hash of a block which has been externally verified to have a valid history.
        // A suitable default value is included with the software and updated from time to time.  Because validity
        //  relative to a piece of software is an objective fact these defaults can be easily reviewed.
        // This setting doesn't force the selection of any particular chain but makes validating some faster by
        //  effectively caching the result of part of the verification.
        BlockMap::const_iterator it = mapBlockIndex.find(hashAssumeValid);
        if (it != mapBlockIndex.end()) {
            if (it->second->GetAncestor(pindex->nHeight) == pindex &&
                pindexBestHeader->GetAncestor(pindex->nHeight) == pindex &&
                pindexBestHeader->nChainWork >= nMinimumChainWork) {
                // This block is a member of the assumed verified chain and an ancestor of the best header.
                // The equivalent time check discourages hash power from extorting the network via DOS attack
                //  into accepting an invalid block through telling users they must manually set assumevalid.
                //  Requiring a software change or burying the invalid block, regardless of the setting, makes
                //  it hard to hide the implication of the demand.  This also avoids having release candidates
                //  that are hardly doing any signature verification at all in testing without having to
                //  artificially set the default assumed verified block further back.
                // The test against nMinimumChainWork prevents the skipping when denied access to any chain at
                //  least as good as the expected chain.
                fScriptChecks = (GetBlockProofEquivalentTime(*pindexBestHeader, *pindex, *pindexBestHeader, chainparams.GetConsensus()) <= 60 * 60 * 24 * 7 * 2);
            }
        }
    }

    int64_t nTime1 = GetTimeMicros();
    nTimeCheck += nTime1 - nTimeStart;
    LogPrint(BCLog::BENCHMARK, "    - Sanity checks: %.2fms [%.2fs]\n", 0.001 * (nTime1 - nTimeStart), nTimeCheck * 0.000001);

    /// MEMEIUM: Check superblock start

    // make sure old budget is the real one
    if (pindex->nHeight == chainparams.GetConsensus().nSuperblockStartBlock &&
        chainparams.GetConsensus().nSuperblockStartHash != uint256() &&
        block.GetHash() != chainparams.GetConsensus().nSuperblockStartHash)
        return state.DoS(100, error("ConnectBlock(): invalid superblock start"),
            REJECT_INVALID, "bad-sb-start");

    /// END MEMEIUM

    // Start enforcing BIP68 (sequence locks) and BIP112 (CHECKSEQUENCEVERIFY) using versionbits logic.
    int nLockTimeFlags = 0;
    if (chainparams.GetConsensus().BIPCSVEnabled) {
        nLockTimeFlags |= LOCKTIME_VERIFY_SEQUENCE;
    }

    // Get the script flags for this block
    unsigned int flags = GetBlockScriptFlags(pindex, chainparams.GetConsensus());

    int64_t nTime2 = GetTimeMicros();
    nTimeForks += nTime2 - nTime1;
    LogPrint(BCLog::BENCHMARK, "    - Fork checks: %.2fms [%.2fs]\n", 0.001 * (nTime2 - nTime1), nTimeForks * 0.000001);

    CBlockUndo blockundo;

    CCheckQueueControl<CScriptCheck> control(fScriptChecks && nScriptCheckThreads ? &scriptcheckqueue : nullptr);

    std::vector<int> prevheights;
    CAmount nFees = 0;
    CAmount specialTxFees = 0;
    int nInputs = 0;
    unsigned int nSigOps = 0;
    CDiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(block.vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos>> vPos;
    vPos.reserve(block.vtx.size());
    blockundo.vtxundo.reserve(block.vtx.size() - 1);
    std::vector<std::pair<CAddressIndexKey, CAmount>> addressIndex;
    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue>> addressUnspentIndex;
    std::vector<std::pair<CSpentIndexKey, CSpentIndexValue>> spentIndex;

    std::vector<PrecomputedTransactionData> txdata;
    std::set<CMessage> setMessages;
    std::vector<std::pair<std::string, CNullAssetTxData>> myNullAssetData;
    txdata.reserve(block.vtx.size()); // Required so that pointers to individual PrecomputedTransactionData don't get invalidated

    bool fDIP0001Active_context = Params().GetConsensus().DIP0001Enabled;
    bool isAssetsactive = Params().IsAssetsActive(chainActive.Tip());
    bool isSyncing = IsInitialBlockDownload();

    for (unsigned int i = 0; i < block.vtx.size(); i++) {
        const CTransaction& tx = *(block.vtx[i]);
        const uint256 txhash = tx.GetHash();

        nInputs += tx.vin.size();

        if (!tx.IsCoinBase()) {
            CAmount txfee = 0;
            CAmount specialTxFee = 0;
            if (!Consensus::CheckTxInputs(tx, state, view, pindex->nHeight, txfee, specialTxFee, !isSyncing)) {
                return error("%s: Consensus::CheckTxInputs: %s, %s", __func__, tx.GetHash().ToString(), FormatStateMessage(state));
            }
            nFees += txfee;
            specialTxFees += specialTxFee;
            if (!MoneyRange(nFees)) {
                return state.DoS(100, error("%s: accumulated fee in the block out of range.", __func__),
                    REJECT_INVALID, "bad-txns-accumulated-fee-outofrange");
            }
            if (!MoneyRange(specialTxFees)) {
                return state.DoS(100, error("%s: accumulated specialTxFees in the block out of range.", __func__),
                    REJECT_INVALID, "bad-txns-accumulated-specialTxFees-outofrange");
            }
            /** MMM START START */
            if (!isAssetsactive) {
                for (auto out : tx.vout)
                    if (out.scriptPubKey.IsAssetScript())
                        return state.DoS(100, error("%s : Received Block with tx that contained an asset when assets wasn't active", __func__), REJECT_INVALID, "bad-txns-assets-not-active");
            } else {
                std::vector<std::pair<std::string, uint256>> vReissueAssets;
                if (!Consensus::CheckTxAssets(tx, state, view, assetsCache, false, vReissueAssets, isAssetsactive, false, &setMessages, block.nTime, &myNullAssetData)) {
                    return error("%s: Consensus::CheckTxAssets: %s, %s", __func__, tx.GetHash().ToString(),
                        FormatStateMessage(state));
                }
            }
            /** MMM ASSETS END */

            // Check that transaction is BIP68 final
            // BIP68 lock checks (as opposed to nLockTime checks) must
            // be in ConnectBlock because they require the UTXO set
            prevheights.resize(tx.vin.size());
            for (size_t j = 0; j < tx.vin.size(); j++) {
                prevheights[j] = view.AccessCoin(tx.vin[j].prevout).nHeight;
            }

            if (!SequenceLocks(tx, nLockTimeFlags, &prevheights, *pindex)) {
                return state.DoS(100, error("%s: contains a non-BIP68-final transaction", __func__),
                    REJECT_INVALID, "bad-txns-nonfinal");
            }

            if (fAddressIndex || fSpentIndex) {
                for (size_t j = 0; j < tx.vin.size(); j++) {
                    const CTxIn input = tx.vin[j];
                    const Coin& coin = view.AccessCoin(tx.vin[j].prevout);
                    const CTxOut& prevout = coin.out;
                    uint160 hashBytes;
                    int addressType;
                    bool isAsset = false;
                    std::string assetName;
                    CAmount assetAmount;

                    if (prevout.scriptPubKey.IsPayToScriptHash()) {
                        hashBytes = uint160(std::vector<unsigned char>(prevout.scriptPubKey.begin() + 2, prevout.scriptPubKey.begin() + 22));
                        addressType = 2;
                    } else if (prevout.scriptPubKey.IsPayToPublicKeyHash()) {
                        hashBytes = uint160(std::vector<unsigned char>(prevout.scriptPubKey.begin() + 3, prevout.scriptPubKey.begin() + 23));
                        addressType = 1;
                    } else if (prevout.scriptPubKey.IsPayToPublicKey()) {
                        hashBytes = Hash160(prevout.scriptPubKey.begin() + 1, prevout.scriptPubKey.end() - 1);
                        addressType = 1;
                    } else {
                        /** MMM START */
                        if (AreAssetsDeployed()) {
                            hashBytes.SetNull();
                            addressType = 0;
                            if (ParseAssetScript(prevout.scriptPubKey, hashBytes, assetName, assetAmount)) {
                                addressType = 1;
                                isAsset = true;
                            }
                        } else {
                            /** MMM END */
                            hashBytes.SetNull();
                            addressType = 0;
                        }
                    }

                    if (fAddressIndex && addressType > 0) {
                        /** MMM START */
                        if (isAsset) {
                            // record spending activity
                            addressIndex.push_back(std::make_pair(CAddressIndexKey(addressType, hashBytes, assetName, pindex->nHeight, i, txhash, j, true), assetAmount * -1));

                            // remove address from unspent index
                            addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(addressType, hashBytes, assetName, input.prevout.hash, input.prevout.n), CAddressUnspentValue()));
                            /** MMM END */
                        } else {
                            // record spending activity
                            addressIndex.push_back(std::make_pair(CAddressIndexKey(addressType, hashBytes, pindex->nHeight, i, txhash, j, true), prevout.nValue * -1));

                            // remove address from unspent index
                            addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(addressType, hashBytes, input.prevout.hash, input.prevout.n), CAddressUnspentValue()));
                        }
                    }

                    if (fSpentIndex) {
                        // add the spent index to determine the txid and input that spent an output
                        // and to find the amount and address from an input
                        spentIndex.push_back(std::make_pair(CSpentIndexKey(input.prevout.hash, input.prevout.n), CSpentIndexValue(txhash, j, pindex->nHeight, prevout.nValue, addressType, hashBytes)));
                    }
                }
            }
        }

        // GetTransactionSigOpCount counts 2 types of sigops:
        // * legacy (always)
        // * p2sh (when P2SH enabled in flags and excludes coinbase)
        nSigOps += GetTransactionSigOpCount(tx, view, flags);
        if (nSigOps > MaxBlockSigOps(fDIP0001Active_context))
            return state.DoS(100, error("ConnectBlock(): too many sigops"),
                REJECT_INVALID, "bad-blk-sigops");

        txdata.emplace_back(tx);
        if (!tx.IsCoinBase()) {
            std::vector<CScriptCheck> vChecks;
            bool fCacheResults = fJustCheck; /* Don't cache results if we're actually connecting blocks (still consult the cache, though) */
            if (!CheckInputs(tx, state, view, fScriptChecks, flags, fCacheResults, fCacheResults, txdata[i], nScriptCheckThreads ? &vChecks : nullptr))
                return error("ConnectBlock(): CheckInputs on %s failed with %s",
                    tx.GetHash().ToString(), FormatStateMessage(state));
            control.Add(vChecks);
        }

        if (fAddressIndex) {
            for (unsigned int k = 0; k < tx.vout.size(); k++) {
                const CTxOut& out = tx.vout[k];

                if (out.scriptPubKey.IsPayToScriptHash()) {
                    std::vector<unsigned char> hashBytes(out.scriptPubKey.begin() + 2, out.scriptPubKey.begin() + 22);

                    // record receiving activity
                    addressIndex.push_back(std::make_pair(CAddressIndexKey(2, uint160(hashBytes), pindex->nHeight, i, txhash, k, false), out.nValue));

                    // record unspent output
                    addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(2, uint160(hashBytes), txhash, k), CAddressUnspentValue(out.nValue, out.scriptPubKey, pindex->nHeight)));

                } else if (out.scriptPubKey.IsPayToPublicKeyHash()) {
                    std::vector<unsigned char> hashBytes(out.scriptPubKey.begin() + 3, out.scriptPubKey.begin() + 23);

                    // record receiving activity
                    addressIndex.push_back(std::make_pair(CAddressIndexKey(1, uint160(hashBytes), pindex->nHeight, i, txhash, k, false), out.nValue));

                    // record unspent output
                    addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(1, uint160(hashBytes), txhash, k), CAddressUnspentValue(out.nValue, out.scriptPubKey, pindex->nHeight)));

                } else if (out.scriptPubKey.IsPayToPublicKey()) {
                    uint160 hashBytes(Hash160(out.scriptPubKey.begin() + 1, out.scriptPubKey.end() - 1));
                    addressIndex.push_back(std::make_pair(CAddressIndexKey(1, hashBytes, pindex->nHeight, i, txhash, k, false), out.nValue));
                    addressUnspentIndex.push_back(std::make_pair(CAddressUnspentKey(1, hashBytes, txhash, k), CAddressUnspentValue(out.nValue, out.scriptPubKey, pindex->nHeight)));
                } else {
                    /** MMM START */
                    if (AreAssetsDeployed()) {
                        std::string assetName;
                        CAmount assetAmount;
                        uint160 hashBytes;

                        if (ParseAssetScript(out.scriptPubKey, hashBytes, assetName, assetAmount)) {
                            // record receiving activity
                            addressIndex.push_back(std::make_pair(
                                CAddressIndexKey(1, hashBytes, assetName, pindex->nHeight, i, txhash, k, false),
                                assetAmount));

                            // record unspent output
                            addressUnspentIndex.push_back(
                                std::make_pair(CAddressUnspentKey(1, hashBytes, assetName, txhash, k),
                                    CAddressUnspentValue(assetAmount, out.scriptPubKey,
                                        pindex->nHeight)));
                        }
                    } else {
                        continue;
                    }
                    /** MMM END */
                }
            }
        }

        CTxUndo undoDummy;
        if (i > 0) {
            blockundo.vtxundo.push_back(CTxUndo());
        }

        /** MMM START */
        // Create the basic empty string pair for the undoblock
        std::pair<std::string, CBlockAssetUndo> undoPair = std::make_pair("", CBlockAssetUndo());
        std::pair<std::string, CBlockAssetUndo>* undoAssetData = &undoPair;
        /** MMM END */

        UpdateCoins(tx, view, i == 0 ? undoDummy : blockundo.vtxundo.back(), pindex->nHeight, block.GetHash(), assetsCache, undoAssetData);

        vPos.push_back(std::make_pair(tx.GetHash(), pos));
        pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
    }
    int64_t nTime3 = GetTimeMicros();
    nTimeConnect += nTime3 - nTime2;
    LogPrint(BCLog::BENCHMARK, "      - Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin) [%.2fs]\n", (unsigned)block.vtx.size(), 0.001 * (nTime3 - nTime2), 0.001 * (nTime3 - nTime2) / block.vtx.size(), nInputs <= 1 ? 0 : 0.001 * (nTime3 - nTime2) / (nInputs - 1), nTimeConnect * 0.000001);

    if (!control.Wait())
        return state.DoS(100, error("%s: CheckQueue failed", __func__), REJECT_INVALID, "block-validation-failed");
    int64_t nTime4 = GetTimeMicros();
    nTimeVerify += nTime4 - nTime2;
    LogPrint(BCLog::BENCHMARK, "    - Verify %u txins: %.2fms (%.3fms/txin) [%.2fs]\n", nInputs - 1, 0.001 * (nTime4 - nTime2), nInputs <= 1 ? 0 : 0.001 * (nTime4 - nTime2) / (nInputs - 1), nTimeVerify * 0.000001);


    // MEMEIUM

    // It's possible that we simply don't have enough data and this could fail
    // (i.e. block itself could be a correct one and we need to store it),
    // that's why this is in ConnectBlock. Could be the other way around however -
    // the peer who sent us this block is missing some data and wasn't able
    // to recognize that block is actually invalid.

    // MEMEIUM : CHECK TRANSACTIONS FOR INSTANTSEND

    if (sporkManager.IsSporkActive(SPORK_3_INSTANTSEND_BLOCK_FILTERING)) {
        // Require other nodes to comply, send them some data in case they are missing it.
        for (const auto& tx : block.vtx) {
            // skip txes that have no inputs
            if (tx->vin.empty()) continue;
            llmq::CInstantSendLockPtr conflictLock = llmq::quorumInstantSendManager->GetConflictingLock(*tx);
            if (!conflictLock) {
                continue;
            }
            if (llmq::chainLocksHandler->HasChainLock(pindex->nHeight, pindex->GetBlockHash())) {
                llmq::quorumInstantSendManager->RemoveChainLockConflictingLock(::SerializeHash(*conflictLock), *conflictLock);
                assert(llmq::quorumInstantSendManager->GetConflictingLock(*tx) == nullptr);
            } else {
                // The node which relayed this should switch to correct chain.
                // TODO: relay instantsend data/proof.
                LOCK(cs_main);
                return state.DoS(10, error("ConnectBlock(MEMEIUM): transaction %s conflicts with transaction lock %s", tx->GetHash().ToString(), conflictLock->txid.ToString()),
                    REJECT_INVALID, "conflict-tx-lock");
            }
        }
    } else {
        LogPrintf("ConnectBlock(MEMEIUM): spork is off, skipping transaction locking checks\n");
    }

    int64_t nTime5_1 = GetTimeMicros();
    nTimeISFilter += nTime5_1 - nTime4;
    LogPrint(BCLog::BENCHMARK, "      - IS filter: %.2fms [%.2fs]\n", 0.001 * (nTime5_1 - nTime4), nTimeISFilter * 0.000001);

    // MEMEIUM : MODIFIED TO CHECK SMARTNODE PAYMENTS AND SUPERBLOCKS

    // TODO: resync data (both ways?) and try to reprocess this block later.
    CAmount blockReward = nFees + GetBlockSubsidy(pindex->pprev->nBits, pindex->pprev->nHeight, chainparams.GetConsensus());
    std::string strError = "";

    int64_t nTime5_2 = GetTimeMicros();
    nTimeSubsidy += nTime5_2 - nTime5_1;
    LogPrint(BCLog::BENCHMARK, "      - GetBlockSubsidy: %.2fms [%.2fs]\n", 0.001 * (nTime5_2 - nTime5_1), nTimeSubsidy * 0.000001);

    if (!IsBlockValueValid(block, pindex->nHeight, (blockReward + specialTxFees), strError)) {
        return state.DoS(0, error("ConnectBlock(MEMEIUM): %s", strError), REJECT_INVALID, "bad-cb-amount");
    }

    int64_t nTime5_3 = GetTimeMicros();
    nTimeValueValid += nTime5_3 - nTime5_2;
    LogPrint(BCLog::BENCHMARK, "      - IsBlockValueValid: %.2fms [%.2fs]\n", 0.001 * (nTime5_3 - nTime5_2), nTimeValueValid * 0.000001);
    if (!IsBlockPayeeValid(*block.vtx[0], pindex->nHeight, blockReward, specialTxFees)) {
        return state.DoS(0, error("ConnectBlock(MEMEIUM): couldn't find smartnode or superblock payments"),
            REJECT_INVALID, "bad-cb-payee");
    }

    int64_t nTime5_4 = GetTimeMicros();
    nTimePayeeValid += nTime5_4 - nTime5_3;
    LogPrint(BCLog::BENCHMARK, "      - IsBlockPayeeValid: %.2fms [%.2fs]\n", 0.001 * (nTime5_4 - nTime5_3), nTimePayeeValid * 0.000001);
    if (!ProcessSpecialTxsInBlock(block, pindex, state, fJustCheck, fScriptChecks)) {
        return error("ConnectBlock(MEMEIUM): ProcessSpecialTxsInBlock for block %s failed with %s",
            pindex->GetBlockHash().ToString(), FormatStateMessage(state));
    }

    int64_t nTime5_5 = GetTimeMicros();
    nTimeProcessSpecial += nTime5_5 - nTime5_4;
    LogPrint(BCLog::BENCHMARK, "      - ProcessSpecialTxsInBlock: %.2fms [%.2fs]\n", 0.001 * (nTime5_5 - nTime5_4), nTimeProcessSpecial * 0.000001);

    int64_t nTime5 = GetTimeMicros();
    nTimeMemeiumSpecific += nTime5 - nTime4;
    LogPrint(BCLog::BENCHMARK, "    - Memeium specific: %.2fms [%.2fs]\n", 0.001 * (nTime5 - nTime4), nTimeMemeiumSpecific * 0.000001);

    // END MEMEIUM

    if (fJustCheck)
        return true;

    // Write undo information to disk
    if (pindex->GetUndoPos().IsNull() || !pindex->IsValid(BLOCK_VALID_SCRIPTS)) {
        if (pindex->GetUndoPos().IsNull()) {
            CDiskBlockPos _pos;
            if (!FindUndoPos(state, pindex->nFile, _pos, ::GetSerializeSize(blockundo, SER_DISK, CLIENT_VERSION) + 40))
                return error("ConnectBlock(): FindUndoPos failed");
            if (!UndoWriteToDisk(blockundo, _pos, pindex->pprev->GetBlockHash(), chainparams.MessageStart()))
                return AbortNode(state, "Failed to write undo data");

            // update nUndoPos in block index
            pindex->nUndoPos = _pos.nPos;
            pindex->nStatus |= BLOCK_HAVE_UNDO;
        }

        pindex->RaiseValidity(BLOCK_VALID_SCRIPTS);
        setDirtyBlockIndex.insert(pindex);
    }

    if (fTxIndex)
        if (!pblocktree->WriteTxIndex(vPos))
            return AbortNode(state, "Failed to write transaction index");

    if (fAddressIndex) {
        if (!pblocktree->WriteAddressIndex(addressIndex)) {
            return AbortNode(state, "Failed to write address index");
        }

        if (!pblocktree->UpdateAddressUnspentIndex(addressUnspentIndex)) {
            return AbortNode(state, "Failed to write address unspent index");
        }
    }

    if (fSpentIndex)
        if (!pblocktree->UpdateSpentIndex(spentIndex))
            return AbortNode(state, "Failed to write transaction index");

    if (fTimestampIndex)
        if (!pblocktree->WriteTimestampIndex(CTimestampIndexKey(pindex->nTime, pindex->GetBlockHash())))
            return AbortNode(state, "Failed to write timestamp index");

    if (fMessaging && setMessages.size()) {
        LOCK(cs_messaging);
        for (auto message : setMessages) {
            int nHeight = 0;
            if (pindex)
                nHeight = pindex->nHeight;
            message.nBlockHeight = nHeight;

            // reenable later
            if (message.nExpiredTime == 0 || GetTime() < message.nExpiredTime)
                GetMainSignals().NewAssetMessage(message);

            if (IsChannelSubscribed(message.strName)) {
                AddMessage(message);
            }
        }
    }
#ifdef ENABLE_WALLET
    if (myNullAssetData.size() && pmyrestricteddb) {
        for (auto item : myNullAssetData) {
            if (IsAssetNameAQualifier(item.second.asset_name)) {
                // TODO we can add block height to this data also, and use it to pull more info on when this was tagged/untagged
                pmyrestricteddb->WriteTaggedAddress(item.first, item.second.asset_name, item.second.flag ? true : false, block.nTime);
            } else if (IsAssetNameAnRestricted(item.second.asset_name)) {
                pmyrestricteddb->WriteRestrictedAddress(item.first, item.second.asset_name, item.second.flag ? true : false, block.nTime);
            }


            if (vpwallets.size())
                // reenable later
                vpwallets[0]->UpdateMyRestrictedAssets(item.first, item.second.asset_name, item.second.flag, block.nTime);
        }
    }
#endif


    // add this block to the view's block chain
    view.SetBestBlock(pindex->GetBlockHash());

    int64_t nTime6 = GetTimeMicros();
    nTimeIndex += nTime6 - nTime5;
    LogPrint(BCLog::BENCHMARK, "    - Index writing: %.2fms [%.2fs]\n", 0.001 * (nTime6 - nTime5), nTimeIndex * 0.000001);

    evoDb->WriteBestBlock(pindex->GetBlockHash());

    int64_t nTime7 = GetTimeMicros();
    nTimeCallbacks += nTime7 - nTime6;
    LogPrint(BCLog::BENCHMARK, "    - Callbacks: %.2fms [%.2fs]\n", 0.001 * (nTime7 - nTime6), nTimeCallbacks * 0.000001);

    return true;
}

/**
 * Update the on-disk chain state.
 * The caches and indexes are flushed depending on the mode we're called with
 * if they're too large, if it's been a while since the last write,
 * or always and in all cases if we're in prune mode and are deleting files.
 */
bool static FlushStateToDisk(const CChainParams& chainparams, CValidationState& state, FlushStateMode mode, int nManualPruneHeight)
{
    int64_t nMempoolUsage = mempool.DynamicMemoryUsage();
    LOCK(cs_main);
    static int64_t nLastWrite = 0;
    static int64_t nLastFlush = 0;
    static int64_t nLastSetChain = 0;
    std::set<int> setFilesToPrune;
    bool fFlushForPrune = false;
    bool fDoFullFlush = false;
    int64_t nNow = 0;
    try {
        {
            LOCK(cs_LastBlockFile);
            if (fPruneMode && (fCheckForPruning || nManualPruneHeight > 0) && !fReindex) {
                if (nManualPruneHeight > 0) {
                    FindFilesToPruneManual(setFilesToPrune, nManualPruneHeight);
                } else {
                    FindFilesToPrune(setFilesToPrune, chainparams.PruneAfterHeight());
                    fCheckForPruning = false;
                }
                if (!setFilesToPrune.empty()) {
                    fFlushForPrune = true;
                    if (!fHavePruned) {
                        pblocktree->WriteFlag("prunedblockfiles", true);
                        fHavePruned = true;
                    }
                }
            }
            nNow = GetTimeMicros();
            // Avoid writing/flushing immediately after startup.
            if (nLastWrite == 0) {
                nLastWrite = nNow;
            }
            if (nLastFlush == 0) {
                nLastFlush = nNow;
            }
            if (nLastSetChain == 0) {
                nLastSetChain = nNow;
            }

            // Get the size of the memory used by the asset cache.
            int64_t assetDynamicSize = 0;
            int64_t assetDirtyCacheSize = 0;
            size_t assetMapAmountSize = 0;
            if (AreAssetsDeployed()) {
                auto currentActiveAssetCache = GetCurrentAssetCache();
                if (currentActiveAssetCache) {
                    assetDynamicSize = currentActiveAssetCache->DynamicMemoryUsage();
                    assetDirtyCacheSize = currentActiveAssetCache->GetCacheSizeV2();
                    assetMapAmountSize = currentActiveAssetCache->mapAssetsAddressAmount.size();
                }
            }

            int messageCacheSize = 0;

            if (fMessaging) {
                messageCacheSize = GetMessageDirtyCacheSize();
            }

            int64_t nMempoolSizeMax = gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
            int64_t cacheSize = pcoinsTip->DynamicMemoryUsage();
            cacheSize += evoDb->GetMemoryUsage();
            int64_t nTotalSpace = nCoinCacheUsage + std::max<int64_t>(nMempoolSizeMax - nMempoolUsage, 0);
            // The cache is large and we're within 10% and 10 MiB of the limit, but we have time now (not in the middle of a block processing).
            bool fCacheLarge = mode == FLUSH_STATE_PERIODIC && cacheSize > std::max((9 * nTotalSpace) / 10, nTotalSpace - MAX_BLOCK_COINSDB_USAGE * 1024 * 1024);
            // The cache is over the limit, we have to write now.
            bool fCacheCritical = mode == FLUSH_STATE_IF_NEEDED && cacheSize > nCoinCacheUsage;
            // It's been a while since we wrote the block index to disk. Do this frequently, so we don't need to redownload after a crash.
            bool fPeriodicWrite = mode == FLUSH_STATE_PERIODIC && nNow > nLastWrite + (int64_t)DATABASE_WRITE_INTERVAL * 1000000;
            // It's been very long since we flushed the cache. Do this infrequently, to optimize cache usage.
            bool fPeriodicFlush = mode == FLUSH_STATE_PERIODIC && nNow > nLastFlush + (int64_t)DATABASE_FLUSH_INTERVAL * 1000000;
            // Combine all conditions that result in a full cache flush.
            fDoFullFlush = (mode == FLUSH_STATE_ALWAYS) || fCacheLarge || fCacheCritical || fPeriodicFlush || fFlushForPrune;
            // Write blocks and block index to disk.
            if (fDoFullFlush || fPeriodicWrite) {
                // Depend on nMinDiskSpace to ensure we can write block index
                if (!CheckDiskSpace(0))
                    return state.Error("out of disk space");
                // First make sure all block and undo data is flushed to disk.
                FlushBlockFile();
                // Then update all block file information (which may refer to block and undo files).
                {
                    std::vector<std::pair<int, const CBlockFileInfo*>> vFiles;
                    vFiles.reserve(setDirtyFileInfo.size());
                    for (std::set<int>::iterator it = setDirtyFileInfo.begin(); it != setDirtyFileInfo.end();) {
                        vFiles.push_back(std::make_pair(*it, &vinfoBlockFile[*it]));
                        setDirtyFileInfo.erase(it++);
                    }
                    std::vector<const CBlockIndex*> vBlocks;
                    vBlocks.reserve(setDirtyBlockIndex.size());
                    for (std::set<CBlockIndex*>::iterator it = setDirtyBlockIndex.begin(); it != setDirtyBlockIndex.end();) {
                        vBlocks.push_back(*it);
                        setDirtyBlockIndex.erase(it++);
                    }
                    if (!pblocktree->WriteBatchSync(vFiles, nLastBlockFile, vBlocks)) {
                        return AbortNode(state, "Failed to write to block index database");
                    }
                }
                // Finally remove any pruned files
                if (fFlushForPrune)
                    UnlinkPrunedFiles(setFilesToPrune);
                nLastWrite = nNow;
            }
            // Flush best chain related state. This can only be done if the blocks / block index write was also done.
            if (fDoFullFlush) {
                // Typical Coin structures on disk are around 48 bytes in size.
                // Pushing a new one to the database can cause it to be written
                // twice (once in the log, and once in the tables). This is already
                // an overestimation, as most will delete an existing entry or
                // overwrite one. Still, use a conservative safety factor of 2.
                if (!CheckDiskSpace(48 * 2 * 2 * pcoinsTip->GetCacheSize()))
                    return state.Error("out of disk space");
                // Flush the chainstate (which may refer to block index entries).
                if (!pcoinsTip->Flush())
                    return AbortNode(state, "Failed to write to coin database");
                if (!evoDb->CommitRootTransaction()) {
                    return AbortNode(state, "Failed to commit EvoDB");
                }
                /** MMM START */
                // Flush the assetstate
                if (AreAssetsDeployed()) {
                    // Flush the assetstate
                    auto currentActiveAssetCache = GetCurrentAssetCache();
                    if (currentActiveAssetCache) {
                        if (!currentActiveAssetCache->DumpCacheToDatabase())
                            return AbortNode(state, "Failed to write to asset database");
                    }
                }

                // Write the reissue mempool data to database
                if (passetsdb)
                    passetsdb->WriteReissuedMempoolState();

                if (fMessaging) {
                    if (pmessagedb) {
                        LOCK(cs_messaging);
                        if (!pmessagedb->Flush())
                            return AbortNode(state, "Failed to Flush the message database");
                    }

                    if (pmessagechanneldb) {
                        LOCK(cs_messaging);
                        if (!pmessagechanneldb->Flush())
                            return AbortNode(state, "Failed to Flush the message channel database");
                    }
                }
                /** MMM END */

                nLastFlush = nNow;
            }
        }
        if (fDoFullFlush || ((mode == FLUSH_STATE_ALWAYS || mode == FLUSH_STATE_PERIODIC) && nNow > nLastSetChain + (int64_t)DATABASE_WRITE_INTERVAL * 1000000)) {
            // Update best block in wallet (so we can detect restored wallets).
            GetMainSignals().SetBestChain(chainActive.GetLocator());
            nLastSetChain = nNow;
        }
    } catch (const std::runtime_error& e) {
        return AbortNode(state, std::string("System error while flushing: ") + e.what());
    }
    return true;
}

void FlushStateToDisk()
{
    CValidationState state;
    const CChainParams& chainparams = Params();
    FlushStateToDisk(chainparams, state, FLUSH_STATE_ALWAYS);
}

void PruneAndFlush()
{
    CValidationState state;
    fCheckForPruning = true;
    const CChainParams& chainparams = Params();
    FlushStateToDisk(chainparams, state, FLUSH_STATE_NONE);
}

static void DoWarning(const std::string& strWarning)
{
    static bool fWarned = false;
    SetMiscWarning(strWarning);
    if (!fWarned) {
        AlertNotify(strWarning);
        fWarned = true;
    }
}

/** Update chainActive and related internal data structures. */
void static UpdateTip(CBlockIndex* pindexNew, const CChainParams& chainParams)
{
    chainActive.SetTip(pindexNew);

    // New best block
    mempool.AddTransactionsUpdated(1);

    cvBlockChange.notify_all();

    std::vector<std::string> warningMessages;
    if (!IsInitialBlockDownload()) {
        int nUpgraded = 0;
        const CBlockIndex* pindex = chainActive.Tip();
        for (int bit = 0; bit < VERSIONBITS_NUM_BITS; bit++) {
            WarningBitsConditionChecker checker(bit);
            ThresholdState state = checker.GetStateFor(pindex, chainParams.GetConsensus(), warningcache[bit]);
            if (state == THRESHOLD_ACTIVE || state == THRESHOLD_LOCKED_IN) {
                const std::string strWarning = strprintf(_("Warning: unknown new rules activated (versionbit %i)"), bit);
                if (state == THRESHOLD_ACTIVE) {
                    DoWarning(strWarning);
                } else {
                    warningMessages.push_back(strWarning);
                }
            }
        }
        // Check the version of the last 100 blocks to see if we need to upgrade:
        for (int i = 0; i < 100 && pindex != nullptr; i++) {
            int32_t nExpectedVersion = ComputeBlockVersion(pindex->pprev, chainParams.GetConsensus());
            if (pindex->nVersion > VERSIONBITS_LAST_OLD_BLOCK_VERSION && (pindex->nVersion & ~nExpectedVersion) != 0)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            warningMessages.push_back(strprintf(_("%d of last 100 blocks have unexpected version"), nUpgraded));
        if (nUpgraded > 100 / 2) {
            std::string strWarning = _("Warning: Unknown block versions being mined! It's possible unknown rules are in effect");
            // notify GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            DoWarning(strWarning);
        }
    }
    std::string strMessage = strprintf("%s: new best=%s height=%d version=0x%08x log2_work=%.8f tx=%lu date='%s' progress=%f cache=%.1fMiB(%utxo)", __func__,
        chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(), chainActive.Tip()->nVersion,
        log(chainActive.Tip()->nChainWork.getdouble()) / log(2.0), (unsigned long)chainActive.Tip()->nChainTx,
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()),
        GuessVerificationProgress(chainParams.TxData(), chainActive.Tip()), pcoinsTip->DynamicMemoryUsage() * (1.0 / (1 << 20)), pcoinsTip->GetCacheSize());
    strMessage += strprintf(" evodb_cache=%.1fMiB", evoDb->GetMemoryUsage() * (1.0 / (1 << 20)));
    if (!warningMessages.empty())
        strMessage += strprintf(" warning='%s'", boost::algorithm::join(warningMessages, ", "));
    LogPrintf("%s\n", strMessage);
}

/** Disconnect chainActive's tip.
 * After calling, the mempool will be in an inconsistent state, with
 * transactions from disconnected blocks being added to disconnectpool.  You
 * should make the mempool consistent again by calling UpdateMempoolForReorg.
 * with cs_main held.
 *
 * If disconnectpool is nullptr, then no disconnected transactions are added to
 * disconnectpool (note that the caller is responsible for mempool consistency
 * in any case).
 */
bool static DisconnectTip(CValidationState& state, const CChainParams& chainparams, DisconnectedBlockTransactions* disconnectpool)
{
    CBlockIndex* pindexDelete = chainActive.Tip();
    assert(pindexDelete);
    // Read block from disk.
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
    CBlock& block = *pblock;
    if (!ReadBlockFromDisk(block, pindexDelete, chainparams.GetConsensus()))
        return AbortNode(state, "Failed to read block");
    // Apply the block atomically to the chain state.
    int64_t nStart = GetTimeMicros();
    {
        auto dbTx = evoDb->BeginTransaction();

        CCoinsViewCache view(pcoinsTip);
        CAssetsCache assetCache;
        assert(view.GetBestBlock() == pindexDelete->GetBlockHash());
        if (DisconnectBlock(block, pindexDelete, view, &assetCache) != DISCONNECT_OK)
            return error("DisconnectTip(): DisconnectBlock %s failed", pindexDelete->GetBlockHash().ToString());
        bool flushed = view.Flush();
        assert(flushed);
        dbTx->Commit();
    }
    LogPrint(BCLog::BENCHMARK, "- Disconnect block: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(chainparams, state, FLUSH_STATE_IF_NEEDED))
        return false;

    if (disconnectpool) {
        // Save transactions to re-add to mempool at end of reorg
        for (auto it = block.vtx.rbegin(); it != block.vtx.rend(); ++it) {
            disconnectpool->addTransaction(*it);
        }
        while (disconnectpool->DynamicMemoryUsage() > MAX_DISCONNECTED_TX_POOL_SIZE * 1000) {
            // Drop the earliest entry, and remove its children from the mempool.
            auto it = disconnectpool->queuedTx.get<insertion_order>().begin();
            mempool.removeRecursive(**it, MemPoolRemovalReason::REORG);
            disconnectpool->removeEntry(it);
        }
    }

    // Update chainActive and related variables.
    chainActive.SetTip(pindexDelete->pprev);
    UpdateTip(pindexDelete->pprev, chainparams);
    // Let wallets know transactions went from 1-confirmed to
    // 0-confirmed or conflicted:
    GetMainSignals().BlockDisconnected(pblock, pindexDelete);
    return true;
}

static int64_t nTimeReadFromDisk = 0;
static int64_t nTimeConnectTotal = 0;
static int64_t nTimeFlush = 0;
static int64_t nTimeChainState = 0;
static int64_t nTimePostConnect = 0;

struct PerBlockConnectTrace {
    CBlockIndex* pindex = nullptr;
    std::shared_ptr<const CBlock> pblock;
    std::shared_ptr<std::vector<CTransactionRef>> conflictedTxs;
    PerBlockConnectTrace() :
        conflictedTxs(std::make_shared<std::vector<CTransactionRef>>()) {}
};
/**
 * Used to track blocks whose transactions were applied to the UTXO state as a
 * part of a single ActivateBestChainStep call.
 *
 * This class also tracks transactions that are removed from the mempool as
 * conflicts (per block) and can be used to pass all those transactions
 * through SyncTransaction.
 *
 * This class assumes (and asserts) that the conflicted transactions for a given
 * block are added via mempool callbacks prior to the BlockConnected() associated
 * with those transactions. If any transactions are marked conflicted, it is
 * assumed that an associated block will always be added.
 *
 * This class is single-use, once you call GetBlocksConnected() you have to throw
 * it away and make a new one.
 */
class ConnectTrace
{
private:
    std::vector<PerBlockConnectTrace> blocksConnected;
    CTxMemPool& pool;

public:
    ConnectTrace(CTxMemPool& _pool) :
        blocksConnected(1), pool(_pool)
    {
        pool.NotifyEntryRemoved.connect(boost::bind(&ConnectTrace::NotifyEntryRemoved, this, _1, _2));
    }

    ~ConnectTrace()
    {
        pool.NotifyEntryRemoved.disconnect(boost::bind(&ConnectTrace::NotifyEntryRemoved, this, _1, _2));
    }

    void BlockConnected(CBlockIndex* pindex, std::shared_ptr<const CBlock> pblock)
    {
        assert(!blocksConnected.back().pindex);
        assert(pindex);
        assert(pblock);
        blocksConnected.back().pindex = pindex;
        blocksConnected.back().pblock = std::move(pblock);
        blocksConnected.emplace_back();
    }

    std::vector<PerBlockConnectTrace>& GetBlocksConnected()
    {
        // We always keep one extra block at the end of our list because
        // blocks are added after all the conflicted transactions have
        // been filled in. Thus, the last entry should always be an empty
        // one waiting for the transactions from the next block. We pop
        // the last entry here to make sure the list we return is sane.
        assert(!blocksConnected.back().pindex);
        assert(blocksConnected.back().conflictedTxs->empty());
        blocksConnected.pop_back();
        return blocksConnected;
    }

    void NotifyEntryRemoved(CTransactionRef txRemoved, MemPoolRemovalReason reason)
    {
        assert(!blocksConnected.back().pindex);
        if (reason == MemPoolRemovalReason::CONFLICT) {
            blocksConnected.back().conflictedTxs->emplace_back(std::move(txRemoved));
        }
    }
};

/**
 * Connect a new block to chainActive. pblock is either nullptr or a pointer to a CBlock
 * corresponding to pindexNew, to bypass loading it again from disk.
 *
 * The block is added to connectTrace if connection succeeds.
 */
bool static ConnectTip(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindexNew, const std::shared_ptr<const CBlock>& pblock, ConnectTrace& connectTrace, DisconnectedBlockTransactions& disconnectpool)
{
    assert(pindexNew->pprev == chainActive.Tip());
    // Read block from disk.
    int64_t nTime1 = GetTimeMicros();
    std::shared_ptr<const CBlock> pthisBlock;
    if (!pblock) {
        std::shared_ptr<CBlock> pblockNew = std::make_shared<CBlock>();
        if (!ReadBlockFromDisk(*pblockNew, pindexNew, chainparams.GetConsensus()))
            return AbortNode(state, "Failed to read block");
        pthisBlock = pblockNew;
    } else {
        pthisBlock = pblock;
    }
    const CBlock& blockConnecting = *pthisBlock;
    // Apply the block atomically to the chain state.
    int64_t nTime2 = GetTimeMicros();
    nTimeReadFromDisk += nTime2 - nTime1;
    int64_t nTime3;
    LogPrint(BCLog::BENCHMARK, "  - Load block from disk: %.2fms [%.2fs]\n", (nTime2 - nTime1) * 0.001, nTimeReadFromDisk * 0.000001);

    /** MEMEIUM ASSETS START */
    // Initialize sets used from removing asset entries from the mempool
    ConnectedBlockAssetData assetDataFromBlock;
    /** MEMEIUM ASSETS END */

    {
        auto dbTx = evoDb->BeginTransaction();

        CCoinsViewCache view(pcoinsTip);
        /** MEMEIUM ASSETS START */
        // Create the empty asset cache, that will be sent into the connect block
        // All new data will be added to the cache, and will be flushed back into passets after a successful
        // Connect Block cycle
        CAssetsCache assetCache;
        /** MEMEIUM ASSETS END */

        bool rv = ConnectBlock(blockConnecting, state, pindexNew, view, chainparams, &assetCache);
        GetMainSignals().BlockChecked(blockConnecting, state);
        if (!rv) {
            if (state.IsInvalid())
                InvalidBlockFound(pindexNew, state);
            return error("ConnectTip(): ConnectBlock %s failed with %s", pindexNew->GetBlockHash().ToString(), FormatStateMessage(state));
        }

        /** MEMEIUM ASSETS START */
        // Get the newly created assets, from the connectblock assetCache so we can remove the correct assets from the mempool
        assetDataFromBlock = {assetCache.setNewAssetsToAdd};

        // Remove all tx hashes, that were marked as reissued script from the mapReissuedTx.
        // Without this check, you wouldn't be able to reissue for those assets again, as this maps block it
        for (auto tx : blockConnecting.vtx) {
            uint256 txHash = tx->GetHash();
            if (mapReissuedTx.count(txHash)) {
                mapReissuedAssets.erase(mapReissuedTx.at(txHash));
                mapReissuedTx.erase(txHash);
            }
        }
        /** MEMEIUM ASSETS END */
        nTime3 = GetTimeMicros();
        nTimeConnectTotal += nTime3 - nTime2;
        LogPrint(BCLog::BENCHMARK, "  - Connect total: %.2fms [%.2fs]\n", (nTime3 - nTime2) * 0.001, nTimeConnectTotal * 0.000001);
        bool flushed = view.Flush();
        assert(flushed);
        dbTx->Commit();

        /** MEMEIUM ASSETS START */
        bool assetFlushed = assetCache.Flush();
        /** MEMEIUM ASSETS END */
    }
    int64_t nTime4 = GetTimeMicros();
    nTimeFlush += nTime4 - nTime3;
    LogPrint(BCLog::BENCHMARK, "  - Flush: %.2fms [%.2fs]\n", (nTime4 - nTime3) * 0.001, nTimeFlush * 0.000001);
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(chainparams, state, FLUSH_STATE_IF_NEEDED))
        return false;
    int64_t nTime5 = GetTimeMicros();
    nTimeChainState += nTime5 - nTime4;
    LogPrint(BCLog::BENCHMARK, "  - Writing chainstate: %.2fms [%.2fs]\n", (nTime5 - nTime4) * 0.001, nTimeChainState * 0.000001);
    // Remove conflicting transactions from the mempool.;
    mempool.removeForBlock(blockConnecting.vtx, pindexNew->nHeight);
    disconnectpool.removeForBlock(blockConnecting.vtx);
    // Update chainActive & related variables.
    UpdateTip(pindexNew, chainparams);

    int64_t nTime6 = GetTimeMicros();
    nTimePostConnect += nTime6 - nTime5;
    nTimeTotal += nTime6 - nTime1;
    LogPrint(BCLog::BENCHMARK, "  - Connect postprocess: %.2fms [%.2fs]\n", (nTime6 - nTime5) * 0.001, nTimePostConnect * 0.000001);
    LogPrint(BCLog::BENCHMARK, "- Connect block: %.2fms [%.2fs]\n", (nTime6 - nTime1) * 0.001, nTimeTotal * 0.000001);

    connectTrace.BlockConnected(pindexNew, std::move(pthisBlock));

    /** MMM START */

    //  Determine if the new block height has any pending snapshot requests,
    //      and if so, capture a snapshot of the relevant target assets.
    if (pSnapshotRequestDb != nullptr) {
        //  Retrieve the scheduled snapshot requests
        std::set<CSnapshotRequestDBEntry> assetsToSnapshot;
        if (pSnapshotRequestDb->RetrieveSnapshotRequestsForHeight("", pindexNew->nHeight, assetsToSnapshot)) {
            //  Loop through them
            for (auto const& assetEntry : assetsToSnapshot) {
                //  Add a snapshot entry for the target asset ownership
                if (!pAssetSnapshotDb->AddAssetOwnershipSnapshot(assetEntry.assetName, pindexNew->nHeight)) {
                    LogPrint(BCLog::REWARDS, "ConnectTip: Failed to snapshot owners for '%s' at height %d!\n",
                        assetEntry.assetName.c_str(), pindexNew->nHeight);
                }
            }
        } else {
            LogPrint(BCLog::REWARDS, "ConnectTip: Failed to load payable Snapshot Requests at height %d!\n", pindexNew->nHeight);
        }
    }

#ifdef ENABLE_WALLET
    if (vpwallets.size()) {
        CheckRewardDistributions(vpwallets[0]);
    }
#endif
    /** MMM END */
    return true;
}

/**
 * Return the tip of the chain with the most work in it, that isn't
 * known to be invalid (it's however far from certain to be valid).
 */
static CBlockIndex* FindMostWorkChain()
{
    do {
        CBlockIndex* pindexNew = nullptr;

        // Find the best candidate header.
        {
            std::set<CBlockIndex*, CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexCandidates.rbegin();
            if (it == setBlockIndexCandidates.rend())
                return nullptr;
            pindexNew = *it;
        }

        // Check whether all blocks on the path between the currently active chain and the candidate are valid.
        // Just going until the active chain is an optimization, as we know all blocks in it are valid already.
        CBlockIndex* pindexTest = pindexNew;
        bool fInvalidAncestor = false;
        while (pindexTest && !chainActive.Contains(pindexTest)) {
            assert(pindexTest->nChainTx || pindexTest->nHeight == 0);

            // Pruned nodes may have entries in setBlockIndexCandidates for
            // which block files have been deleted.  Remove those as candidates
            // for the most work chain if we come across them; we can't switch
            // to a chain unless we have all the non-active-chain parent blocks.
            bool fFailedChain = pindexTest->nStatus & BLOCK_FAILED_MASK;
            bool fMissingData = !(pindexTest->nStatus & BLOCK_HAVE_DATA);
            if (fFailedChain || fMissingData) {
                // Candidate chain is not usable (either invalid or missing data)
                if (fFailedChain && (pindexBestInvalid == nullptr || pindexNew->nChainWork > pindexBestInvalid->nChainWork))
                    pindexBestInvalid = pindexNew;
                CBlockIndex* pindexFailed = pindexNew;
                // Remove the entire chain from the set.
                while (pindexTest != pindexFailed) {
                    if (fFailedChain) {
                        pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
                    } else if (fMissingData) {
                        // If we're missing data, then add back to mapBlocksUnlinked,
                        // so that if the block arrives in the future we can try adding
                        // to setBlockIndexCandidates again.
                        mapBlocksUnlinked.insert(std::make_pair(pindexFailed->pprev, pindexFailed));
                    }
                    setBlockIndexCandidates.erase(pindexFailed);
                    pindexFailed = pindexFailed->pprev;
                }
                setBlockIndexCandidates.erase(pindexTest);
                fInvalidAncestor = true;
                break;
            }
            pindexTest = pindexTest->pprev;
        }
        if (!fInvalidAncestor)
            return pindexNew;
    } while (true);
}

/** Delete all entries in setBlockIndexCandidates that are worse than the current tip. */
static void PruneBlockIndexCandidates()
{
    // Note that we can't delete the current block itself, as we may need to return to it later in case a
    // reorganization to a better block fails.
    std::set<CBlockIndex*, CBlockIndexWorkComparator>::iterator it = setBlockIndexCandidates.begin();
    while (it != setBlockIndexCandidates.end() && setBlockIndexCandidates.value_comp()(*it, chainActive.Tip())) {
        setBlockIndexCandidates.erase(it++);
    }
    // Either the current tip or a successor of it we're working towards is left in setBlockIndexCandidates.
    assert(!setBlockIndexCandidates.empty());
}

/**
 * Try to make some progress towards making pindexMostWork the active block.
 * pblock is either nullptr or a pointer to a CBlock corresponding to pindexMostWork.
 */
static bool ActivateBestChainStep(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindexMostWork, const std::shared_ptr<const CBlock>& pblock, bool& fInvalidFound, ConnectTrace& connectTrace)
{
    AssertLockHeld(cs_main);
    const CBlockIndex* pindexOldTip = chainActive.Tip();
    const CBlockIndex* pindexFork = chainActive.FindFork(pindexMostWork);

    // Disconnect active blocks which are no longer in the best chain.
    bool fBlocksDisconnected = false;
    DisconnectedBlockTransactions disconnectpool;
    while (chainActive.Tip() && chainActive.Tip() != pindexFork) {
        if (!DisconnectTip(state, chainparams, &disconnectpool)) {
            // This is likely a fatal error, but keep the mempool consistent,
            // just in case. Only remove from the mempool in this case.
            UpdateMempoolForReorg(disconnectpool, false);
            return false;
        }
        fBlocksDisconnected = true;
    }

    // Build list of new blocks to connect.
    std::vector<CBlockIndex*> vpindexToConnect;
    bool fContinue = true;
    int nHeight = pindexFork ? pindexFork->nHeight : -1;
    while (fContinue && nHeight != pindexMostWork->nHeight) {
        // Don't iterate the entire list of potential improvements toward the best tip, as we likely only need
        // a few blocks along the way.
        int nTargetHeight = std::min(nHeight + 32, pindexMostWork->nHeight);
        vpindexToConnect.clear();
        vpindexToConnect.reserve(nTargetHeight - nHeight);
        CBlockIndex* pindexIter = pindexMostWork->GetAncestor(nTargetHeight);
        while (pindexIter && pindexIter->nHeight != nHeight) {
            vpindexToConnect.push_back(pindexIter);
            pindexIter = pindexIter->pprev;
        }
        nHeight = nTargetHeight;

        // Connect new blocks.
        for (CBlockIndex* pindexConnect : reverse_iterate(vpindexToConnect)) {
            if (!ConnectTip(state, chainparams, pindexConnect, pindexConnect == pindexMostWork ? pblock : std::shared_ptr<const CBlock>(), connectTrace, disconnectpool)) {
                if (state.IsInvalid()) {
                    // The block violates a consensus rule.
                    if (!state.CorruptionPossible())
                        InvalidChainFound(vpindexToConnect.back());
                    state = CValidationState();
                    fInvalidFound = true;
                    fContinue = false;
                    break;
                } else {
                    // A system error occurred (disk space, database error, ...).
                    // Make the mempool consistent with the current tip, just in case
                    // any observers try to use it before shutdown.
                    UpdateMempoolForReorg(disconnectpool, false);
                    return false;
                }
            } else {
                PruneBlockIndexCandidates();
                if (!pindexOldTip || chainActive.Tip()->nChainWork > pindexOldTip->nChainWork) {
                    // We're in a better position than we were. Return temporarily to release the lock.
                    fContinue = false;
                    break;
                }
            }
        }
    }

    if (fBlocksDisconnected) {
        // If any blocks were disconnected, disconnectpool may be non empty.  Add
        // any disconnected transactions back to the mempool.
        UpdateMempoolForReorg(disconnectpool, true);
    }
    mempool.check(pcoinsTip);

    // Callbacks/notifications for a new best chain.
    if (fInvalidFound)
        CheckForkWarningConditionsOnNewFork(vpindexToConnect.back());
    else
        CheckForkWarningConditions();

    return true;
}

static void NotifyHeaderTip() LOCKS_EXCLUDED(cs_main)
{
    bool fNotify = false;
    bool fInitialBlockDownload = false;
    static CBlockIndex* pindexHeaderOld = nullptr;
    CBlockIndex* pindexHeader = nullptr;
    {
        LOCK(cs_main);
        pindexHeader = pindexBestHeader;

        if (pindexHeader != pindexHeaderOld) {
            fNotify = true;
            fInitialBlockDownload = IsInitialBlockDownload();
            pindexHeaderOld = pindexHeader;
        }
    }
    // Send block tip changed notifications without cs_main
    if (fNotify) {
        uiInterface.NotifyHeaderTip(fInitialBlockDownload, pindexHeader);
        GetMainSignals().NotifyHeaderTip(pindexHeader, fInitialBlockDownload);
    }
}

/**
 * Make the best chain active, in multiple steps. The result is either failure
 * or an activated best chain. pblock is either nullptr or a pointer to a block
 * that is already loaded (to avoid loading it again from disk).
 */
bool ActivateBestChain(CValidationState& state, const CChainParams& chainparams, std::shared_ptr<const CBlock> pblock)
{
    // Note that while we're often called here from ProcessNewBlock, this is
    // far from a guarantee. Things in the P2P/RPC will often end up calling
    // us in the middle of ProcessNewBlock - do not assume pblock is set
    // sanely for performance or correctness!
    AssertLockNotHeld(cs_main);

    // make sure that no matter what, only one thread is executing ActivateBestChain. This avoids a race condition when
    // validation signals are invoked, which might result in out-of-order execution.
    static CCriticalSection cs_activateBestChain;
    LOCK(cs_activateBestChain);

    CBlockIndex* pindexMostWork = nullptr;
    CBlockIndex* pindexNewTip = nullptr;
    int nStopAtHeight = gArgs.GetArg("-stopatheight", DEFAULT_STOPATHEIGHT);
    do {
        boost::this_thread::interruption_point();
        if (ShutdownRequested())
            break;

        const CBlockIndex* pindexFork;
        bool fInitialDownload;
        {
            LOCK(cs_main);
            ConnectTrace connectTrace(mempool); // Destructed before cs_main is unlocked

            CBlockIndex* pindexOldTip = chainActive.Tip();
            if (pindexMostWork == nullptr) {
                pindexMostWork = FindMostWorkChain();
            }

            // Whether we have anything to do at all.
            if (pindexMostWork == nullptr || pindexMostWork == chainActive.Tip())
                return true;

            bool fInvalidFound = false;
            std::shared_ptr<const CBlock> nullBlockPtr;
            if (!ActivateBestChainStep(state, chainparams, pindexMostWork, pblock && pblock->GetHash() == pindexMostWork->GetBlockHash() ? pblock : nullBlockPtr, fInvalidFound, connectTrace))
                return false;

            if (fInvalidFound) {
                // Wipe cache, we may need another branch now.
                pindexMostWork = nullptr;
            }
            pindexNewTip = chainActive.Tip();
            pindexFork = chainActive.FindFork(pindexOldTip);
            fInitialDownload = IsInitialBlockDownload();

            for (const PerBlockConnectTrace& trace : connectTrace.GetBlocksConnected()) {
                assert(trace.pblock && trace.pindex);
                GetMainSignals().BlockConnected(trace.pblock, trace.pindex, *trace.conflictedTxs);
            }
        }
        // When we reach this point, we switched to a new tip (stored in pindexNewTip).

        // Notifications/callbacks that can run without cs_main

        // Notify external listeners about the new tip.
        GetMainSignals().UpdatedBlockTip(pindexNewTip, pindexFork, fInitialDownload);

        // Always notify the UI if a new block tip was connected
        if (pindexFork != pindexNewTip) {
            uiInterface.NotifyBlockTip(fInitialDownload, pindexNewTip);
        }

        if (nStopAtHeight && pindexNewTip && pindexNewTip->nHeight >= nStopAtHeight) StartShutdown();
    } while (pindexNewTip != pindexMostWork);
    CheckBlockIndex(chainparams.GetConsensus());

    // Write changes periodically to disk, after relay.
    if (!FlushStateToDisk(chainparams, state, FLUSH_STATE_PERIODIC)) {
        return false;
    }

    return true;
}


bool PreciousBlock(CValidationState& state, const CChainParams& params, CBlockIndex* pindex)
{
    {
        LOCK(cs_main);
        if (pindex->nChainWork < chainActive.Tip()->nChainWork) {
            // Nothing to do, this block is not at the tip.
            return true;
        }
        if (chainActive.Tip()->nChainWork > nLastPreciousChainwork) {
            // The chain has been extended since the last call, reset the counter.
            nBlockReverseSequenceId = -1;
        }
        nLastPreciousChainwork = chainActive.Tip()->nChainWork;
        setBlockIndexCandidates.erase(pindex);
        pindex->nSequenceId = nBlockReverseSequenceId;
        if (nBlockReverseSequenceId > std::numeric_limits<int32_t>::min()) {
            // We can't keep reducing the counter if somebody really wants to
            // call preciousblock 2**31-1 times on the same set of tips...
            nBlockReverseSequenceId--;
        }
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && pindex->nChainTx) {
            setBlockIndexCandidates.insert(pindex);
            PruneBlockIndexCandidates();
        }
    }
    return ActivateBestChain(state, params);
}

bool InvalidateBlock(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);

    // We first disconnect backwards and then mark the blocks as invalid.
    // This prevents a case where pruned nodes may fail to invalidateblock
    // and be left unable to start as they have no tip candidates (as there
    // are no blocks that meet the "have data and are not invalid per
    // nStatus" criteria for inclusion in setBlockIndexCandidates).

    bool pindex_was_in_chain = false;
    CBlockIndex* invalid_walk_tip = chainActive.Tip();

    if (pindex == pindexBestHeader) {
        pindexBestInvalid = pindexBestHeader;
        pindexBestHeader = pindexBestHeader->pprev;
    }

    DisconnectedBlockTransactions disconnectpool;
    while (chainActive.Contains(pindex)) {
        const CBlockIndex* pindexOldTip = chainActive.Tip();
        pindex_was_in_chain = true;
        // ActivateBestChain considers blocks already in chainActive
        // unconditionally valid already, so force disconnect away from it.
        if (!DisconnectTip(state, chainparams, &disconnectpool)) {
            // It's probably hopeless to try to make the mempool consistent
            // here if DisconnectTip failed, but we can try.
            UpdateMempoolForReorg(disconnectpool, false);
            return false;
        }
        if (pindexOldTip == pindexBestHeader) {
            pindexBestInvalid = pindexBestHeader;
            pindexBestHeader = pindexBestHeader->pprev;
        }
    }

    // Now mark the blocks we just disconnected as descendants invalid
    // (note this may not be all descendants).
    while (pindex_was_in_chain && invalid_walk_tip != pindex) {
        invalid_walk_tip->nStatus |= BLOCK_FAILED_CHILD;
        setDirtyBlockIndex.insert(invalid_walk_tip);
        setBlockIndexCandidates.erase(invalid_walk_tip);
        invalid_walk_tip = invalid_walk_tip->pprev;
    }

    // Mark the block itself as invalid.
    pindex->nStatus |= BLOCK_FAILED_VALID;
    setDirtyBlockIndex.insert(pindex);
    setBlockIndexCandidates.erase(pindex);
    g_failed_blocks.insert(pindex);

    // DisconnectTip will add transactions to disconnectpool; try to add these
    // back to the mempool.
    UpdateMempoolForReorg(disconnectpool, true);

    // The resulting new best tip may not be in setBlockIndexCandidates anymore, so
    // add it again.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && !setBlockIndexCandidates.value_comp()(it->second, chainActive.Tip())) {
            setBlockIndexCandidates.insert(it->second);
        }
        it++;
    }

    InvalidChainFound(pindex);
    GetMainSignals().UpdatedBlockTip(chainActive.Tip(), nullptr, IsInitialBlockDownload());
    uiInterface.NotifyBlockTip(IsInitialBlockDownload(), pindex->pprev);
    return true;
}

bool ResetBlockFailureFlags(CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);

    int nHeight = pindex->nHeight;

    // Remove the invalidity flag from this block and all its descendants.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (!it->second->IsValid() && it->second->GetAncestor(nHeight) == pindex) {
            it->second->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(it->second);
            if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && setBlockIndexCandidates.value_comp()(chainActive.Tip(), it->second)) {
                setBlockIndexCandidates.insert(it->second);
            }
            if (it->second == pindexBestInvalid) {
                // Reset invalid block marker if it was pointing to one of those.
                pindexBestInvalid = nullptr;
            }
            g_failed_blocks.erase(it->second);
        }
        it++;
    }

    // Remove the invalidity flag from all ancestors too.
    while (pindex != nullptr) {
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            pindex->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(pindex);
        }
        pindex = pindex->pprev;
    }
    return true;
}

static CBlockIndex* AddToBlockIndex(const CBlockHeader& block, enum BlockStatus nStatus = BLOCK_VALID_TREE)
{
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end())
        return it->second;

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(block);
    assert(pindexNew);
    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    pindexNew->nSequenceId = 0;
    BlockMap::iterator mi = mapBlockIndex.insert(std::make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = mapBlockIndex.find(block.hashPrevBlock);
    if (miPrev != mapBlockIndex.end()) {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();
    }
    pindexNew->nTimeMax = (pindexNew->pprev ? std::max(pindexNew->pprev->nTimeMax, pindexNew->nTime) : pindexNew->nTime);
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + GetBlockProof(*pindexNew);
    if (nStatus & BLOCK_VALID_MASK) {
        pindexNew->RaiseValidity(nStatus);
        if (pindexBestHeader == nullptr || pindexBestHeader->nChainWork < pindexNew->nChainWork)
            pindexBestHeader = pindexNew;
    } else {
        pindexNew->RaiseValidity(BLOCK_VALID_TREE); // required validity level
        pindexNew->nStatus |= nStatus;
    }

    setDirtyBlockIndex.insert(pindexNew);

    // track prevBlockHash -> pindex (multimap)
    if (pindexNew->pprev) {
        mapPrevBlockIndex.emplace(pindexNew->pprev->GetBlockHash(), pindexNew);
    }

    return pindexNew;
}

/** Mark a block as having its data received and checked (up to BLOCK_VALID_TRANSACTIONS). */
bool ReceivedBlockTransactions(const CBlock& block, CValidationState& state, CBlockIndex* pindexNew, const CDiskBlockPos& pos)
{
    pindexNew->nTx = block.vtx.size();
    pindexNew->nChainTx = 0;
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus |= BLOCK_HAVE_DATA;
    pindexNew->RaiseValidity(BLOCK_VALID_TRANSACTIONS);
    setDirtyBlockIndex.insert(pindexNew);

    if (pindexNew->pprev == nullptr || pindexNew->pprev->nChainTx) {
        // If pindexNew is the genesis block or all parents are BLOCK_VALID_TRANSACTIONS.
        std::deque<CBlockIndex*> queue;
        queue.push_back(pindexNew);

        // Recursively process any descendant blocks that now may be eligible to be connected.
        while (!queue.empty()) {
            CBlockIndex* pindex = queue.front();
            queue.pop_front();
            pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
            {
                LOCK(cs_nBlockSequenceId);
                pindex->nSequenceId = nBlockSequenceId++;
            }
            if (chainActive.Tip() == nullptr || !setBlockIndexCandidates.value_comp()(pindex, chainActive.Tip())) {
                setBlockIndexCandidates.insert(pindex);
            }
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = mapBlocksUnlinked.equal_range(pindex);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator it = range.first;
                queue.push_back(it->second);
                range.first++;
                mapBlocksUnlinked.erase(it);
            }
        }
    } else {
        if (pindexNew->pprev && pindexNew->pprev->IsValid(BLOCK_VALID_TREE)) {
            mapBlocksUnlinked.insert(std::make_pair(pindexNew->pprev, pindexNew));
        }
    }

    return true;
}

static bool FindBlockPos(CValidationState& state, CDiskBlockPos& pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown = false)
{
    LOCK(cs_LastBlockFile);

    unsigned int nFile = fKnown ? pos.nFile : nLastBlockFile;
    if (vinfoBlockFile.size() <= nFile) {
        vinfoBlockFile.resize(nFile + 1);
    }

    if (!fKnown) {
        while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            nFile++;
            if (vinfoBlockFile.size() <= nFile) {
                vinfoBlockFile.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = vinfoBlockFile[nFile].nSize;
    }

    if ((int)nFile != nLastBlockFile) {
        if (!fKnown) {
            LogPrintf("Leaving block file %i: %s\n", nLastBlockFile, vinfoBlockFile[nLastBlockFile].ToString());
        }
        FlushBlockFile(!fKnown);
        nLastBlockFile = nFile;
    }

    vinfoBlockFile[nFile].AddBlock(nHeight, nTime);
    if (fKnown)
        vinfoBlockFile[nFile].nSize = std::max(pos.nPos + nAddSize, vinfoBlockFile[nFile].nSize);
    else
        vinfoBlockFile[nFile].nSize += nAddSize;

    if (!fKnown) {
        unsigned int nOldChunks = (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks = (vinfoBlockFile[nFile].nSize + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        if (nNewChunks > nOldChunks) {
            if (fPruneMode)
                fCheckForPruning = true;
            if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos)) {
                FILE* file = OpenBlockFile(pos);
                if (file) {
                    LogPrintf("Pre-allocating up to position 0x%x in blk%05u.dat\n", nNewChunks * BLOCKFILE_CHUNK_SIZE, pos.nFile);
                    AllocateFileRange(file, pos.nPos, nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos);
                    fclose(file);
                }
            } else
                return state.Error("out of disk space");
        }
    }

    setDirtyFileInfo.insert(nFile);
    return true;
}

static bool FindUndoPos(CValidationState& state, int nFile, CDiskBlockPos& pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    unsigned int nNewSize;
    pos.nPos = vinfoBlockFile[nFile].nUndoSize;
    nNewSize = vinfoBlockFile[nFile].nUndoSize += nAddSize;
    setDirtyFileInfo.insert(nFile);

    unsigned int nOldChunks = (pos.nPos + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    unsigned int nNewChunks = (nNewSize + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    if (nNewChunks > nOldChunks) {
        if (fPruneMode)
            fCheckForPruning = true;
        if (CheckDiskSpace(nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos)) {
            FILE* file = OpenUndoFile(pos);
            if (file) {
                LogPrintf("Pre-allocating up to position 0x%x in rev%05u.dat\n", nNewChunks * UNDOFILE_CHUNK_SIZE, pos.nFile);
                AllocateFileRange(file, pos.nPos, nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos);
                fclose(file);
            }
        } else
            return state.Error("out of disk space");
    }

    return true;
}

static bool CheckBlockHeader(const CBlockHeader& block, CValidationState& state, const Consensus::Params& consensusParams, bool fCheckPOW = true)
{
    // Check proof of work matches claimed amount
    if (fCheckPOW && !CheckPOW(block, consensusParams))
        return state.DoS(50, false, REJECT_INVALID, "high-hash", false, "proof of work failed");

    // Check DevNet
    if (!consensusParams.hashDevnetGenesisBlock.IsNull() &&
        block.hashPrevBlock == consensusParams.hashGenesisBlock &&
        block.GetHash() != consensusParams.hashDevnetGenesisBlock) {
        return state.DoS(100, error("CheckBlockHeader(): wrong devnet genesis"),
            REJECT_INVALID, "devnet-genesis");
    }

    return true;
}

bool CheckBlock(const CBlock& block, CValidationState& state, const Consensus::Params& consensusParams, int nHeight, bool fCheckPOW, bool fCheckMerkleRoot, bool fDBCheck)
{
    // These are checks that are independent of context.
    if (block.fChecked)
        return true;

    // Check that the header is valid (particularly PoW).  This is mostly
    // redundant with the call in AcceptBlockHeader.
    if (!CheckBlockHeader(block, state, consensusParams, fCheckPOW))
        return false;

    // Check the merkle root.
    if (fCheckMerkleRoot) {
        bool mutated;
        uint256 hashMerkleRoot2 = BlockMerkleRoot(block, &mutated);
        if (block.hashMerkleRoot != hashMerkleRoot2)
            return state.DoS(100, false, REJECT_INVALID, "bad-txnmrklroot", true, "hashMerkleRoot mismatch");

        // Check for merkle tree malleability (CVE-2012-2459): repeating sequences
        // of transactions in a block without affecting the merkle root of a block,
        // while still invalidating it.
        if (mutated)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-duplicate", true, "duplicate transaction");
    }

    // All potential-corruption validation must be done before we do any
    // transaction validation, as otherwise we may mark the header as invalid
    // because we receive the wrong transactions for it.

    // Size limits (relaxed)
    if (block.vtx.empty() || block.vtx.size() > MaxBlockSize(true) || ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION) > MaxBlockSize(true))
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-length", false, "size limits failed");

    // First transaction must be coinbase, the rest must not be
    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase())
        return state.DoS(100, false, REJECT_INVALID, "bad-cb-missing", false, "first tx is not coinbase");
    for (unsigned int i = 1; i < block.vtx.size(); i++)
        if (block.vtx[i]->IsCoinBase())
            return state.DoS(100, false, REJECT_INVALID, "bad-cb-multiple", false, "more than one coinbase");
    CAmount blockReward = GetBlockSubsidy(1, nHeight - 1, Params().GetConsensus(), false);
    // Check transactions
    bool fCheckBlock = true;
    bool fCheckMempool = false;
    for (const auto& tx : block.vtx) {
        // We only want to check the blocks when they are added to our chain
        // We want to make sure when nodes shutdown and restart that they still
        // verify the blocks in the database correctly even if Enforce Value BIP is active
        fCheckBlock = true;
        if (fDBCheck) {
            fCheckBlock = false;
        }
        if (!CheckTransaction(*tx, state, nHeight - 1, blockReward, fCheckMempool, fCheckBlock))
            return state.Invalid(false, state.GetRejectCode(), state.GetRejectReason(),
                strprintf("Transaction check failed (tx hash %s) %s", tx->GetHash().ToString(), state.GetDebugMessage()));
    }

    unsigned int nSigOps = 0;
    for (const auto& tx : block.vtx) {
        nSigOps += GetLegacySigOpCount(*tx);
    }
    // sigops limits (relaxed)
    if (nSigOps > MaxBlockSigOps(true))
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-sigops", false, "out-of-bounds SigOpCount");

    if (fCheckPOW && fCheckMerkleRoot)
        block.fChecked = true;

    return true;
}

/** Context-dependent validity checks.
 *  By "context", we mean only the previous block headers, but not the UTXO
 *  set; UTXO-related validity checks are done in ConnectBlock(). */
static bool ContextualCheckBlockHeader(const CBlockHeader& block, CValidationState& state, const CChainParams& params, const CBlockIndex* pindexPrev, int64_t nAdjustedTime)
{
    assert(pindexPrev != nullptr);
    const int nHeight = pindexPrev->nHeight + 1;

    // Check proof of work
    const Consensus::Params& consensusParams = params.GetConsensus();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN && nHeight <= 68589) {
        // architecture issues with DGW v1 and v2)
        unsigned int nBitsNext = GetNextWorkRequired(pindexPrev, &block, consensusParams);
        double n1 = ConvertBitsToDouble(block.nBits);
        double n2 = ConvertBitsToDouble(nBitsNext);

        if (abs(n1 - n2) > n1 * 0.5)
            return state.DoS(100, error("%s : incorrect proof of work (DGW pre-fork) - %f %f %f at %d", __func__, abs(n1 - n2), n1, n2, nHeight),
                REJECT_INVALID, "bad-diffbits");
    } else {
        if (block.nBits != GetNextWorkRequired(pindexPrev, &block, consensusParams))
            return state.DoS(100, false, REJECT_INVALID, "bad-diffbits", false, strprintf("incorrect proof of work at %d", nHeight));
    }

    // Check against checkpoints
    if (fCheckpointsEnabled) {
        // Don't accept any forks from the main chain prior to last checkpoint.
        // GetLastCheckpoint finds the last checkpoint in MapCheckpoints that's in our
        // MapBlockIndex.
        CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(params.Checkpoints());
        if (pcheckpoint && nHeight < pcheckpoint->nHeight)
            return state.DoS(100, error("%s: forked chain older than last checkpoint (height %d)", __func__, nHeight), REJECT_CHECKPOINT, "bad-fork-prior-to-checkpoint");
    }

    // Check timestamp against prev
    if (block.GetBlockTime() <= pindexPrev->GetMedianTimePast())
        return state.Invalid(false, REJECT_INVALID, "time-too-old", strprintf("block's timestamp is too early %d %d", block.GetBlockTime(), pindexPrev->GetMedianTimePast()));

    // Check timestamp
    if (block.GetBlockTime() > nAdjustedTime + MAX_FUTURE_BLOCK_TIME)
        return state.Invalid(false, REJECT_INVALID, "time-too-new", strprintf("block timestamp too far in the future %d %d", block.GetBlockTime(), nAdjustedTime + MAX_FUTURE_BLOCK_TIME));

    // check for version 2, 3 and 4 upgrades
    //    if((block.nVersion < 2 && consensusParams.BIP34Enabled) ||
    //       (block.nVersion < 3 && consensusParams.BIP66Enabled) ||
    //       (block.nVersion < 4 && consensusParams.BIP65Enabled))
    //            return state.Invalid(false, REJECT_OBSOLETE, strprintf("bad-version(0x%08x)", block.nVersion),
    //                                 strprintf("rejected nVersion=0x%08x block", block.nVersion));

    return true;
}

static bool ContextualCheckBlock(const CBlock& block, CValidationState& state, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    const int nHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;

    // Start enforcing BIP113 (Median Time Past) using versionbits logic.
    int nLockTimeFlags = 0;
    if (consensusParams.BIPCSVEnabled) {
        nLockTimeFlags |= LOCKTIME_MEDIAN_TIME_PAST;
    }

    int64_t nLockTimeCutoff = (nLockTimeFlags & LOCKTIME_MEDIAN_TIME_PAST)
                                  ? pindexPrev->GetMedianTimePast()
                                  : block.GetBlockTime();

    bool fDIP0001Active_context = consensusParams.DIP0001Enabled;
    bool fDIP0003Active_context = consensusParams.DIP0003Enabled;

    // Size limits
    unsigned int nMaxBlockSize = MaxBlockSize(fDIP0001Active_context);
    if (block.vtx.empty() || block.vtx.size() > nMaxBlockSize || ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION) > nMaxBlockSize)
        return state.DoS(10, false, REJECT_INVALID, "bad-blk-length", false, "size limits failed");

    // Check that all transactions are finalized and not over-sized
    // Also count sigops
    unsigned int nSigOps = 0;
    for (const auto& tx : block.vtx) {
        if (!IsFinalTx(*tx, nHeight, nLockTimeCutoff)) {
            return state.DoS(10, false, REJECT_INVALID, "bad-txns-nonfinal", false, "non-final transaction");
        }
        if (!ContextualCheckTransaction(*tx, state, consensusParams, pindexPrev)) {
            return false;
        }
        nSigOps += GetLegacySigOpCount(*tx);
    }

    // Check sigops
    if (nSigOps > MaxBlockSigOps(fDIP0001Active_context))
        return state.DoS(10, false, REJECT_INVALID, "bad-blk-sigops", false, "out-of-bounds SigOpCount");

    // Enforce rule that the coinbase starts with serialized block height
    // After DIP3/DIP4 activation, we don't enforce the height in the input script anymore.
    // The CbTx special transaction payload will then contain the height, which is checked in CheckCbTx
    if (consensusParams.BIP34Enabled && !fDIP0003Active_context) {
        CScript expect = CScript() << nHeight;
        if (block.vtx[0]->vin[0].scriptSig.size() < expect.size() ||
            !std::equal(expect.begin(), expect.end(), block.vtx[0]->vin[0].scriptSig.begin())) {
            return state.DoS(100, false, REJECT_INVALID, "bad-cb-height", false, "block height mismatch in coinbase");
        }
    }

    if (fDIP0003Active_context) {
        if (nHeight != 0 && block.vtx[0]->nType != TRANSACTION_COINBASE) {
            return state.DoS(100, false, REJECT_INVALID, "bad-cb-type", false, "coinbase is not a CbTx");
        }
    }

    return true;
}

static bool AcceptBlockHeader(const CBlockHeader& block, CValidationState& state, const CChainParams& chainparams, CBlockIndex** ppindex)
{
    AssertLockHeld(cs_main);
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator miSelf = mapBlockIndex.find(hash);
    CBlockIndex* pindex = nullptr;

    // TODO : ENABLE BLOCK CACHE IN SPECIFIC CASES
    if (hash != chainparams.GetConsensus().hashGenesisBlock) {
        if (miSelf != mapBlockIndex.end()) {
            // Block header is already known.
            pindex = miSelf->second;
            if (ppindex)
                *ppindex = pindex;
            if (pindex->nStatus & BLOCK_FAILED_MASK)
                return state.Invalid(error("%s: block %s is marked invalid", __func__, hash.ToString()), 0, "duplicate");
            return true;
        }

        if (!CheckBlockHeader(block, state, chainparams.GetConsensus()))
            return error("%s: Consensus::CheckBlockHeader: %s, %s", __func__, hash.ToString(), FormatStateMessage(state));

        // Get prev block index
        CBlockIndex* pindexPrev = nullptr;
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end())
            return state.DoS(10, error("%s: prev block not found", __func__), 0, "prev-blk-not-found");
        pindexPrev = (*mi).second;
        assert(pindexPrev);

        if (pindexPrev->nStatus & BLOCK_FAILED_MASK)
            return state.DoS(100, error("%s: prev block invalid", __func__), REJECT_INVALID, "bad-prevblk");

        if (pindexPrev->nStatus & BLOCK_CONFLICT_CHAINLOCK)
            // it's ok-ish, the other node is probably missing the latest chainlock
            return state.DoS(10, error("%s: prev block %s conflicts with chainlock", __func__, block.hashPrevBlock.ToString()), REJECT_INVALID, "bad-prevblk-chainlock");

        if (!ContextualCheckBlockHeader(block, state, chainparams, pindexPrev, GetAdjustedTime()))
            return error("%s: Consensus::ContextualCheckBlockHeader: %s, %s", __func__, hash.ToString(), FormatStateMessage(state));

        if (!pindexPrev->IsValid(BLOCK_VALID_SCRIPTS)) {
            for (const CBlockIndex* failedit : g_failed_blocks) {
                if (pindexPrev->GetAncestor(failedit->nHeight) == failedit) {
                    assert(failedit->nStatus & BLOCK_FAILED_VALID);
                    CBlockIndex* invalid_walk = pindexPrev;
                    while (invalid_walk != failedit) {
                        invalid_walk->nStatus |= BLOCK_FAILED_CHILD;
                        setDirtyBlockIndex.insert(invalid_walk);
                        invalid_walk = invalid_walk->pprev;
                    }
                    return state.DoS(100, error("%s: prev block invalid", __func__), REJECT_INVALID, "bad-prevblk");
                }
            }
        }

        if (llmq::chainLocksHandler->HasConflictingChainLock(pindexPrev->nHeight + 1, hash)) {
            if (pindex == nullptr) {
                AddToBlockIndex(block, BLOCK_CONFLICT_CHAINLOCK);
            }
            return state.DoS(10, error("%s: header %s conflicts with chainlock", __func__, hash.ToString()), REJECT_INVALID, "bad-chainlock");
        }
    }
    if (pindex == nullptr)
        pindex = AddToBlockIndex(block);

    if (ppindex)
        *ppindex = pindex;

    CheckBlockIndex(chainparams.GetConsensus());

    // Notify external listeners about accepted block header
    GetMainSignals().AcceptedBlockHeader(pindex);

    return true;
}

// Exposed wrapper for AcceptBlockHeader
bool ProcessNewBlockHeaders(const std::vector<CBlockHeader>& headers, CValidationState& state, const CChainParams& chainparams, const CBlockIndex** ppindex, CBlockHeader* first_invalid)
{
    if (first_invalid != nullptr) first_invalid->SetNull();
    {
        LOCK(cs_main);
        for (const CBlockHeader& header : headers) {
            CBlockIndex* pindex = nullptr; // Use a temp pindex instead of ppindex to avoid a const_cast
            if (!AcceptBlockHeader(header, state, chainparams, &pindex)) {
                if (first_invalid) *first_invalid = header;
                return false;
            }
            if (ppindex) {
                *ppindex = pindex;
            }
        }
    }
    NotifyHeaderTip();
    return true;
}

/** Store block on disk. If dbp is non-nullptr, the file is known to already reside on disk */
static bool AcceptBlock(const std::shared_ptr<const CBlock>& pblock, CValidationState& state, const CChainParams& chainparams, CBlockIndex** ppindex, bool fRequested, const CDiskBlockPos* dbp, bool* fNewBlock)
{
    const CBlock& block = *pblock;

    if (fNewBlock) *fNewBlock = false;
    AssertLockHeld(cs_main);

    CBlockIndex* pindexDummy = nullptr;
    CBlockIndex*& pindex = ppindex ? *ppindex : pindexDummy;

    if (!AcceptBlockHeader(block, state, chainparams, &pindex))
        return false;

    // Try to process all requested blocks that we don't have, but only
    // process an unrequested block if it's new and has enough work to
    // advance our tip, and isn't too many blocks ahead.
    bool fAlreadyHave = pindex->nStatus & BLOCK_HAVE_DATA;
    bool fHasMoreOrSameWork = (chainActive.Tip() ? pindex->nChainWork >= chainActive.Tip()->nChainWork : true);
    // Blocks that are too out-of-order needlessly limit the effectiveness of
    // pruning, because pruning will not delete block files that contain any
    // blocks which are too close in height to the tip.  Apply this test
    // regardless of whether pruning is enabled; it should generally be safe to
    // not process unrequested blocks.
    bool fTooFarAhead = (pindex->nHeight > int(chainActive.Height() + MIN_BLOCKS_TO_KEEP));

    // TODO: Decouple this function from the block download logic by removing fRequested
    // This requires some new chain data structure to efficiently look up if a
    // block is in a chain leading to a candidate for best tip, despite not
    // being such a candidate itself.

    // TODO: deal better with return value and error conditions for duplicate
    // and unrequested blocks.
    if (fAlreadyHave) return true;
    if (!fRequested) {                        // If we didn't ask for it:
        if (pindex->nTx != 0) return true;    // This is a previously-processed block that was pruned
        if (!fHasMoreOrSameWork) return true; // Don't process less-work chains
        if (fTooFarAhead) return true;        // Block height is too high

        // Protect against DoS attacks from low-work chains.
        // If our tip is behind, a peer could try to send us
        // low-work blocks on a fake chain that we would never
        // request; don't process these.
        if (pindex->nChainWork < nMinimumChainWork) return true;
    }
    if (fNewBlock) *fNewBlock = true;

    auto currentActiveAssetCache = GetCurrentAssetCache();
    // Dont force the CheckBlock asset duplciates when checking from this state
    if (!CheckBlock(block, state, chainparams.GetConsensus(), pindex->nHeight) ||
        !ContextualCheckBlock(block, state, chainparams.GetConsensus(), pindex->pprev)) {
        if (state.IsInvalid() && !state.CorruptionPossible()) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
            setDirtyBlockIndex.insert(pindex);
        }
        return error("%s: %s", __func__, FormatStateMessage(state));
    }

    // Header is valid/has work, merkle tree is good...RELAY NOW
    // (but if it does not build on our best tip, let the SendMessages loop relay it)
    if (!IsInitialBlockDownload() && chainActive.Tip() == pindex->pprev)
        GetMainSignals().NewPoWValidBlock(pindex, pblock);

    int nHeight = pindex->nHeight;

    // Write block to history file
    try {
        unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
        CDiskBlockPos blockPos;
        if (dbp != nullptr)
            blockPos = *dbp;
        if (!FindBlockPos(state, blockPos, nBlockSize + 8, nHeight, block.GetBlockTime(), dbp != nullptr))
            return error("AcceptBlock(): FindBlockPos failed");
        if (dbp == nullptr)
            if (!WriteBlockToDisk(block, blockPos, chainparams.MessageStart()))
                AbortNode(state, "Failed to write block");
        if (!ReceivedBlockTransactions(block, state, pindex, blockPos))
            return error("AcceptBlock(): ReceivedBlockTransactions failed");
    } catch (const std::runtime_error& e) {
        return AbortNode(state, std::string("System error: ") + e.what());
    }

    if (fCheckForPruning)
        FlushStateToDisk(chainparams, state, FLUSH_STATE_NONE); // we just allocated more disk space for block files

    return true;
}

bool ProcessNewBlock(const CChainParams& chainparams, const std::shared_ptr<const CBlock> pblock, bool fForceProcessing, bool* fNewBlock)
{
    AssertLockNotHeld(cs_main);

    {
        CBlockIndex* pindex = nullptr;
        if (fNewBlock) *fNewBlock = false;
        CValidationState state;
        // Ensure that CheckBlock() passes before calling AcceptBlock, as
        // belt-and-suspenders.
        int nHeight = chainActive.Tip()->nHeight + 1;
        bool ret = CheckBlock(*pblock, state, chainparams.GetConsensus(), nHeight);

        LOCK(cs_main);

        if (ret) {
            // Store to disk
            ret = AcceptBlock(pblock, state, chainparams, &pindex, fForceProcessing, nullptr, fNewBlock);
        }
        CheckBlockIndex(chainparams.GetConsensus());
        if (!ret) {
            GetMainSignals().BlockChecked(*pblock, state);
            return error("%s: AcceptBlock FAILED: %s", __func__, FormatStateMessage(state));
        }
    }

    NotifyHeaderTip();

    CValidationState state; // Only used to report errors, not invalidity - ignore it
    if (!ActivateBestChain(state, chainparams, pblock))
        return error("%s: ActivateBestChain failed: %s", __func__, FormatStateMessage(state));

    LogPrintf("%s : ACCEPTED\n", __func__);
    return true;
}

bool TestBlockValidity(CValidationState& state, const CChainParams& chainparams, const CBlock& block, CBlockIndex* pindexPrev, bool fCheckPOW, bool fCheckMerkleRoot)
{
    AssertLockHeld(cs_main);
    assert(pindexPrev && pindexPrev == chainActive.Tip());

    uint256 hash = block.GetHash();
    if (llmq::chainLocksHandler->HasConflictingChainLock(pindexPrev->nHeight + 1, hash)) {
        return state.DoS(10, error("%s: conflicting with chainlock", __func__), REJECT_INVALID, "bad-chainlock");
    }

    CCoinsViewCache viewNew(pcoinsTip);
    CBlockIndex indexDummy(block);
    indexDummy.pprev = pindexPrev;
    indexDummy.nHeight = pindexPrev->nHeight + 1;

    // begin tx and let it rollback
    auto dbTx = evoDb->BeginTransaction();
    /** MMM ASSETS START */
    CAssetsCache assetCache = *GetCurrentAssetCache();
    /** MMM ASSETS END */
    // NOTE: CheckBlockHeader is called by CheckBlock
    if (!ContextualCheckBlockHeader(block, state, chainparams, pindexPrev, GetAdjustedTime()))
        return error("%s: Consensus::ContextualCheckBlockHeader: %s", __func__, FormatStateMessage(state));
    if (!CheckBlock(block, state, chainparams.GetConsensus(), indexDummy.nHeight, fCheckPOW, fCheckMerkleRoot))
        return error("%s: Consensus::CheckBlock: %s", __func__, FormatStateMessage(state));
    if (!ContextualCheckBlock(block, state, chainparams.GetConsensus(), pindexPrev))
        return error("%s: Consensus::ContextualCheckBlock: %s", __func__, FormatStateMessage(state));
    if (!ConnectBlock(block, state, &indexDummy, viewNew, chainparams, &assetCache, true))
        return false;
    assert(state.IsValid());

    return true;
}

/**
 * BLOCK PRUNING CODE
 */

/* Calculate the amount of disk space the block & undo files currently use */
static uint64_t CalculateCurrentUsage()
{
    uint64_t retval = 0;
    for (const CBlockFileInfo& file : vinfoBlockFile) {
        retval += file.nSize + file.nUndoSize;
    }
    return retval;
}

/* Prune a block file (modify associated database entries)*/
void PruneOneBlockFile(const int fileNumber)
{
    for (BlockMap::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); ++it) {
        CBlockIndex* pindex = it->second;
        if (pindex->nFile == fileNumber) {
            pindex->nStatus &= ~BLOCK_HAVE_DATA;
            pindex->nStatus &= ~BLOCK_HAVE_UNDO;
            pindex->nFile = 0;
            pindex->nDataPos = 0;
            pindex->nUndoPos = 0;
            setDirtyBlockIndex.insert(pindex);

            // Prune from mapBlocksUnlinked -- any block we prune would have
            // to be downloaded again in order to consider its chain, at which
            // point it would be considered as a candidate for
            // mapBlocksUnlinked or setBlockIndexCandidates.
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = mapBlocksUnlinked.equal_range(pindex->pprev);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator _it = range.first;
                range.first++;
                if (_it->second == pindex) {
                    mapBlocksUnlinked.erase(_it);
                }
            }
        }
    }

    vinfoBlockFile[fileNumber].SetNull();
    setDirtyFileInfo.insert(fileNumber);
}


void UnlinkPrunedFiles(const std::set<int>& setFilesToPrune)
{
    for (std::set<int>::iterator it = setFilesToPrune.begin(); it != setFilesToPrune.end(); ++it) {
        CDiskBlockPos pos(*it, 0);
        fs::remove(GetBlockPosFilename(pos, "blk"));
        fs::remove(GetBlockPosFilename(pos, "rev"));
        LogPrintf("Prune: %s deleted blk/rev (%05u)\n", __func__, *it);
    }
}

/* Calculate the block/rev files to delete based on height specified by user with RPC command pruneblockchain */
static void FindFilesToPruneManual(std::set<int>& setFilesToPrune, int nManualPruneHeight)
{
    assert(fPruneMode && nManualPruneHeight > 0);

    LOCK2(cs_main, cs_LastBlockFile);
    if (chainActive.Tip() == nullptr)
        return;

    // last block to prune is the lesser of (user-specified height, MIN_BLOCKS_TO_KEEP from the tip)
    unsigned int nLastBlockWeCanPrune = std::min((unsigned)nManualPruneHeight, chainActive.Tip()->nHeight - MIN_BLOCKS_TO_KEEP);
    int count = 0;
    for (int fileNumber = 0; fileNumber < nLastBlockFile; fileNumber++) {
        if (vinfoBlockFile[fileNumber].nSize == 0 || vinfoBlockFile[fileNumber].nHeightLast > nLastBlockWeCanPrune)
            continue;
        PruneOneBlockFile(fileNumber);
        setFilesToPrune.insert(fileNumber);
        count++;
    }
    LogPrintf("Prune (Manual): prune_height=%d removed %d blk/rev pairs\n", nLastBlockWeCanPrune, count);
}

/* This function is called from the RPC code for pruneblockchain */
void PruneBlockFilesManual(int nManualPruneHeight)
{
    CValidationState state;
    const CChainParams& chainparams = Params();
    FlushStateToDisk(chainparams, state, FLUSH_STATE_NONE, nManualPruneHeight);
}

/**
 * Prune block and undo files (blk???.dat and undo???.dat) so that the disk space used is less than a user-defined target.
 * The user sets the target (in MB) on the command line or in config file.  This will be run on startup and whenever new
 * space is allocated in a block or undo file, staying below the target. Changing back to unpruned requires a reindex
 * (which in this case means the blockchain must be re-downloaded.)
 *
 * Pruning functions are called from FlushStateToDisk when the global fCheckForPruning flag has been set.
 * Block and undo files are deleted in lock-step (when blk00003.dat is deleted, so is rev00003.dat.)
 * Pruning cannot take place until the longest chain is at least a certain length (100000 on mainnet, 1000 on testnet, 1000 on regtest).
 * Pruning will never delete a block within a defined distance (currently 288) from the active chain's tip.
 * The block index is updated by unsetting HAVE_DATA and HAVE_UNDO for any blocks that were stored in the deleted files.
 * A db flag records the fact that at least some block files have been pruned.
 *
 * @param[out]   setFilesToPrune   The set of file indices that can be unlinked will be returned
 */
static void FindFilesToPrune(std::set<int>& setFilesToPrune, uint64_t nPruneAfterHeight)
{
    LOCK2(cs_main, cs_LastBlockFile);
    if (chainActive.Tip() == nullptr || nPruneTarget == 0) {
        return;
    }
    if ((uint64_t)chainActive.Tip()->nHeight <= nPruneAfterHeight) {
        return;
    }

    unsigned int nLastBlockWeCanPrune = chainActive.Tip()->nHeight - MIN_BLOCKS_TO_KEEP;
    uint64_t nCurrentUsage = CalculateCurrentUsage();
    // We don't check to prune until after we've allocated new space for files
    // So we should leave a buffer under our target to account for another allocation
    // before the next pruning.
    uint64_t nBuffer = BLOCKFILE_CHUNK_SIZE + UNDOFILE_CHUNK_SIZE;
    uint64_t nBytesToPrune;
    int count = 0;

    if (nCurrentUsage + nBuffer >= nPruneTarget) {
        for (int fileNumber = 0; fileNumber < nLastBlockFile; fileNumber++) {
            nBytesToPrune = vinfoBlockFile[fileNumber].nSize + vinfoBlockFile[fileNumber].nUndoSize;

            if (vinfoBlockFile[fileNumber].nSize == 0)
                continue;

            if (nCurrentUsage + nBuffer < nPruneTarget) // are we below our target?
                break;

            // don't prune files that could have a block within MIN_BLOCKS_TO_KEEP of the main chain's tip but keep scanning
            if (vinfoBlockFile[fileNumber].nHeightLast > nLastBlockWeCanPrune)
                continue;

            PruneOneBlockFile(fileNumber);
            // Queue up the files for removal
            setFilesToPrune.insert(fileNumber);
            nCurrentUsage -= nBytesToPrune;
            count++;
        }
    }

    LogPrint(BCLog::PRUNE, "Prune: target=%dMiB actual=%dMiB diff=%dMiB max_prune_height=%d removed %d blk/rev pairs\n",
        nPruneTarget / 1024 / 1024, nCurrentUsage / 1024 / 1024,
        ((int64_t)nPruneTarget - (int64_t)nCurrentUsage) / 1024 / 1024,
        nLastBlockWeCanPrune, count);
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
    uint64_t nFreeBytesAvailable = fs::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
        return AbortNode("Disk space is low!", _("Error: Disk space is low!"));

    return true;
}

static FILE* OpenDiskFile(const CDiskBlockPos& pos, const char* prefix, bool fReadOnly)
{
    if (pos.IsNull())
        return nullptr;
    fs::path path = GetBlockPosFilename(pos, prefix);
    fs::create_directories(path.parent_path());
    FILE* file = fsbridge::fopen(path, "rb+");
    if (!file && !fReadOnly)
        file = fsbridge::fopen(path, "wb+");
    if (!file) {
        LogPrintf("Unable to open file %s\n", path.string());
        return nullptr;
    }
    if (pos.nPos) {
        if (fseek(file, pos.nPos, SEEK_SET)) {
            LogPrintf("Unable to seek to position %u of %s\n", pos.nPos, path.string());
            fclose(file);
            return nullptr;
        }
    }
    return file;
}

FILE* OpenBlockFile(const CDiskBlockPos& pos, bool fReadOnly)
{
    return OpenDiskFile(pos, "blk", fReadOnly);
}

/** Open an undo file (rev?????.dat) */
static FILE* OpenUndoFile(const CDiskBlockPos& pos, bool fReadOnly)
{
    return OpenDiskFile(pos, "rev", fReadOnly);
}

fs::path GetBlockPosFilename(const CDiskBlockPos& pos, const char* prefix)
{
    return GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, pos.nFile);
}

CBlockIndex* InsertBlockIndex(uint256 hash)
{
    if (hash.IsNull())
        return nullptr;

    // Return existing
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw std::runtime_error(std::string(__func__) + ": new CBlockIndex failed");
    mi = mapBlockIndex.insert(std::make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

bool static LoadBlockIndexDB(const CChainParams& chainparams)
{
    if (!pblocktree->LoadBlockIndexGuts(chainparams.GetConsensus(), InsertBlockIndex))
        return false;

    boost::this_thread::interruption_point();

    // Calculate nChainWork
    std::vector<std::pair<int, CBlockIndex*>> vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    for (const std::pair<uint256, CBlockIndex*>& item : mapBlockIndex) {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(std::make_pair(pindex->nHeight, pindex));

        // build mapPrevBlockIndex
        if (pindex->pprev) {
            mapPrevBlockIndex.emplace(pindex->pprev->GetBlockHash(), pindex);
        }
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    for (const std::pair<int, CBlockIndex*>& item : vSortedByHeight) {
        CBlockIndex* pindex = item.second;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + GetBlockProof(*pindex);
        pindex->nTimeMax = (pindex->pprev ? std::max(pindex->pprev->nTimeMax, pindex->nTime) : pindex->nTime);
        // We can link the chain of blocks for which we've received transactions at some point.
        // Pruned nodes may have deleted the block.
        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx) {
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                } else {
                    pindex->nChainTx = 0;
                    mapBlocksUnlinked.insert(std::make_pair(pindex->pprev, pindex));
                }
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }
        if (!(pindex->nStatus & BLOCK_FAILED_MASK) && pindex->pprev && (pindex->pprev->nStatus & BLOCK_FAILED_MASK)) {
            pindex->nStatus |= BLOCK_FAILED_CHILD;
            setDirtyBlockIndex.insert(pindex);
        }
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && (pindex->nChainTx || pindex->pprev == nullptr))
            setBlockIndexCandidates.insert(pindex);
        if (pindex->nStatus & BLOCK_FAILED_MASK && (!pindexBestInvalid || pindex->nChainWork > pindexBestInvalid->nChainWork))
            pindexBestInvalid = pindex;
        if (pindex->pprev)
            pindex->BuildSkip();
        if (pindex->IsValid(BLOCK_VALID_TREE) && (pindexBestHeader == nullptr || CBlockIndexWorkComparator()(pindexBestHeader, pindex)))
            pindexBestHeader = pindex;
    }

    // Load block file info
    pblocktree->ReadLastBlockFile(nLastBlockFile);
    vinfoBlockFile.resize(nLastBlockFile + 1);
    LogPrintf("%s: last block file = %i\n", __func__, nLastBlockFile);
    for (int nFile = 0; nFile <= nLastBlockFile; nFile++) {
        pblocktree->ReadBlockFileInfo(nFile, vinfoBlockFile[nFile]);
    }
    LogPrintf("%s: last block file info: %s\n", __func__, vinfoBlockFile[nLastBlockFile].ToString());
    for (int nFile = nLastBlockFile + 1; true; nFile++) {
        CBlockFileInfo info;
        if (pblocktree->ReadBlockFileInfo(nFile, info)) {
            vinfoBlockFile.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    std::set<int> setBlkDataFiles;
    for (const std::pair<uint256, CBlockIndex*>& item : mapBlockIndex) {
        CBlockIndex* pindex = item.second;
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(pindex->nFile);
        }
    }
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++) {
        CDiskBlockPos pos(*it, 0);
        if (CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION).IsNull()) {
            return false;
        }
    }

    // Check whether we have ever pruned block & undo files
    pblocktree->ReadFlag("prunedblockfiles", fHavePruned);
    if (fHavePruned)
        LogPrintf("LoadBlockIndexDB(): Block files have previously been pruned\n");

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    pblocktree->ReadReindexing(fReindexing);
    fReindex |= fReindexing;

    // Check whether we have a transaction index
    pblocktree->ReadFlag("txindex", fTxIndex);
    LogPrintf("%s: transaction index %s\n", __func__, fTxIndex ? "enabled" : "disabled");

    // Check whether we have an asset index
    pblocktree->ReadFlag("assetindex", fAssetIndex);
    LogPrintf("%s: asset index %s\n", __func__, fAssetIndex ? "enabled" : "disabled");

    // Check whether we have an address index
    pblocktree->ReadFlag("addressindex", fAddressIndex);
    LogPrintf("%s: address index %s\n", __func__, fAddressIndex ? "enabled" : "disabled");

    // Check whether we have a timestamp index
    pblocktree->ReadFlag("timestampindex", fTimestampIndex);
    LogPrintf("%s: timestamp index %s\n", __func__, fTimestampIndex ? "enabled" : "disabled");

    // Check whether we have a spent index
    pblocktree->ReadFlag("spentindex", fSpentIndex);
    LogPrintf("%s: spent index %s\n", __func__, fSpentIndex ? "enabled" : "disabled");

    return true;
}

bool LoadChainTip(const CChainParams& chainparams)
{
    if (chainActive.Tip() && chainActive.Tip()->GetBlockHash() == pcoinsTip->GetBestBlock()) return true;

    if (pcoinsTip->GetBestBlock().IsNull() && mapBlockIndex.size() == 1) {
        // In case we just added the genesis block, connect it now, so
        // that we always have a chainActive.Tip() when we return.
        LogPrintf("%s: Connecting genesis block...\n", __func__);
        CValidationState state;
        if (!ActivateBestChain(state, chainparams)) {
            return false;
        }
    }

    // Load pointer to end of best chain
    BlockMap::iterator it = mapBlockIndex.find(pcoinsTip->GetBestBlock());
    if (it == mapBlockIndex.end())
        return false;
    chainActive.SetTip(it->second);

    PruneBlockIndexCandidates();

    LogPrintf("Loaded best chain: hashBestChain=%s height=%d date=%s progress=%f\n",
        chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(),
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()),
        GuessVerificationProgress(chainparams.TxData(), chainActive.Tip()));
    return true;
}

CVerifyDB::CVerifyDB()
{
    uiInterface.ShowProgress(_("Verifying blocks..."), 0);
}

CVerifyDB::~CVerifyDB()
{
    uiInterface.ShowProgress("", 100);
}

bool CVerifyDB::VerifyDB(const CChainParams& chainparams, CCoinsView* coinsview, int nCheckLevel, int nCheckDepth)
{
    LOCK(cs_main);
    if (chainActive.Tip() == nullptr || chainActive.Tip()->pprev == nullptr)
        return true;

    // begin tx and let it rollback
    auto dbTx = evoDb->BeginTransaction();

    // Verify blocks in the best chain
    if (nCheckDepth <= 0 || nCheckDepth > chainActive.Height())
        nCheckDepth = chainActive.Height();
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    LogPrintf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
    CCoinsViewCache coins(coinsview);
    CBlockIndex* pindexState = chainActive.Tip();
    CBlockIndex* pindexFailure = nullptr;
    int nGoodTransactions = 0;
    CValidationState state;
    int reportDone = 0;
    auto currentActiveAssetCache = GetCurrentAssetCache();
    CAssetsCache assetCache(*currentActiveAssetCache);
    LogPrintf("[0%%]...");
    for (CBlockIndex* pindex = chainActive.Tip(); pindex && pindex->pprev; pindex = pindex->pprev) {
        boost::this_thread::interruption_point();
        int percentageDone = std::max(1, std::min(99, (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * (nCheckLevel >= 4 ? 50 : 100))));
        if (reportDone < percentageDone / 10) {
            // report every 10% step
            LogPrintf("[%d%%]...", percentageDone);
            reportDone = percentageDone / 10;
        }
        uiInterface.ShowProgress(_("Verifying blocks..."), percentageDone);
        if (pindex->nHeight < chainActive.Height() - nCheckDepth)
            break;
        if (fPruneMode && !(pindex->nStatus & BLOCK_HAVE_DATA)) {
            // If pruning, only go back as far as we have data.
            LogPrintf("VerifyDB(): block verification stopping at height %d (pruning, no data)\n", pindex->nHeight);
            break;
        }
        CBlock block;
        // check level 0: read from disk
        if (!ReadBlockFromDisk(block, pindex, chainparams.GetConsensus()))
            return error("VerifyDB(): *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
        // check level 1: verify block validity
        if (nCheckLevel >= 1 && !CheckBlock(block, state, chainparams.GetConsensus(), pindex->nHeight))
            return error("%s: *** found bad block at %d, hash=%s (%s)\n", __func__,
                pindex->nHeight, pindex->GetBlockHash().ToString(), FormatStateMessage(state));
        // check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            CDiskBlockPos pos = pindex->GetUndoPos();
            if (!pos.IsNull()) {
                if (!UndoReadFromDisk(undo, pos, pindex->pprev->GetBlockHash()))
                    return error("VerifyDB(): *** found bad undo data at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
            }
        }
        // check level 3: check for inconsistencies during memory-only disconnect of tip blocks
        if (nCheckLevel >= 3 && pindex == pindexState && (coins.DynamicMemoryUsage() + pcoinsTip->DynamicMemoryUsage()) <= nCoinCacheUsage) {
            assert(coins.GetBestBlock() == pindex->GetBlockHash());
            DisconnectResult res = DisconnectBlock(block, pindex, coins, &assetCache, true, false);
            if (res == DISCONNECT_FAILED) {
                return error("VerifyDB(): *** irrecoverable inconsistency in block data at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            }
            pindexState = pindex->pprev;
            if (res == DISCONNECT_UNCLEAN) {
                nGoodTransactions = 0;
                pindexFailure = pindex;
            } else {
                nGoodTransactions += block.vtx.size();
            }
        }

        //        CDeterministicMNList mnList = deterministicMNManager->GetListForBlock(pindex);
        //        std::cout << "UpdateLLMQParams at height " << pindex->nHeight << " listSize "<< mnList.GetAllMNsCount() << endl;
        //        UpdateLLMQParams(mnList.GetAllMNsCount(), pindex->nHeight);
        if (ShutdownRequested())
            return true;
    }
    if (pindexFailure)
        return error("VerifyDB(): *** coin database inconsistencies found (last %i blocks, %i good transactions before that)\n", chainActive.Height() - pindexFailure->nHeight + 1, nGoodTransactions);

    // check level 4: try reconnecting blocks
    if (nCheckLevel >= 4) {
        CBlockIndex* pindex = pindexState;
        while (pindex != chainActive.Tip()) {
            boost::this_thread::interruption_point();
            uiInterface.ShowProgress(_("Verifying blocks..."), std::max(1, std::min(99, 100 - (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * 50))));
            pindex = chainActive.Next(pindex);
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex, chainparams.GetConsensus()))
                return error("VerifyDB(): *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            if (!ConnectBlock(block, state, pindex, coins, chainparams, &assetCache))
                return error("VerifyDB(): *** found unconnectable block at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
        }
    }

    LogPrintf("[DONE].\n");
    LogPrintf("No coin database inconsistencies in last %i blocks (%i transactions)\n", chainActive.Height() - pindexState->nHeight, nGoodTransactions);

    return true;
}

/** Apply the effects of a block on the utxo cache, ignoring that it may already have been applied. */
static bool RollforwardBlock(const CBlockIndex* pindex, CCoinsViewCache& inputs, const CChainParams& params, CAssetsCache* assetsCache = nullptr)
{
    // TODO: merge with ConnectBlock
    CBlock block;
    if (!ReadBlockFromDisk(block, pindex, params.GetConsensus())) {
        return error("ReplayBlock(): ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
    }

    // MUST process special txes before updating UTXO to ensure consistency between mempool and block processing
    CValidationState state;
    if (!ProcessSpecialTxsInBlock(block, pindex, state, false /*fJustCheck*/, false /*fScriptChecks*/)) {
        return error("RollforwardBlock(MMM): ProcessSpecialTxsInBlock for block %s failed with %s",
            pindex->GetBlockHash().ToString(), FormatStateMessage(state));
    }

    for (const CTransactionRef& tx : block.vtx) {
        if (!tx->IsCoinBase()) {
            for (const CTxIn& txin : tx->vin) {
                inputs.SpendCoin(txin.prevout, nullptr, assetsCache);
            }
        }
        // Pass check = true as every addition may be an overwrite.
        AddCoins(inputs, *tx, pindex->nHeight, pindex->GetBlockHash(), true, assetsCache);
    }
    return true;
}

bool ReplayBlocks(const CChainParams& params, CCoinsView* view)
{
    LOCK(cs_main);

    CCoinsViewCache cache(view);
    auto currentActiveAssetCache = GetCurrentAssetCache();
    CAssetsCache assetsCache(*currentActiveAssetCache);

    std::vector<uint256> hashHeads = view->GetHeadBlocks();
    if (hashHeads.empty()) return true; // We're already in a consistent state.
    if (hashHeads.size() != 2) return error("ReplayBlocks(): unknown inconsistent state");

    uiInterface.ShowProgress(_("Replaying blocks..."), 0);
    LogPrintf("Replaying blocks\n");

    const CBlockIndex* pindexOld = nullptr;  // Old tip during the interrupted flush.
    const CBlockIndex* pindexNew;            // New tip during the interrupted flush.
    const CBlockIndex* pindexFork = nullptr; // Latest block common to both the old and the new tip.

    if (mapBlockIndex.count(hashHeads[0]) == 0) {
        return error("ReplayBlocks(): reorganization to unknown block requested");
    }
    pindexNew = mapBlockIndex[hashHeads[0]];

    if (!hashHeads[1].IsNull()) { // The old tip is allowed to be 0, indicating it's the first flush.
        if (mapBlockIndex.count(hashHeads[1]) == 0) {
            return error("ReplayBlocks(): reorganization from unknown block requested");
        }
        pindexOld = mapBlockIndex[hashHeads[1]];
        pindexFork = LastCommonAncestor(pindexOld, pindexNew);
        assert(pindexFork != nullptr);
    }

    // Rollback along the old branch.
    while (pindexOld != pindexFork) {
        if (pindexOld->nHeight > 0) { // Never disconnect the genesis block.
            CBlock block;
            if (!ReadBlockFromDisk(block, pindexOld, params.GetConsensus())) {
                return error("RollbackBlock(): ReadBlockFromDisk() failed at %d, hash=%s", pindexOld->nHeight, pindexOld->GetBlockHash().ToString());
            }
            LogPrintf("Rolling back %s (%i)\n", pindexOld->GetBlockHash().ToString(), pindexOld->nHeight);
            DisconnectResult res = DisconnectBlock(block, pindexOld, cache, &assetsCache);
            if (res == DISCONNECT_FAILED) {
                return error("RollbackBlock(): DisconnectBlock failed at %d, hash=%s", pindexOld->nHeight, pindexOld->GetBlockHash().ToString());
            }
            // If DISCONNECT_UNCLEAN is returned, it means a non-existing UTXO was deleted, or an existing UTXO was
            // overwritten. It corresponds to cases where the block-to-be-disconnect never had all its operations
            // applied to the UTXO set. However, as both writing a UTXO and deleting a UTXO are idempotent operations,
            // the result is still a version of the UTXO set with the effects of that block undone.
        }
        pindexOld = pindexOld->pprev;
    }

    // Roll forward from the forking point to the new tip.
    int nForkHeight = pindexFork ? pindexFork->nHeight : 0;
    for (int nHeight = nForkHeight + 1; nHeight <= pindexNew->nHeight; ++nHeight) {
        const CBlockIndex* pindex = pindexNew->GetAncestor(nHeight);
        LogPrintf("Rolling forward %s (%i)\n", pindex->GetBlockHash().ToString(), nHeight);
        if (!RollforwardBlock(pindex, cache, params)) return false;
    }

    cache.SetBestBlock(pindexNew->GetBlockHash());
    cache.Flush();
    assetsCache.Flush();
    uiInterface.ShowProgress("", 100);
    return true;
}


// May NOT be used after any connections are up as much
// of the peer-processing logic assumes a consistent
// block index state
void UnloadBlockIndex()
{
    LOCK(cs_main);
    setBlockIndexCandidates.clear();
    chainActive.SetTip(nullptr);
    pindexBestInvalid = nullptr;
    pindexBestHeader = nullptr;
    mempool.clear();
    mapBlocksUnlinked.clear();
    vinfoBlockFile.clear();
    nLastBlockFile = 0;
    nBlockSequenceId = 1;
    setDirtyBlockIndex.clear();
    g_failed_blocks.clear();
    setDirtyFileInfo.clear();
    versionbitscache.Clear();
    for (int b = 0; b < VERSIONBITS_NUM_BITS; b++) {
        warningcache[b].clear();
    }

    for (BlockMap::value_type& entry : mapBlockIndex) {
        delete entry.second;
    }
    mapBlockIndex.clear();
    fHavePruned = false;
}

bool LoadBlockIndex(const CChainParams& chainparams)
{
    // Load block index from databases
    bool needs_init = fReindex;
    if (!fReindex) {
        bool ret = LoadBlockIndexDB(chainparams);
        if (!ret) return false;
        needs_init = mapBlockIndex.empty();
    }

    if (needs_init) {
        // Everything here is for *new* reindex/DBs. Thus, though
        // LoadBlockIndexDB may have set fReindex if we shut down
        // mid-reindex previously, we don't check fReindex and
        // instead only check it prior to LoadBlockIndexDB to set
        // needs_init.

        LogPrintf("Initializing databases...\n");
        // Use the provided setting for -txindex in the new database
        fTxIndex = gArgs.GetBoolArg("-txindex", DEFAULT_TXINDEX);
        pblocktree->WriteFlag("txindex", fTxIndex);

        // Use the provided setting for -assetindex in the new database
        fAssetIndex = gArgs.GetBoolArg("-assetindex", DEFAULT_ASSETINDEX);
        pblocktree->WriteFlag("assetindex", fAssetIndex);

        // Use the provided setting for -addressindex in the new database
        fAddressIndex = gArgs.GetBoolArg("-addressindex", DEFAULT_ADDRESSINDEX);
        pblocktree->WriteFlag("addressindex", fAddressIndex);

        // Use the provided setting for -timestampindex in the new database
        fTimestampIndex = gArgs.GetBoolArg("-timestampindex", DEFAULT_TIMESTAMPINDEX);
        pblocktree->WriteFlag("timestampindex", fTimestampIndex);

        // Use the provided setting for -spentindex in the new database
        fSpentIndex = gArgs.GetBoolArg("-spentindex", DEFAULT_SPENTINDEX);
        pblocktree->WriteFlag("spentindex", fSpentIndex);
    }
    return true;
}

static bool AddGenesisBlock(const CChainParams& chainparams, const CBlock& block, CValidationState& state)
{
    // Start new block file
    unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
    CDiskBlockPos blockPos;
    if (!FindBlockPos(state, blockPos, nBlockSize + 8, 0, block.GetBlockTime()))
        return error("%s: FindBlockPos failed", __func__);
    if (!WriteBlockToDisk(block, blockPos, chainparams.MessageStart()))
        return error("%s: writing genesis block to disk failed", __func__);
    CBlockIndex* pindex = AddToBlockIndex(block);
    if (!ReceivedBlockTransactions(block, state, pindex, blockPos))
        return error("%s: genesis block not accepted", __func__);
    return true;
}

bool LoadGenesisBlock(const CChainParams& chainparams)
{
    LOCK(cs_main);

    // Check whether we're already initialized by checking for genesis in
    // mapBlockIndex. Note that we can't use chainActive here, since it is
    // set based on the coins db, not the block index db, which is the only
    // thing loaded at this point.
    if (mapBlockIndex.count(chainparams.GenesisBlock().GetHash()))
        return true;

    try {
        CValidationState state;

        if (!AddGenesisBlock(chainparams, chainparams.GenesisBlock(), state))
            return false;

        if (chainparams.NetworkIDString() == CBaseChainParams::DEVNET) {
            // We can't continue if devnet genesis block is invalid
            std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(
                chainparams.DevNetGenesisBlock());
            // skip founder check
            bool fCheckBlock = CheckBlock(*shared_pblock, state, chainparams.GetConsensus(), 0);
            assert(fCheckBlock);
            if (!AcceptBlock(shared_pblock, state, chainparams, nullptr, true, nullptr, nullptr))
                return false;
        }
    } catch (const std::runtime_error& e) {
        return error("%s: failed to initialize block database: %s", __func__, e.what());
    }

    return true;
}

bool LoadExternalBlockFile(const CChainParams& chainparams, FILE* fileIn, CDiskBlockPos* dbp)
{
    // Map of disk positions for blocks with unknown parent (only used for reindex)
    static std::multimap<uint256, CDiskBlockPos> mapBlocksUnknownParent;
    int64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    try {
        unsigned int nMaxBlockSize = MaxBlockSize(true);
        // This takes over fileIn and calls fclose() on it in the CBufferedFile destructor
        CBufferedFile blkdat(fileIn, 2 * nMaxBlockSize, nMaxBlockSize + 8, SER_DISK, CLIENT_VERSION);
        uint64_t nRewind = blkdat.GetPos();
        while (!blkdat.eof()) {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            nRewind++;         // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try {
                // locate a header
                unsigned char buf[CMessageHeader::MESSAGE_START_SIZE];
                blkdat.FindByte(chainparams.MessageStart()[0]);
                nRewind = blkdat.GetPos() + 1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, chainparams.MessageStart(), CMessageHeader::MESSAGE_START_SIZE))
                    continue;
                // read size
                blkdat >> nSize;
                if (nSize < 80 || nSize > nMaxBlockSize)
                    continue;
            } catch (const std::exception&) {
                // no valid block header found; don't complain
                break;
            }
            try {
                // read block
                uint64_t nBlockPos = blkdat.GetPos();
                if (dbp)
                    dbp->nPos = nBlockPos;
                blkdat.SetLimit(nBlockPos + nSize);
                blkdat.SetPos(nBlockPos);
                std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
                CBlock& block = *pblock;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // detect out of order blocks, and store them for later
                uint256 hash = block.GetHash();
                {
                    LOCK(cs_main);
                    if (hash != chainparams.GetConsensus().hashGenesisBlock && mapBlockIndex.find(block.hashPrevBlock) == mapBlockIndex.end()) {
                        LogPrint(BCLog::REINDEX, "%s: Out of order block %s, parent %s not known\n", __func__, hash.ToString(),
                            block.hashPrevBlock.ToString());
                        if (dbp)
                            mapBlocksUnknownParent.insert(std::make_pair(block.hashPrevBlock, *dbp));
                        continue;
                    }

                    // process in case the block isn't known yet
                    if (mapBlockIndex.count(hash) == 0 || (mapBlockIndex[hash]->nStatus & BLOCK_HAVE_DATA) == 0) {
                        CValidationState state;
                        if (AcceptBlock(pblock, state, chainparams, nullptr, true, dbp, nullptr))
                            nLoaded++;
                        if (state.IsError())
                            break;
                    } else if (hash != chainparams.GetConsensus().hashGenesisBlock && mapBlockIndex[hash]->nHeight % 1000 == 0) {
                        LogPrint(BCLog::REINDEX, "Block Import: already had block %s at height %d\n", hash.ToString(), mapBlockIndex[hash]->nHeight);
                    }
                }

                // Activate the genesis block so normal node progress can continue
                if (hash == chainparams.GetConsensus().hashGenesisBlock) {
                    CValidationState state;
                    if (!ActivateBestChain(state, chainparams)) {
                        break;
                    }
                }

                NotifyHeaderTip();

                // Recursively process earlier encountered successors of this block
                std::deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty()) {
                    uint256 head = queue.front();
                    queue.pop_front();
                    std::pair<std::multimap<uint256, CDiskBlockPos>::iterator, std::multimap<uint256, CDiskBlockPos>::iterator> range = mapBlocksUnknownParent.equal_range(head);
                    while (range.first != range.second) {
                        std::multimap<uint256, CDiskBlockPos>::iterator it = range.first;
                        std::shared_ptr<CBlock> pblockrecursive = std::make_shared<CBlock>();
                        if (ReadBlockFromDisk(*pblockrecursive, it->second, chainparams.GetConsensus())) {
                            LogPrint(BCLog::REINDEX, "%s: Processing out of order child %s of %s\n", __func__, pblockrecursive->GetHash().ToString(),
                                head.ToString());
                            LOCK(cs_main);
                            CValidationState dummy;
                            if (AcceptBlock(pblockrecursive, dummy, chainparams, nullptr, true, &it->second, nullptr)) {
                                nLoaded++;
                                queue.push_back(pblockrecursive->GetHash());
                            }
                        }
                        range.first++;
                        mapBlocksUnknownParent.erase(it);
                        NotifyHeaderTip();
                    }
                }
            } catch (const std::exception& e) {
                LogPrintf("%s: Deserialize or I/O error - %s\n", __func__, e.what());
            }
        }
    } catch (const std::runtime_error& e) {
        AbortNode(std::string("System error: ") + e.what());
    }
    if (nLoaded > 0)
        LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

void static CheckBlockIndex(const Consensus::Params& consensusParams)
{
    if (!fCheckBlockIndex) {
        return;
    }

    LOCK(cs_main);

    // During a reindex, we read the genesis block and call CheckBlockIndex before ActivateBestChain,
    // so we have the genesis block in mapBlockIndex but no active chain.  (A few of the tests when
    // iterating the block tree require that chainActive has been initialized.)
    if (chainActive.Height() < 0) {
        assert(mapBlockIndex.size() <= 1);
        return;
    }

    // Build forward-pointing map of the entire block tree.
    std::multimap<CBlockIndex*, CBlockIndex*> forward;
    for (BlockMap::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); it++) {
        forward.insert(std::make_pair(it->second->pprev, it->second));
    }

    assert(forward.size() == mapBlockIndex.size());

    std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> rangeGenesis = forward.equal_range(nullptr);
    CBlockIndex* pindex = rangeGenesis.first->second;
    rangeGenesis.first++;
    assert(rangeGenesis.first == rangeGenesis.second); // There is only one index entry with parent nullptr.

    // Iterate over the entire block tree, using depth-first search.
    // Along the way, remember whether there are blocks on the path from genesis
    // block being explored which are the first to have certain properties.
    size_t nNodes = 0;
    int nHeight = 0;
    CBlockIndex* pindexFirstInvalid = nullptr;              // Oldest ancestor of pindex which is invalid.
    CBlockIndex* pindexFirstMissing = nullptr;              // Oldest ancestor of pindex which does not have BLOCK_HAVE_DATA.
    CBlockIndex* pindexFirstNeverProcessed = nullptr;       // Oldest ancestor of pindex for which nTx == 0.
    CBlockIndex* pindexFirstNotTreeValid = nullptr;         // Oldest ancestor of pindex which does not have BLOCK_VALID_TREE (regardless of being valid or not).
    CBlockIndex* pindexFirstNotTransactionsValid = nullptr; // Oldest ancestor of pindex which does not have BLOCK_VALID_TRANSACTIONS (regardless of being valid or not).
    CBlockIndex* pindexFirstNotChainValid = nullptr;        // Oldest ancestor of pindex which does not have BLOCK_VALID_CHAIN (regardless of being valid or not).
    CBlockIndex* pindexFirstNotScriptsValid = nullptr;      // Oldest ancestor of pindex which does not have BLOCK_VALID_SCRIPTS (regardless of being valid or not).
    while (pindex != nullptr) {
        nNodes++;
        if (pindexFirstInvalid == nullptr && pindex->nStatus & BLOCK_FAILED_VALID) pindexFirstInvalid = pindex;
        if (pindexFirstMissing == nullptr && !(pindex->nStatus & BLOCK_HAVE_DATA)) pindexFirstMissing = pindex;
        if (pindexFirstNeverProcessed == nullptr && pindex->nTx == 0) pindexFirstNeverProcessed = pindex;
        if (pindex->pprev != nullptr && pindexFirstNotTreeValid == nullptr && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE) pindexFirstNotTreeValid = pindex;
        if (pindex->pprev != nullptr && pindexFirstNotTransactionsValid == nullptr && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TRANSACTIONS) pindexFirstNotTransactionsValid = pindex;
        if (pindex->pprev != nullptr && pindexFirstNotChainValid == nullptr && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_CHAIN) pindexFirstNotChainValid = pindex;
        if (pindex->pprev != nullptr && pindexFirstNotScriptsValid == nullptr && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS) pindexFirstNotScriptsValid = pindex;

        // Begin: actual consistency checks.
        if (pindex->pprev == nullptr) {
            // Genesis block checks.
            assert(pindex->GetBlockHash() == consensusParams.hashGenesisBlock); // Genesis block's hash must match.
            assert(pindex == chainActive.Genesis());                            // The current active chain's genesis block must be this block.
        }
        if (pindex->nChainTx == 0) assert(pindex->nSequenceId <= 0); // nSequenceId can't be set positive for blocks that aren't linked (negative is used for preciousblock)
        // VALID_TRANSACTIONS is equivalent to nTx > 0 for all nodes (whether or not pruning has occurred).
        // HAVE_DATA is only equivalent to nTx > 0 (or VALID_TRANSACTIONS) if no pruning has occurred.
        if (!fHavePruned) {
            // If we've never pruned, then HAVE_DATA should be equivalent to nTx > 0
            assert(!(pindex->nStatus & BLOCK_HAVE_DATA) == (pindex->nTx == 0));
            assert(pindexFirstMissing == pindexFirstNeverProcessed);
        } else {
            // If we have pruned, then we can only say that HAVE_DATA implies nTx > 0
            if (pindex->nStatus & BLOCK_HAVE_DATA) assert(pindex->nTx > 0);
        }
        if (pindex->nStatus & BLOCK_HAVE_UNDO) assert(pindex->nStatus & BLOCK_HAVE_DATA);
        assert(((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TRANSACTIONS) == (pindex->nTx > 0)); // This is pruning-independent.
        // All parents having had data (at some point) is equivalent to all parents being VALID_TRANSACTIONS, which is equivalent to nChainTx being set.
        assert((pindexFirstNeverProcessed != nullptr) == (pindex->nChainTx == 0)); // nChainTx != 0 is used to signal that all parent blocks have been processed (but may have been pruned).
        assert((pindexFirstNotTransactionsValid != nullptr) == (pindex->nChainTx == 0));
        assert(pindex->nHeight == nHeight);                                                                             // nHeight must be consistent.
        assert(pindex->pprev == nullptr || pindex->nChainWork >= pindex->pprev->nChainWork);                            // For every block except the genesis block, the chainwork must be larger than the parent's.
        assert(nHeight < 2 || (pindex->pskip && (pindex->pskip->nHeight < nHeight)));                                   // The pskip pointer must point back for all but the first 2 blocks.
        assert(pindexFirstNotTreeValid == nullptr);                                                                     // All mapBlockIndex entries must at least be TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TREE) assert(pindexFirstNotTreeValid == nullptr);       // TREE valid implies all parents are TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_CHAIN) assert(pindexFirstNotChainValid == nullptr);     // CHAIN valid implies all parents are CHAIN valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_SCRIPTS) assert(pindexFirstNotScriptsValid == nullptr); // SCRIPTS valid implies all parents are SCRIPTS valid
        if (pindexFirstInvalid == nullptr) {
            // Checks for not-invalid blocks.
            assert((pindex->nStatus & BLOCK_FAILED_MASK) == 0); // The failed mask cannot be set for blocks without invalid parents.
        }
        if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) && pindexFirstNeverProcessed == nullptr) {
            if (pindexFirstInvalid == nullptr) {
                // If this block sorts at least as good as the current tip and
                // is valid and we have all data for its parents, it must be in
                // setBlockIndexCandidates.  chainActive.Tip() must also be there
                // even if some data has been pruned.
                if (pindexFirstMissing == nullptr || pindex == chainActive.Tip()) {
                    assert(setBlockIndexCandidates.count(pindex));
                }
                // If some parent is missing, then it could be that this block was in
                // setBlockIndexCandidates but had to be removed because of the missing data.
                // In this case it must be in mapBlocksUnlinked -- see test below.
            }
        } else { // If this block sorts worse than the current tip or some ancestor's block has never been seen, it cannot be in setBlockIndexCandidates.
            assert(setBlockIndexCandidates.count(pindex) == 0);
        }
        // Check whether this block is in mapBlocksUnlinked.
        std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> rangeUnlinked = mapBlocksUnlinked.equal_range(pindex->pprev);
        bool foundInUnlinked = false;
        while (rangeUnlinked.first != rangeUnlinked.second) {
            assert(rangeUnlinked.first->first == pindex->pprev);
            if (rangeUnlinked.first->second == pindex) {
                foundInUnlinked = true;
                break;
            }
            rangeUnlinked.first++;
        }
        if (pindex->pprev && (pindex->nStatus & BLOCK_HAVE_DATA) && pindexFirstNeverProcessed != nullptr && pindexFirstInvalid == nullptr) {
            // If this block has block data available, some parent was never received, and has no invalid parents, it must be in mapBlocksUnlinked.
            assert(foundInUnlinked);
        }
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) assert(!foundInUnlinked); // Can't be in mapBlocksUnlinked if we don't HAVE_DATA
        if (pindexFirstMissing == nullptr) assert(!foundInUnlinked);        // We aren't missing data for any parent -- cannot be in mapBlocksUnlinked.
        if (pindex->pprev && (pindex->nStatus & BLOCK_HAVE_DATA) && pindexFirstNeverProcessed == nullptr && pindexFirstMissing != nullptr) {
            // We HAVE_DATA for this block, have received data for all parents at some point, but we're currently missing data for some parent.
            assert(fHavePruned); // We must have pruned.
            // This block may have entered mapBlocksUnlinked if:
            //  - it has a descendant that at some point had more work than the
            //    tip, and
            //  - we tried switching to that descendant but were missing
            //    data for some intermediate block between chainActive and the
            //    tip.
            // So if this block is itself better than chainActive.Tip() and it wasn't in
            // setBlockIndexCandidates, then it must be in mapBlocksUnlinked.
            if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) && setBlockIndexCandidates.count(pindex) == 0) {
                if (pindexFirstInvalid == nullptr) {
                    assert(foundInUnlinked);
                }
            }
        }
        // assert(pindex->GetBlockHash() == pindex->GetBlockHeader().GetHash()); // Perhaps too slow
        // End: actual consistency checks.

        // Try descending into the first subnode.
        std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = forward.equal_range(pindex);
        if (range.first != range.second) {
            // A subnode was found.
            pindex = range.first->second;
            nHeight++;
            continue;
        }
        // This is a leaf node.
        // Move upwards until we reach a node of which we have not yet visited the last child.
        while (pindex) {
            // We are going to either move to a parent or a sibling of pindex.
            // If pindex was the first with a certain property, unset the corresponding variable.
            if (pindex == pindexFirstInvalid) pindexFirstInvalid = nullptr;
            if (pindex == pindexFirstMissing) pindexFirstMissing = nullptr;
            if (pindex == pindexFirstNeverProcessed) pindexFirstNeverProcessed = nullptr;
            if (pindex == pindexFirstNotTreeValid) pindexFirstNotTreeValid = nullptr;
            if (pindex == pindexFirstNotTransactionsValid) pindexFirstNotTransactionsValid = nullptr;
            if (pindex == pindexFirstNotChainValid) pindexFirstNotChainValid = nullptr;
            if (pindex == pindexFirstNotScriptsValid) pindexFirstNotScriptsValid = nullptr;
            // Find our parent.
            CBlockIndex* pindexPar = pindex->pprev;
            // Find which child we just visited.
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> rangePar = forward.equal_range(pindexPar);
            while (rangePar.first->second != pindex) {
                assert(rangePar.first != rangePar.second); // Our parent must have at least the node we're coming from as child.
                rangePar.first++;
            }
            // Proceed to the next one.
            rangePar.first++;
            if (rangePar.first != rangePar.second) {
                // Move to the sibling.
                pindex = rangePar.first->second;
                break;
            } else {
                // Move up further.
                pindex = pindexPar;
                nHeight--;
                continue;
            }
        }
    }

    // Check that we actually traversed the entire map.
    assert(nNodes == forward.size());
}

std::string CBlockFileInfo::ToString() const
{
    return strprintf("CBlockFileInfo(blocks=%u, size=%u, heights=%u...%u, time=%s...%s)", nBlocks, nSize, nHeightFirst, nHeightLast, DateTimeStrFormat("%Y-%m-%d", nTimeFirst), DateTimeStrFormat("%Y-%m-%d", nTimeLast));
}

CBlockFileInfo* GetBlockFileInfo(size_t n)
{
    return &vinfoBlockFile.at(n);
}

ThresholdState VersionBitsTipState(const Consensus::Params& params, Consensus::DeploymentPos pos)
{
    AssertLockHeld(cs_main);
    return VersionBitsState(chainActive.Tip(), params, pos, versionbitscache);
}

BIP9Stats VersionBitsTipStatistics(const Consensus::Params& params, Consensus::DeploymentPos pos)
{
    LOCK(cs_main);
    return VersionBitsStatistics(chainActive.Tip(), params, pos);
}

int VersionBitsTipStateSinceHeight(const Consensus::Params& params, Consensus::DeploymentPos pos)
{
    LOCK(cs_main);
    return VersionBitsStateSinceHeight(chainActive.Tip(), params, pos, versionbitscache);
}

static const uint64_t MEMPOOL_DUMP_VERSION = 1;

bool LoadMempool(void)
{
    const CChainParams& chainparams = Params();
    int64_t nExpiryTimeout = gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60;
    FILE* filestr = fsbridge::fopen(GetDataDir() / "mempool.dat", "rb");
    CAutoFile file(filestr, SER_DISK, CLIENT_VERSION);
    if (file.IsNull()) {
        LogPrintf("Failed to open mempool file from disk. Continuing anyway.\n");
        return false;
    }

    int64_t count = 0;
    int64_t skipped = 0;
    int64_t failed = 0;
    int64_t nNow = GetTime();

    try {
        uint64_t version;
        file >> version;
        if (version != MEMPOOL_DUMP_VERSION) {
            return false;
        }
        uint64_t num;
        file >> num;
        while (num--) {
            CTransactionRef tx;
            int64_t nTime;
            int64_t nFeeDelta;
            file >> tx;
            file >> nTime;
            file >> nFeeDelta;

            CAmount amountdelta = nFeeDelta;
            if (amountdelta) {
                mempool.PrioritiseTransaction(tx->GetHash(), amountdelta);
            }
            CValidationState state;
            if (nTime + nExpiryTimeout > nNow) {
                LOCK(cs_main);
                AcceptToMemoryPoolWithTime(chainparams, mempool, state, tx, true, nullptr, nTime, false, 0, false);
                if (state.IsValid()) {
                    ++count;
                } else {
                    ++failed;
                }
            } else {
                ++skipped;
            }
            if (ShutdownRequested())
                return false;
        }
        std::map<uint256, CAmount> mapDeltas;
        file >> mapDeltas;

        for (const auto& i : mapDeltas) {
            mempool.PrioritiseTransaction(i.first, i.second);
        }
    } catch (const std::exception& e) {
        LogPrintf("Failed to deserialize mempool data on disk: %s. Continuing anyway.\n", e.what());
        return false;
    }

    LogPrintf("Imported mempool transactions from disk: %i successes, %i failed, %i expired\n", count, failed, skipped);
    return true;
}

void DumpMempool(void)
{
    int64_t start = GetTimeMicros();

    std::map<uint256, CAmount> mapDeltas;
    std::vector<TxMempoolInfo> vinfo;

    {
        LOCK(mempool.cs);
        for (const auto& i : mempool.mapDeltas) {
            mapDeltas[i.first] = i.second;
        }
        vinfo = mempool.infoAll();
    }

    int64_t mid = GetTimeMicros();

    try {
        FILE* filestr = fsbridge::fopen(GetDataDir() / "mempool.dat.new", "wb");
        if (!filestr) {
            return;
        }

        CAutoFile file(filestr, SER_DISK, CLIENT_VERSION);

        uint64_t version = MEMPOOL_DUMP_VERSION;
        file << version;

        file << (uint64_t)vinfo.size();
        for (const auto& i : vinfo) {
            file << *(i.tx);
            file << (int64_t)i.nTime;
            file << (int64_t)i.nFeeDelta;
            mapDeltas.erase(i.tx->GetHash());
        }

        file << mapDeltas;
        FileCommit(file.Get());
        file.fclose();
        RenameOver(GetDataDir() / "mempool.dat.new", GetDataDir() / "mempool.dat");
        int64_t last = GetTimeMicros();
        LogPrintf("Dumped mempool: %gs to copy, %gs to dump\n", (mid - start) * 0.000001, (last - mid) * 0.000001);
    } catch (const std::exception& e) {
        LogPrintf("Failed to dump mempool: %s. Continuing anyway.\n", e.what());
    }
}

//! Guess how far we are in the verification process at the given block index
double GuessVerificationProgress(const ChainTxData& data, CBlockIndex* pindex)
{
    if (pindex == nullptr)
        return 0.0;

    int64_t nNow = time(nullptr);

    double fTxTotal;

    if (pindex->nChainTx <= data.nTxCount) {
        fTxTotal = data.nTxCount + (nNow - data.nTime) * data.dTxRate;
    } else {
        fTxTotal = pindex->nChainTx + (nNow - pindex->GetBlockTime()) * data.dTxRate;
    }

    return pindex->nChainTx / fTxTotal;
}

class CMainCleanup
{
public:
    CMainCleanup() {}
    ~CMainCleanup()
    {
        // block headers
        BlockMap::iterator it1 = mapBlockIndex.begin();
        for (; it1 != mapBlockIndex.end(); it1++)
            delete (*it1).second;
        mapBlockIndex.clear();
    }
} instance_of_cmaincleanup;

bool AreAssetsDeployed()
{
    return Params().IsAssetsActive(chainActive.Tip());
}

CAssetsCache* GetCurrentAssetCache()
{
    return passets;
}
