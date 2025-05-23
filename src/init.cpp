// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2020 The Dash Core developers
// Copyright (c) 2020 The Memeium developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/memeium-config.h"
#endif

#include "init.h"

#include "addrman.h"
#include "amount.h"
#include "assets/assetdb.h"
#include "assets/assets.h"
#include "assets/snapshotrequestdb.h"
#include "base58.h"
#include "chain.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "compat/sanity.h"
#include "consensus/validation.h"
#include "fs.h"
#include "httprpc.h"
#include "httpserver.h"
#include "key.h"
#include "miner.h"
#include "net.h"
#include "net_processing.h"
#include "netbase.h"
#include "policy/feerate.h"
#include "policy/fees.h"
#include "policy/policy.h"
#include "rpc/blockchain.h"
#include "rpc/mining.h"
#include "rpc/register.h"
#include "rpc/server.h"
#include "scheduler.h"
#include "script/sigcache.h"
#include "script/standard.h"
#include "timedata.h"
#include "torcontrol.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validation.h"
#include "validationinterface.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include "dsnotificationinterface.h"
#include "flat-database.h"
#include "governance/governance.h"
#include "smartnode/activesmartnode.h"
#ifdef ENABLE_WALLET
#include "keepass.h"
#endif
#include "messagesigner.h"
#include "netfulfilledman.h"
#include "smartnode/smartnode-meta.h"
#include "smartnode/smartnode-payments.h"
#include "smartnode/smartnode-sync.h"
#include "smartnode/smartnode-utils.h"
#ifdef ENABLE_WALLET
#include "privatesend/privatesend-client.h"
#endif // ENABLE_WALLET
#include "privatesend/privatesend-server.h"
#include "spork.h"
#include "warnings.h"

#include "evo/deterministicmns.h"
#include "llmq/quorums_init.h"

#include <primitives/powcache.h>

#include <memory>
#include <stdint.h>
#include <stdio.h>

#include "bls/bls.h"

#ifndef WIN32
#include <signal.h>
#endif

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/bind.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/thread.hpp>
#include <openssl/crypto.h>

#if ENABLE_ZMQ
#include "zmq/zmqnotificationinterface.h"
#endif

bool fFeeEstimatesInitialized = false;
static const bool DEFAULT_PROXYRANDOMIZE = true;
static const bool DEFAULT_REST_ENABLE = false;
static const bool DEFAULT_DISABLE_SAFEMODE = false;
static const bool DEFAULT_STOPAFTERBLOCKIMPORT = false;


std::unique_ptr<CConnman> g_connman;
std::unique_ptr<PeerLogicValidation> peerLogic;

#if ENABLE_ZMQ
static CZMQNotificationInterface* pzmqNotificationInterface = nullptr;
#endif

static CDSNotificationInterface* pdsNotificationInterface = nullptr;

#ifdef WIN32
// Win32 LevelDB doesn't use filedescriptors, and the ones used for
// accessing block files don't count towards the fd_set size limit
// anyway.
#define MIN_CORE_FILEDESCRIPTORS 0
#else
#define MIN_CORE_FILEDESCRIPTORS 150
#endif

static const char* FEE_ESTIMATES_FILENAME = "fee_estimates.dat";

//////////////////////////////////////////////////////////////////////////////
//
// Shutdown
//

//
// Thread management and startup/shutdown:
//
// The network-processing threads are all part of a thread group
// created by AppInit() or the Qt main() function.
//
// A clean exit happens when StartShutdown() or the SIGTERM
// signal handler sets fRequestShutdown, which triggers
// the DetectShutdownThread(), which interrupts the main thread group.
// DetectShutdownThread() then exits, which causes AppInit() to
// continue (it .joins the shutdown thread).
// Shutdown() is then
// called to clean up database connections, and stop other
// threads that should only be stopped after the main network-processing
// threads have exited.
//
// Shutdown for Qt is very similar, only it uses a QTimer to detect
// fRequestShutdown getting set, and then does the normal Qt
// shutdown thing.
//

std::atomic<bool> fRequestShutdown(false);
std::atomic<bool> fRequestRestart(false);
std::atomic<bool> fDumpMempoolLater(false);

void StartShutdown()
{
    fRequestShutdown = true;
}
void StartRestart()
{
    fRequestShutdown = fRequestRestart = true;
}
bool ShutdownRequested()
{
    return fRequestShutdown;
}

/**
 * This is a minimally invasive approach to shutdown on LevelDB read errors from the
 * chainstate, while keeping user interface out of the common library, which is shared
 * between bitcoind, and bitcoin-qt and non-server tools.
 */
class CCoinsViewErrorCatcher : public CCoinsViewBacked
{
public:
    CCoinsViewErrorCatcher(CCoinsView* view) :
        CCoinsViewBacked(view) {}
    bool GetCoin(const COutPoint& outpoint, Coin& coin) const override
    {
        try {
            return CCoinsViewBacked::GetCoin(outpoint, coin);
        } catch (const std::runtime_error& e) {
            uiInterface.ThreadSafeMessageBox(_("Error reading from database, shutting down."), "", CClientUIInterface::MSG_ERROR);
            LogPrintf("Error reading from database: %s\n", e.what());
            // Starting the shutdown sequence and returning false to the caller would be
            // interpreted as 'entry not found' (as opposed to unable to read data), and
            // could lead to invalid interpretation. Just exit immediately, as we can't
            // continue anyway, and all writes should be atomic.
            abort();
        }
    }
    // Writes do not need similar protection, as failure to write is handled by the caller.
};

static CCoinsViewErrorCatcher* pcoinscatcher = nullptr;
static std::unique_ptr<ECCVerifyHandle> globalVerifyHandle;

void Interrupt(boost::thread_group& threadGroup)
{
    InterruptHTTPServer();
    InterruptHTTPRPC();
    InterruptRPC();
    InterruptREST();
    InterruptTorControl();
    llmq::InterruptLLMQSystem();
    if (g_connman)
        g_connman->Interrupt();
    threadGroup.interrupt_all();
}

/** Preparing steps before shutting down or restarting the wallet */
void PrepareShutdown()
{
    LogPrintf("%s: In progress...\n", __func__);
    static CCriticalSection cs_Shutdown;
    TRY_LOCK(cs_Shutdown, lockShutdown);
    if (!lockShutdown)
        return;

    /// Note: Shutdown() must be able to handle cases in which initialization failed part of the way,
    /// for example if the data directory was found to be locked.
    /// Be sure that anything that writes files or flushes caches only does this if the respective
    /// module was initialized.
    RenameThread("memeium-shutoff");
    mempool.AddTransactionsUpdated(1);
    StopHTTPRPC();
    StopREST();
    StopRPC();
    StopHTTPServer();
    llmq::StopLLMQSystem();

    // fRPCInWarmup should be `false` if we completed the loading sequence
    // before a shutdown request was received
    std::string statusmessage;
    bool fRPCInWarmup = RPCIsInWarmup(&statusmessage);

#ifdef ENABLE_WALLET
    if (privateSendClient.fEnablePrivateSend && !fRPCInWarmup) {
        // Stop PrivateSend, release keys
        privateSendClient.fPrivateSendRunning = false;
        privateSendClient.ResetPool();
    }
    for (CWalletRef pwallet : vpwallets) {
        pwallet->Flush(false);
    }
#endif
    MapPort(false);

    // Because these depend on each-other, we make sure that neither can be
    // using the other before destroying them.
    UnregisterValidationInterface(peerLogic.get());
    if (g_connman) g_connman->Stop();
    peerLogic.reset();
    g_connman.reset();

    if (!fLiteMode && !fRPCInWarmup) {
        // STORE DATA CACHES INTO SERIALIZED DAT FILES
        CFlatDB<CSmartnodeMetaMan> flatdb1("mncache.dat", "magicSmartnodeCache");
        flatdb1.Dump(mmetaman);
        CFlatDB<CGovernanceManager> flatdb3("governance.dat", "magicGovernanceCache");
        flatdb3.Dump(governance);
        CFlatDB<CNetFulfilledRequestManager> flatdb4("netfulfilled.dat", "magicFulfilledCache");
        flatdb4.Dump(netfulfilledman);
        CFlatDB<CSporkManager> flatdb6("sporks.dat", "magicSporkCache");
        flatdb6.Dump(sporkManager);
        CFlatDB<CPowCache> flatdb7("powcache.dat", "powCache");
        flatdb7.Dump(CPowCache::Instance());
    }

    if (fDumpMempoolLater && gArgs.GetArg("-persistmempool", DEFAULT_PERSIST_MEMPOOL)) {
        DumpMempool();
    }

    if (fFeeEstimatesInitialized) {
        ::feeEstimator.FlushUnconfirmed(::mempool);
        fs::path est_path = GetDataDir() / FEE_ESTIMATES_FILENAME;
        CAutoFile est_fileout(fsbridge::fopen(est_path, "wb"), SER_DISK, CLIENT_VERSION);
        if (!est_fileout.IsNull())
            ::feeEstimator.Write(est_fileout);
        else
            LogPrintf("%s: Failed to write fee estimates to %s\n", __func__, est_path.string());
        fFeeEstimatesInitialized = false;
    }

    // FlushStateToDisk generates a SetBestChain callback, which we should avoid missing
    if (pcoinsTip != nullptr) {
        FlushStateToDisk();
    }

    // After there are no more peers/RPC left to give us new data which may generate
    // CValidationInterface callbacks, flush them...
    GetMainSignals().FlushBackgroundCallbacks();

    // Any future callbacks will be dropped. This should absolutely be safe - if
    // missing a callback results in an unrecoverable situation, unclean shutdown
    // would too. The only reason to do the above flushes is to let the wallet catch
    // up with our current chain to avoid any strange pruning edge cases and make
    // next startup faster by avoiding rescan.

    {
        LOCK(cs_main);
        if (pcoinsTip != nullptr) {
            FlushStateToDisk();
        }
        delete pcoinsTip;
        pcoinsTip = nullptr;
        delete pcoinscatcher;
        pcoinscatcher = nullptr;
        delete pcoinsdbview;
        pcoinsdbview = nullptr;
        delete pblocktree;
        pblocktree = nullptr;

        /** MMM START */
        delete passets;
        passets = nullptr;

        delete passetsdb;
        passetsdb = nullptr;

        delete passetsCache;
        passetsCache = nullptr;

        delete pMessagesCache;
        pMessagesCache = nullptr;

        delete pMessageSubscribedChannelsCache;
        pMessageSubscribedChannelsCache = nullptr;

        delete pMessagesSeenAddressCache;
        pMessagesSeenAddressCache = nullptr;

        delete pmessagechanneldb;
        pmessagechanneldb = nullptr;

        delete pmessagedb;
        pmessagedb = nullptr;

        delete pmyrestricteddb;
        pmyrestricteddb = nullptr;

        delete passetsVerifierCache;
        passetsVerifierCache = nullptr;

        delete passetsQualifierCache;
        passetsQualifierCache = nullptr;

        delete passetsRestrictionCache;
        passetsRestrictionCache = nullptr;

        delete passetsGlobalRestrictionCache;
        passetsGlobalRestrictionCache = nullptr;

        delete prestricteddb;
        prestricteddb = nullptr;

        delete pmessagechanneldb;
        pmessagechanneldb = nullptr;

        delete pSnapshotRequestDb;
        pSnapshotRequestDb = nullptr;

        delete pAssetSnapshotDb;
        pAssetSnapshotDb = nullptr;

        delete pDistributeSnapshotDb;
        pDistributeSnapshotDb = nullptr;

        /** MMM END */
        llmq::DestroyLLMQSystem();
        delete deterministicMNManager;
        deterministicMNManager = nullptr;
        delete evoDb;
        evoDb = nullptr;
    }
#ifdef ENABLE_WALLET
    for (CWalletRef pwallet : vpwallets) {
        pwallet->Flush(true);
    }
#endif

#if ENABLE_ZMQ
    if (pzmqNotificationInterface) {
        UnregisterValidationInterface(pzmqNotificationInterface);
        delete pzmqNotificationInterface;
        pzmqNotificationInterface = nullptr;
    }
#endif

    if (pdsNotificationInterface) {
        UnregisterValidationInterface(pdsNotificationInterface);
        delete pdsNotificationInterface;
        pdsNotificationInterface = nullptr;
    }
    if (fSmartnodeMode) {
        UnregisterValidationInterface(activeSmartnodeManager);
    }

    // make sure to clean up BLS keys before global destructors are called (they have allocated from the secure memory pool)
    activeSmartnodeInfo.blsKeyOperator.reset();
    activeSmartnodeInfo.blsPubKeyOperator.reset();

#ifndef WIN32
    try {
        fs::remove(GetPidFile());
    } catch (const fs::filesystem_error& e) {
        LogPrintf("%s: Unable to remove pidfile: %s\n", __func__, e.what());
    }
#endif
    UnregisterAllValidationInterfaces();
    GetMainSignals().UnregisterBackgroundSignalScheduler();
}

/**
 * Shutdown is split into 2 parts:
 * Part 1: shut down everything but the main wallet instance (done in PrepareShutdown() )
 * Part 2: delete wallet instance
 *
 * In case of a restart PrepareShutdown() was already called before, but this method here gets
 * called implicitly when the parent object is deleted. In this case we have to skip the
 * PrepareShutdown() part because it was already executed and just delete the wallet instance.
 */
void Shutdown()
{
    // Shutdown part 1: prepare shutdown
    if (!fRequestRestart) {
        PrepareShutdown();
    }
    // Shutdown part 2: Stop TOR thread and delete wallet instance
    StopTorControl();
#ifdef ENABLE_WALLET
    for (CWalletRef pwallet : vpwallets) {
        delete pwallet;
    }
    vpwallets.clear();
#endif
    globalVerifyHandle.reset();
    ECC_Stop();
    LogPrintf("%s: done\n", __func__);
}

/**
 * Signal handlers are very limited in what they are allowed to do.
 * The execution context the handler is invoked in is not guaranteed,
 * so we restrict handler operations to just touching variables:
 */
static void HandleSIGTERM(int)
{
    fRequestShutdown = true;
}

static void HandleSIGHUP(int)
{
    fReopenDebugLog = true;
}

#ifndef WIN32
static void registerSignalHandler(int signal, void (*handler)(int))
{
    struct sigaction sa;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(signal, &sa, nullptr);
}
#endif

void OnRPCStarted()
{
    uiInterface.NotifyBlockTip.connect(&RPCNotifyBlockChange);
}

void OnRPCStopped()
{
    uiInterface.NotifyBlockTip.disconnect(&RPCNotifyBlockChange);
    RPCNotifyBlockChange(false, nullptr);
    cvBlockChange.notify_all();
    LogPrint(BCLog::RPC, "RPC stopped.\n");
}

void OnRPCPreCommand(const CRPCCommand& cmd)
{
    // Observe safe mode
    std::string strWarning = GetWarnings("rpc");
    if (strWarning != "" && !gArgs.GetBoolArg("-disablesafemode", DEFAULT_DISABLE_SAFEMODE) &&
        !cmd.okSafeMode)
        throw JSONRPCError(RPC_FORBIDDEN_BY_SAFE_MODE, std::string("Safe mode: ") + strWarning);
}

std::string HelpMessage(HelpMessageMode mode)
{
    const auto defaultBaseParams = CreateBaseChainParams(CBaseChainParams::MAIN);
    const auto testnetBaseParams = CreateBaseChainParams(CBaseChainParams::TESTNET);
    const auto defaultChainParams = CreateChainParams(CBaseChainParams::MAIN);
    const auto testnetChainParams = CreateChainParams(CBaseChainParams::TESTNET);
    const bool showDebug = gArgs.GetBoolArg("-help-debug", false);

    // When adding new options to the categories, please keep and ensure alphabetical ordering.
    // Do not translate _(...) -help-debug options, Many technical terms, and only a very small audience, so is unnecessary stress to translators.
    std::string strUsage = HelpMessageGroup(_("Options:"));
    strUsage += HelpMessageOpt("-?", _("Print this help message and exit"));
    strUsage += HelpMessageOpt("-version", _("Print version and exit"));
    strUsage += HelpMessageOpt("-alertnotify=<cmd>", _("Execute command when a relevant alert is received or we see a really long fork (%s in cmd is replaced by message)"));
    strUsage += HelpMessageOpt("-blocknotify=<cmd>", _("Execute command when the best block changes (%s in cmd is replaced by block hash)"));
    if (showDebug)
        strUsage += HelpMessageOpt("-blocksonly", strprintf(_("Whether to operate in a blocks only mode (default: %u)"), DEFAULT_BLOCKSONLY));
    strUsage += HelpMessageOpt("-assumevalid=<hex>", strprintf(_("If this block is in the chain assume that it and its ancestors are valid and potentially skip their script verification (0 to verify all, default: %s, testnet: %s)"), defaultChainParams->GetConsensus().defaultAssumeValid.GetHex(), testnetChainParams->GetConsensus().defaultAssumeValid.GetHex()));
    strUsage += HelpMessageOpt("-conf=<file>", strprintf(_("Specify configuration file (default: %s)"), BITCOIN_CONF_FILENAME));
    if (mode == HMM_BITCOIND) {
#if HAVE_DECL_DAEMON
        strUsage += HelpMessageOpt("-daemon", _("Run in the background as a daemon and accept commands"));
#endif
    }
    strUsage += HelpMessageOpt("-datadir=<dir>", _("Specify data directory"));
    if (showDebug) {
        strUsage += HelpMessageOpt("-dbbatchsize", strprintf("Maximum database write batch size in bytes (default: %u)", nDefaultDbBatchSize));
    }
    strUsage += HelpMessageOpt("-dbcache=<n>", strprintf(_("Set database cache size in megabytes (%d to %d, default: %d)"), nMinDbCache, nMaxDbCache, nDefaultDbCache));
    strUsage += HelpMessageOpt("-loadblock=<file>", _("Imports blocks from external blk000??.dat file on startup"));
    strUsage += HelpMessageOpt("-maxorphantxsize=<n>", strprintf(_("Maximum total size of all orphan transactions in megabytes (default: %u)"), DEFAULT_MAX_ORPHAN_TRANSACTIONS_SIZE));
    strUsage += HelpMessageOpt("-maxmempool=<n>", strprintf(_("Keep the transaction memory pool below <n> megabytes (default: %u)"), DEFAULT_MAX_MEMPOOL_SIZE));
    strUsage += HelpMessageOpt("-mempoolexpiry=<n>", strprintf(_("Do not keep transactions in the mempool longer than <n> hours (default: %u)"), DEFAULT_MEMPOOL_EXPIRY));
    if (showDebug) {
        strUsage += HelpMessageOpt("-minimumchainwork=<hex>", strprintf("Minimum work assumed to exist on a valid chain in hex (default: %s, testnet: %s)", defaultChainParams->GetConsensus().nMinimumChainWork.GetHex(), testnetChainParams->GetConsensus().nMinimumChainWork.GetHex()));
    }
    strUsage += HelpMessageOpt("-persistmempool", strprintf(_("Whether to save the mempool on shutdown and load on restart (default: %u)"), DEFAULT_PERSIST_MEMPOOL));
    strUsage += HelpMessageOpt("-syncmempool", strprintf(_("Sync mempool from other nodes on start (default: %u)"), DEFAULT_SYNC_MEMPOOL));
    strUsage += HelpMessageOpt("-blockreconstructionextratxn=<n>", strprintf(_("Extra transactions to keep in memory for compact block reconstructions (default: %u)"), DEFAULT_BLOCK_RECONSTRUCTION_EXTRA_TXN));
    strUsage += HelpMessageOpt("-par=<n>", strprintf(_("Set the number of script verification threads (%u to %d, 0 = auto, <0 = leave that many cores free, default: %d)"),
                                               -GetNumCores(), MAX_SCRIPTCHECK_THREADS, DEFAULT_SCRIPTCHECK_THREADS));
#ifndef WIN32
    strUsage += HelpMessageOpt("-pid=<file>", strprintf(_("Specify pid file (default: %s)"), BITCOIN_PID_FILENAME));
#endif
    strUsage += HelpMessageOpt("-prune=<n>", strprintf(_("Reduce storage requirements by enabling pruning (deleting) of old blocks. This allows the pruneblockchain RPC to be called to delete specific blocks, and enables automatic pruning of old blocks if a target size in MiB is provided. This mode is incompatible with -txindex and -rescan. "
                                                         "Warning: Reverting this setting requires re-downloading the entire blockchain. "
                                                         "(default: 0 = disable pruning blocks, 1 = allow manual pruning via RPC, >%u = automatically prune block files to stay under the specified target size in MiB)"),
                                                 MIN_DISK_SPACE_FOR_BLOCK_FILES / 1024 / 1024));
    strUsage += HelpMessageOpt("-reindex-chainstate", _("Rebuild chain state from the currently indexed blocks"));
    strUsage += HelpMessageOpt("-reindex", _("Rebuild chain state and block index from the blk*.dat files on disk"));
#ifndef WIN32
    strUsage += HelpMessageOpt("-sysperms", _("Create new files with system default permissions, instead of umask 077 (only effective with disabled wallet functionality)"));
#endif
    strUsage += HelpMessageOpt("-txindex", strprintf(_("Maintain a full transaction index, used by the getrawtransaction rpc call (default: %u)"), DEFAULT_TXINDEX));

    strUsage += HelpMessageOpt("-addressindex", strprintf(_("Maintain a full address index, used to query for the balance, txids and unspent outputs for addresses (default: %u)"), DEFAULT_ADDRESSINDEX));
    strUsage += HelpMessageOpt("-timestampindex", strprintf(_("Maintain a timestamp index for block hashes, used to query blocks hashes by a range of timestamps (default: %u)"), DEFAULT_TIMESTAMPINDEX));
    strUsage += HelpMessageOpt("-spentindex", strprintf(_("Maintain a full spent index, used to query the spending txid and input index for an outpoint (default: %u)"), DEFAULT_SPENTINDEX));

    strUsage += HelpMessageGroup(_("Connection options:"));
    strUsage += HelpMessageOpt("-addnode=<ip>", _("Add a node to connect to and attempt to keep the connection open (see the `addnode` RPC command help for more info)"));
    strUsage += HelpMessageOpt("-allowprivatenet", strprintf(_("Allow RFC1918 addresses to be relayed and connected to (default: %u)"), DEFAULT_ALLOWPRIVATENET));
    strUsage += HelpMessageOpt("-banscore=<n>", strprintf(_("Threshold for disconnecting misbehaving peers (default: %u)"), DEFAULT_BANSCORE_THRESHOLD));
    strUsage += HelpMessageOpt("-bantime=<n>", strprintf(_("Number of seconds to keep misbehaving peers from reconnecting (default: %u)"), DEFAULT_MISBEHAVING_BANTIME));
    strUsage += HelpMessageOpt("-bind=<addr>", _("Bind to given address and always listen on it. Use [host]:port notation for IPv6"));
    strUsage += HelpMessageOpt("-connect=<ip>", _("Connect only to the specified node(s); -connect=0 disables automatic connections (the rules for this peer are the same as for -addnode)"));
    strUsage += HelpMessageOpt("-discover", _("Discover own IP addresses (default: 1 when listening and no -externalip or -proxy)"));
    strUsage += HelpMessageOpt("-dns", _("Allow DNS lookups for -addnode, -seednode and -connect") + " " + strprintf(_("(default: %u)"), DEFAULT_NAME_LOOKUP));
    strUsage += HelpMessageOpt("-dnsseed", _("Query for peer addresses via DNS lookup, if low on addresses (default: 1 unless -connect used)"));
    strUsage += HelpMessageOpt("-externalip=<ip>", _("Specify your own public address"));
    strUsage += HelpMessageOpt("-forcednsseed", strprintf(_("Always query for peer addresses via DNS lookup (default: %u)"), DEFAULT_FORCEDNSSEED));
    strUsage += HelpMessageOpt("-listen", _("Accept connections from outside (default: 1 if no -proxy or -connect)"));
    strUsage += HelpMessageOpt("-listenonion", strprintf(_("Automatically create Tor hidden service (default: %d)"), DEFAULT_LISTEN_ONION));
    strUsage += HelpMessageOpt("-maxconnections=<n>", strprintf(_("Maintain at most <n> connections to peers (temporary service connections excluded) (default: %u)"), DEFAULT_MAX_PEER_CONNECTIONS));
    strUsage += HelpMessageOpt("-maxreceivebuffer=<n>", strprintf(_("Maximum per-connection receive buffer, <n>*1000 bytes (default: %u)"), DEFAULT_MAXRECEIVEBUFFER));
    strUsage += HelpMessageOpt("-maxsendbuffer=<n>", strprintf(_("Maximum per-connection send buffer, <n>*1000 bytes (default: %u)"), DEFAULT_MAXSENDBUFFER));
    strUsage += HelpMessageOpt("-maxtimeadjustment", strprintf(_("Maximum allowed median peer time offset adjustment. Local perspective of time may be influenced by peers forward or backward by this amount. (default: %u seconds)"), DEFAULT_MAX_TIME_ADJUSTMENT));
    strUsage += HelpMessageOpt("-onion=<ip:port>", strprintf(_("Use separate SOCKS5 proxy to reach peers via Tor hidden services (default: %s)"), "-proxy"));
    strUsage += HelpMessageOpt("-onlynet=<net>", _("Only connect to nodes in network <net> (ipv4, ipv6 or onion)"));
    strUsage += HelpMessageOpt("-permitbaremultisig", strprintf(_("Relay non-P2SH multisig (default: %u)"), DEFAULT_PERMIT_BAREMULTISIG));
    strUsage += HelpMessageOpt("-peerbloomfilters", strprintf(_("Support filtering of blocks and transaction with bloom filters (default: %u)"), DEFAULT_PEERBLOOMFILTERS));
    strUsage += HelpMessageOpt("-port=<port>", strprintf(_("Listen for connections on <port> (default: %u or testnet: %u)"), defaultChainParams->GetDefaultPort(), testnetChainParams->GetDefaultPort()));
    strUsage += HelpMessageOpt("-proxy=<ip:port>", _("Connect through SOCKS5 proxy"));
    strUsage += HelpMessageOpt("-proxyrandomize", strprintf(_("Randomize credentials for every proxy connection. This enables Tor stream isolation (default: %u)"), DEFAULT_PROXYRANDOMIZE));
    strUsage += HelpMessageOpt("-seednode=<ip>", _("Connect to a node to retrieve peer addresses, and disconnect"));
    strUsage += HelpMessageOpt("-timeout=<n>", strprintf(_("Specify connection timeout in milliseconds (minimum: 1, default: %d)"), DEFAULT_CONNECT_TIMEOUT));
    strUsage += HelpMessageOpt("-torcontrol=<ip>:<port>", strprintf(_("Tor control port to use if onion listening enabled (default: %s)"), DEFAULT_TOR_CONTROL));
    strUsage += HelpMessageOpt("-torpassword=<pass>", _("Tor control port password (default: empty)"));
#ifdef USE_UPNP
#if USE_UPNP
    strUsage += HelpMessageOpt("-upnp", _("Use UPnP to map the listening port (default: 1 when listening and no -proxy)"));
#else
    strUsage += HelpMessageOpt("-upnp", strprintf(_("Use UPnP to map the listening port (default: %u)"), 0));
#endif
#endif
    strUsage += HelpMessageOpt("-whitebind=<addr>", _("Bind to given address and whitelist peers connecting to it. Use [host]:port notation for IPv6"));
    strUsage += HelpMessageOpt("-whitelist=<IP address or network>", _("Whitelist peers connecting from the given IP address (e.g. 1.2.3.4) or CIDR notated network (e.g. 1.2.3.0/24). Can be specified multiple times.") +
                                                                         " " + _("Whitelisted peers cannot be DoS banned and their transactions are always relayed, even if they are already in the mempool, useful e.g. for a gateway"));
    strUsage += HelpMessageOpt("-maxuploadtarget=<n>", strprintf(_("Tries to keep outbound traffic under the given target (in MiB per 24h), 0 = no limit (default: %d)"), DEFAULT_MAX_UPLOAD_TARGET));

#ifdef ENABLE_WALLET
    strUsage += CWallet::GetWalletHelpString(showDebug);
    if (mode == HMM_BITCOIN_QT)
        strUsage += HelpMessageOpt("-windowtitle=<name>", _("Wallet window title"));
#endif

#if ENABLE_ZMQ
    strUsage += HelpMessageGroup(_("ZeroMQ notification options:"));
    strUsage += HelpMessageOpt("-zmqpubhashblock=<address>", _("Enable publish hash block in <address>"));
    strUsage += HelpMessageOpt("-zmqpubhashtx=<address>", _("Enable publish hash transaction in <address>"));
    strUsage += HelpMessageOpt("-zmqpubhashtxlock=<address>", _("Enable publish hash transaction (locked via InstantSend) in <address>"));
    strUsage += HelpMessageOpt("-zmqpubhashgovernancevote=<address>", _("Enable publish hash of governance votes in <address>"));
    strUsage += HelpMessageOpt("-zmqpubhashgovernanceobject=<address>", _("Enable publish hash of governance objects (like proposals) in <address>"));
    strUsage += HelpMessageOpt("-zmqpubhashinstantsenddoublespend=<address>", _("Enable publish transaction hashes of attempted InstantSend double spend in <address>"));
    strUsage += HelpMessageOpt("-zmqpubrawblock=<address>", _("Enable publish raw block in <address>"));
    strUsage += HelpMessageOpt("-zmqpubrawtx=<address>", _("Enable publish raw transaction in <address>"));
    strUsage += HelpMessageOpt("-zmqpubrawtxlock=<address>", _("Enable publish raw transaction (locked via InstantSend) in <address>"));
    strUsage += HelpMessageOpt("-zmqpubrawinstantsenddoublespend=<address>", _("Enable publish raw transactions of attempted InstantSend double spend in <address>"));
#endif

    strUsage += HelpMessageGroup(_("Debugging/Testing options:"));
    strUsage += HelpMessageOpt("-uacomment=<cmt>", _("Append comment to the user agent string"));
    if (showDebug) {
        strUsage += HelpMessageOpt("-checkblocks=<n>", strprintf(_("How many blocks to check at startup (default: %u, 0 = all)"), DEFAULT_CHECKBLOCKS));
        strUsage += HelpMessageOpt("-checklevel=<n>", strprintf(_("How thorough the block verification of -checkblocks is (0-4, default: %u)"), DEFAULT_CHECKLEVEL));
        strUsage += HelpMessageOpt("-checkblockindex", strprintf("Do a full consistency check for mapBlockIndex, setBlockIndexCandidates, chainActive and mapBlocksUnlinked occasionally. Also sets -checkmempool (default: %u)", defaultChainParams->DefaultConsistencyChecks()));
        strUsage += HelpMessageOpt("-checkmempool=<n>", strprintf("Run checks every <n> transactions (default: %u)", defaultChainParams->DefaultConsistencyChecks()));
        strUsage += HelpMessageOpt("-checkpoints", strprintf("Disable expensive verification for known chain history (default: %u)", DEFAULT_CHECKPOINTS_ENABLED));
        strUsage += HelpMessageOpt("-disablesafemode", strprintf("Disable safemode, override a real safe mode event (default: %u)", DEFAULT_DISABLE_SAFEMODE));
        strUsage += HelpMessageOpt("-testsafemode", strprintf("Force safe mode (default: %u)", DEFAULT_TESTSAFEMODE));
        strUsage += HelpMessageOpt("-dropmessagestest=<n>", "Randomly drop 1 of every <n> network messages");
        strUsage += HelpMessageOpt("-fuzzmessagestest=<n>", "Randomly fuzz 1 of every <n> network messages");
        strUsage += HelpMessageOpt("-stopafterblockimport", strprintf("Stop running after importing blocks from disk (default: %u)", DEFAULT_STOPAFTERBLOCKIMPORT));
        strUsage += HelpMessageOpt("-stopatheight", strprintf("Stop running after reaching the given height in the main chain (default: %u)", DEFAULT_STOPATHEIGHT));

        strUsage += HelpMessageOpt("-limitancestorcount=<n>", strprintf("Do not accept transactions if number of in-mempool ancestors is <n> or more (default: %u)", DEFAULT_ANCESTOR_LIMIT));
        strUsage += HelpMessageOpt("-limitancestorsize=<n>", strprintf("Do not accept transactions whose size with all in-mempool ancestors exceeds <n> kilobytes (default: %u)", DEFAULT_ANCESTOR_SIZE_LIMIT));
        strUsage += HelpMessageOpt("-limitdescendantcount=<n>", strprintf("Do not accept transactions if any ancestor would have <n> or more in-mempool descendants (default: %u)", DEFAULT_DESCENDANT_LIMIT));
        strUsage += HelpMessageOpt("-limitdescendantsize=<n>", strprintf("Do not accept transactions if any ancestor would have more than <n> kilobytes of in-mempool descendants (default: %u).", DEFAULT_DESCENDANT_SIZE_LIMIT));
        strUsage += HelpMessageOpt("-vbparams=<deployment>:<start>:<end>(:<window>:<threshold>)", "Use given start/end times for specified version bits deployment (regtest-only). Specifying window and threshold is optional.");
        strUsage += HelpMessageOpt("-watchquorums=<n>", strprintf("Watch and validate quorum communication (default: %u)", llmq::DEFAULT_WATCH_QUORUMS));
    }
    strUsage += HelpMessageOpt("-debug=<category>", strprintf(_("Output debugging information (default: %u, supplying <category> is optional)"), 0) + ". " +
                                                        _("If <category> is not supplied or if <category> = 1, output all debugging information.") + " " + _("<category> can be:") + " " + ListLogCategories() + ".");
    strUsage += HelpMessageOpt("-debugexclude=<category>", strprintf(_("Exclude debugging information for a category. Can be used in conjunction with -debug=1 to output debug logs for all categories except one or more specified categories.")));
    if (showDebug)
        strUsage += HelpMessageOpt("-nodebug", "Turn off debugging messages, same as -debug=0");
    strUsage += HelpMessageOpt("-help-debug", _("Show all debugging options (usage: --help -help-debug)"));
    strUsage += HelpMessageOpt("-logips", strprintf(_("Include IP addresses in debug output (default: %u)"), DEFAULT_LOGIPS));
    strUsage += HelpMessageOpt("-logtimestamps", strprintf(_("Prepend debug output with timestamp (default: %u)"), DEFAULT_LOGTIMESTAMPS));
    if (showDebug) {
        strUsage += HelpMessageOpt("-logtimemicros", strprintf("Add microsecond precision to debug timestamps (default: %u)", DEFAULT_LOGTIMEMICROS));
        strUsage += HelpMessageOpt("-logthreadnames", strprintf("Add thread names to debug messages (default: %u)", DEFAULT_LOGTHREADNAMES));
        strUsage += HelpMessageOpt("-mocktime=<n>", "Replace actual time with <n> seconds since epoch (default: 0)");
        strUsage += HelpMessageOpt("-maxsigcachesize=<n>", strprintf("Limit sum of signature cache and script execution cache sizes to <n> MiB (default: %u)", DEFAULT_MAX_SIG_CACHE_SIZE));
        strUsage += HelpMessageOpt("-maxtipage=<n>", strprintf("Maximum tip age in seconds to consider node in initial block download (default: %u)", DEFAULT_MAX_TIP_AGE));
    }
    strUsage += HelpMessageOpt("-maxtxfee=<amt>", strprintf(_("Maximum total fees (in %s) to use in a single wallet transaction or raw transaction; setting this too low may abort large transactions (default: %s)"),
                                                      CURRENCY_UNIT, FormatMoney(DEFAULT_TRANSACTION_MAXFEE)));
    strUsage += HelpMessageOpt("-printtoconsole", _("Send trace/debug info to console instead of debug.log file"));
    strUsage += HelpMessageOpt("-printtodebuglog", strprintf(_("Send trace/debug info to debug.log file (default: %u)"), 1));
    if (showDebug) {
        strUsage += HelpMessageOpt("-printpriority", strprintf("Log transaction fee per kB when mining blocks (default: %u)", DEFAULT_PRINTPRIORITY));
    }
    strUsage += HelpMessageOpt("-shrinkdebugfile", _("Shrink debug.log file on client startup (default: 1 when no -debug)"));
    AppendParamsHelpMessages(strUsage, showDebug);
    strUsage += HelpMessageOpt("-litemode", strprintf(_("Disable all Memeium specific functionality (Smartnodes, PrivateSend, InstantSend, Governance) (0-1, default: %u)"), 0));
    strUsage += HelpMessageOpt("-sporkaddr=<memeiumaddress>", strprintf(_("Override spork address. Only useful for regtest and devnet. Using this on mainnet or testnet will ban you.")));
    strUsage += HelpMessageOpt("-minsporkkeys=<n>", strprintf(_("Overrides minimum spork signers to change spork value. Only useful for regtest and devnet. Using this on mainnet or testnet will ban you.")));

    strUsage += HelpMessageGroup(_("Smartnode options:"));
    strUsage += HelpMessageOpt("-smartnodeblsprivkey=<hex>", _("Set the smartnode BLS private key and enable the client to act as a smartnode"));

#ifdef ENABLE_WALLET
    strUsage += HelpMessageGroup(_("PrivateSend options:"));
    strUsage += HelpMessageOpt("-enableprivatesend", strprintf(_("Enable use of PrivateSend for funds stored in this wallet (0-1, default: %u)"), 0));
    strUsage += HelpMessageOpt("-privatesendautostart", strprintf(_("Start PrivateSend automatically (0-1, default: %u)"), DEFAULT_PRIVATESEND_AUTOSTART));
    strUsage += HelpMessageOpt("-privatesendmultisession", strprintf(_("Enable multiple PrivateSend mixing sessions per block, experimental (0-1, default: %u)"), DEFAULT_PRIVATESEND_MULTISESSION));
    strUsage += HelpMessageOpt("-privatesendsessions=<n>", strprintf(_("Use N separate smartnodes in parallel to mix funds (%u-%u, default: %u)"), MIN_PRIVATESEND_SESSIONS, MAX_PRIVATESEND_SESSIONS, DEFAULT_PRIVATESEND_SESSIONS));
    strUsage += HelpMessageOpt("-privatesendrounds=<n>", strprintf(_("Use N separate smartnodes for each denominated input to mix funds (%u-%u, default: %u)"), MIN_PRIVATESEND_ROUNDS, MAX_PRIVATESEND_ROUNDS, DEFAULT_PRIVATESEND_ROUNDS));
    strUsage += HelpMessageOpt("-privatesendamount=<n>", strprintf(_("Target PrivateSend balance (%u-%u, default: %u)"), MIN_PRIVATESEND_AMOUNT, MAX_PRIVATESEND_AMOUNT, DEFAULT_PRIVATESEND_AMOUNT));
    strUsage += HelpMessageOpt("-privatesenddenoms=<n>", strprintf(_("Create up to N inputs of each denominated amount (%u-%u, default: %u)"), MIN_PRIVATESEND_DENOMS, MAX_PRIVATESEND_DENOMS, DEFAULT_PRIVATESEND_DENOMS));
#endif // ENABLE_WALLET

    strUsage += HelpMessageGroup(_("InstantSend options:"));
    strUsage += HelpMessageOpt("-instantsendnotify=<cmd>", _("Execute command when a wallet InstantSend transaction is successfully locked (%s in cmd is replaced by TxID)"));


    strUsage += HelpMessageGroup(_("Node relay options:"));
    if (showDebug) {
        strUsage += HelpMessageOpt("-acceptnonstdtxn", strprintf("Relay and mine \"non-standard\" transactions (%sdefault: %u)", "testnet/regtest only; ", defaultChainParams->RequireStandard()));
        strUsage += HelpMessageOpt("-incrementalrelayfee=<amt>", strprintf("Fee rate (in %s/kB) used to define cost of relay, used for mempool limiting and BIP 125 replacement. (default: %s)", CURRENCY_UNIT, FormatMoney(DEFAULT_INCREMENTAL_RELAY_FEE)));
        strUsage += HelpMessageOpt("-dustrelayfee=<amt>", strprintf("Fee rate (in %s/kB) used to defined dust, the value of an output such that it will cost more than its value in fees at this fee rate to spend it. (default: %s)", CURRENCY_UNIT, FormatMoney(DUST_RELAY_TX_FEE)));
    }
    strUsage += HelpMessageOpt("-bytespersigop", strprintf(_("Minimum bytes per sigop in transactions we relay and mine (default: %u)"), DEFAULT_BYTES_PER_SIGOP));
    strUsage += HelpMessageOpt("-datacarrier", strprintf(_("Relay and mine data carrier transactions (default: %u)"), DEFAULT_ACCEPT_DATACARRIER));
    strUsage += HelpMessageOpt("-datacarriersize", strprintf(_("Maximum size of data in data carrier transactions we relay and mine (default: %u)"), MAX_OP_RETURN_RELAY));
    strUsage += HelpMessageOpt("-minrelaytxfee=<amt>", strprintf(_("Fees (in %s/kB) smaller than this are considered zero fee for relaying, mining and transaction creation (default: %s)"),
                                                           CURRENCY_UNIT, FormatMoney(DEFAULT_MIN_RELAY_TX_FEE)));
    strUsage += HelpMessageOpt("-whitelistrelay", strprintf(_("Accept relayed transactions received from whitelisted peers even when not relaying transactions (default: %d)"), DEFAULT_WHITELISTRELAY));
    strUsage += HelpMessageOpt("-whitelistforcerelay", strprintf(_("Force relay of transactions from whitelisted peers even if they violate local relay policy (default: %d)"), DEFAULT_WHITELISTFORCERELAY));

    strUsage += HelpMessageGroup(_("Block creation options:"));
    strUsage += HelpMessageOpt("-blockmaxsize=<n>", strprintf(_("Set maximum block size in bytes (default: %d)"), DEFAULT_BLOCK_MAX_SIZE));
    strUsage += HelpMessageOpt("-blockmintxfee=<amt>", strprintf(_("Set lowest fee rate (in %s/kB) for transactions to be included in block creation. (default: %s)"), CURRENCY_UNIT, FormatMoney(DEFAULT_BLOCK_MIN_TX_FEE)));
    if (showDebug)
        strUsage += HelpMessageOpt("-blockversion=<n>", "Override block version to test forking scenarios");

    strUsage += HelpMessageGroup(_("RPC server options:"));
    strUsage += HelpMessageOpt("-server", _("Accept command line and JSON-RPC commands"));
    strUsage += HelpMessageOpt("-rest", strprintf(_("Accept public REST requests (default: %u)"), DEFAULT_REST_ENABLE));
    strUsage += HelpMessageOpt("-rpcbind=<addr>[:port]", _("Bind to given address to listen for JSON-RPC connections. This option is ignored unless -rpcallowip is also passed. Port is optional and overrides -rpcport. Use [host]:port notation for IPv6. This option can be specified multiple times (default: 127.0.0.1 and ::1 i.e., localhost, or if -rpcallowip has been specified, 0.0.0.0 and :: i.e., all addresses)"));
    strUsage += HelpMessageOpt("-rpccookiefile=<loc>", _("Location of the auth cookie (default: data dir)"));
    strUsage += HelpMessageOpt("-rpcuser=<user>", _("Username for JSON-RPC connections"));
    strUsage += HelpMessageOpt("-rpcpassword=<pw>", _("Password for JSON-RPC connections"));
    strUsage += HelpMessageOpt("-rpcauth=<userpw>", _("Username and hashed password for JSON-RPC connections. The field <userpw> comes in the format: <USERNAME>:<SALT>$<HASH>. A canonical python script is included in share/rpcuser. The client then connects normally using the rpcuser=<USERNAME>/rpcpassword=<PASSWORD> pair of arguments. This option can be specified multiple times"));
    strUsage += HelpMessageOpt("-rpcport=<port>", strprintf(_("Listen for JSON-RPC connections on <port> (default: %u or testnet: %u)"), defaultBaseParams->RPCPort(), testnetBaseParams->RPCPort()));
    strUsage += HelpMessageOpt("-rpcallowip=<ip>", _("Allow JSON-RPC connections from specified source. Valid for <ip> are a single IP (e.g. 1.2.3.4), a network/netmask (e.g. 1.2.3.4/255.255.255.0) or a network/CIDR (e.g. 1.2.3.4/24). This option can be specified multiple times"));
    strUsage += HelpMessageOpt("-rpcthreads=<n>", strprintf(_("Set the number of threads to service RPC calls (default: %d)"), DEFAULT_HTTP_THREADS));
    if (showDebug) {
        strUsage += HelpMessageOpt("-rpcworkqueue=<n>", strprintf("Set the depth of the work queue to service RPC calls (default: %d)", DEFAULT_HTTP_WORKQUEUE));
        strUsage += HelpMessageOpt("-rpcservertimeout=<n>", strprintf("Timeout during HTTP requests (default: %d)", DEFAULT_HTTP_SERVER_TIMEOUT));
    }

    return strUsage;
}

std::string LicenseInfo()
{
    const std::string URL_SOURCE_CODE = "<https://github.com/The-Memeium-Endeavor/memeium>";
    const std::string URL_WEBSITE = "<https://memeium.org>";

    return CopyrightHolders(_("Copyright (C)"), 2014, COPYRIGHT_YEAR) + "\n" +
           "\n" +
           strprintf(_("Please contribute if you find %s useful. "
                       "Visit %s for further information about the software."),
               PACKAGE_NAME, URL_WEBSITE) +
           "\n" +
           strprintf(_("The source code is available from %s."),
               URL_SOURCE_CODE) +
           "\n" +
           "\n" +
           _("This is experimental software.") + "\n" +
           strprintf(_("Distributed under the MIT software license, see the accompanying file %s or %s"), "COPYING", "<https://opensource.org/licenses/MIT>") + "\n" +
           "\n" +
           strprintf(_("This product includes software developed by the OpenSSL Project for use in the OpenSSL Toolkit %s and cryptographic software written by Eric Young and UPnP software written by Thomas Bernard."), "<https://www.openssl.org>") +
           "\n";
}

static void BlockNotifyCallback(bool initialSync, const CBlockIndex* pBlockIndex)
{
    if (initialSync || !pBlockIndex)
        return;

    std::string strCmd = gArgs.GetArg("-blocknotify", "");

    boost::replace_all(strCmd, "%s", pBlockIndex->GetBlockHash().GetHex());
    boost::thread t(runCommand, strCmd); // thread runs free
}

static bool fHaveGenesis = false;
static boost::mutex cs_GenesisWait;
static CConditionVariable condvar_GenesisWait;

static void BlockNotifyGenesisWait(bool, const CBlockIndex* pBlockIndex)
{
    if (pBlockIndex != nullptr) {
        {
            boost::unique_lock<boost::mutex> lock_GenesisWait(cs_GenesisWait);
            fHaveGenesis = true;
        }
        condvar_GenesisWait.notify_all();
    }
}

struct CImportingNow {
    CImportingNow()
    {
        assert(fImporting == false);
        fImporting = true;
    }

    ~CImportingNow()
    {
        assert(fImporting == true);
        fImporting = false;
    }
};


// If we're using -prune with -reindex, then delete block files that will be ignored by the
// reindex.  Since reindexing works by starting at block file 0 and looping until a blockfile
// is missing, do the same here to delete any later block files after a gap.  Also delete all
// rev files since they'll be rewritten by the reindex anyway.  This ensures that vinfoBlockFile
// is in sync with what's actually on disk by the time we start downloading, so that pruning
// works correctly.
void CleanupBlockRevFiles()
{
    std::map<std::string, fs::path> mapBlockFiles;

    // Glob all blk?????.dat and rev?????.dat files from the blocks directory.
    // Remove the rev files immediately and insert the blk file paths into an
    // ordered map keyed by block file index.
    LogPrintf("Removing unusable blk?????.dat and rev?????.dat files for -reindex with -prune\n");
    fs::path blocksdir = GetDataDir() / "blocks";
    for (fs::directory_iterator it(blocksdir); it != fs::directory_iterator(); it++) {
        if (is_regular_file(*it) &&
            it->path().filename().string().length() == 12 &&
            it->path().filename().string().substr(8, 4) == ".dat") {
            if (it->path().filename().string().substr(0, 3) == "blk")
                mapBlockFiles[it->path().filename().string().substr(3, 5)] = it->path();
            else if (it->path().filename().string().substr(0, 3) == "rev")
                remove(it->path());
        }
    }

    // Remove all block files that aren't part of a contiguous set starting at
    // zero by walking the ordered map (keys are block file indices) by
    // keeping a separate counter.  Once we hit a gap (or if 0 doesn't exist)
    // start removing block files.
    int nContigCounter = 0;
    for (const std::pair<std::string, fs::path>& item : mapBlockFiles) {
        if (atoi(item.first) == nContigCounter) {
            nContigCounter++;
            continue;
        }
        remove(item.second);
    }
}

void ThreadImport(std::vector<fs::path> vImportFiles)
{
    const CChainParams& chainparams = Params();
    RenameThread("memeium-loadblk");

    {
        CImportingNow imp;

        // -reindex
        if (fReindex) {
            int nFile = 0;
            while (true) {
                CDiskBlockPos pos(nFile, 0);
                if (!fs::exists(GetBlockPosFilename(pos, "blk")))
                    break; // No block files left to reindex
                FILE* file = OpenBlockFile(pos, true);
                if (!file)
                    break; // This error is logged in OpenBlockFile
                LogPrintf("Reindexing block file blk%05u.dat...\n", (unsigned int)nFile);
                LoadExternalBlockFile(chainparams, file, &pos);
                nFile++;
            }
            pblocktree->WriteReindexing(false);
            fReindex = false;
            LogPrintf("Reindexing finished\n");
            // To avoid ending up in a situation without genesis block, re-try initializing (no-op if reindexing worked):
            LoadGenesisBlock(chainparams);
        }

        // hardcoded $DATADIR/bootstrap.dat
        fs::path pathBootstrap = GetDataDir() / "bootstrap.dat";
        if (fs::exists(pathBootstrap)) {
            FILE* file = fsbridge::fopen(pathBootstrap, "rb");
            if (file) {
                fs::path pathBootstrapOld = GetDataDir() / "bootstrap.dat.old";
                LogPrintf("Importing bootstrap.dat...\n");
                LoadExternalBlockFile(chainparams, file);
                RenameOver(pathBootstrap, pathBootstrapOld);
            } else {
                LogPrintf("Warning: Could not open bootstrap file %s\n", pathBootstrap.string());
            }
        }

        // -loadblock=
        for (const fs::path& path : vImportFiles) {
            FILE* file = fsbridge::fopen(path, "rb");
            if (file) {
                LogPrintf("Importing blocks file %s...\n", path.string());
                LoadExternalBlockFile(chainparams, file);
            } else {
                LogPrintf("Warning: Could not open blocks file %s\n", path.string());
            }
        }

        // scan for better chains in the block chain database, that are not yet connected in the active best chain
        CValidationState state;
        if (!ActivateBestChain(state, chainparams)) {
            LogPrintf("Failed to connect best block (%s)\n", FormatStateMessage(state));
            StartShutdown();
        }

        if (gArgs.GetBoolArg("-stopafterblockimport", DEFAULT_STOPAFTERBLOCKIMPORT)) {
            LogPrintf("Stopping after block import\n");
            StartShutdown();
        }
    } // End scope of CImportingNow

    // force UpdatedBlockTip to initialize nCachedBlockHeight for DS, MN payments and budgets
    // but don't call it directly to prevent triggering of other listeners like zmq etc.
    // GetMainSignals().UpdatedBlockTip(chainActive.Tip());
    pdsNotificationInterface->InitializeCurrentBlockTip();

    {
        // Get all UTXOs for each MN collateral in one go so that we can fill coin cache early
        // and reduce further locking overhead for cs_main in other parts of code inclluding GUI
        LogPrintf("Filling coin cache with smartnode UTXOs...\n");
        LOCK(cs_main);
        int64_t nStart = GetTimeMillis();
        auto mnList = deterministicMNManager->GetListAtChainTip();
        mnList.ForEachMN(false, [&](const CDeterministicMNCPtr& dmn) {
            Coin coin;
            GetUTXOCoin(dmn->collateralOutpoint, coin);
        });
        LogPrintf("Filling coin cache with smartnode UTXOs: done in %dms\n", GetTimeMillis() - nStart);
    }

    if (fSmartnodeMode) {
        assert(activeSmartnodeManager);
        activeSmartnodeManager->Init();
    }

#ifdef ENABLE_WALLET
    // we can't do this before DIP3 is fully initialized
    for (CWalletRef pwallet : vpwallets) {
        pwallet->AutoLockSmartnodeCollaterals();
    }
#endif

    if (gArgs.GetArg("-persistmempool", DEFAULT_PERSIST_MEMPOOL)) {
        LoadMempool();
        fDumpMempoolLater = !fRequestShutdown;
    }
}

/** Sanity checks
 *  Ensure that Memeium Core is running in a usable environment with all
 *  necessary library support.
 */
bool InitSanityCheck(void)
{
    if (!ECC_InitSanityCheck()) {
        InitError("Elliptic curve cryptography sanity check failure. Aborting.");
        return false;
    }

    if (!glibc_sanity_test() || !glibcxx_sanity_test())
        return false;

    if (!BLSInit()) {
        return false;
    }

    if (!Random_SanityCheck()) {
        InitError("OS cryptographic RNG sanity check failure. Aborting.");
        return false;
    }

    return true;
}

bool AppInitServers(boost::thread_group& threadGroup)
{
    RPCServer::OnStarted(&OnRPCStarted);
    RPCServer::OnStopped(&OnRPCStopped);
    RPCServer::OnPreCommand(&OnRPCPreCommand);
    if (!InitHTTPServer())
        return false;
    if (!StartRPC())
        return false;
    if (!StartHTTPRPC())
        return false;
    if (gArgs.GetBoolArg("-rest", DEFAULT_REST_ENABLE) && !StartREST())
        return false;
    if (!StartHTTPServer())
        return false;
    return true;
}

// Parameter interaction based on rules
void InitParameterInteraction()
{
    // when specifying an explicit binding address, you want to listen on it
    // even when -connect or -proxy is specified
    if (gArgs.IsArgSet("-bind")) {
        if (gArgs.SoftSetBoolArg("-listen", true))
            LogPrintf("%s: parameter interaction: -bind set -> setting -listen=1\n", __func__);
    }
    if (gArgs.IsArgSet("-whitebind")) {
        if (gArgs.SoftSetBoolArg("-listen", true))
            LogPrintf("%s: parameter interaction: -whitebind set -> setting -listen=1\n", __func__);
    }

    if (gArgs.IsArgSet("-smartnodeblsprivkey")) {
        // smartnodes MUST accept connections from outside
        gArgs.ForceSetArg("-listen", "1");
        LogPrintf("%s: parameter interaction: -smartnodeblsprivkey=... -> setting -listen=1\n", __func__);
#ifdef ENABLE_WALLET
        // smartnode should not have wallet enabled
        gArgs.ForceSetArg("-disablewallet", "1");
        LogPrintf("%s: parameter interaction: -smartnodeblsprivkey=... -> setting -disablewallet=1\n", __func__);
#endif // ENABLE_WALLET
        if (gArgs.GetArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS) < DEFAULT_SN_MAX_PEER_CONNECTIONS) {
            // smartnodes MUST be able to handle at least DEFAULT_MAX_PEER_CONNECTIONS connections
            gArgs.ForceSetArg("-maxconnections", itostr(DEFAULT_SN_MAX_PEER_CONNECTIONS));
            LogPrintf("%s: parameter interaction: -smartnodeblsprivkey=... -> setting -maxconnections=%d instead of specified -maxconnections=%d\n",
                __func__, DEFAULT_SN_MAX_PEER_CONNECTIONS, gArgs.GetArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS));
        }
    }

    if (gArgs.IsArgSet("-connect")) {
        // when only connecting to trusted nodes, do not seed via DNS, or listen by default
        if (gArgs.SoftSetBoolArg("-dnsseed", false))
            LogPrintf("%s: parameter interaction: -connect set -> setting -dnsseed=0\n", __func__);
        if (gArgs.SoftSetBoolArg("-listen", false))
            LogPrintf("%s: parameter interaction: -connect set -> setting -listen=0\n", __func__);
    }

    if (gArgs.IsArgSet("-proxy")) {
        // to protect privacy, do not listen by default if a default proxy server is specified
        if (gArgs.SoftSetBoolArg("-listen", false))
            LogPrintf("%s: parameter interaction: -proxy set -> setting -listen=0\n", __func__);
        // to protect privacy, do not use UPNP when a proxy is set. The user may still specify -listen=1
        // to listen locally, so don't rely on this happening through -listen below.
        if (gArgs.SoftSetBoolArg("-upnp", false))
            LogPrintf("%s: parameter interaction: -proxy set -> setting -upnp=0\n", __func__);
        // to protect privacy, do not discover addresses by default
        if (gArgs.SoftSetBoolArg("-discover", false))
            LogPrintf("%s: parameter interaction: -proxy set -> setting -discover=0\n", __func__);
    }

    if (!gArgs.GetBoolArg("-listen", DEFAULT_LISTEN)) {
        // do not map ports or try to retrieve public IP when not listening (pointless)
        if (gArgs.SoftSetBoolArg("-upnp", false))
            LogPrintf("%s: parameter interaction: -listen=0 -> setting -upnp=0\n", __func__);
        if (gArgs.SoftSetBoolArg("-discover", false))
            LogPrintf("%s: parameter interaction: -listen=0 -> setting -discover=0\n", __func__);
        if (gArgs.SoftSetBoolArg("-listenonion", false))
            LogPrintf("%s: parameter interaction: -listen=0 -> setting -listenonion=0\n", __func__);
    }

    if (gArgs.IsArgSet("-externalip")) {
        // if an explicit public IP is specified, do not try to find others
        if (gArgs.SoftSetBoolArg("-discover", false))
            LogPrintf("%s: parameter interaction: -externalip set -> setting -discover=0\n", __func__);
    }

    // disable whitelistrelay in blocksonly mode
    if (gArgs.GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY)) {
        if (gArgs.SoftSetBoolArg("-whitelistrelay", false))
            LogPrintf("%s: parameter interaction: -blocksonly=1 -> setting -whitelistrelay=0\n", __func__);
    }

    // Forcing relay from whitelisted hosts implies we will accept relays from them in the first place.
    if (gArgs.GetBoolArg("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY)) {
        if (gArgs.SoftSetBoolArg("-whitelistrelay", true))
            LogPrintf("%s: parameter interaction: -whitelistforcerelay=1 -> setting -whitelistrelay=1\n", __func__);
    }

#ifdef ENABLE_WALLET
    if (gArgs.IsArgSet("-hdseed") && IsHex(gArgs.GetArg("-hdseed", "not hex")) && (gArgs.IsArgSet("-mnemonic") || gArgs.IsArgSet("-mnemonicpassphrase"))) {
        gArgs.ForceRemoveArg("-mnemonic");
        gArgs.ForceRemoveArg("-mnemonicpassphrase");
        LogPrintf("%s: parameter interaction: can't use -hdseed and -mnemonic/-mnemonicpassphrase together, will prefer -seed\n", __func__);
    }
#endif // ENABLE_WALLET

    // Make sure additional indexes are recalculated correctly in VerifyDB
    // (we must reconnect blocks whenever we disconnect them for these indexes to work)
    bool fAdditionalIndexes =
        gArgs.GetBoolArg("-addressindex", DEFAULT_ADDRESSINDEX) ||
        gArgs.GetBoolArg("-spentindex", DEFAULT_SPENTINDEX) ||
        gArgs.GetBoolArg("-timestampindex", DEFAULT_TIMESTAMPINDEX);

    if (fAdditionalIndexes && gArgs.GetArg("-checklevel", DEFAULT_CHECKLEVEL) < 4) {
        gArgs.ForceSetArg("-checklevel", "4");
        LogPrintf("%s: parameter interaction: additional indexes -> setting -checklevel=4\n", __func__);
    }
}

static std::string ResolveErrMsg(const char* const optname, const std::string& strBind)
{
    return strprintf(_("Cannot resolve -%s address: '%s'"), optname, strBind);
}

void InitLogging()
{
    fPrintToConsole = gArgs.GetBoolArg("-printtoconsole", false);
    fPrintToDebugLog = gArgs.GetBoolArg("-printtodebuglog", true) && !fPrintToConsole;
    fLogTimestamps = gArgs.GetBoolArg("-logtimestamps", DEFAULT_LOGTIMESTAMPS);
    fLogTimeMicros = gArgs.GetBoolArg("-logtimemicros", DEFAULT_LOGTIMEMICROS);
    fLogThreadNames = gArgs.GetBoolArg("-logthreadnames", DEFAULT_LOGTHREADNAMES);
    fLogIPs = gArgs.GetBoolArg("-logips", DEFAULT_LOGIPS);

    LogPrintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    LogPrintf("Memeium Core version %s\n", FormatFullVersion());
}

namespace
{ // Variables internal to initialization process only

int nMaxConnections;
int nUserMaxConnections;
int nFD;
ServiceFlags nLocalServices = NODE_NETWORK;

} // namespace

[[noreturn]] static void new_handler_terminate()
{
    // Rather than throwing std::bad-alloc if allocation fails, terminate
    // immediately to (try to) avoid chain corruption.
    // Since LogPrintf may itself allocate memory, set the handler directly
    // to terminate first.
    std::set_new_handler(std::terminate);
    LogPrintf("Error: Out of memory. Terminating.\n");

    // The log was successful, terminate now.
    std::terminate();
};

bool AppInitBasicSetup()
{
    // ********************************************************* Step 1: setup
#ifdef _MSC_VER
    // Turn off Microsoft heap dump noise
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, CreateFileA("NUL", GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, 0));
    // Disable confusing "helpful" text message on abort, Ctrl-C
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#ifdef WIN32
    // Enable Data Execution Prevention (DEP)
    // Minimum supported OS versions: WinXP SP3, WinVista >= SP1, Win Server 2008
    // A failure is non-critical and needs no further attention!
#ifndef PROCESS_DEP_ENABLE
    // We define this here, because GCCs winbase.h limits this to _WIN32_WINNT >= 0x0601 (Windows 7),
    // which is not correct. Can be removed, when GCCs winbase.h is fixed!
#define PROCESS_DEP_ENABLE 0x00000001
#endif
    typedef BOOL(WINAPI * PSETPROCDEPPOL)(DWORD);
    PSETPROCDEPPOL setProcDEPPol = (PSETPROCDEPPOL)GetProcAddress(GetModuleHandleA("Kernel32.dll"), "SetProcessDEPPolicy");
    if (setProcDEPPol != nullptr) setProcDEPPol(PROCESS_DEP_ENABLE);
#endif

    if (!SetupNetworking())
        return InitError("Initializing networking failed");

#ifndef WIN32
    if (!gArgs.GetBoolArg("-sysperms", false)) {
        umask(077);
    }

    // Clean shutdown on SIGTERM
    registerSignalHandler(SIGTERM, HandleSIGTERM);
    registerSignalHandler(SIGINT, HandleSIGTERM);

    // Reopen debug.log on SIGHUP
    registerSignalHandler(SIGHUP, HandleSIGHUP);

    // Ignore SIGPIPE, otherwise it will bring the daemon down if the client closes unexpectedly
    signal(SIGPIPE, SIG_IGN);
#endif

    std::set_new_handler(new_handler_terminate);

    return true;
}

bool AppInitParameterInteraction()
{
    const CChainParams& chainparams = Params();
    // ********************************************************* Step 2: parameter interactions

    // also see: InitParameterInteraction()

    // if using block pruning, then disallow txindex
    if (gArgs.GetArg("-prune", 0)) {
        if (gArgs.GetBoolArg("-txindex", DEFAULT_TXINDEX))
            return InitError(_("Prune mode is incompatible with -txindex."));
    }

    if (gArgs.IsArgSet("-devnet")) {
        // Require setting of ports when running devnet
        if (gArgs.GetArg("-listen", DEFAULT_LISTEN) && !gArgs.IsArgSet("-port")) {
            return InitError(_("-port must be specified when -devnet and -listen are specified"));
        }
        if (gArgs.GetArg("-server", false) && !gArgs.IsArgSet("-rpcport")) {
            return InitError(_("-rpcport must be specified when -devnet and -server are specified"));
        }
        if (gArgs.GetArgs("-devnet").size() > 1) {
            return InitError(_("-devnet can only be specified once"));
        }
    }

    fAllowPrivateNet = gArgs.GetBoolArg("-allowprivatenet", DEFAULT_ALLOWPRIVATENET);

    // -bind and -whitebind can't be set when not listening
    size_t nUserBind = gArgs.GetArgs("-bind").size() + gArgs.GetArgs("-whitebind").size();
    if (nUserBind != 0 && !gArgs.GetBoolArg("-listen", DEFAULT_LISTEN)) {
        return InitError("Cannot set -bind or -whitebind together with -listen=0");
    }

    // Make sure enough file descriptors are available
    int nBind = std::max(nUserBind, size_t(1));
    nUserMaxConnections = gArgs.GetArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS);
    nMaxConnections = std::max(nUserMaxConnections, 0);

    // Trim requested connection counts, to fit into system limitations
    nMaxConnections = std::max(std::min(nMaxConnections, (int)(FD_SETSIZE - nBind - MIN_CORE_FILEDESCRIPTORS - MAX_ADDNODE_CONNECTIONS)), 0);
    nFD = RaiseFileDescriptorLimit(nMaxConnections + MIN_CORE_FILEDESCRIPTORS + MAX_ADDNODE_CONNECTIONS);
    if (nFD < MIN_CORE_FILEDESCRIPTORS)
        return InitError(_("Not enough file descriptors available."));
    nMaxConnections = std::min(nFD - MIN_CORE_FILEDESCRIPTORS - MAX_ADDNODE_CONNECTIONS, nMaxConnections);

    if (nMaxConnections < nUserMaxConnections)
        InitWarning(strprintf(_("Reducing -maxconnections from %d to %d, because of system limitations."), nUserMaxConnections, nMaxConnections));

    // ********************************************************* Step 3: parameter-to-internal-flags

    if (gArgs.IsArgSet("-debug")) {
        // Special-case: if -debug=0/-nodebug is set, turn off debugging messages
        const std::vector<std::string> categories = gArgs.GetArgs("-debug");

        if (!(gArgs.GetBoolArg("-nodebug", false) || find(categories.begin(), categories.end(), std::string("0")) != categories.end())) {
            for (const auto& cat : categories) {
                uint64_t flag;
                if (!GetLogCategory(&flag, &cat)) {
                    InitWarning(strprintf(_("Unsupported logging category %s=%s."), "-debug", cat));
                }
                logCategories |= flag;
            }
        }
    }

    // Now remove the logging categories which were explicitly excluded
    for (const std::string& cat : gArgs.GetArgs("-debugexclude")) {
        uint64_t flag;
        if (!GetLogCategory(&flag, &cat)) {
            InitWarning(strprintf(_("Unsupported logging category %s=%s."), "-debugexclude", cat));
        }
        logCategories &= ~flag;
    }

    // Check for -debugnet
    if (gArgs.GetBoolArg("-debugnet", false))
        InitWarning(_("Unsupported argument -debugnet ignored, use -debug=net."));
    // Check for -socks - as this is a privacy risk to continue, exit here
    if (gArgs.IsArgSet("-socks"))
        return InitError(_("Unsupported argument -socks found. Setting SOCKS version isn't possible anymore, only SOCKS5 proxies are supported."));
    // Check for -tor - as this is a privacy risk to continue, exit here
    if (gArgs.GetBoolArg("-tor", false))
        return InitError(_("Unsupported argument -tor found, use -onion."));

    if (gArgs.GetBoolArg("-benchmark", false))
        InitWarning(_("Unsupported argument -benchmark ignored, use -debug=bench."));

    if (gArgs.GetBoolArg("-whitelistalwaysrelay", false))
        InitWarning(_("Unsupported argument -whitelistalwaysrelay ignored, use -whitelistrelay and/or -whitelistforcerelay."));

    if (gArgs.IsArgSet("-blockminsize"))
        InitWarning("Unsupported argument -blockminsize ignored.");

    // Checkmempool and checkblockindex default to true in regtest mode
    int ratio = std::min<int>(std::max<int>(gArgs.GetArg("-checkmempool", chainparams.DefaultConsistencyChecks() ? 1 : 0), 0), 1000000);
    if (ratio != 0) {
        mempool.setSanityCheck(1.0 / ratio);
    }
    fCheckBlockIndex = gArgs.GetBoolArg("-checkblockindex", chainparams.DefaultConsistencyChecks());
    fCheckpointsEnabled = gArgs.GetBoolArg("-checkpoints", DEFAULT_CHECKPOINTS_ENABLED);

    hashAssumeValid = uint256S(gArgs.GetArg("-assumevalid", chainparams.GetConsensus().defaultAssumeValid.GetHex()));
    if (!hashAssumeValid.IsNull())
        LogPrintf("Assuming ancestors of block %s have valid signatures.\n", hashAssumeValid.GetHex());
    else
        LogPrintf("Validating signatures for all blocks.\n");

    if (gArgs.IsArgSet("-minimumchainwork")) {
        const std::string minChainWorkStr = gArgs.GetArg("-minimumchainwork", "");
        if (!IsHexNumber(minChainWorkStr)) {
            return InitError(strprintf("Invalid non-hex (%s) minimum chain work value specified", minChainWorkStr));
        }
        nMinimumChainWork = UintToArith256(uint256S(minChainWorkStr));
    } else {
        nMinimumChainWork = UintToArith256(chainparams.GetConsensus().nMinimumChainWork);
    }
    LogPrintf("Setting nMinimumChainWork=%s\n", nMinimumChainWork.GetHex());
    if (nMinimumChainWork < UintToArith256(chainparams.GetConsensus().nMinimumChainWork)) {
        LogPrintf("Warning: nMinimumChainWork set below default value of %s\n", chainparams.GetConsensus().nMinimumChainWork.GetHex());
    }

    // mempool limits
    int64_t nMempoolSizeMax = gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
    int64_t nMempoolSizeMin = gArgs.GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT) * 1000 * 40;
    if (nMempoolSizeMax < 0 || nMempoolSizeMax < nMempoolSizeMin)
        return InitError(strprintf(_("-maxmempool must be at least %d MB"), std::ceil(nMempoolSizeMin / 1000000.0)));
    // incremental relay fee sets the minimum feerate increase necessary for BIP 125 replacement in the mempool
    // and the amount the mempool min fee increases above the feerate of txs evicted due to mempool limiting.
    if (gArgs.IsArgSet("-incrementalrelayfee")) {
        CAmount n = 0;
        if (!ParseMoney(gArgs.GetArg("-incrementalrelayfee", ""), n))
            return InitError(AmountErrMsg("incrementalrelayfee", gArgs.GetArg("-incrementalrelayfee", "")));
        incrementalRelayFee = CFeeRate(n);
    }

    // -par=0 means autodetect, but nScriptCheckThreads==0 means no concurrency
    nScriptCheckThreads = gArgs.GetArg("-par", DEFAULT_SCRIPTCHECK_THREADS);
    if (nScriptCheckThreads <= 0)
        nScriptCheckThreads += GetNumCores();
    if (nScriptCheckThreads <= 1)
        nScriptCheckThreads = 0;
    else if (nScriptCheckThreads > MAX_SCRIPTCHECK_THREADS)
        nScriptCheckThreads = MAX_SCRIPTCHECK_THREADS;

    // block pruning; get the amount of disk space (in MiB) to allot for block & undo files
    int64_t nPruneArg = gArgs.GetArg("-prune", 0);
    if (nPruneArg < 0) {
        return InitError(_("Prune cannot be configured with a negative value."));
    }
    nPruneTarget = (uint64_t)nPruneArg * 1024 * 1024;
    if (nPruneArg == 1) { // manual pruning: -prune=1
        LogPrintf("Block pruning enabled.  Use RPC call pruneblockchain(height) to manually prune block and undo files.\n");
        nPruneTarget = std::numeric_limits<uint64_t>::max();
        fPruneMode = true;
    } else if (nPruneTarget) {
        if (gArgs.GetBoolArg("-regtest", false)) {
            // we use 1MB blocks to test this on regtest
            if (nPruneTarget < 550 * 1024 * 1024) {
                return InitError(strprintf(_("Prune configured below the minimum of %d MiB.  Please use a higher number."), 550));
            }
        } else {
            if (nPruneTarget < MIN_DISK_SPACE_FOR_BLOCK_FILES) {
                return InitError(strprintf(_("Prune configured below the minimum of %d MiB.  Please use a higher number."), MIN_DISK_SPACE_FOR_BLOCK_FILES / 1024 / 1024));
            }
        }
        LogPrintf("Prune configured to target %uMiB on disk for block and undo files.\n", nPruneTarget / 1024 / 1024);
        fPruneMode = true;
    }

    RegisterAllCoreRPCCommands(tableRPC);
#ifdef ENABLE_WALLET
    RegisterWalletRPCCommands(tableRPC);
#endif

    nConnectTimeout = gArgs.GetArg("-timeout", DEFAULT_CONNECT_TIMEOUT);
    if (nConnectTimeout <= 0)
        nConnectTimeout = DEFAULT_CONNECT_TIMEOUT;

    if (gArgs.IsArgSet("-minrelaytxfee")) {
        CAmount n = 0;
        if (!ParseMoney(gArgs.GetArg("-minrelaytxfee", ""), n)) {
            return InitError(AmountErrMsg("minrelaytxfee", gArgs.GetArg("-minrelaytxfee", "")));
        }
        // High fee check is done afterward in CWallet::ParameterInteraction()
        ::minRelayTxFee = CFeeRate(n);
    } else if (incrementalRelayFee > ::minRelayTxFee) {
        // Allow only setting incrementalRelayFee to control both
        ::minRelayTxFee = incrementalRelayFee;
        LogPrintf("Increasing minrelaytxfee to %s to match incrementalrelayfee\n", ::minRelayTxFee.ToString());
    }

    // Sanity check argument for min fee for including tx in block
    // TODO: Harmonize which arguments need sanity checking and where that happens
    if (gArgs.IsArgSet("-blockmintxfee")) {
        CAmount n = 0;
        if (!ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n))
            return InitError(AmountErrMsg("blockmintxfee", gArgs.GetArg("-blockmintxfee", "")));
    }

    // Feerate used to define dust.  Shouldn't be changed lightly as old
    // implementations may inadvertently create non-standard transactions
    if (gArgs.IsArgSet("-dustrelayfee")) {
        CAmount n = 0;
        if (!ParseMoney(gArgs.GetArg("-dustrelayfee", ""), n) || 0 == n)
            return InitError(AmountErrMsg("dustrelayfee", gArgs.GetArg("-dustrelayfee", "")));
        dustRelayFee = CFeeRate(n);
    }

    fRequireStandard = !gArgs.GetBoolArg("-acceptnonstdtxn", !chainparams.RequireStandard());
    if (chainparams.RequireStandard() && !fRequireStandard)
        return InitError(strprintf("acceptnonstdtxn is not currently supported for %s chain", chainparams.NetworkIDString()));
    nBytesPerSigOp = gArgs.GetArg("-bytespersigop", nBytesPerSigOp);

#ifdef ENABLE_WALLET
    if (!CWallet::ParameterInteraction())
        return false;
#endif // ENABLE_WALLET

    fIsBareMultisigStd = gArgs.GetBoolArg("-permitbaremultisig", DEFAULT_PERMIT_BAREMULTISIG);
    fAcceptDatacarrier = gArgs.GetBoolArg("-datacarrier", DEFAULT_ACCEPT_DATACARRIER);
    nMaxDatacarrierBytes = gArgs.GetArg("-datacarriersize", nMaxDatacarrierBytes);

    // Option to startup with mocktime set (used for regression testing):
    SetMockTime(gArgs.GetArg("-mocktime", 0)); // SetMockTime(0) is a no-op

    if (gArgs.GetBoolArg("-peerbloomfilters", DEFAULT_PEERBLOOMFILTERS))
        nLocalServices = ServiceFlags(nLocalServices | NODE_BLOOM);

    nMaxTipAge = gArgs.GetArg("-maxtipage", DEFAULT_MAX_TIP_AGE);

    if (gArgs.IsArgSet("-vbparams")) {
        // Allow overriding version bits parameters for testing
        if (!chainparams.MineBlocksOnDemand()) {
            return InitError("Version bits parameters may only be overridden on regtest.");
        }
        for (const std::string& strDeployment : gArgs.GetArgs("-vbparams")) {
            std::vector<std::string> vDeploymentParams;
            boost::split(vDeploymentParams, strDeployment, boost::is_any_of(":"));
            if (vDeploymentParams.size() != 3 && vDeploymentParams.size() != 5) {
                return InitError("Version bits parameters malformed, expecting deployment:start:end or deployment:start:end:window:threshold");
            }
            int64_t nStartTime, nTimeout, nWindowSize = -1, nThreshold = -1;
            if (!ParseInt64(vDeploymentParams[1], &nStartTime)) {
                return InitError(strprintf("Invalid nStartTime (%s)", vDeploymentParams[1]));
            }
            if (!ParseInt64(vDeploymentParams[2], &nTimeout)) {
                return InitError(strprintf("Invalid nTimeout (%s)", vDeploymentParams[2]));
            }
            if (vDeploymentParams.size() == 5) {
                if (!ParseInt64(vDeploymentParams[3], &nWindowSize)) {
                    return InitError(strprintf("Invalid nWindowSize (%s)", vDeploymentParams[3]));
                }
                if (!ParseInt64(vDeploymentParams[4], &nThreshold)) {
                    return InitError(strprintf("Invalid nThreshold (%s)", vDeploymentParams[4]));
                }
            }
            bool found = false;
            for (int j = 0; j < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++j) {
                if (vDeploymentParams[0].compare(VersionBitsDeploymentInfo[j].name) == 0) {
                    UpdateVersionBitsParameters(Consensus::DeploymentPos(j), nStartTime, nTimeout, nWindowSize, nThreshold);
                    found = true;
                    LogPrintf("Setting version bits activation parameters for %s to start=%ld, timeout=%ld, window=%ld, threshold=%ld\n", vDeploymentParams[0], nStartTime, nTimeout, nWindowSize, nThreshold);
                    break;
                }
            }
            if (!found) {
                return InitError(strprintf("Invalid deployment (%s)", vDeploymentParams[0]));
            }
        }
    }

    /*if (gArgs.IsArgSet("-dip3params")) {
        // Allow overriding budget parameters for testing
        if (!chainparams.MineBlocksOnDemand()) {
            return InitError("DIP3 parameters may only be overridden on regtest.");
        }
        std::string strDIP3Params = gArgs.GetArg("-dip3params", "");
        std::vector<std::string> vDIP3Params;
        boost::split(vDIP3Params, strDIP3Params, boost::is_any_of(":"));
        if (vDIP3Params.size() != 2) {
            return InitError("DIP3 parameters malformed, expecting DIP3ActivationHeight:DIP3EnforcementHeight");
        }
        int nDIP3ActivationHeight, nDIP3EnforcementHeight;
        if (!ParseInt32(vDIP3Params[0], &nDIP3ActivationHeight)) {
            return InitError(strprintf("Invalid nDIP3ActivationHeight (%s)", vDIP3Params[0]));
        }
        if (!ParseInt32(vDIP3Params[1], &nDIP3EnforcementHeight)) {
            return InitError(strprintf("Invalid nDIP3EnforcementHeight (%s)", vDIP3Params[1]));
        }
        UpdateDIP3Parameters(nDIP3ActivationHeight, nDIP3EnforcementHeight);
    }*/

    if (gArgs.IsArgSet("-budgetparams")) {
        // Allow overriding budget parameters for testing
        if (!chainparams.MineBlocksOnDemand()) {
            return InitError("Budget parameters may only be overridden on regtest.");
        }

        std::string strBudgetParams = gArgs.GetArg("-budgetparams", "");
        std::vector<std::string> vBudgetParams;
        boost::split(vBudgetParams, strBudgetParams, boost::is_any_of(":"));
        if (vBudgetParams.size() != 3) {
            return InitError("Budget parameters malformed, expecting smartnodePaymentsStartBlock:budgetPaymentsStartBlock:superblockStartBlock");
        }
        int nSmartnodePaymentsStartBlock, nBudgetPaymentsStartBlock, nSuperblockStartBlock;
        if (!ParseInt32(vBudgetParams[0], &nSmartnodePaymentsStartBlock)) {
            return InitError(strprintf("Invalid nSmartnodePaymentsStartBlock (%s)", vBudgetParams[0]));
        }
        if (!ParseInt32(vBudgetParams[1], &nBudgetPaymentsStartBlock)) {
            return InitError(strprintf("Invalid nBudgetPaymentsStartBlock (%s)", vBudgetParams[1]));
        }
        if (!ParseInt32(vBudgetParams[2], &nSuperblockStartBlock)) {
            return InitError(strprintf("Invalid nSuperblockStartBlock (%s)", vBudgetParams[2]));
        }
        UpdateBudgetParameters(nSmartnodePaymentsStartBlock, nBudgetPaymentsStartBlock, nSuperblockStartBlock);
    }

    if (chainparams.NetworkIDString() == CBaseChainParams::DEVNET) {
        int nMinimumDifficultyBlocks = gArgs.GetArg("-minimumdifficultyblocks", chainparams.GetConsensus().nMinimumDifficultyBlocks);
        int nHighSubsidyBlocks = gArgs.GetArg("-highsubsidyblocks", chainparams.GetConsensus().nHighSubsidyBlocks);
        int nHighSubsidyFactor = gArgs.GetArg("-highsubsidyfactor", chainparams.GetConsensus().nHighSubsidyFactor);
        UpdateDevnetSubsidyAndDiffParams(nMinimumDifficultyBlocks, nHighSubsidyBlocks, nHighSubsidyFactor);
    } else if (gArgs.IsArgSet("-minimumdifficultyblocks") || gArgs.IsArgSet("-highsubsidyblocks") || gArgs.IsArgSet("-highsubsidyfactor")) {
        return InitError("Difficulty and subsidy parameters may only be overridden on devnet.");
    }

    if (chainparams.NetworkIDString() == CBaseChainParams::DEVNET) {
        std::string llmqTypeChainLocks = gArgs.GetArg("-llmqchainlocks", Params().GetConsensus().llmqs.at(Params().GetConsensus().llmqTypeChainLocks).name);
        Consensus::LLMQType llmqType = Consensus::LLMQ_NONE;
        for (const auto& p : Params().GetConsensus().llmqs) {
            if (p.second.name == llmqTypeChainLocks) {
                llmqType = p.first;
                break;
            }
        }
        if (llmqType == Consensus::LLMQ_NONE) {
            return InitError("Invalid LLMQ type specified for -llmqchainlocks.");
        }
        UpdateDevnetLLMQChainLocks(llmqType);
    } else if (gArgs.IsArgSet("-llmqchainlocks")) {
        return InitError("LLMQ type for ChainLocks can only be overridden on devnet.");
    }

    if (gArgs.IsArgSet("-maxorphantx")) {
        InitWarning("-maxorphantx is not supported anymore. Use -maxorphantxsize instead.");
    }

    if (gArgs.IsArgSet("-smartnode")) {
        InitWarning(_("-smartnode option is deprecated and ignored, specifying -smartnodeblsprivkey is enough to start this node as a smartnode."));
    }

    return true;
}

static bool LockDataDirectory(bool probeOnly)
{
    std::string strDataDir = GetDataDir().string();

    // Make sure only a single Memeium Core process is using the data directory.
    fs::path pathLockFile = GetDataDir() / ".lock";
    FILE* file = fsbridge::fopen(pathLockFile, "a"); // empty lock file; created if it doesn't exist.
    if (file) fclose(file);

    try {
        static boost::interprocess::file_lock lock(pathLockFile.string().c_str());
        if (!lock.try_lock()) {
            return InitError(strprintf(_("Cannot obtain a lock on data directory %s. %s is probably already running."), strDataDir, _(PACKAGE_NAME)));
        }
        if (probeOnly) {
            lock.unlock();
        }
    } catch (const boost::interprocess::interprocess_exception& e) {
        return InitError(strprintf(_("Cannot obtain a lock on data directory %s. %s is probably already running.") + " %s.", strDataDir, _(PACKAGE_NAME), e.what()));
    }
    return true;
}

bool AppInitSanityChecks()
{
    // ********************************************************* Step 4: sanity checks

    // Initialize elliptic curve code
    std::string sha256_algo = SHA256AutoDetect();
    LogPrintf("Using the '%s' SHA256 implementation\n", sha256_algo);
    RandomInit();
    ECC_Start();
    globalVerifyHandle.reset(new ECCVerifyHandle());

    // Sanity check
    if (!InitSanityCheck())
        return InitError(strprintf(_("Initialization sanity check failed. %s is shutting down."), _(PACKAGE_NAME)));

    // Probe the data directory lock to give an early error message, if possible
    // We cannot hold the data directory lock here, as the forking for daemon() hasn't yet happened,
    // and a fork will cause weird behavior to it.
    return LockDataDirectory(true);
}

bool AppInitLockDataDirectory()
{
    // After daemonization get the data directory lock again and hold on to it until exit
    // This creates a slight window for a race condition to happen, however this condition is harmless: it
    // will at most make us exit without printing a message to console.
    if (!LockDataDirectory(false)) {
        // Detailed error printed inside LockDataDirectory
        return false;
    }
    return true;
}

bool AppInitMain(boost::thread_group& threadGroup, CScheduler& scheduler)
{
    const CChainParams& chainparams = Params();
    // ********************************************************* Step 4a: application initialization
#ifndef WIN32
    CreatePidFile(GetPidFile(), getpid());
#endif
    if (gArgs.GetBoolArg("-shrinkdebugfile", logCategories == BCLog::NONE)) {
        // Do this first since it both loads a bunch of debug.log into memory,
        // and because this needs to happen before any other debug.log printing
        ShrinkDebugFile();
    }

    if (fPrintToDebugLog)
        OpenDebugLog();

    if (!fLogTimestamps)
        LogPrintf("Startup time: %s\n", DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime()));
    LogPrintf("Default data directory %s\n", GetDefaultDataDir().string());
    LogPrintf("Using data directory %s\n", GetDataDir().string());
    LogPrintf("Using config file %s\n", GetConfigFile(gArgs.GetArg("-conf", BITCOIN_CONF_FILENAME)).string());
    LogPrintf("Using at most %i automatic connections (%i file descriptors available)\n", nMaxConnections, nFD);

    InitSignatureCache();
    InitScriptExecutionCache();

    LogPrintf("Using %u threads for script verification\n", nScriptCheckThreads);
    if (nScriptCheckThreads) {
        for (int i = 0; i < nScriptCheckThreads - 1; i++)
            threadGroup.create_thread(&ThreadScriptCheck);
    }

    std::vector<std::string> vSporkAddresses;
    if (gArgs.IsArgSet("-sporkaddr")) {
        vSporkAddresses = gArgs.GetArgs("-sporkaddr");
    } else {
        vSporkAddresses = Params().SporkAddresses();
    }
    for (const auto& address : vSporkAddresses) {
        if (!sporkManager.SetSporkAddress(address)) {
            return InitError(_("Invalid spork address specified with -sporkaddr"));
        }
    }

    int minsporkkeys = gArgs.GetArg("-minsporkkeys", Params().MinSporkKeys());
    if (!sporkManager.SetMinSporkKeys(minsporkkeys)) {
        return InitError(_("Invalid minimum number of spork signers specified with -minsporkkeys"));
    }


    if (gArgs.IsArgSet("-sporkkey")) { // spork priv key
        if (!sporkManager.SetPrivKey(gArgs.GetArg("-sporkkey", ""))) {
            return InitError(_("Unable to sign spork message, wrong key?"));
        }
    }

    // Start the lightweight task scheduler thread
    CScheduler::Function serviceLoop = boost::bind(&CScheduler::serviceQueue, &scheduler);
    threadGroup.create_thread(boost::bind(&TraceThread<CScheduler::Function>, "scheduler", serviceLoop));

    GetMainSignals().RegisterBackgroundSignalScheduler(scheduler);

    /* Start the RPC server already.  It will be started in "warmup" mode
     * and not really process calls already (but it will signify connections
     * that the server is there and will be ready later).  Warmup mode will
     * be disabled when initialisation is finished.
     */
    if (gArgs.GetBoolArg("-server", false)) {
        uiInterface.InitMessage.connect(SetRPCWarmupStatus);
        if (!AppInitServers(threadGroup))
            return InitError(_("Unable to start HTTP server. See debug log for details."));
    }

    // ********************************************************* Step 5: Backup wallet and verify wallet database integrity
#ifdef ENABLE_WALLET
    if (!CWallet::InitAutoBackup())
        return false;

    if (!CWallet::Verify())
        return false;

    // Initialize KeePass Integration
    keePassInt.init();
#endif // ENABLE_WALLET
    bool fGenerate = gArgs.GetBoolArg("-regtest", false) ? false : DEFAULT_GENERATE;
    GenerateMemeiums(fGenerate, gArgs.GetArg("-genproclimit", DEFAULT_GENERATE_THREADS), chainparams);
    // ********************************************************* Step 6: network initialization
    // Note that we absolutely cannot open any actual connections
    // until the very end ("start node") as the UTXO/block state
    // is not yet setup and may end up being set up twice if we
    // need to reindex later.
    assert(!g_connman);
    g_connman = std::unique_ptr<CConnman>(new CConnman(GetRand(std::numeric_limits<uint64_t>::max()), GetRand(std::numeric_limits<uint64_t>::max())));
    CConnman& connman = *g_connman;

    peerLogic.reset(new PeerLogicValidation(&connman, scheduler));
    RegisterValidationInterface(peerLogic.get());

    // sanitize comments per BIP-0014, format user agent and check total size
    std::vector<std::string> uacomments;

    if (chainparams.NetworkIDString() == CBaseChainParams::DEVNET) {
        // Add devnet name to user agent. This allows to disconnect nodes immediately if they don't belong to our own devnet
        uacomments.push_back(strprintf("devnet=%s", GetDevNetName()));
    }

    for (const std::string& cmt : gArgs.GetArgs("-uacomment")) {
        if (cmt != SanitizeString(cmt, SAFE_CHARS_UA_COMMENT))
            return InitError(strprintf(_("User Agent comment (%s) contains unsafe characters."), cmt));
        uacomments.push_back(cmt);
    }
    strSubVersion = FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, uacomments);
    if (strSubVersion.size() > MAX_SUBVERSION_LENGTH) {
        return InitError(strprintf(_("Total length of network version string (%i) exceeds maximum length (%i). Reduce the number or size of uacomments."),
            strSubVersion.size(), MAX_SUBVERSION_LENGTH));
    }

    if (gArgs.IsArgSet("-onlynet")) {
        std::set<enum Network> nets;
        for (const std::string& snet : gArgs.GetArgs("-onlynet")) {
            enum Network net = ParseNetwork(snet);
            if (net == NET_UNROUTABLE)
                return InitError(strprintf(_("Unknown network specified in -onlynet: '%s'"), snet));
            nets.insert(net);
        }
        for (int n = 0; n < NET_MAX; n++) {
            enum Network net = (enum Network)n;
            if (!nets.count(net))
                SetLimited(net);
        }
    }

    // Check for host lookup allowed before parsing any network related parameters
    fNameLookup = gArgs.GetBoolArg("-dns", DEFAULT_NAME_LOOKUP);

    bool proxyRandomize = gArgs.GetBoolArg("-proxyrandomize", DEFAULT_PROXYRANDOMIZE);
    // -proxy sets a proxy for all outgoing network traffic
    // -noproxy (or -proxy=0) as well as the empty string can be used to not set a proxy, this is the default
    std::string proxyArg = gArgs.GetArg("-proxy", "");
    SetLimited(NET_TOR);
    if (proxyArg != "" && proxyArg != "0") {
        CService proxyAddr;
        if (!Lookup(proxyArg.c_str(), proxyAddr, 9050, fNameLookup)) {
            return InitError(strprintf(_("Invalid -proxy address or hostname: '%s'"), proxyArg));
        }

        proxyType addrProxy = proxyType(proxyAddr, proxyRandomize);
        if (!addrProxy.IsValid())
            return InitError(strprintf(_("Invalid -proxy address or hostname: '%s'"), proxyArg));

        SetProxy(NET_IPV4, addrProxy);
        SetProxy(NET_IPV6, addrProxy);
        SetProxy(NET_TOR, addrProxy);
        SetNameProxy(addrProxy);
        SetLimited(NET_TOR, false); // by default, -proxy sets onion as reachable, unless -noonion later
    }

    // -onion can be used to set only a proxy for .onion, or override normal proxy for .onion addresses
    // -noonion (or -onion=0) disables connecting to .onion entirely
    // An empty string is used to not override the onion proxy (in which case it defaults to -proxy set above, or none)
    std::string onionArg = gArgs.GetArg("-onion", "");
    if (onionArg != "") {
        if (onionArg == "0") {   // Handle -noonion/-onion=0
            SetLimited(NET_TOR); // set onions as unreachable
        } else {
            CService onionProxy;
            if (!Lookup(onionArg.c_str(), onionProxy, 9050, fNameLookup)) {
                return InitError(strprintf(_("Invalid -onion address or hostname: '%s'"), onionArg));
            }
            proxyType addrOnion = proxyType(onionProxy, proxyRandomize);
            if (!addrOnion.IsValid())
                return InitError(strprintf(_("Invalid -onion address or hostname: '%s'"), onionArg));
            SetProxy(NET_TOR, addrOnion);
            SetLimited(NET_TOR, false);
        }
    }

    // see Step 2: parameter interactions for more information about these
    fListen = gArgs.GetBoolArg("-listen", DEFAULT_LISTEN);
    fDiscover = gArgs.GetBoolArg("-discover", true);
    fRelayTxes = !gArgs.GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY);

    for (const std::string& strAddr : gArgs.GetArgs("-externalip")) {
        CService addrLocal;
        if (Lookup(strAddr.c_str(), addrLocal, GetListenPort(), fNameLookup) && addrLocal.IsValid())
            AddLocal(addrLocal, LOCAL_MANUAL);
        else
            return InitError(ResolveErrMsg("externalip", strAddr));
    }

#if ENABLE_ZMQ
    pzmqNotificationInterface = CZMQNotificationInterface::Create();

    if (pzmqNotificationInterface) {
        RegisterValidationInterface(pzmqNotificationInterface);
    }
#endif

    pdsNotificationInterface = new CDSNotificationInterface(connman);
    RegisterValidationInterface(pdsNotificationInterface);

    uint64_t nMaxOutboundLimit = 0; // unlimited unless -maxuploadtarget is set
    uint64_t nMaxOutboundTimeframe = MAX_UPLOAD_TIMEFRAME;

    if (gArgs.IsArgSet("-maxuploadtarget")) {
        nMaxOutboundLimit = gArgs.GetArg("-maxuploadtarget", DEFAULT_MAX_UPLOAD_TARGET) * 1024 * 1024;
    }

    // ********************************************************* Step 7a: check lite mode and load sporks

    // lite mode disables all Memeium-specific functionality
    fLiteMode = gArgs.GetBoolArg("-litemode", false);
    LogPrintf("fLiteMode %d\n", fLiteMode);

    if (fLiteMode) {
        InitWarning(_("You are starting in lite mode, most Memeium-specific functionality is disabled."));
    }

    if ((!fLiteMode && fTxIndex == false) && chainparams.NetworkIDString() != CBaseChainParams::REGTEST) { // TODO remove this when pruning is fixed. See https://github.com/The-Memeium-Endeavor/memeium/pull/1817 and https://github.com/The-Memeium-Endeavor/memeium/pull/1743
        return InitError(_("Transaction index can't be disabled in full mode. Either start with -litemode command line switch or enable transaction index."));
    }

    if (!fLiteMode) {
        uiInterface.InitMessage(_("Loading sporks cache..."));
        CFlatDB<CSporkManager> flatdb6("sporks.dat", "magicSporkCache");
        if (!flatdb6.Load(sporkManager)) {
            return InitError(_("Failed to load sporks cache from") + "\n" + (GetDataDir() / "sporks.dat").string());
        }
    }

    // ********************************************************* Step 7b: load block chain

    fReindex = gArgs.GetBoolArg("-reindex", false);
    bool fReindexChainState = gArgs.GetBoolArg("-reindex-chainstate", false);

    // cache size calculations
    int64_t nTotalCache = (gArgs.GetArg("-dbcache", nDefaultDbCache) << 20);
    nTotalCache = std::max(nTotalCache, nMinDbCache << 20); // total cache cannot be less than nMinDbCache
    nTotalCache = std::min(nTotalCache, nMaxDbCache << 20); // total cache cannot be greater than nMaxDbcache
    int64_t nBlockTreeDBCache = nTotalCache / 8;
    nBlockTreeDBCache = std::min(nBlockTreeDBCache, (gArgs.GetBoolArg("-txindex", DEFAULT_TXINDEX) ? nMaxBlockDBAndTxIndexCache : nMaxBlockDBCache) << 20);
    nTotalCache -= nBlockTreeDBCache;
    int64_t nCoinDBCache = std::min(nTotalCache / 2, (nTotalCache / 4) + (1 << 23)); // use 25%-50% of the remainder for disk cache
    nCoinDBCache = std::min(nCoinDBCache, nMaxCoinsDBCache << 20);                   // cap total coins db cache
    nTotalCache -= nCoinDBCache;
    nCoinCacheUsage = nTotalCache; // the rest goes to in-memory cache
    int64_t nMempoolSizeMax = gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
    int64_t nEvoDbCache = 1024 * 1024 * 16; // TODO
    LogPrintf("Cache configuration:\n");
    LogPrintf("* Using %.1fMiB for block index database\n", nBlockTreeDBCache * (1.0 / 1024 / 1024));
    LogPrintf("* Using %.1fMiB for chain state database\n", nCoinDBCache * (1.0 / 1024 / 1024));
    LogPrintf("* Using %.1fMiB for in-memory UTXO set (plus up to %.1fMiB of unused mempool space)\n", nCoinCacheUsage * (1.0 / 1024 / 1024), nMempoolSizeMax * (1.0 / 1024 / 1024));

    bool fLoaded = false;
    int64_t nStart = GetTimeMillis();

    while (!fLoaded && !fRequestShutdown) {
        bool fReset = fReindex;
        std::string strLoadError;

        uiInterface.InitMessage(_("Loading block index..."));

        nStart = GetTimeMillis();
        do {
            try {
                UnloadBlockIndex();
                delete pcoinsTip;
                delete pcoinsdbview;
                delete pcoinscatcher;
                delete pblocktree;

                llmq::DestroyLLMQSystem();
                delete deterministicMNManager;
                delete evoDb;

                evoDb = new CEvoDB(nEvoDbCache, false, fReset || fReindexChainState);
                deterministicMNManager = new CDeterministicMNManager(*evoDb);
                pblocktree = new CBlockTreeDB(nBlockTreeDBCache, false, fReset);
                llmq::InitLLMQSystem(*evoDb, &scheduler, false, fReset || fReindexChainState);


                /** MMM START */
                {
                    // Basic assets
                    delete passets;
                    delete passetsdb;
                    delete passetsCache;

                    // Messaging assets
                    delete pmessagedb;
                    delete pmessagechanneldb;
                    delete pMessagesCache;
                    delete pMessagesSeenAddressCache;
                    delete pMessageSubscribedChannelsCache;

                    // My restricted assets
                    delete pmyrestricteddb;

                    // Restricted assets
                    delete prestricteddb;
                    delete passetsVerifierCache;
                    delete passetsQualifierCache;
                    delete passetsRestrictionCache;
                    delete passetsGlobalRestrictionCache;

                    //  Rewards
                    delete pSnapshotRequestDb;
                    delete pAssetSnapshotDb;
                    delete pDistributeSnapshotDb;

                    // Basic assets
                    passetsdb = new CAssetsDB(nBlockTreeDBCache, false, fReset);
                    passets = new CAssetsCache();
                    passetsCache = new CLRUCache<std::string, CDatabasedAssetData>(MAX_CACHE_ASSETS_SIZE);

                    // Messaging assets
                    pMessagesCache = new CLRUCache<std::string, CMessage>(1000);
                    pMessageSubscribedChannelsCache = new CLRUCache<std::string, int>(1000);
                    pMessagesSeenAddressCache = new CLRUCache<std::string, int>(1000);
                    pmessagedb = new CMessageDB(nBlockTreeDBCache, false, false);
                    pmessagechanneldb = new CMessageChannelDB(nBlockTreeDBCache, false, false);

                    // My restricted assets
                    pmyrestricteddb = new CMyRestrictedDB(nBlockTreeDBCache, false, false);

                    // Restricted assets
                    prestricteddb = new CRestrictedDB(nBlockTreeDBCache, false, fReset);
                    passetsVerifierCache = new CLRUCache<std::string, CNullAssetTxVerifierString>(
                        MAX_CACHE_ASSETS_SIZE);
                    passetsQualifierCache = new CLRUCache<std::string, int8_t>(MAX_CACHE_ASSETS_SIZE);
                    passetsRestrictionCache = new CLRUCache<std::string, int8_t>(MAX_CACHE_ASSETS_SIZE);
                    passetsGlobalRestrictionCache = new CLRUCache<std::string, int8_t>(MAX_CACHE_ASSETS_SIZE);

                    // Rewards
                    pSnapshotRequestDb = new CSnapshotRequestDB(nBlockTreeDBCache, false, false);
                    pAssetSnapshotDb = new CAssetSnapshotDB(nBlockTreeDBCache, false, false);
                    pDistributeSnapshotDb = new CDistributeSnapshotRequestDB(nBlockTreeDBCache, false, false);

                    // Read for fAssetIndex to make sure that we only load asset address balances if it if true
                    pblocktree->ReadFlag("assetindex", fAssetIndex);
                    // Need to load assets before we verify the database
                    if (!passetsdb->LoadAssets()) {
                        strLoadError = _("Failed to load Assets Database");
                        break;
                    }

                    if (!passetsdb->ReadReissuedMempoolState())
                        LogPrintf(
                            "Database failed to load last Reissued Mempool State. Will have to start from empty state");

                    LogPrintf("Successfully loaded assets from database.\nCache of assets size: %d\n",
                        passetsCache->Size());

                    // Check for changed -disablemessaging state
                    if (gArgs.GetArg("-disablemessaging", false)) {
                        LogPrintf("Messaging is disabled\n");
                        fMessaging = false;
                    } else {
                        LogPrintf("Messaging is enabled\n");
                    }
                }
                /** MMM END */

                if (fReset) {
                    pblocktree->WriteReindexing(true);
                    // If we're reindexing in prune mode, wipe away unusable block files and all undo data files
                    if (fPruneMode)
                        CleanupBlockRevFiles();
                }

                if (fRequestShutdown) break;

                // LoadBlockIndex will load fTxIndex from the db, or set it if
                // we're reindexing. It will also load fHavePruned if we've
                // ever removed a block file from disk.
                // Note that it also sets fReindex based on the disk flag!
                // From here on out fReindex and fReset mean something different!
                if (!LoadBlockIndex(chainparams)) {
                    strLoadError = _("Error loading block database");
                    break;
                }

                // If the loaded chain has a wrong genesis, bail out immediately
                // (we're likely using a testnet datadir, or the other way around).
                if (!mapBlockIndex.empty() && mapBlockIndex.count(chainparams.GetConsensus().hashGenesisBlock) == 0)
                    return InitError(_("Incorrect or no genesis block found. Wrong datadir for network?"));

                if (!chainparams.GetConsensus().hashDevnetGenesisBlock.IsNull() && !mapBlockIndex.empty() && mapBlockIndex.count(chainparams.GetConsensus().hashDevnetGenesisBlock) == 0)
                    return InitError(_("Incorrect or no devnet genesis block found. Wrong datadir for devnet specified?"));

                // Check for changed -txindex state
                if (fTxIndex != gArgs.GetBoolArg("-txindex", DEFAULT_TXINDEX)) {
                    strLoadError = _("You need to rebuild the database using -reindex to change -txindex");
                    break;
                }

                // Check for changed -addressindex state
                if (fAddressIndex != gArgs.GetBoolArg("-addressindex", DEFAULT_ADDRESSINDEX)) {
                    strLoadError = _("You need to rebuild the database using -reindex to change -addressindex");
                    break;
                }

                // Check for changed -timestampindex state
                if (fTimestampIndex != gArgs.GetBoolArg("-timestampindex", DEFAULT_TIMESTAMPINDEX)) {
                    strLoadError = _("You need to rebuild the database using -reindex to change -timestampindex");
                    break;
                }

                // Check for changed -spentindex state
                if (fSpentIndex != gArgs.GetBoolArg("-spentindex", DEFAULT_SPENTINDEX)) {
                    strLoadError = _("You need to rebuild the database using -reindex to change -spentindex");
                    break;
                }

                // Check for changed -prune state.  What we are concerned about is a user who has pruned blocks
                // in the past, but is now trying to run unpruned.
                if (fHavePruned && !fPruneMode) {
                    strLoadError = _("You need to rebuild the database using -reindex to go back to unpruned mode.  This will redownload the entire blockchain");
                    break;
                }

                // At this point blocktree args are consistent with what's on disk.
                // If we're not mid-reindex (based on disk + args), add a genesis block on disk
                // (otherwise we use the one already on disk).
                // This is called again in ThreadImport after the reindex completes.
                if (!fReindex && !LoadGenesisBlock(chainparams)) {
                    strLoadError = _("Error initializing block database");
                    break;
                }

                // At this point we're either in reindex or we've loaded a useful
                // block tree into mapBlockIndex!

                pcoinsdbview = new CCoinsViewDB(nCoinDBCache, false, fReset || fReindexChainState);
                pcoinscatcher = new CCoinsViewErrorCatcher(pcoinsdbview);

                // If necessary, upgrade from older database format.
                // This is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
                if (!pcoinsdbview->Upgrade()) {
                    strLoadError = _("Error upgrading chainstate database");
                    break;
                }

                // ReplayBlocks is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
                if (!ReplayBlocks(chainparams, pcoinsdbview)) {
                    strLoadError = _("Unable to replay blocks. You will need to rebuild the database using -reindex-chainstate.");
                    break;
                }

                // The on-disk coinsdb is now in a good state, create the cache
                pcoinsTip = new CCoinsViewCache(pcoinscatcher);

                bool is_coinsview_empty = fReset || fReindexChainState || pcoinsTip->GetBestBlock().IsNull();
                if (!is_coinsview_empty) {
                    // LoadChainTip sets chainActive based on pcoinsTip's best block
                    if (!LoadChainTip(chainparams)) {
                        strLoadError = _("Error initializing block database");
                        break;
                    }
                    assert(chainActive.Tip() != NULL);
                }

                // deterministicMNManager->UpgradeDBIfNeeded();

                if (!is_coinsview_empty) {
                    uiInterface.InitMessage(_("Verifying blocks..."));
                    if (fHavePruned && gArgs.GetArg("-checkblocks", DEFAULT_CHECKBLOCKS) > MIN_BLOCKS_TO_KEEP) {
                        LogPrintf("Prune: pruned datadir may not have more than %d blocks; only checking available blocks",
                            MIN_BLOCKS_TO_KEEP);
                    }

                    {
                        LOCK(cs_main);
                        CBlockIndex* tip = chainActive.Tip();
                        RPCNotifyBlockChange(true, tip);
                        if (tip && tip->nTime > GetAdjustedTime() + 2 * 60 * 60) {
                            strLoadError = _("The block database contains a block which appears to be from the future. "
                                             "This may be due to your computer's date and time being set incorrectly. "
                                             "Only rebuild the block database if you are sure that your computer's date and time are correct");
                            break;
                        }
                    }

                    if (!CVerifyDB().VerifyDB(chainparams, pcoinsdbview, gArgs.GetArg("-checklevel", DEFAULT_CHECKLEVEL),
                            gArgs.GetArg("-checkblocks", DEFAULT_CHECKBLOCKS))) {
                        strLoadError = _("Corrupted block database detected");
                        break;
                    }
                }
            } catch (const std::exception& e) {
                LogPrintf("%s\n", e.what());
                strLoadError = _("Error opening block database");
                break;
            }

            fLoaded = true;
        } while (false);

        if (!fLoaded && !fRequestShutdown) {
            // first suggest a reindex
            if (!fReset) {
                bool fRet = uiInterface.ThreadSafeQuestion(
                    strLoadError + ".\n\n" + _("Do you want to rebuild the block database now?"),
                    strLoadError + ".\nPlease restart with -reindex or -reindex-chainstate to recover.",
                    "", CClientUIInterface::MSG_ERROR | CClientUIInterface::BTN_ABORT);
                if (fRet) {
                    fReindex = true;
                    fRequestShutdown = false;
                } else {
                    LogPrintf("Aborted block database rebuild. Exiting.\n");
                    return false;
                }
            } else {
                return InitError(strLoadError);
            }
        }
    }

    // As LoadBlockIndex can take several minutes, it's possible the user
    // requested to kill the GUI during the last operation. If so, exit.
    // As the program has not fully started yet, Shutdown() is possibly overkill.
    if (fRequestShutdown) {
        LogPrintf("Shutdown requested. Exiting.\n");
        return false;
    }
    if (fLoaded) {
        LogPrintf(" block index %15dms\n", GetTimeMillis() - nStart);
    }

    fs::path est_path = GetDataDir() / FEE_ESTIMATES_FILENAME;
    CAutoFile est_filein(fsbridge::fopen(est_path, "rb"), SER_DISK, CLIENT_VERSION);
    // Allowed to fail as this file IS missing on first startup.
    if (!est_filein.IsNull())
        ::feeEstimator.Read(est_filein);
    fFeeEstimatesInitialized = true;

    // ********************************************************* Step 8a: load powcache.dat

    {
        fs::path pathDB = GetDataDir();
        std::string strDBName = "powcache.dat";

        // Always load the powcache if available:
        uiInterface.InitMessage(_("Loading POW cache..."));
        CFlatDB<CPowCache> flatdb7(strDBName, "powCache");
        if (!flatdb7.Load(CPowCache::Instance())) {
            return InitError(_("Failed to load POW cache from") + "\n" + (pathDB / strDBName).string());
        }
    }

    // ********************************************************* Step 8b: load wallet
#ifdef ENABLE_WALLET
    if (!CWallet::InitLoadWallet())
        return false;
#else
    LogPrintf("No wallet support compiled in!\n");
#endif

    // As InitLoadWallet can take several minutes, it's possible the user
    // requested to kill the GUI during the last operation. If so, exit.
    if (fRequestShutdown) {
        LogPrintf("Shutdown requested. Exiting.\n");
        return false;
    }

    // ********************************************************* Step 9: data directory maintenance

    // if pruning, unset the service bit and perform the initial blockstore prune
    // after any wallet rescanning has taken place.
    if (fPruneMode) {
        LogPrintf("Unsetting NODE_NETWORK on prune mode\n");
        nLocalServices = ServiceFlags(nLocalServices & ~NODE_NETWORK);
        if (!fReindex) {
            uiInterface.InitMessage(_("Pruning blockstore..."));
            PruneAndFlush();
        }
    }

    // As PruneAndFlush can take several minutes, it's possible the user
    // requested to kill the GUI during the last operation. If so, exit.
    if (fRequestShutdown) {
        LogPrintf("Shutdown requested. Exiting.\n");
        return false;
    }

    // ********************************************************* Step 10a: Prepare Smartnode related stuff
    fSmartnodeMode = false;
    std::string strMasterNodeBLSPrivKey = gArgs.GetArg("-smartnodeblsprivkey", "");
    if (!strMasterNodeBLSPrivKey.empty()) {
        auto binKey = ParseHex(strMasterNodeBLSPrivKey);
        CBLSSecretKey keyOperator;
        keyOperator.SetBuf(binKey);
        if (!keyOperator.IsValid()) {
            return InitError(_("Invalid smartnodeblsprivkey. Please see documentation."));
        }
        fSmartnodeMode = true;
        activeSmartnodeInfo.blsKeyOperator = std::make_unique<CBLSSecretKey>(keyOperator);
        activeSmartnodeInfo.blsPubKeyOperator = std::make_unique<CBLSPublicKey>(activeSmartnodeInfo.blsKeyOperator->GetPublicKey());
        LogPrintf("SMARTNODE:\n");
        LogPrintf("  blsPubKeyOperator: %s\n", keyOperator.GetPublicKey().ToString());
    }

    if (fLiteMode && fSmartnodeMode) {
        return InitError(_("You can not start a smartnode in lite mode."));
    }

    if (fSmartnodeMode) {
#ifdef ENABLE_WALLET
        if (!vpwallets.empty()) {
            return InitError(_("You can not start a smartnode with wallet enabled."));
        }
#endif // ENABLE_WALLET
       //  Create and register activeSmartnodeManager, will init later in ThreadImport
        activeSmartnodeManager = new CActiveSmartnodeManager();
        RegisterValidationInterface(activeSmartnodeManager);
    }

    if (activeSmartnodeInfo.blsKeyOperator == nullptr) {
        activeSmartnodeInfo.blsKeyOperator = std::make_unique<CBLSSecretKey>();
    }
    if (activeSmartnodeInfo.blsPubKeyOperator == nullptr) {
        activeSmartnodeInfo.blsPubKeyOperator = std::make_unique<CBLSPublicKey>();
    }

    // ********************************************************* Step 10b: setup PrivateSend

#ifdef ENABLE_WALLET
    int nMaxRounds = MAX_PRIVATESEND_ROUNDS;

    if (vpwallets.empty()) {
        privateSendClient.fEnablePrivateSend = privateSendClient.fPrivateSendRunning = false;
    } else {
        privateSendClient.fEnablePrivateSend = gArgs.GetBoolArg("-enableprivatesend", !fLiteMode);
        privateSendClient.fPrivateSendRunning = vpwallets[0]->IsLocked() ? false : gArgs.GetBoolArg("-privatesendautostart", DEFAULT_PRIVATESEND_AUTOSTART);
    }
    privateSendClient.fPrivateSendMultiSession = gArgs.GetBoolArg("-privatesendmultisession", DEFAULT_PRIVATESEND_MULTISESSION);
    privateSendClient.nPrivateSendSessions = std::min(std::max((int)gArgs.GetArg("-privatesendsessions", DEFAULT_PRIVATESEND_SESSIONS), MIN_PRIVATESEND_SESSIONS), MAX_PRIVATESEND_SESSIONS);
    privateSendClient.nPrivateSendRounds = std::min(std::max((int)gArgs.GetArg("-privatesendrounds", DEFAULT_PRIVATESEND_ROUNDS), MIN_PRIVATESEND_ROUNDS), nMaxRounds);
    privateSendClient.nPrivateSendAmount = std::min(std::max((int)gArgs.GetArg("-privatesendamount", DEFAULT_PRIVATESEND_AMOUNT), MIN_PRIVATESEND_AMOUNT), MAX_PRIVATESEND_AMOUNT);
    privateSendClient.nPrivateSendDenoms = std::min(std::max((int)gArgs.GetArg("-privatesenddenoms", DEFAULT_PRIVATESEND_DENOMS), MIN_PRIVATESEND_DENOMS), MAX_PRIVATESEND_DENOMS);

    if (privateSendClient.fEnablePrivateSend) {
        LogPrintf("PrivateSend: autostart=%d, multisession=%d, "
                  "sessions=%d, rounds=%d, amount=%d, denoms=%d\n",
            privateSendClient.fPrivateSendRunning, privateSendClient.fPrivateSendMultiSession,
            privateSendClient.nPrivateSendSessions, privateSendClient.nPrivateSendRounds,
            privateSendClient.nPrivateSendAmount, privateSendClient.nPrivateSendDenoms);
    }
#endif // ENABLE_WALLET

    CPrivateSend::InitStandardDenominations();

    // ********************************************************* Step 10b: Load cache data

    // LOAD SERIALIZED DAT FILES INTO DATA CACHES FOR INTERNAL USE

    bool fLoadCacheFiles = !(fLiteMode || fReindex || fReindexChainState);
    {
        LOCK(cs_main);
        // was blocks/chainstate deleted?
        if (chainActive.Tip() == nullptr) {
            fLoadCacheFiles = false;
        }
    }
    fs::path pathDB = GetDataDir();
    std::string strDBName;

    strDBName = "mncache.dat";
    uiInterface.InitMessage(_("Loading smartnode cache..."));
    CFlatDB<CSmartnodeMetaMan> flatdb1(strDBName, "magicSmartnodeCache");
    if (fLoadCacheFiles) {
        if (!flatdb1.Load(mmetaman)) {
            return InitError(_("Failed to load smartnode cache from") + "\n" + (pathDB / strDBName).string());
        }
    } else {
        CSmartnodeMetaMan mmetamanTmp;
        if (!flatdb1.Dump(mmetamanTmp)) {
            return InitError(_("Failed to clear smartnode cache at") + "\n" + (pathDB / strDBName).string());
        }
    }

    strDBName = "governance.dat";
    uiInterface.InitMessage(_("Loading governance cache..."));
    CFlatDB<CGovernanceManager> flatdb3(strDBName, "magicGovernanceCache");
    if (fLoadCacheFiles) {
        if (!flatdb3.Load(governance)) {
            return InitError(_("Failed to load governance cache from") + "\n" + (pathDB / strDBName).string());
        }
        governance.InitOnLoad();
    } else {
        CGovernanceManager governanceTmp;
        if (!flatdb3.Dump(governanceTmp)) {
            return InitError(_("Failed to clear governance cache at") + "\n" + (pathDB / strDBName).string());
        }
    }

    strDBName = "netfulfilled.dat";
    uiInterface.InitMessage(_("Loading fulfilled requests cache..."));
    CFlatDB<CNetFulfilledRequestManager> flatdb4(strDBName, "magicFulfilledCache");
    if (fLoadCacheFiles) {
        if (!flatdb4.Load(netfulfilledman)) {
            return InitError(_("Failed to load fulfilled requests cache from") + "\n" + (pathDB / strDBName).string());
        }
    } else {
        CNetFulfilledRequestManager netfulfilledmanTmp;
        if (!flatdb4.Dump(netfulfilledmanTmp)) {
            return InitError(_("Failed to clear fulfilled requests cache at") + "\n" + (pathDB / strDBName).string());
        }
    }


    // ********************************************************* Step 10c: schedule Memeium-specific tasks

    if (!fLiteMode) {
        scheduler.scheduleEvery(boost::bind(&CNetFulfilledRequestManager::DoMaintenance, boost::ref(netfulfilledman)), 60 * 1000);
        scheduler.scheduleEvery(boost::bind(&CSmartnodeSync::DoMaintenance, boost::ref(smartnodeSync), boost::ref(*g_connman)), 1 * 1000);

        scheduler.scheduleEvery(boost::bind(&CGovernanceManager::DoMaintenance, boost::ref(governance), boost::ref(*g_connman)), 60 * 5 * 1000);
    }

    scheduler.scheduleEvery(boost::bind(&CSmartnodeUtils::DoMaintenance, boost::ref(*g_connman)), 1 * 1000);

    // Periodic flush of POW Cache if cache has grown enough
    scheduler.scheduleEvery(boost::bind(&CPowCache::DoMaintenance, &CPowCache::Instance()), 60 * 1000);

    if (fSmartnodeMode) {
        scheduler.scheduleEvery(boost::bind(&CPrivateSendServer::DoMaintenance, boost::ref(privateSendServer), boost::ref(*g_connman)), 1 * 1000);
#ifdef ENABLE_WALLET
    } else if (privateSendClient.fEnablePrivateSend) {
        scheduler.scheduleEvery(boost::bind(&CPrivateSendClientManager::DoMaintenance, boost::ref(privateSendClient), boost::ref(*g_connman)), 1 * 1000);
#endif // ENABLE_WALLET
    }

    llmq::StartLLMQSystem();

    // ********************************************************* Step 11: import blocks

    if (!CheckDiskSpace())
        return false;

    // Either install a handler to notify us when genesis activates, or set fHaveGenesis directly.
    // No locking, as this happens before any background thread is started.
    if (chainActive.Tip() == nullptr) {
        uiInterface.NotifyBlockTip.connect(BlockNotifyGenesisWait);
    } else {
        fHaveGenesis = true;
    }

    if (gArgs.IsArgSet("-blocknotify"))
        uiInterface.NotifyBlockTip.connect(BlockNotifyCallback);

    std::vector<fs::path> vImportFiles;
    for (const std::string& strFile : gArgs.GetArgs("-loadblock")) {
        vImportFiles.push_back(strFile);
    }

    threadGroup.create_thread(boost::bind(&ThreadImport, vImportFiles));

    // Wait for genesis block to be processed
    {
        boost::unique_lock<boost::mutex> lock(cs_GenesisWait);
        while (!fHaveGenesis) {
            condvar_GenesisWait.wait(lock);
        }
        uiInterface.NotifyBlockTip.disconnect(BlockNotifyGenesisWait);
    }

    // As importing blocks can take several minutes, it's possible the user
    // requested to kill the GUI during one of the last operations. If so, exit.
    if (fRequestShutdown) {
        LogPrintf("Shutdown requested. Exiting.\n");
        return false;
    }

    // ********************************************************* Step 12: start node

    //// debug print
    LogPrintf("mapBlockIndex.size() = %u\n", mapBlockIndex.size());
    LogPrintf("chainActive.Height() = %d\n", chainActive.Height());
    if (gArgs.GetBoolArg("-listenonion", DEFAULT_LISTEN_ONION))
        StartTorControl(threadGroup, scheduler);

    Discover(threadGroup);

    // Map ports with UPnP
    MapPort(gArgs.GetBoolArg("-upnp", DEFAULT_UPNP));

    CConnman::Options connOptions;
    connOptions.nLocalServices = nLocalServices;
    connOptions.nMaxConnections = nMaxConnections;
    connOptions.nMaxOutbound = std::min(MAX_OUTBOUND_CONNECTIONS, connOptions.nMaxConnections);
    connOptions.nMaxAddnode = MAX_ADDNODE_CONNECTIONS;
    connOptions.nMaxFeeler = 1;
    connOptions.nBestHeight = chainActive.Height();
    connOptions.uiInterface = &uiInterface;
    connOptions.m_msgproc = peerLogic.get();
    connOptions.nSendBufferMaxSize = 1000 * gArgs.GetArg("-maxsendbuffer", DEFAULT_MAXSENDBUFFER);
    connOptions.nReceiveFloodSize = 1000 * gArgs.GetArg("-maxreceivebuffer", DEFAULT_MAXRECEIVEBUFFER);

    connOptions.nMaxOutboundTimeframe = nMaxOutboundTimeframe;
    connOptions.nMaxOutboundLimit = nMaxOutboundLimit;

    for (const std::string& strBind : gArgs.GetArgs("-bind")) {
        CService addrBind;
        if (!Lookup(strBind.c_str(), addrBind, GetListenPort(), false)) {
            return InitError(ResolveErrMsg("bind", strBind));
        }
        connOptions.vBinds.push_back(addrBind);
    }
    for (const std::string& strBind : gArgs.GetArgs("-whitebind")) {
        CService addrBind;
        if (!Lookup(strBind.c_str(), addrBind, 0, false)) {
            return InitError(ResolveErrMsg("whitebind", strBind));
        }
        if (addrBind.GetPort() == 0) {
            return InitError(strprintf(_("Need to specify a port with -whitebind: '%s'"), strBind));
        }
        connOptions.vWhiteBinds.push_back(addrBind);
    }

    for (const auto& net : gArgs.GetArgs("-whitelist")) {
        CSubNet subnet;
        LookupSubNet(net.c_str(), subnet);
        if (!subnet.IsValid())
            return InitError(strprintf(_("Invalid netmask specified in -whitelist: '%s'"), net));
        connOptions.vWhitelistedRange.push_back(subnet);
    }

    if (gArgs.IsArgSet("-seednode")) {
        connOptions.vSeedNodes = gArgs.GetArgs("-seednode");
    }

    if (!connman.Start(scheduler, connOptions)) {
        return false;
    }

    // ********************************************************* Step 13: finished

    SetRPCWarmupFinished();
    uiInterface.InitMessage(_("Done loading"));

#ifdef ENABLE_WALLET
    for (CWalletRef pwallet : vpwallets) {
        pwallet->postInitProcess(scheduler);
    }
#endif

    // Final check if the user requested to kill the GUI during one of the last operations. If so, exit.
    if (fRequestShutdown) {
        LogPrintf("Shutdown requested. Exiting.\n");
        return false;
    }

    return true;
}
