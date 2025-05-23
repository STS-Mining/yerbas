// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2020 The Dash Core developers
// Copyright (c) 2020 The Memeium developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/memeium-config.h"
#endif

#include "net.h"
#include "netmessagemaker.h"

#include "addrman.h"
#include "chainparams.h"
#include "clientversion.h"
#include "consensus/consensus.h"
#include "crypto/common.h"
#include "crypto/sha256.h"
#include "hash.h"
#include "netbase.h"
#include "primitives/transaction.h"
#include "scheduler.h"
#include "ui_interface.h"
#include "utilstrencodings.h"
#include "validation.h"

#include "evo/deterministicmns.h"
#include "privatesend/privatesend.h"
#include "smartnode/smartnode-sync.h"

#ifdef WIN32
#include <string.h>
#else
#include <fcntl.h>
#endif

#ifdef USE_POLL
#include <poll.h>
#endif

#ifdef USE_EPOLL
#include <sys/epoll.h>
#endif

#ifdef USE_KQUEUE
#include <sys/event.h>
#endif

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/miniwget.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

#include <math.h>
#include <unordered_map>

// Dump addresses to peers.dat and banlist.dat every 15 minutes (900s)
#define DUMP_ADDRESSES_INTERVAL 900

// We add a random period time (0 to 1 seconds) to feeler connections to prevent synchronization.
#define FEELER_SLEEP_WINDOW 1

// MSG_NOSIGNAL is not available on some platforms, if it doesn't exist define it as 0
#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

// MSG_DONTWAIT is not available on some platforms, if it doesn't exist define it as 0
#if !defined(MSG_DONTWAIT)
#define MSG_DONTWAIT 0
#endif

// Fix for ancient MinGW versions, that don't have defined these in ws2tcpip.h.
// Todo: Can be removed when our pull-tester is upgraded to a modern MinGW version.
#ifdef WIN32
#ifndef PROTECTION_LEVEL_UNRESTRICTED
#define PROTECTION_LEVEL_UNRESTRICTED 10
#endif
#ifndef IPV6_PROTECTION_LEVEL
#define IPV6_PROTECTION_LEVEL 23
#endif
#endif

/** Used to pass flags to the Bind() function */
enum BindFlags {
    BF_NONE = 0,
    BF_EXPLICIT = (1U << 0),
    BF_REPORT_ERROR = (1U << 1),
    BF_WHITELIST = (1U << 2),
};

const static std::string NET_MESSAGE_COMMAND_OTHER = "*other*";

constexpr const CConnman::CFullyConnectedOnly CConnman::FullyConnectedOnly;
constexpr const CConnman::CAllNodes CConnman::AllNodes;

static const uint64_t RANDOMIZER_ID_NETGROUP = 0x6c0edd8036ef4036ULL;       // SHA256("netgroup")[0:8]
static const uint64_t RANDOMIZER_ID_LOCALHOSTNONCE = 0xd93e69e2bbfa5735ULL; // SHA256("localhostnonce")[0:8]
//
// Global state variables
//
bool fDiscover = true;
bool fListen = true;
bool fRelayTxes = true;
CCriticalSection cs_mapLocalHost;
std::map<CNetAddr, LocalServiceInfo> mapLocalHost;
static bool vfLimited[NET_MAX] = {};
std::string strSubVersion;

unordered_limitedmap<uint256, int64_t, StaticSaltedHasher> mapAlreadyAskedFor(MAX_INV_SZ, MAX_INV_SZ * 2);

void CConnman::AddOneShot(const std::string& strDest)
{
    LOCK(cs_vOneShots);
    vOneShots.push_back(strDest);
}

unsigned short GetListenPort()
{
    return (unsigned short)(gArgs.GetArg("-port", Params().GetDefaultPort()));
}

// find 'best' local address for a particular peer
bool GetLocal(CService& addr, const CNetAddr* paddrPeer)
{
    if (!fListen)
        return false;

    int nBestScore = -1;
    int nBestReachability = -1;
    {
        LOCK(cs_mapLocalHost);
        for (std::map<CNetAddr, LocalServiceInfo>::iterator it = mapLocalHost.begin(); it != mapLocalHost.end(); it++) {
            int nScore = (*it).second.nScore;
            int nReachability = (*it).first.GetReachabilityFrom(paddrPeer);
            if (nReachability > nBestReachability || (nReachability == nBestReachability && nScore > nBestScore)) {
                addr = CService((*it).first, (*it).second.nPort);
                nBestReachability = nReachability;
                nBestScore = nScore;
            }
        }
    }
    return nBestScore >= 0;
}

//! Convert the pnSeeds6 array into usable address objects.
static std::vector<CAddress> convertSeed6(const std::vector<SeedSpec6>& vSeedsIn)
{
    // It'll only connect to one or two seed nodes because once it connects,
    // it'll get a pile of addresses with newer timestamps.
    // Seed nodes are given a random 'last seen time' of between one and two
    // weeks ago.
    const int64_t nOneWeek = 7 * 24 * 60 * 60;
    std::vector<CAddress> vSeedsOut;
    vSeedsOut.reserve(vSeedsIn.size());
    for (std::vector<SeedSpec6>::const_iterator i(vSeedsIn.begin()); i != vSeedsIn.end(); ++i) {
        struct in6_addr ip;
        memcpy(&ip, i->addr, sizeof(ip));
        CAddress addr(CService(ip, i->port), NODE_NETWORK);
        addr.nTime = GetTime() - GetRand(nOneWeek) - nOneWeek;
        vSeedsOut.push_back(addr);
    }
    return vSeedsOut;
}

// get best local address for a particular peer as a CAddress
// Otherwise, return the unroutable 0.0.0.0 but filled in with
// the normal parameters, since the IP may be changed to a useful
// one by discovery.
CAddress GetLocalAddress(const CNetAddr* paddrPeer, ServiceFlags nLocalServices)
{
    CAddress ret(CService(CNetAddr(), GetListenPort()), nLocalServices);
    CService addr;
    if (GetLocal(addr, paddrPeer)) {
        ret = CAddress(addr, nLocalServices);
    }
    ret.nTime = GetAdjustedTime();
    return ret;
}

int GetnScore(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    if (mapLocalHost.count(addr) == LOCAL_NONE)
        return 0;
    return mapLocalHost[addr].nScore;
}

// Is our peer's addrLocal potentially useful as an external IP source?
bool IsPeerAddrLocalGood(CNode* pnode)
{
    CService addrLocal = pnode->GetAddrLocal();
    return fDiscover && pnode->addr.IsRoutable() && addrLocal.IsRoutable() &&
           !IsLimited(addrLocal.GetNetwork());
}

// pushes our own address to a peer
void AdvertiseLocal(CNode* pnode)
{
    if (fListen && pnode->fSuccessfullyConnected) {
        CAddress addrLocal = GetLocalAddress(&pnode->addr, pnode->GetLocalServices());
        // If discovery is enabled, sometimes give our peer the address it
        // tells us that it sees us as in case it has a better idea of our
        // address than we do.
        if (IsPeerAddrLocalGood(pnode) && (!addrLocal.IsRoutable() ||
                                              GetRand((GetnScore(addrLocal) > LOCAL_MANUAL) ? 8 : 2) == 0)) {
            addrLocal.SetIP(pnode->GetAddrLocal());
        }
        if (addrLocal.IsRoutable()) {
            LogPrint(BCLog::NET, "AdvertiseLocal: advertising address %s\n", addrLocal.ToString());
            FastRandomContext insecure_rand;
            pnode->PushAddress(addrLocal, insecure_rand);
        }
    }
}

// learn a new local address
bool AddLocal(const CService& addr, int nScore)
{
    if (!addr.IsRoutable() && Params().RequireRoutableExternalIP())
        return false;

    if (!fDiscover && nScore < LOCAL_MANUAL)
        return false;

    if (IsLimited(addr))
        return false;

    LogPrintf("AddLocal(%s,%i)\n", addr.ToString(), nScore);

    {
        LOCK(cs_mapLocalHost);
        bool fAlready = mapLocalHost.count(addr) > 0;
        LocalServiceInfo& info = mapLocalHost[addr];
        if (!fAlready || nScore >= info.nScore) {
            info.nScore = nScore + (fAlready ? 1 : 0);
            info.nPort = addr.GetPort();
        }
    }

    return true;
}

bool AddLocal(const CNetAddr& addr, int nScore)
{
    return AddLocal(CService(addr, GetListenPort()), nScore);
}

bool RemoveLocal(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    LogPrintf("RemoveLocal(%s)\n", addr.ToString());
    mapLocalHost.erase(addr);
    return true;
}

/** Make a particular network entirely off-limits (no automatic connects to it) */
void SetLimited(enum Network net, bool fLimited)
{
    if (net == NET_UNROUTABLE || net == NET_INTERNAL)
        return;
    LOCK(cs_mapLocalHost);
    vfLimited[net] = fLimited;
}

bool IsLimited(enum Network net)
{
    LOCK(cs_mapLocalHost);
    return vfLimited[net];
}

bool IsLimited(const CNetAddr& addr)
{
    return IsLimited(addr.GetNetwork());
}

/** vote for a local address */
bool SeenLocal(const CService& addr)
{
    {
        LOCK(cs_mapLocalHost);
        if (mapLocalHost.count(addr) == 0)
            return false;
        mapLocalHost[addr].nScore++;
    }
    return true;
}


/** check whether a given address is potentially local */
bool IsLocal(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    return mapLocalHost.count(addr) > 0;
}

/** check whether a given network is one we can probably connect to */
bool IsReachable(enum Network net)
{
    LOCK(cs_mapLocalHost);
    return !vfLimited[net];
}

/** check whether a given address is in a network we can probably connect to */
bool IsReachable(const CNetAddr& addr)
{
    enum Network net = addr.GetNetwork();
    return IsReachable(net);
}


CNode* CConnman::FindNode(const CNetAddr& ip)
{
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes)
        if ((CNetAddr)pnode->addr == ip)
            return (pnode);
    return nullptr;
}

CNode* CConnman::FindNode(const CSubNet& subNet)
{
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes)
        if (subNet.Match((CNetAddr)pnode->addr))
            return (pnode);
    return nullptr;
}

CNode* CConnman::FindNode(const std::string& addrName)
{
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes) {
        if (pnode->GetAddrName() == addrName) {
            return (pnode);
        }
    }
    return nullptr;
}

CNode* CConnman::FindNode(const CService& addr)
{
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes)
        if ((CService)pnode->addr == addr)
            return (pnode);
    return nullptr;
}

bool CConnman::CheckIncomingNonce(uint64_t nonce)
{
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes) {
        if (!pnode->fSuccessfullyConnected && !pnode->fInbound && pnode->GetLocalNonce() == nonce)
            return false;
    }
    return true;
}

/** Get the bind address for a socket as CAddress */
static CAddress GetBindAddress(SOCKET sock)
{
    CAddress addr_bind;
    struct sockaddr_storage sockaddr_bind;
    socklen_t sockaddr_bind_len = sizeof(sockaddr_bind);
    if (sock != INVALID_SOCKET) {
        if (!getsockname(sock, (struct sockaddr*)&sockaddr_bind, &sockaddr_bind_len)) {
            addr_bind.SetSockAddr((const struct sockaddr*)&sockaddr_bind);
        } else {
            LogPrint(BCLog::NET, "Warning: getsockname failed\n");
        }
    }
    return addr_bind;
}

CNode* CConnman::ConnectNode(CAddress addrConnect, const char* pszDest, bool fCountFailure)
{
    if (pszDest == nullptr) {
        bool fAllowLocal = Params().AllowMultiplePorts() && addrConnect.GetPort() != GetListenPort();
        if (!fAllowLocal && IsLocal(addrConnect)) {
            return nullptr;
        }

        // Look for an existing connection
        CNode* pnode = FindNode((CService)addrConnect);
        if (pnode) {
            LogPrintf("Failed to open new connection, already connected\n");
            return nullptr;
        }
    }

    /// debug print
    if (fLogIPs) {
        LogPrint(BCLog::NET, "trying connection %s lastseen=%.1fhrs\n",
            pszDest ? pszDest : addrConnect.ToString(),
            pszDest ? 0.0 : (double)(GetAdjustedTime() - addrConnect.nTime) / 3600.0);
    } else {
        LogPrint(BCLog::NET, "trying connection lastseen=%.1fhrs\n",
            pszDest ? 0.0 : (double)(GetAdjustedTime() - addrConnect.nTime) / 3600.0);
    }

    // Connect
    SOCKET hSocket;
    bool proxyConnectionFailed = false;
    if (pszDest ? ConnectSocketByName(addrConnect, hSocket, pszDest, Params().GetDefaultPort(), nConnectTimeout, &proxyConnectionFailed) : ConnectSocket(addrConnect, hSocket, nConnectTimeout, &proxyConnectionFailed)) {
        if (!IsSelectableSocket(hSocket)) {
            LogPrintf("Cannot create connection: non-selectable socket created (fd >= FD_SETSIZE ?)\n");
            CloseSocket(hSocket);
            return nullptr;
        }

        if (pszDest && addrConnect.IsValid()) {
            // It is possible that we already have a connection to the IP/port pszDest resolved to.
            // In that case, drop the connection that was just created, and return the existing CNode instead.
            // Also store the name we used to connect in that CNode, so that future FindNode() calls to that
            // name catch this early.
            LOCK(cs_vNodes);
            CNode* pnode = FindNode((CService)addrConnect);
            if (pnode) {
                pnode->MaybeSetAddrName(std::string(pszDest));
                CloseSocket(hSocket);
                LogPrintf("Failed to open new connection, already connected\n");
                return nullptr;
            }
        }

        addrman.Attempt(addrConnect, fCountFailure);

        // Add node
        NodeId id = GetNewNodeId();
        uint64_t nonce = GetDeterministicRandomizer(RANDOMIZER_ID_LOCALHOSTNONCE).Write(id).Finalize();
        CAddress addr_bind = GetBindAddress(hSocket);
        CNode* pnode = new CNode(id, nLocalServices, GetBestHeight(), hSocket, addrConnect, CalculateKeyedNetGroup(addrConnect), nonce, addr_bind, pszDest ? pszDest : "", false);
        pnode->AddRef();


        return pnode;
    } else if (!proxyConnectionFailed) {
        // If connecting to the node failed, and failure is not caused by a problem connecting to
        // the proxy, mark this as an attempt.
        addrman.Attempt(addrConnect, fCountFailure);
    }

    return nullptr;
}

void CConnman::DumpBanlist()
{
    SweepBanned(); // clean unused entries (if bantime has expired)

    if (!BannedSetIsDirty())
        return;

    int64_t nStart = GetTimeMillis();

    CBanDB bandb;
    banmap_t banmap;
    GetBanned(banmap);
    if (bandb.Write(banmap)) {
        SetBannedSetDirty(false);
    }

    LogPrint(BCLog::NET, "Flushed %d banned node ips/subnets to banlist.dat  %dms\n",
        banmap.size(), GetTimeMillis() - nStart);
}

void CNode::CloseSocketDisconnect()
{
    fDisconnect = true;
    LOCK(cs_hSocket);
    if (hSocket != INVALID_SOCKET) {
        LogPrint(BCLog::NET, "disconnecting peer=%d\n", id);
        CloseSocket(hSocket);
    }
}

void CConnman::ClearBanned()
{
    {
        LOCK(cs_setBanned);
        setBanned.clear();
        setBannedIsDirty = true;
    }
    DumpBanlist(); // store banlist to disk
    if (clientInterface)
        clientInterface->BannedListChanged();
}

bool CConnman::IsBanned(CNetAddr ip)
{
    LOCK(cs_setBanned);
    for (banmap_t::iterator it = setBanned.begin(); it != setBanned.end(); it++) {
        CSubNet subNet = (*it).first;
        CBanEntry banEntry = (*it).second;

        if (subNet.Match(ip) && GetTime() < banEntry.nBanUntil) {
            return true;
        }
    }
    return false;
}

bool CConnman::IsBanned(CSubNet subnet)
{
    LOCK(cs_setBanned);
    banmap_t::iterator i = setBanned.find(subnet);
    if (i != setBanned.end()) {
        CBanEntry banEntry = (*i).second;
        if (GetTime() < banEntry.nBanUntil) {
            return true;
        }
    }
    return false;
}

void CConnman::Ban(const CNetAddr& addr, const BanReason& banReason, int64_t bantimeoffset, bool sinceUnixEpoch)
{
    CSubNet subNet(addr);
    Ban(subNet, banReason, bantimeoffset, sinceUnixEpoch);
}

void CConnman::Ban(const CSubNet& subNet, const BanReason& banReason, int64_t bantimeoffset, bool sinceUnixEpoch)
{
    CBanEntry banEntry(GetTime());
    banEntry.banReason = banReason;
    if (bantimeoffset <= 0) {
        bantimeoffset = gArgs.GetArg("-bantime", DEFAULT_MISBEHAVING_BANTIME);
        sinceUnixEpoch = false;
    }
    banEntry.nBanUntil = (sinceUnixEpoch ? 0 : GetTime()) + bantimeoffset;

    {
        LOCK(cs_setBanned);
        if (setBanned[subNet].nBanUntil < banEntry.nBanUntil) {
            setBanned[subNet] = banEntry;
            setBannedIsDirty = true;
        } else
            return;
    }
    if (clientInterface)
        clientInterface->BannedListChanged();
    {
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodes) {
            if (subNet.Match((CNetAddr)pnode->addr))
                pnode->fDisconnect = true;
        }
    }
    if (banReason == BanReasonManuallyAdded)
        DumpBanlist(); // store banlist to disk immediately if user requested ban
}

bool CConnman::Unban(const CNetAddr& addr)
{
    CSubNet subNet(addr);
    return Unban(subNet);
}

bool CConnman::Unban(const CSubNet& subNet)
{
    {
        LOCK(cs_setBanned);
        if (!setBanned.erase(subNet))
            return false;
        setBannedIsDirty = true;
    }
    if (clientInterface)
        clientInterface->BannedListChanged();
    DumpBanlist(); // store banlist to disk immediately
    return true;
}

void CConnman::GetBanned(banmap_t& banMap)
{
    LOCK(cs_setBanned);
    // Sweep the banlist so expired bans are not returned
    SweepBanned();
    banMap = setBanned; // create a thread safe copy
}

void CConnman::SetBanned(const banmap_t& banMap)
{
    LOCK(cs_setBanned);
    setBanned = banMap;
    setBannedIsDirty = true;
}

void CConnman::SweepBanned()
{
    int64_t now = GetTime();

    LOCK(cs_setBanned);
    banmap_t::iterator it = setBanned.begin();
    while (it != setBanned.end()) {
        CSubNet subNet = (*it).first;
        CBanEntry banEntry = (*it).second;
        if (now > banEntry.nBanUntil) {
            setBanned.erase(it++);
            setBannedIsDirty = true;
            LogPrint(BCLog::NET, "%s: Removed banned node ip/subnet from banlist.dat: %s\n", __func__, subNet.ToString());
        } else
            ++it;
    }
}

bool CConnman::BannedSetIsDirty()
{
    LOCK(cs_setBanned);
    return setBannedIsDirty;
}

void CConnman::SetBannedSetDirty(bool dirty)
{
    LOCK(cs_setBanned); // reuse setBanned lock for the isDirty flag
    setBannedIsDirty = dirty;
}


bool CConnman::IsWhitelistedRange(const CNetAddr& addr)
{
    for (const CSubNet& subnet : vWhitelistedRange) {
        if (subnet.Match(addr))
            return true;
    }
    return false;
}

std::string CNode::GetAddrName() const
{
    LOCK(cs_addrName);
    return addrName;
}

void CNode::MaybeSetAddrName(const std::string& addrNameIn)
{
    LOCK(cs_addrName);
    if (addrName.empty()) {
        addrName = addrNameIn;
    }
}

CService CNode::GetAddrLocal() const
{
    LOCK(cs_addrLocal);
    return addrLocal;
}

void CNode::SetAddrLocal(const CService& addrLocalIn)
{
    LOCK(cs_addrLocal);
    if (addrLocal.IsValid()) {
        error("Addr local already set for node: %i. Refusing to change from %s to %s", id, addrLocal.ToString(), addrLocalIn.ToString());
    } else {
        addrLocal = addrLocalIn;
    }
}

std::string CNode::GetLogString() const
{
    return fLogIPs ? addr.ToString() : strprintf("%d", id);
}

#undef X
#define X(name) stats.name = name
void CNode::copyStats(CNodeStats& stats)
{
    stats.nodeid = this->GetId();
    X(nServices);
    X(addr);
    X(addrBind);
    {
        LOCK(cs_filter);
        X(fRelayTxes);
    }
    X(nLastSend);
    X(nLastRecv);
    X(nTimeConnected);
    X(nTimeOffset);
    stats.addrName = GetAddrName();
    X(nVersion);
    {
        LOCK(cs_SubVer);
        X(cleanSubVer);
    }
    X(fInbound);
    X(m_manual_connection);
    X(nStartingHeight);
    {
        LOCK(cs_vSend);
        X(mapSendBytesPerMsgCmd);
        X(nSendBytes);
    }
    {
        LOCK(cs_vRecv);
        X(mapRecvBytesPerMsgCmd);
        X(nRecvBytes);
    }
    X(fWhitelisted);
    X(nProcessedAddrs);
    X(nRatelimitedAddrs);

    // It is common for nodes with good ping times to suddenly become lagged,
    // due to a new block arriving or other large transfer.
    // Merely reporting pingtime might fool the caller into thinking the node was still responsive,
    // since pingtime does not update until the ping is complete, which might take a while.
    // So, if a ping is taking an unusually long time in flight,
    // the caller can immediately detect that this is happening.
    int64_t nPingUsecWait = 0;
    if ((0 != nPingNonceSent) && (0 != nPingUsecStart)) {
        nPingUsecWait = GetTimeMicros() - nPingUsecStart;
    }

    // Raw ping time is in microseconds, but show it to user as whole seconds (Memeium users should be well used to small numbers with many decimal places by now :)
    stats.dPingTime = (((double)nPingUsecTime) / 1e6);
    stats.dMinPing = (((double)nMinPingUsecTime) / 1e6);
    stats.dPingWait = (((double)nPingUsecWait) / 1e6);

    // Leave string empty if addrLocal invalid (not filled in yet)
    CService addrLocalUnlocked = GetAddrLocal();
    stats.addrLocal = addrLocalUnlocked.IsValid() ? addrLocalUnlocked.ToString() : "";

    {
        LOCK(cs_mnauth);
        X(verifiedProRegTxHash);
    }
}
#undef X

bool CNode::ReceiveMsgBytes(const char* pch, unsigned int nBytes, bool& complete)
{
    complete = false;
    int64_t nTimeMicros = GetTimeMicros();
    LOCK(cs_vRecv);
    nLastRecv = nTimeMicros / 1000000;
    nRecvBytes += nBytes;
    while (nBytes > 0) {
        // get current incomplete message, or create a new one
        if (vRecvMsg.empty() ||
            vRecvMsg.back().complete())
            vRecvMsg.push_back(CNetMessage(Params().MessageStart(), SER_NETWORK, INIT_PROTO_VERSION));

        CNetMessage& msg = vRecvMsg.back();

        // absorb network data
        int handled;
        if (!msg.in_data) {
            handled = msg.readHeader(pch, nBytes);
        } else {
            handled = msg.readData(pch, nBytes);
        }

        if (handled < 0)
            return false;

        if (msg.in_data && msg.hdr.nMessageSize > MAX_PROTOCOL_MESSAGE_LENGTH) {
            LogPrint(BCLog::NET, "Oversized message from peer=%i, disconnecting\n", GetId());
            return false;
        }

        pch += handled;
        nBytes -= handled;

        if (msg.complete()) {
            // store received bytes per message command
            // to prevent a memory DOS, only allow valid commands
            mapMsgCmdSize::iterator i = mapRecvBytesPerMsgCmd.find(msg.hdr.pchCommand);
            if (i == mapRecvBytesPerMsgCmd.end())
                i = mapRecvBytesPerMsgCmd.find(NET_MESSAGE_COMMAND_OTHER);
            assert(i != mapRecvBytesPerMsgCmd.end());
            i->second += msg.hdr.nMessageSize + CMessageHeader::HEADER_SIZE;

            msg.nTime = nTimeMicros;
            complete = true;
        }
    }

    return true;
}

void CNode::SetSendVersion(int nVersionIn)
{
    // Send version may only be changed in the version message, and
    // only one version message is allowed per session. We can therefore
    // treat this value as const and even atomic as long as it's only used
    // once a version message has been successfully processed. Any attempt to
    // set this twice is an error.
    if (nSendVersion != 0) {
        error("Send version already set for node: %i. Refusing to change from %i to %i", id, nSendVersion, nVersionIn);
    } else {
        nSendVersion = nVersionIn;
    }
}

int CNode::GetSendVersion() const
{
    // The send version should always be explicitly set to
    // INIT_PROTO_VERSION rather than using this value until SetSendVersion
    // has been called.
    if (nSendVersion == 0) {
        error("Requesting unset send version for node: %i. Using %i", id, INIT_PROTO_VERSION);
        return INIT_PROTO_VERSION;
    }
    return nSendVersion;
}


int CNetMessage::readHeader(const char* pch, unsigned int nBytes)
{
    // copy data to temporary parsing buffer
    unsigned int nRemaining = 24 - nHdrPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    memcpy(&hdrbuf[nHdrPos], pch, nCopy);
    nHdrPos += nCopy;

    // if header incomplete, exit
    if (nHdrPos < 24)
        return nCopy;

    // deserialize to CMessageHeader
    try {
        hdrbuf >> hdr;
    } catch (const std::exception&) {
        return -1;
    }

    // reject messages larger than MAX_SIZE
    if (hdr.nMessageSize > MAX_SIZE)
        return -1;

    // switch state to reading message data
    in_data = true;

    return nCopy;
}

int CNetMessage::readData(const char* pch, unsigned int nBytes)
{
    unsigned int nRemaining = hdr.nMessageSize - nDataPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    if (vRecv.size() < nDataPos + nCopy) {
        // Allocate up to 256 KiB ahead, but never more than the total message size.
        vRecv.resize(std::min(hdr.nMessageSize, nDataPos + nCopy + 256 * 1024));
    }

    hasher.Write((const unsigned char*)pch, nCopy);
    memcpy(&vRecv[nDataPos], pch, nCopy);
    nDataPos += nCopy;

    return nCopy;
}

const uint256& CNetMessage::GetMessageHash() const
{
    assert(complete());
    if (data_hash.IsNull())
        hasher.Finalize(data_hash.begin());
    return data_hash;
}


// requires LOCK(cs_vSend)
size_t CConnman::SocketSendData(CNode* pnode) const
{
    auto it = pnode->vSendMsg.begin();
    size_t nSentSize = 0;

    while (it != pnode->vSendMsg.end()) {
        const auto& data = *it;
        assert(data.size() > pnode->nSendOffset);
        int nBytes = 0;
        {
            LOCK(pnode->cs_hSocket);
            if (pnode->hSocket == INVALID_SOCKET)
                break;
            nBytes = send(pnode->hSocket, reinterpret_cast<const char*>(data.data()) + pnode->nSendOffset, data.size() - pnode->nSendOffset, MSG_NOSIGNAL | MSG_DONTWAIT);
        }
        if (nBytes > 0) {
            pnode->nLastSend = GetSystemTimeInSeconds();
            pnode->nSendBytes += nBytes;
            pnode->nSendOffset += nBytes;
            nSentSize += nBytes;
            if (pnode->nSendOffset == data.size()) {
                pnode->nSendOffset = 0;
                pnode->nSendSize -= data.size();
                pnode->fPauseSend = pnode->nSendSize > nSendBufferMaxSize;
                it++;
            } else {
                // could not send full message; stop sending more
                break;
            }
        } else {
            if (nBytes < 0) {
                // error
                int nErr = WSAGetLastError();
                if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS) {
                    LogPrintf("socket send error %s\n", NetworkErrorString(nErr));
                    pnode->fDisconnect = true;
                }
            }
            // couldn't send anything at all
            break;
        }
    }

    if (it == pnode->vSendMsg.end()) {
        assert(pnode->nSendOffset == 0);
        assert(pnode->nSendSize == 0);
    }
    pnode->vSendMsg.erase(pnode->vSendMsg.begin(), it);
    return nSentSize;
}

struct NodeEvictionCandidate {
    NodeId id;
    int64_t nTimeConnected;
    int64_t nMinPingUsecTime;
    int64_t nLastBlockTime;
    int64_t nLastTXTime;
    bool fRelevantServices;
    bool fRelayTxes;
    bool fBloomFilter;
    uint64_t nKeyedNetGroup;
};

static bool ReverseCompareNodeMinPingTime(const NodeEvictionCandidate& a, const NodeEvictionCandidate& b)
{
    return a.nMinPingUsecTime > b.nMinPingUsecTime;
}

static bool ReverseCompareNodeTimeConnected(const NodeEvictionCandidate& a, const NodeEvictionCandidate& b)
{
    return a.nTimeConnected > b.nTimeConnected;
}

static bool CompareNetGroupKeyed(const NodeEvictionCandidate& a, const NodeEvictionCandidate& b)
{
    return a.nKeyedNetGroup < b.nKeyedNetGroup;
}

static bool CompareNodeBlockTime(const NodeEvictionCandidate& a, const NodeEvictionCandidate& b)
{
    // There is a fall-through here because it is common for a node to have many peers which have not yet relayed a block.
    if (a.nLastBlockTime != b.nLastBlockTime) return a.nLastBlockTime < b.nLastBlockTime;
    if (a.fRelevantServices != b.fRelevantServices) return b.fRelevantServices;
    return a.nTimeConnected > b.nTimeConnected;
}

static bool CompareNodeTXTime(const NodeEvictionCandidate& a, const NodeEvictionCandidate& b)
{
    // There is a fall-through here because it is common for a node to have more than a few peers that have not yet relayed txn.
    if (a.nLastTXTime != b.nLastTXTime) return a.nLastTXTime < b.nLastTXTime;
    if (a.fRelayTxes != b.fRelayTxes) return b.fRelayTxes;
    if (a.fBloomFilter != b.fBloomFilter) return a.fBloomFilter;
    return a.nTimeConnected > b.nTimeConnected;
}

/** Try to find a connection to evict when the node is full.
 *  Extreme care must be taken to avoid opening the node to attacker
 *   triggered network partitioning.
 *  The strategy used here is to protect a small number of peers
 *   for each of several distinct characteristics which are difficult
 *   to forge.  In order to partition a node the attacker must be
 *   simultaneously better at all of them than honest peers.
 */
bool CConnman::AttemptToEvictConnection()
{
    std::vector<NodeEvictionCandidate> vEvictionCandidates;
    {
        LOCK(cs_vNodes);

        for (CNode* node : vNodes) {
            if (node->fWhitelisted)
                continue;
            if (!node->fInbound)
                continue;
            if (node->fDisconnect)
                continue;

            if (fSmartnodeMode) {
                // This handles eviction protected nodes. Nodes are always protected for a short time after the connection
                // was accepted. This short time is meant for the VERSION/VERACK exchange and the possible MNAUTH that might
                // follow when the incoming connection is from another smartnode. When a message other than MNAUTH
                // is received after VERSION/VERACK, the protection is lifted immediately.
                bool isProtected = GetSystemTimeInSeconds() - node->nTimeConnected < INBOUND_EVICTION_PROTECTION_TIME;
                if (node->nTimeFirstMessageReceived != 0 && !node->fFirstMessageIsMNAUTH) {
                    isProtected = false;
                }
                // if MNAUTH was valid, the node is always protected (and at the same time not accounted when
                // checking incoming connection limits)
                if (!node->verifiedProRegTxHash.IsNull()) {
                    isProtected = true;
                }
                if (isProtected) {
                    continue;
                }
            }

            NodeEvictionCandidate candidate = {node->GetId(), node->nTimeConnected, node->nMinPingUsecTime,
                node->nLastBlockTime, node->nLastTXTime,
                HasAllDesirableServiceFlags(node->nServices),
                node->fRelayTxes, node->pfilter != nullptr, node->nKeyedNetGroup};
            vEvictionCandidates.push_back(candidate);
        }
    }

    if (vEvictionCandidates.empty()) return false;

    // Protect connections with certain characteristics

    // Deterministically select 4 peers to protect by netgroup.
    // An attacker cannot predict which netgroups will be protected
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), CompareNetGroupKeyed);
    vEvictionCandidates.erase(vEvictionCandidates.end() - std::min(4, static_cast<int>(vEvictionCandidates.size())), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Protect the 8 nodes with the lowest minimum ping time.
    // An attacker cannot manipulate this metric without physically moving nodes closer to the target.
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), ReverseCompareNodeMinPingTime);
    vEvictionCandidates.erase(vEvictionCandidates.end() - std::min(8, static_cast<int>(vEvictionCandidates.size())), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Protect 4 nodes that most recently sent us transactions.
    // An attacker cannot manipulate this metric without performing useful work.
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), CompareNodeTXTime);
    vEvictionCandidates.erase(vEvictionCandidates.end() - std::min(4, static_cast<int>(vEvictionCandidates.size())), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Protect 4 nodes that most recently sent us blocks.
    // An attacker cannot manipulate this metric without performing useful work.
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), CompareNodeBlockTime);
    vEvictionCandidates.erase(vEvictionCandidates.end() - std::min(4, static_cast<int>(vEvictionCandidates.size())), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Protect the half of the remaining nodes which have been connected the longest.
    // This replicates the non-eviction implicit behavior, and precludes attacks that start later.
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), ReverseCompareNodeTimeConnected);
    vEvictionCandidates.erase(vEvictionCandidates.end() - static_cast<int>(vEvictionCandidates.size() / 2), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Identify the network group with the most connections and youngest member.
    // (vEvictionCandidates is already sorted by reverse connect time)
    uint64_t naMostConnections;
    unsigned int nMostConnections = 0;
    int64_t nMostConnectionsTime = 0;
    std::map<uint64_t, std::vector<NodeEvictionCandidate>> mapNetGroupNodes;
    for (const NodeEvictionCandidate& node : vEvictionCandidates) {
        mapNetGroupNodes[node.nKeyedNetGroup].push_back(node);
        int64_t grouptime = mapNetGroupNodes[node.nKeyedNetGroup][0].nTimeConnected;
        size_t groupsize = mapNetGroupNodes[node.nKeyedNetGroup].size();

        if (groupsize > nMostConnections || (groupsize == nMostConnections && grouptime > nMostConnectionsTime)) {
            nMostConnections = groupsize;
            nMostConnectionsTime = grouptime;
            naMostConnections = node.nKeyedNetGroup;
        }
    }

    // Reduce to the network group with the most connections
    vEvictionCandidates = std::move(mapNetGroupNodes[naMostConnections]);

    // Disconnect from the network group with the most connections
    NodeId evicted = vEvictionCandidates.front().id;
    LOCK(cs_vNodes);
    for (std::vector<CNode*>::const_iterator it(vNodes.begin()); it != vNodes.end(); ++it) {
        if ((*it)->GetId() == evicted) {
            (*it)->fDisconnect = true;
            return true;
        }
    }
    return false;
}

void CConnman::AcceptConnection(const ListenSocket& hListenSocket)
{
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    SOCKET hSocket = accept(hListenSocket.socket, (struct sockaddr*)&sockaddr, &len);
    CAddress addr;
    int nInbound = 0;
    int nVerifiedInboundSmartnodes = 0;
    int nMaxInbound = nMaxConnections - (nMaxOutbound + nMaxFeeler);

    if (hSocket != INVALID_SOCKET) {
        if (!addr.SetSockAddr((const struct sockaddr*)&sockaddr)) {
            LogPrintf("Warning: Unknown socket family\n");
        }
    }

    bool whitelisted = hListenSocket.whitelisted || IsWhitelistedRange(addr);
    {
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodes) {
            if (pnode->fInbound) {
                nInbound++;
                if (!pnode->verifiedProRegTxHash.IsNull()) {
                    nVerifiedInboundSmartnodes++;
                }
            }
        }
    }

    if (hSocket == INVALID_SOCKET) {
        int nErr = WSAGetLastError();
        if (nErr != WSAEWOULDBLOCK)
            LogPrintf("socket error accept failed: %s\n", NetworkErrorString(nErr));
        return;
    }

    std::string strDropped;
    if (fLogIPs) {
        strDropped = strprintf("connection from %s dropped", addr.ToString());
    } else {
        strDropped = "connection dropped";
    }

    if (!fNetworkActive) {
        LogPrintf("%s: not accepting new connections\n", strDropped);
        CloseSocket(hSocket);
        return;
    }

    if (!IsSelectableSocket(hSocket)) {
        LogPrintf("%s: non-selectable socket\n", strDropped);
        CloseSocket(hSocket);
        return;
    }

    // According to the internet TCP_NODELAY is not carried into accepted sockets
    // on all platforms.  Set it again here just to be sure.
    SetSocketNoDelay(hSocket);

    if (IsBanned(addr) && !whitelisted) {
        LogPrintf("%s (banned)\n", strDropped);
        CloseSocket(hSocket);
        return;
    }

    // Evict connections until we are below nMaxInbound. In case eviction protection resulted in nodes to not be evicted
    // before, they might get evicted in batches now (after the protection timeout).
    // We don't evict verified MN connections and also don't take them into account when checking limits. We can do this
    // because we know that such connections are naturally limited by the total number of MNs, so this is not usable
    // for attacks.
    while (nInbound - nVerifiedInboundSmartnodes >= nMaxInbound) {
        if (!AttemptToEvictConnection()) {
            // No connection to evict, disconnect the new connection
            LogPrint(BCLog::NET, "failed to find an eviction candidate - connection dropped (full)\n");
            CloseSocket(hSocket);
            return;
        }
        nInbound--;
    }

    // don't accept incoming connections until fully synced
    if (fSmartnodeMode && !smartnodeSync.IsSynced()) {
        LogPrint(BCLog::NET, "AcceptConnection -- smartnode is not synced yet, skipping inbound connection attempt\n");
        CloseSocket(hSocket);
        return;
    }

    NodeId id = GetNewNodeId();
    uint64_t nonce = GetDeterministicRandomizer(RANDOMIZER_ID_LOCALHOSTNONCE).Write(id).Finalize();
    CAddress addr_bind = GetBindAddress(hSocket);

    CNode* pnode = new CNode(id, nLocalServices, GetBestHeight(), hSocket, addr, CalculateKeyedNetGroup(addr), nonce, addr_bind, "", true);
    pnode->AddRef();
    pnode->fWhitelisted = whitelisted;
    m_msgproc->InitializeNode(pnode);

    if (fLogIPs) {
        LogPrint(BCLog::NET, "connection from %s accepted\n", addr.ToString());
    } else {
        LogPrint(BCLog::NET, "connection accepted\n");
    }

    {
        LOCK(cs_vNodes);
        vNodes.push_back(pnode);
    }
}

void CConnman::ThreadSocketHandler()
{
    unsigned int nPrevNodeCount = 0;
    while (!interruptNet) {
        //
        // Disconnect nodes
        //
        {
            LOCK(cs_vNodes);
            // Disconnect unused nodes
            std::vector<CNode*> vNodesCopy = vNodes;
            for (CNode* pnode : vNodesCopy) {
                if (pnode->fDisconnect) {
                    if (fLogIPs) {
                        LogPrintf("ThreadSocketHandler -- removing node: peer=%d addr=%s nRefCount=%d fInbound=%d fSmartnode=%d\n",
                            pnode->GetId(), pnode->addr.ToString(), pnode->GetRefCount(), pnode->fInbound, pnode->fSmartnode);
                    } else {
                        LogPrintf("ThreadSocketHandler -- removing node: peer=%d nRefCount=%d fInbound=%d fSmartnode=%d\n",
                            pnode->GetId(), pnode->GetRefCount(), pnode->fInbound, pnode->fSmartnode);
                    }

                    // remove from vNodes
                    vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());

                    // release outbound grant (if any)
                    pnode->grantOutbound.Release();
                    pnode->grantSmartnodeOutbound.Release();

                    // close socket and cleanup
                    pnode->CloseSocketDisconnect();

                    // hold in disconnected pool until all refs are released
                    pnode->Release();
                    vNodesDisconnected.push_back(pnode);
                }
            }
        }
        {
            // Delete disconnected nodes
            std::list<CNode*> vNodesDisconnectedCopy = vNodesDisconnected;
            for (CNode* pnode : vNodesDisconnectedCopy) {
                // wait until threads are done using it
                if (pnode->GetRefCount() <= 0) {
                    bool fDelete = false;
                    {
                        TRY_LOCK(pnode->cs_inventory, lockInv);
                        if (lockInv) {
                            TRY_LOCK(pnode->cs_vSend, lockSend);
                            if (lockSend) {
                                fDelete = true;
                            }
                        }
                    }
                    if (fDelete) {
                        vNodesDisconnected.remove(pnode);
                        DeleteNode(pnode);
                    }
                }
            }
        }
        size_t vNodesSize;
        {
            LOCK(cs_vNodes);
            vNodesSize = vNodes.size();
        }
        if (vNodesSize != nPrevNodeCount) {
            nPrevNodeCount = vNodesSize;
            if (clientInterface)
                clientInterface->NotifyNumConnectionsChanged(nPrevNodeCount);
        }

        //
        // Find which sockets have data to receive
        //
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000; // frequency to poll pnode->vSend

        fd_set fdsetRecv;
        fd_set fdsetSend;
        fd_set fdsetError;
        FD_ZERO(&fdsetRecv);
        FD_ZERO(&fdsetSend);
        FD_ZERO(&fdsetError);
        SOCKET hSocketMax = 0;
        bool have_fds = false;

#ifndef WIN32
        // We add a pipe to the read set so that the select() call can be woken up from the outside
        // This is done when data is available for sending and at the same time optimistic sending was disabled
        // when pushing the data.
        // This is currently only implemented for POSIX compliant systems. This means that Windows will fall back to
        // timing out after 50ms and then trying to send. This is ok as we assume that heavy-load daemons are usually
        // run on Linux and friends.
        FD_SET(wakeupPipe[0], &fdsetRecv);
        hSocketMax = std::max(hSocketMax, (SOCKET)wakeupPipe[0]);
        have_fds = true;
#endif

        for (const ListenSocket& hListenSocket : vhListenSocket) {
            FD_SET(hListenSocket.socket, &fdsetRecv);
            hSocketMax = std::max(hSocketMax, hListenSocket.socket);
            have_fds = true;
        }

        {
            LOCK(cs_vNodes);
            for (CNode* pnode : vNodes) {
                // Implement the following logic:
                // * If there is data to send, select() for sending data. As this only
                //   happens when optimistic write failed, we choose to first drain the
                //   write buffer in this case before receiving more. This avoids
                //   needlessly queueing received data, if the remote peer is not themselves
                //   receiving data. This means properly utilizing TCP flow control signalling.
                // * Otherwise, if there is space left in the receive buffer, select() for
                //   receiving data.
                // * Hand off all complete messages to the processor, to be handled without
                //   blocking here.

                bool select_recv = !pnode->fPauseRecv;
                bool select_send;
                {
                    LOCK(pnode->cs_vSend);
                    select_send = !pnode->vSendMsg.empty();
                }

                LOCK(pnode->cs_hSocket);
                if (pnode->hSocket == INVALID_SOCKET)
                    continue;

                FD_SET(pnode->hSocket, &fdsetError);
                hSocketMax = std::max(hSocketMax, pnode->hSocket);
                have_fds = true;

                if (select_send) {
                    FD_SET(pnode->hSocket, &fdsetSend);
                    continue;
                }
                if (select_recv) {
                    FD_SET(pnode->hSocket, &fdsetRecv);
                }
            }
        }

        wakeupSelectNeeded = true;
        int nSelect = select(have_fds ? hSocketMax + 1 : 0,
            &fdsetRecv, &fdsetSend, &fdsetError, &timeout);
        wakeupSelectNeeded = false;
        if (interruptNet)
            return;

        if (nSelect == SOCKET_ERROR) {
            if (have_fds) {
                int nErr = WSAGetLastError();
                LogPrintf("socket select error %s\n", NetworkErrorString(nErr));
                for (unsigned int i = 0; i <= hSocketMax; i++)
                    FD_SET(i, &fdsetRecv);
            }
            FD_ZERO(&fdsetSend);
            FD_ZERO(&fdsetError);
            if (!interruptNet.sleep_for(std::chrono::milliseconds(timeout.tv_usec / 1000)))
                return;
        }

#ifndef WIN32
        // drain the wakeup pipe
        if (FD_ISSET(wakeupPipe[0], &fdsetRecv)) {
            LogPrint(BCLog::NET, "woke up select()\n");
            char buf[128];
            while (true) {
                int r = read(wakeupPipe[0], buf, sizeof(buf));
                if (r <= 0) {
                    break;
                }
            }
        }
#endif

        //
        // Accept new connections
        //
        for (const ListenSocket& hListenSocket : vhListenSocket) {
            if (hListenSocket.socket != INVALID_SOCKET && FD_ISSET(hListenSocket.socket, &fdsetRecv)) {
                AcceptConnection(hListenSocket);
            }
        }

        //
        // Service each socket
        //
        std::vector<CNode*> vNodesCopy = CopyNodeVector();
        for (CNode* pnode : vNodesCopy) {
            if (interruptNet)
                return;

            //
            // Receive
            //
            bool recvSet = false;
            bool sendSet = false;
            bool errorSet = false;
            {
                LOCK(pnode->cs_hSocket);
                if (pnode->hSocket == INVALID_SOCKET)
                    continue;
                recvSet = FD_ISSET(pnode->hSocket, &fdsetRecv);
                sendSet = FD_ISSET(pnode->hSocket, &fdsetSend);
                errorSet = FD_ISSET(pnode->hSocket, &fdsetError);
            }
            if (recvSet || errorSet) {
                // typical socket buffer is 8K-64K
                char pchBuf[0x10000];
                int nBytes = 0;
                {
                    LOCK(pnode->cs_hSocket);
                    if (pnode->hSocket == INVALID_SOCKET)
                        continue;
                    nBytes = recv(pnode->hSocket, pchBuf, sizeof(pchBuf), MSG_DONTWAIT);
                }
                if (nBytes > 0) {
                    bool notify = false;
                    if (!pnode->ReceiveMsgBytes(pchBuf, nBytes, notify))
                        pnode->CloseSocketDisconnect();
                    RecordBytesRecv(nBytes);
                    if (notify) {
                        size_t nSizeAdded = 0;
                        auto it(pnode->vRecvMsg.begin());
                        for (; it != pnode->vRecvMsg.end(); ++it) {
                            if (!it->complete())
                                break;
                            nSizeAdded += it->vRecv.size() + CMessageHeader::HEADER_SIZE;
                        }
                        {
                            LOCK(pnode->cs_vProcessMsg);
                            pnode->vProcessMsg.splice(pnode->vProcessMsg.end(), pnode->vRecvMsg, pnode->vRecvMsg.begin(), it);
                            pnode->nProcessQueueSize += nSizeAdded;
                            pnode->fPauseRecv = pnode->nProcessQueueSize > nReceiveFloodSize;
                        }
                        WakeMessageHandler();
                    }
                } else if (nBytes == 0) {
                    // socket closed gracefully
                    if (!pnode->fDisconnect) {
                        LogPrint(BCLog::NET, "socket closed\n");
                    }
                    pnode->CloseSocketDisconnect();
                } else if (nBytes < 0) {
                    // error
                    int nErr = WSAGetLastError();
                    if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS) {
                        if (!pnode->fDisconnect)
                            LogPrintf("socket recv error %s\n", NetworkErrorString(nErr));
                        pnode->CloseSocketDisconnect();
                    }
                }
            }

            //
            // Send
            //
            if (sendSet) {
                LOCK(pnode->cs_vSend);
                size_t nBytes = SocketSendData(pnode);
                if (nBytes) {
                    RecordBytesSent(nBytes);
                }
            }

            //
            // Inactivity checking
            //
            int64_t nTime = GetSystemTimeInSeconds();
            if (nTime - pnode->nTimeConnected > 60) {
                if (pnode->nLastRecv == 0 || pnode->nLastSend == 0) {
                    LogPrint(BCLog::NET, "socket no message in first 60 seconds, %d %d from %d\n", pnode->nLastRecv != 0, pnode->nLastSend != 0, pnode->GetId());
                    pnode->fDisconnect = true;
                } else if (nTime - pnode->nLastSend > TIMEOUT_INTERVAL) {
                    LogPrintf("socket sending timeout: %is\n", nTime - pnode->nLastSend);
                    pnode->fDisconnect = true;
                } else if (nTime - pnode->nLastRecv > (pnode->nVersion > BIP0031_VERSION ? TIMEOUT_INTERVAL : 90 * 60)) {
                    LogPrintf("socket receive timeout: %is\n", nTime - pnode->nLastRecv);
                    pnode->fDisconnect = true;
                } else if (pnode->nPingNonceSent && pnode->nPingUsecStart + TIMEOUT_INTERVAL * 1000000 < GetTimeMicros()) {
                    LogPrintf("ping timeout: %fs\n", 0.000001 * (GetTimeMicros() - pnode->nPingUsecStart));
                    pnode->fDisconnect = true;
                } else if (!pnode->fSuccessfullyConnected) {
                    LogPrintf("version handshake timeout from %d\n", pnode->GetId());
                    pnode->fDisconnect = true;
                }
            }
        }
        ReleaseNodeVector(vNodesCopy);
    }
}

void CConnman::WakeMessageHandler()
{
    {
        std::lock_guard<std::mutex> lock(mutexMsgProc);
        fMsgProcWake = true;
    }
    condMsgProc.notify_one();
}

void CConnman::WakeSelect()
{
#ifndef WIN32
    if (wakeupPipe[1] == -1) {
        return;
    }

    LogPrint(BCLog::NET, "waking up select()\n");

    char buf[1];
    if (write(wakeupPipe[1], buf, 1) != 1) {
        LogPrint(BCLog::NET, "write to wakeupPipe failed\n");
    }
#endif

    wakeupSelectNeeded = false;
}


#ifdef USE_UPNP
void ThreadMapPort()
{
    std::string port = strprintf("%u", GetListenPort());
    const char* multicastif = 0;
    const char* minissdpdpath = 0;
    struct UPNPDev* devlist = 0;
    char lanaddr[64];

#ifndef UPNPDISCOVER_SUCCESS
    /* miniupnpc 1.5 */
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0);
#elif MINIUPNPC_API_VERSION < 14
    /* miniupnpc 1.6 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, &error);
#else
    /* miniupnpc 1.9.20150730 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, 2, &error);
#endif

    struct UPNPUrls urls;
    struct IGDdatas data;
    int r;

    r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
    if (r == 1) {
        if (fDiscover) {
            char externalIPAddress[40];
            r = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIPAddress);
            if (r != UPNPCOMMAND_SUCCESS)
                LogPrintf("UPnP: GetExternalIPAddress() returned %d\n", r);
            else {
                if (externalIPAddress[0]) {
                    CNetAddr resolved;
                    if (LookupHost(externalIPAddress, resolved, false)) {
                        LogPrintf("UPnP: ExternalIPAddress = %s\n", resolved.ToString().c_str());
                        AddLocal(resolved, LOCAL_UPNP);
                    }
                } else
                    LogPrintf("UPnP: GetExternalIPAddress failed.\n");
            }
        }

        std::string strDesc = "Memeium Core " + FormatFullVersion();

        try {
            while (true) {
#ifndef UPNPDISCOVER_SUCCESS
                /* miniupnpc 1.5 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                    port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0);
#else
                /* miniupnpc 1.6 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                    port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

                if (r != UPNPCOMMAND_SUCCESS)
                    LogPrintf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                        port, port, lanaddr, r, strupnperror(r));
                else
                    LogPrintf("UPnP Port Mapping successful.\n");

                MilliSleep(20 * 60 * 1000); // Refresh every 20 minutes
            }
        } catch (const boost::thread_interrupted&) {
            r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "TCP", 0);
            LogPrintf("UPNP_DeletePortMapping() returned: %d\n", r);
            freeUPNPDevlist(devlist);
            devlist = 0;
            FreeUPNPUrls(&urls);
            throw;
        }
    } else {
        LogPrintf("No valid UPnP IGDs found\n");
        freeUPNPDevlist(devlist);
        devlist = 0;
        if (r != 0)
            FreeUPNPUrls(&urls);
    }
}

void MapPort(bool fUseUPnP)
{
    static boost::thread* upnp_thread = nullptr;

    if (fUseUPnP) {
        if (upnp_thread) {
            upnp_thread->interrupt();
            upnp_thread->join();
            delete upnp_thread;
        }
        upnp_thread = new boost::thread(boost::bind(&TraceThread<void (*)()>, "upnp", &ThreadMapPort));
    } else if (upnp_thread) {
        upnp_thread->interrupt();
        upnp_thread->join();
        delete upnp_thread;
        upnp_thread = nullptr;
    }
}

#else
void MapPort(bool)
{
    // Intentionally left blank.
}
#endif


static std::string GetDNSHost(const CDNSSeedData& data, ServiceFlags* requiredServiceBits)
{
    // use default host for non-filter-capable seeds or if we use the default service bits (NODE_NETWORK)
    if (!data.supportsServiceBitsFiltering || *requiredServiceBits == NODE_NETWORK) {
        *requiredServiceBits = NODE_NETWORK;
        return data.host;
    }

    return strprintf("x%x.%s", *requiredServiceBits, data.host);
}


void CConnman::ThreadDNSAddressSeed()
{
    // goal: only query DNS seeds if address need is acute
    // Avoiding DNS seeds when we don't need them improves user privacy by
    //  creating fewer identifying DNS requests, reduces trust by giving seeds
    //  less influence on the network topology, and reduces traffic to the seeds.
    if ((addrman.size() > 0) &&
        (!gArgs.GetBoolArg("-forcednsseed", DEFAULT_FORCEDNSSEED))) {
        if (!interruptNet.sleep_for(std::chrono::seconds(11)))
            return;

        LOCK(cs_vNodes);
        int nRelevant = 0;
        for (auto pnode : vNodes) {
            nRelevant += pnode->fSuccessfullyConnected && !pnode->fFeeler && !pnode->fOneShot && !pnode->m_manual_connection && !pnode->fInbound;
        }
        if (nRelevant >= 2) {
            LogPrintf("P2P peers available. Skipped DNS seeding.\n");
            return;
        }
    }

    const std::vector<CDNSSeedData>& vSeeds = Params().DNSSeeds();
    int found = 0;

    LogPrintf("Loading addresses from DNS seeds (could take a while)\n");

    for (const CDNSSeedData& seed : vSeeds) {
        if (interruptNet) {
            return;
        }
        if (HaveNameProxy()) {
            AddOneShot(seed.host);
        } else {
            std::vector<CNetAddr> vIPs;
            std::vector<CAddress> vAdd;
            ServiceFlags requiredServiceBits = GetDesirableServiceFlags(NODE_NONE);
            std::string host = GetDNSHost(seed, &requiredServiceBits);
            CNetAddr resolveSource;
            if (!resolveSource.SetInternal(host)) {
                continue;
            }
            if (LookupHost(host.c_str(), vIPs, 0, true)) {
                for (const CNetAddr& ip : vIPs) {
                    int nOneDay = 24 * 3600;
                    CAddress addr = CAddress(CService(ip, Params().GetDefaultPort()), requiredServiceBits);
                    addr.nTime = GetTime() - 3 * nOneDay - GetRand(4 * nOneDay); // use a random age between 3 and 7 days old
                    vAdd.push_back(addr);
                    found++;
                }
                addrman.Add(vAdd, resolveSource);
            }
        }
    }

    LogPrintf("%d addresses found from DNS seeds\n", found);
}


void CConnman::DumpAddresses()
{
    int64_t nStart = GetTimeMillis();

    CAddrDB adb;
    adb.Write(addrman);

    LogPrint(BCLog::NET, "Flushed %d addresses to peers.dat  %dms\n",
        addrman.size(), GetTimeMillis() - nStart);
}

void CConnman::DumpData()
{
    DumpAddresses();
    DumpBanlist();
}

void CConnman::ProcessOneShot()
{
    std::string strDest;
    {
        LOCK(cs_vOneShots);
        if (vOneShots.empty())
            return;
        strDest = vOneShots.front();
        vOneShots.pop_front();
    }
    CAddress addr;
    CSemaphoreGrant grant(*semOutbound, true);
    if (grant) {
        if (!OpenNetworkConnection(addr, false, &grant, strDest.c_str(), true))
            AddOneShot(strDest);
    }
}

bool CConnman::GetTryNewOutboundPeer()
{
    return m_try_another_outbound_peer;
}

void CConnman::SetTryNewOutboundPeer(bool flag)
{
    m_try_another_outbound_peer = flag;
    LogPrint(BCLog::NET, "net: setting try another outbound peer=%s\n", flag ? "true" : "false");
}

// Return the number of peers we have over our outbound connection limit
// Exclude peers that are marked for disconnect, or are going to be
// disconnected soon (eg one-shots and feelers)
// Also exclude peers that haven't finished initial connection handshake yet
// (so that we don't decide we're over our desired connection limit, and then
// evict some peer that has finished the handshake)
int CConnman::GetExtraOutboundCount()
{
    int nOutbound = 0;
    {
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodes) {
            // don't count outbound smartnodes
            if (pnode->fSmartnode) {
                continue;
            }
            if (!pnode->fInbound && !pnode->m_manual_connection && !pnode->fFeeler && !pnode->fDisconnect && !pnode->fOneShot && pnode->fSuccessfullyConnected) {
                ++nOutbound;
            }
        }
    }
    return std::max(nOutbound - nMaxOutbound, 0);
}

void CConnman::ThreadOpenConnections()
{
    // Connect to specific addresses
    if (gArgs.IsArgSet("-connect")) {
        for (int64_t nLoop = 0;; nLoop++) {
            ProcessOneShot();
            for (const std::string& strAddr : gArgs.GetArgs("-connect")) {
                CAddress addr(CService(), NODE_NONE);
                OpenNetworkConnection(addr, false, nullptr, strAddr.c_str(), false, false, true);
                for (int i = 0; i < 10 && i < nLoop; i++) {
                    if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                        return;
                }
            }
            if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                return;
        }
    }

    // Initiate network connections
    int64_t nStart = GetTime();

    // Minimum time before next feeler connection (in microseconds).
    int64_t nNextFeeler = PoissonNextSend(nStart * 1000 * 1000, FEELER_INTERVAL);
    while (!interruptNet) {
        ProcessOneShot();

        if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
            return;

        CSemaphoreGrant grant(*semOutbound);
        if (interruptNet)
            return;

        // Add seed nodes if DNS seeds are all down (an infrastructure attack?).
        if (addrman.size() == 0 && (GetTime() - nStart > 60)) {
            static bool done = false;
            if (!done) {
                LogPrintf("Adding fixed seed nodes as DNS doesn't seem to be available.\n");
                CNetAddr local;
                local.SetInternal("fixedseeds");
                addrman.Add(convertSeed6(Params().FixedSeeds()), local);
                done = true;
            }
        }

        //
        // Choose an address to connect to based on most recently seen
        //
        CAddress addrConnect;

        // Only connect out to one peer per network group (/16 for IPv4).
        // Do this here so we don't have to critsect vNodes inside mapAddresses critsect.
        // This is only done for mainnet and testnet
        int nOutbound = 0;
        std::set<std::vector<unsigned char>> setConnected;
        if (!Params().AllowMultipleAddressesFromGroup()) {
            LOCK(cs_vNodes);
            for (CNode* pnode : vNodes) {
                if (!pnode->fInbound && !pnode->fSmartnode && !pnode->m_manual_connection) {
                    // Netgroups for inbound and addnode peers are not excluded because our goal here
                    // is to not use multiple of our limited outbound slots on a single netgroup
                    // but inbound and addnode peers do not use our outbound slots.  Inbound peers
                    // also have the added issue that they're attacker controlled and could be used
                    // to prevent us from connecting to particular hosts if we used them here.
                    setConnected.insert(pnode->addr.GetGroup());
                    nOutbound++;
                }
            }
        }

        // Feeler Connections
        //
        // Design goals:
        //  * Increase the number of connectable addresses in the tried table.
        //
        // Method:
        //  * Choose a random address from new and attempt to connect to it if we can connect
        //    successfully it is added to tried.
        //  * Start attempting feeler connections only after node finishes making outbound
        //    connections.
        //  * Only make a feeler connection once every few minutes.
        //
        bool fFeeler = false;

        if (nOutbound >= nMaxOutbound && !GetTryNewOutboundPeer()) {
            int64_t nTime = GetTimeMicros(); // The current time right now (in microseconds).
            if (nTime > nNextFeeler) {
                nNextFeeler = PoissonNextSend(nTime, FEELER_INTERVAL);
                fFeeler = true;
            } else {
                continue;
            }
        }
        auto mnList = deterministicMNManager->GetListAtChainTip();

        int64_t nANow = GetAdjustedTime();
        int nTries = 0;
        while (!interruptNet) {
            CAddrInfo addr = addrman.Select(fFeeler);

            bool isSmartnode = mnList.GetMNByService(addr) != nullptr;

            // if we selected an invalid address, restart
            if (!addr.IsValid() || setConnected.count(addr.GetGroup()))
                break;

            // if we selected a local address, restart (local addresses are allowed in regtest and devnet)
            bool fAllowLocal = Params().AllowMultiplePorts() && addrConnect.GetPort() != GetListenPort();
            if (!fAllowLocal && IsLocal(addrConnect))
                break;

            // If we didn't find an appropriate destination after trying 100 addresses fetched from addrman,
            // stop this loop, and let the outer loop run again (which sleeps, adds seed nodes, recalculates
            // already-connected network ranges, ...) before trying new addrman addresses.
            nTries++;
            if (nTries > 100)
                break;

            if (IsLimited(addr))
                continue;

            // only consider very recently tried nodes after 30 failed attempts
            if (nANow - addr.nLastTry < 600 && nTries < 30)
                continue;

            // for non-feelers, require all the services we'll want,
            // for feelers, only require they be a full node (only because most
            // SPV clients don't have a good address DB available)
            if (!isSmartnode && !fFeeler && !HasAllDesirableServiceFlags(addr.nServices)) {
                continue;
            } else if (!isSmartnode && fFeeler && !MayHaveUsefulAddressDB(addr.nServices)) {
                continue;
            }

            // do not allow non-default ports, unless after 50 invalid addresses selected already
            if ((!isSmartnode || !Params().AllowMultiplePorts()) && addr.GetPort() != Params().GetDefaultPort() && addr.GetPort() != GetListenPort() && nTries < 50)
                continue;

            addrConnect = addr;
            break;
        }

        if (addrConnect.IsValid()) {
            if (fFeeler) {
                // Add small amount of random noise before connection to avoid synchronization.
                int randsleep = GetRandInt(FEELER_SLEEP_WINDOW * 1000);
                if (!interruptNet.sleep_for(std::chrono::milliseconds(randsleep)))
                    return;
                if (fLogIPs) {
                    LogPrint(BCLog::NET, "Making feeler connection to %s\n", addrConnect.ToString());
                } else {
                    LogPrint(BCLog::NET, "Making feeler connection\n");
                }
            }

            OpenNetworkConnection(addrConnect, (int)setConnected.size() >= std::min(nMaxConnections - 1, 2), &grant, nullptr, false, fFeeler);
        }
    }
}

std::vector<AddedNodeInfo> CConnman::GetAddedNodeInfo()
{
    std::vector<AddedNodeInfo> ret;

    std::list<std::string> lAddresses(0);
    {
        LOCK(cs_vAddedNodes);
        ret.reserve(vAddedNodes.size());
        for (const std::string& strAddNode : vAddedNodes)
            lAddresses.push_back(strAddNode);
    }


    // Build a map of all already connected addresses (by IP:port and by name) to inbound/outbound and resolved CService
    std::map<CService, bool> mapConnected;
    std::map<std::string, std::pair<bool, CService>> mapConnectedByName;
    {
        LOCK(cs_vNodes);
        for (const CNode* pnode : vNodes) {
            if (pnode->addr.IsValid()) {
                mapConnected[pnode->addr] = pnode->fInbound;
            }
            std::string addrName = pnode->GetAddrName();
            if (!addrName.empty()) {
                mapConnectedByName[std::move(addrName)] = std::make_pair(pnode->fInbound, static_cast<const CService&>(pnode->addr));
            }
        }
    }

    for (const std::string& strAddNode : lAddresses) {
        CService service(LookupNumeric(strAddNode.c_str(), Params().GetDefaultPort()));
        if (service.IsValid()) {
            // strAddNode is an IP:port
            auto it = mapConnected.find(service);
            if (it != mapConnected.end()) {
                ret.push_back(AddedNodeInfo{strAddNode, service, true, it->second});
            } else {
                ret.push_back(AddedNodeInfo{strAddNode, CService(), false, false});
            }
        } else {
            // strAddNode is a name
            auto it = mapConnectedByName.find(strAddNode);
            if (it != mapConnectedByName.end()) {
                ret.push_back(AddedNodeInfo{strAddNode, it->second.second, true, it->second.first});
            } else {
                ret.push_back(AddedNodeInfo{strAddNode, CService(), false, false});
            }
        }
    }

    return ret;
}

void CConnman::ThreadOpenAddedConnections()
{
    {
        LOCK(cs_vAddedNodes);
        vAddedNodes = gArgs.GetArgs("-addnode");
    }

    while (true) {
        CSemaphoreGrant grant(*semAddnode);
        std::vector<AddedNodeInfo> vInfo = GetAddedNodeInfo();
        bool tried = false;
        for (const AddedNodeInfo& info : vInfo) {
            if (!info.fConnected) {
                if (!grant.TryAcquire()) {
                    // If we've used up our semaphore and need a new one, lets not wait here since while we are waiting
                    // the addednodeinfo state might change.
                    break;
                }
                // If strAddedNode is an IP/port, decode it immediately, so
                // OpenNetworkConnection can detect existing connections to that IP/port.
                tried = true;
                CService service(LookupNumeric(info.strAddedNode.c_str(), Params().GetDefaultPort()));
                OpenNetworkConnection(CAddress(service, NODE_NONE), false, &grant, info.strAddedNode.c_str(), false, false, true);
                if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                    return;
            }
        }
        // Retry every 60 seconds if a connection was attempted, otherwise two seconds
        if (!interruptNet.sleep_for(std::chrono::seconds(tried ? 60 : 2)))
            return;
    }
}

void CConnman::ThreadOpenSmartnodeConnections()
{
    // Connecting to specific addresses, no smartnode connections available
    if (gArgs.IsArgSet("-connect") && gArgs.GetArgs("-connect").size() > 0)
        return;

    while (!interruptNet) {
        if (!interruptNet.sleep_for(std::chrono::milliseconds(1000)))
            return;

        std::set<CService> connectedNodes;
        std::set<uint256> connectedProRegTxHashes;
        ForEachNode([&](const CNode* pnode) {
            connectedNodes.emplace(pnode->addr);
            if (!pnode->verifiedProRegTxHash.IsNull()) {
                connectedProRegTxHashes.emplace(pnode->verifiedProRegTxHash);
            }
        });
        auto mnList = deterministicMNManager->GetListAtChainTip();

        CSemaphoreGrant grant(*semSmartnodeOutbound);
        if (interruptNet)
            return;

        int64_t nANow = GetAdjustedTime();

        // NOTE: Process only one pending smartnode at a time

        CService addr;
        {                  // don't hold lock while calling OpenSmartnodeConnection as cs_main is locked deep inside
            LOCK(cs_main); // Lock cs_main first to avoid deadlocks (it is recursively locked deeper)
            LOCK2(cs_vNodes, cs_vPendingSmartnodes);

            std::vector<CService> pending;
            for (const auto& group : smartnodeQuorumNodes) {
                for (const auto& proRegTxHash : group.second) {
                    auto dmn = mnList.GetMN(proRegTxHash);
                    if (!dmn) {
                        continue;
                    }
                    const auto& addr2 = dmn->pdmnState->addr;
                    if (!connectedNodes.count(addr2) && !IsSmartnodeOrDisconnectRequested(addr2) && !connectedProRegTxHashes.count(proRegTxHash)) {
                        auto addrInfo = addrman.GetAddressInfo(addr2);
                        // back off trying connecting to an address if we already tried recently
                        if (addrInfo.IsValid() && nANow - addrInfo.nLastTry < 60) {
                            continue;
                        }
                        pending.emplace_back(addr2);
                    }
                }
            }

            if (!vPendingSmartnodes.empty()) {
                auto addr2 = vPendingSmartnodes.front();
                vPendingSmartnodes.erase(vPendingSmartnodes.begin());
                if (!connectedNodes.count(addr2) && !IsSmartnodeOrDisconnectRequested(addr2)) {
                    pending.emplace_back(addr2);
                }
            }

            if (pending.empty()) {
                // nothing to do, keep waiting
                continue;
            }

            std::random_shuffle(pending.begin(), pending.end());
            addr = pending.front();
        }

        OpenSmartnodeConnection(CAddress(addr, NODE_NETWORK));
        // should be in the list now if connection was opened
        ForNode(addr, CConnman::AllNodes, [&](CNode* pnode) {
            if (pnode->fDisconnect) {
                return false;
            }
            grant.MoveTo(pnode->grantSmartnodeOutbound);
            return true;
        });
    }
}

// if successful, this moves the passed grant to the constructed node
bool CConnman::OpenNetworkConnection(const CAddress& addrConnect, bool fCountFailure, CSemaphoreGrant* grantOutbound, const char* pszDest, bool fOneShot, bool fFeeler, bool manual_connection, bool fConnectToSmartnode)
{
    //
    // Initiate outbound network connection
    //
    if (interruptNet) {
        return false;
    }
    if (!fNetworkActive) {
        return false;
    }
    if (!pszDest) {
        // banned or exact match?
        if (IsBanned(addrConnect) || FindNode(addrConnect.ToStringIPPort()))
            return false;
        // local and not a connection to itself?
        bool fAllowLocal = Params().AllowMultiplePorts() && addrConnect.GetPort() != GetListenPort();
        if (!fAllowLocal && IsLocal(addrConnect))
            return false;
        // if multiple ports for same IP are allowed, search for IP:PORT match, otherwise search for IP-only match
        if ((!Params().AllowMultiplePorts() && FindNode((CNetAddr)addrConnect)) ||
            (Params().AllowMultiplePorts() && FindNode((CService)addrConnect)))
            return false;
    } else if (FindNode(std::string(pszDest)))
        return false;

    CNode* pnode = ConnectNode(addrConnect, pszDest, fCountFailure);

    if (!pnode)
        return false;
    if (grantOutbound)
        grantOutbound->MoveTo(pnode->grantOutbound);
    if (fOneShot)
        pnode->fOneShot = true;
    if (fFeeler)
        pnode->fFeeler = true;
    if (manual_connection)
        pnode->m_manual_connection = true;
    if (fConnectToSmartnode)
        pnode->fSmartnode = true;

    m_msgproc->InitializeNode(pnode);
    {
        LOCK(cs_vNodes);
        vNodes.push_back(pnode);
    }

    return true;
}

bool CConnman::OpenSmartnodeConnection(const CAddress& addrConnect)
{
    return OpenNetworkConnection(addrConnect, false, nullptr, nullptr, false, false, false, true);
}

void CConnman::ThreadMessageHandler()
{
    while (!flagInterruptMsgProc) {
        std::vector<CNode*> vNodesCopy = CopyNodeVector();

        bool fMoreWork = false;

        for (CNode* pnode : vNodesCopy) {
            if (pnode->fDisconnect)
                continue;

            // Receive messages
            bool fMoreNodeWork = m_msgproc->ProcessMessages(pnode, flagInterruptMsgProc);
            fMoreWork |= (fMoreNodeWork && !pnode->fPauseSend);
            if (flagInterruptMsgProc)
                return;
            // Send messages
            {
                LOCK(pnode->cs_sendProcessing);
                m_msgproc->SendMessages(pnode, flagInterruptMsgProc);
            }

            if (flagInterruptMsgProc)
                return;
        }

        ReleaseNodeVector(vNodesCopy);

        std::unique_lock<std::mutex> lock(mutexMsgProc);
        if (!fMoreWork) {
            condMsgProc.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(100), [this] { return fMsgProcWake; });
        }
        fMsgProcWake = false;
    }
}


bool CConnman::BindListenPort(const CService& addrBind, std::string& strError, bool fWhitelisted)
{
    strError = "";
    int nOne = 1;

    // Create socket for listening for incoming connections
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len)) {
        strError = strprintf("Error: Bind address family for %s not supported", addrBind.ToString());
        LogPrintf("%s\n", strError);
        return false;
    }

    SOCKET hListenSocket = socket(((struct sockaddr*)&sockaddr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (hListenSocket == INVALID_SOCKET) {
        strError = strprintf("Error: Couldn't open socket for incoming connections (socket returned error %s)", NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        return false;
    }
    if (!IsSelectableSocket(hListenSocket)) {
        strError = "Error: Couldn't create a listenable socket for incoming connections";
        LogPrintf("%s\n", strError);
        return false;
    }


#ifndef WIN32
#ifdef SO_NOSIGPIPE
    // Different way of disabling SIGPIPE on BSD
    setsockopt(hListenSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&nOne, sizeof(int));
#endif
    // Allow binding if the port is still in TIME_WAIT state after
    // the program was closed and restarted.
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (void*)&nOne, sizeof(int));
    // Disable Nagle's algorithm
    setsockopt(hListenSocket, IPPROTO_TCP, TCP_NODELAY, (void*)&nOne, sizeof(int));
#else
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&nOne, sizeof(int));
    setsockopt(hListenSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&nOne, sizeof(int));
#endif

    // Set to non-blocking, incoming connections will also inherit this
    if (!SetSocketNonBlocking(hListenSocket, true)) {
        CloseSocket(hListenSocket);
        strError = strprintf("BindListenPort: Setting listening socket to non-blocking failed, error %s\n", NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        return false;
    }

    // some systems don't have IPV6_V6ONLY but are always v6only; others do have the option
    // and enable it by default or not. Try to enable it, if possible.
    if (addrBind.IsIPv6()) {
#ifdef IPV6_V6ONLY
#ifdef WIN32
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&nOne, sizeof(int));
#else
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&nOne, sizeof(int));
#endif
#endif
#ifdef WIN32
        int nProtLevel = PROTECTION_LEVEL_UNRESTRICTED;
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_PROTECTION_LEVEL, (const char*)&nProtLevel, sizeof(int));
#endif
    }

    if (::bind(hListenSocket, (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR) {
        int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE)
            strError = strprintf(_("Unable to bind to %s on this computer. %s is probably already running."), addrBind.ToString(), _(PACKAGE_NAME));
        else
            strError = strprintf(_("Unable to bind to %s on this computer (bind returned error %s)"), addrBind.ToString(), NetworkErrorString(nErr));
        LogPrintf("%s\n", strError);
        CloseSocket(hListenSocket);
        return false;
    }
    LogPrintf("Bound to %s\n", addrBind.ToString());

    // Listen for incoming connections
    if (listen(hListenSocket, SOMAXCONN) == SOCKET_ERROR) {
        strError = strprintf(_("Error: Listening for incoming connections failed (listen returned error %s)"), NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        CloseSocket(hListenSocket);
        return false;
    }

    vhListenSocket.push_back(ListenSocket(hListenSocket, fWhitelisted));

    if (addrBind.IsRoutable() && fDiscover && !fWhitelisted)
        AddLocal(addrBind, LOCAL_BIND);

    return true;
}

void Discover(boost::thread_group& threadGroup)
{
    if (!fDiscover)
        return;

#ifdef WIN32
    // Get local host IP
    char pszHostName[256] = "";
    if (gethostname(pszHostName, sizeof(pszHostName)) != SOCKET_ERROR) {
        std::vector<CNetAddr> vaddr;
        if (LookupHost(pszHostName, vaddr, 0, true)) {
            for (const CNetAddr& addr : vaddr) {
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: %s - %s\n", __func__, pszHostName, addr.ToString());
            }
        }
    }
#else
    // Get local host ip
    struct ifaddrs* myaddrs;
    if (getifaddrs(&myaddrs) == 0) {
        for (struct ifaddrs* ifa = myaddrs; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0) continue;
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            if (strcmp(ifa->ifa_name, "lo0") == 0) continue;
            if (ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in* s4 = (struct sockaddr_in*)(ifa->ifa_addr);
                CNetAddr addr(s4->sin_addr);
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: IPv4 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                struct sockaddr_in6* s6 = (struct sockaddr_in6*)(ifa->ifa_addr);
                CNetAddr addr(s6->sin6_addr);
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: IPv6 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
            }
        }
        freeifaddrs(myaddrs);
    }
#endif
}

void CConnman::SetNetworkActive(bool active)
{
    LogPrint(BCLog::NET, "SetNetworkActive: %s\n", active);

    if (fNetworkActive == active) {
        return;
    }

    fNetworkActive = active;

    if (!fNetworkActive) {
        LOCK(cs_vNodes);
        // Close sockets to all nodes
        for (CNode* pnode : vNodes) {
            pnode->CloseSocketDisconnect();
        }
    }

    uiInterface.NotifyNetworkActiveChanged(fNetworkActive);
}

CConnman::CConnman(uint64_t nSeed0In, uint64_t nSeed1In) :
    addrman(Params().AllowMultiplePorts()),
    nSeed0(nSeed0In), nSeed1(nSeed1In)
{
    fNetworkActive = true;
    setBannedIsDirty = false;
    fAddressesInitialized = false;
    nLastNodeId = 0;
    nSendBufferMaxSize = 0;
    nReceiveFloodSize = 0;
    semOutbound = nullptr;
    semAddnode = nullptr;
    semSmartnodeOutbound = nullptr;
    flagInterruptMsgProc = false;
    SetTryNewOutboundPeer(false);

    Options connOptions;
    Init(connOptions);
}

NodeId CConnman::GetNewNodeId()
{
    return nLastNodeId.fetch_add(1, std::memory_order_relaxed);
}


bool CConnman::Bind(const CService& addr, unsigned int flags)
{
    if (!(flags & BF_EXPLICIT) && IsLimited(addr))
        return false;
    std::string strError;
    if (!BindListenPort(addr, strError, (flags & BF_WHITELIST) != 0)) {
        if ((flags & BF_REPORT_ERROR) && clientInterface) {
            clientInterface->ThreadSafeMessageBox(strError, "", CClientUIInterface::MSG_ERROR);
        }
        return false;
    }
    return true;
}

bool CConnman::InitBinds(const std::vector<CService>& binds, const std::vector<CService>& whiteBinds)
{
    bool fBound = false;
    for (const auto& addrBind : binds) {
        fBound |= Bind(addrBind, (BF_EXPLICIT | BF_REPORT_ERROR));
    }
    for (const auto& addrBind : whiteBinds) {
        fBound |= Bind(addrBind, (BF_EXPLICIT | BF_REPORT_ERROR | BF_WHITELIST));
    }
    if (binds.empty() && whiteBinds.empty()) {
        struct in_addr inaddr_any;
        inaddr_any.s_addr = INADDR_ANY;
        fBound |= Bind(CService((in6_addr)IN6ADDR_ANY_INIT, GetListenPort()), BF_NONE);
        fBound |= Bind(CService(inaddr_any, GetListenPort()), !fBound ? BF_REPORT_ERROR : BF_NONE);
    }
    return fBound;
}

bool CConnman::Start(CScheduler& scheduler, const Options& connOptions)
{
    Init(connOptions);

    nTotalBytesRecv = 0;
    nTotalBytesSent = 0;
    nMaxOutboundTotalBytesSentInCycle = 0;
    nMaxOutboundCycleStartTime = 0;

    if (fListen && !InitBinds(connOptions.vBinds, connOptions.vWhiteBinds)) {
        if (clientInterface) {
            clientInterface->ThreadSafeMessageBox(
                _("Failed to listen on any port. Use -listen=0 if you want this."),
                "", CClientUIInterface::MSG_ERROR);
        }
        return false;
    }

    for (const auto& strDest : connOptions.vSeedNodes) {
        AddOneShot(strDest);
    }

    if (clientInterface) {
        clientInterface->InitMessage(_("Loading P2P addresses..."));
    }
    // Load addresses from peers.dat
    int64_t nStart = GetTimeMillis();
    {
        CAddrDB adb;
        if (adb.Read(addrman))
            LogPrintf("Loaded %i addresses from peers.dat  %dms\n", addrman.size(), GetTimeMillis() - nStart);
        else {
            addrman.Clear(); // Addrman can be in an inconsistent state after failure, reset it
            LogPrintf("Invalid or missing peers.dat; recreating\n");
            DumpAddresses();
        }
    }
    if (clientInterface)
        clientInterface->InitMessage(_("Loading banlist..."));
    // Load addresses from banlist.dat
    nStart = GetTimeMillis();
    CBanDB bandb;
    banmap_t banmap;
    if (bandb.Read(banmap)) {
        SetBanned(banmap);        // thread save setter
        SetBannedSetDirty(false); // no need to write down, just read data
        SweepBanned();            // sweep out unused entries

        LogPrint(BCLog::NET, "Loaded %d banned node ips/subnets from banlist.dat  %dms\n",
            banmap.size(), GetTimeMillis() - nStart);
    } else {
        LogPrintf("Invalid or missing banlist.dat; recreating\n");
        SetBannedSetDirty(true); // force write
        DumpBanlist();
    }

    uiInterface.InitMessage(_("Starting network threads..."));

    fAddressesInitialized = true;

    if (semOutbound == nullptr) {
        // initialize semaphore
        semOutbound = new CSemaphore(std::min((nMaxOutbound + nMaxFeeler), nMaxConnections));
    }
    if (semAddnode == nullptr) {
        // initialize semaphore
        semAddnode = new CSemaphore(nMaxAddnode);
    }

    if (semSmartnodeOutbound == nullptr) {
        // initialize semaphore
        semSmartnodeOutbound = new CSemaphore(fSmartnodeMode ? MAX_OUTBOUND_SMARTNODE_CONNECTIONS_ON_MN : MAX_OUTBOUND_SMARTNODE_CONNECTIONS);
    }

    //
    // Start threads
    //
    assert(m_msgproc);
    InterruptSocks5(false);
    interruptNet.reset();
    flagInterruptMsgProc = false;

    {
        std::unique_lock<std::mutex> lock(mutexMsgProc);
        fMsgProcWake = false;
    }

#ifndef WIN32
    if (pipe(wakeupPipe) != 0) {
        wakeupPipe[0] = wakeupPipe[1] = -1;
        LogPrint(BCLog::NET, "pipe() for wakeupPipe failed\n");
    } else {
        int fFlags = fcntl(wakeupPipe[0], F_GETFL, 0);
        if (fcntl(wakeupPipe[0], F_SETFL, fFlags | O_NONBLOCK) == -1) {
            LogPrint(BCLog::NET, "fcntl for O_NONBLOCK on wakeupPipe failed\n");
        }
        fFlags = fcntl(wakeupPipe[1], F_GETFL, 0);
        if (fcntl(wakeupPipe[1], F_SETFL, fFlags | O_NONBLOCK) == -1) {
            LogPrint(BCLog::NET, "fcntl for O_NONBLOCK on wakeupPipe failed\n");
        }
    }
#endif

    // Send and receive from sockets, accept connections
    threadSocketHandler = std::thread(&TraceThread<std::function<void()>>, "net", std::function<void()>(std::bind(&CConnman::ThreadSocketHandler, this)));

    if (!gArgs.GetBoolArg("-dnsseed", true))
        LogPrintf("DNS seeding disabled\n");
    else
        threadDNSAddressSeed = std::thread(&TraceThread<std::function<void()>>, "dnsseed", std::function<void()>(std::bind(&CConnman::ThreadDNSAddressSeed, this)));

    // Initiate outbound connections from -addnode
    threadOpenAddedConnections = std::thread(&TraceThread<std::function<void()>>, "addcon", std::function<void()>(std::bind(&CConnman::ThreadOpenAddedConnections, this)));

    // Initiate outbound connections unless connect=0
    if (!gArgs.IsArgSet("-connect") || gArgs.GetArgs("-connect").size() != 1 || gArgs.GetArgs("-connect")[0] != "0")
        threadOpenConnections = std::thread(&TraceThread<std::function<void()>>, "opencon", std::function<void()>(std::bind(&CConnman::ThreadOpenConnections, this)));

    // Initiate smartnode connections
    threadOpenSmartnodeConnections = std::thread(&TraceThread<std::function<void()>>, "mncon", std::function<void()>(std::bind(&CConnman::ThreadOpenSmartnodeConnections, this)));

    // Process messages
    threadMessageHandler = std::thread(&TraceThread<std::function<void()>>, "msghand", std::function<void()>(std::bind(&CConnman::ThreadMessageHandler, this)));

    // Dump network addresses
    scheduler.scheduleEvery(std::bind(&CConnman::DumpData, this), DUMP_ADDRESSES_INTERVAL * 1000);

    return true;
}

class CNetCleanup
{
public:
    CNetCleanup() {}

    ~CNetCleanup()
    {
#ifdef WIN32
        // Shutdown Windows Sockets
        WSACleanup();
#endif
    }
} instance_of_cnetcleanup;

void CExplicitNetCleanup::callCleanup()
{
    // Explicit call to destructor of CNetCleanup because it's not implicitly called
    // when the wallet is restarted from within the wallet itself.
    CNetCleanup* tmp = new CNetCleanup();
    delete tmp; // Stroustrup's gonna kill me for that
}

void CConnman::Interrupt()
{
    {
        std::lock_guard<std::mutex> lock(mutexMsgProc);
        flagInterruptMsgProc = true;
    }
    condMsgProc.notify_all();

    interruptNet();
    InterruptSocks5(true);

    if (semOutbound) {
        for (int i = 0; i < (nMaxOutbound + nMaxFeeler); i++) {
            semOutbound->post();
        }
    }

    if (semAddnode) {
        for (int i = 0; i < nMaxAddnode; i++) {
            semAddnode->post();
        }
    }

    if (semSmartnodeOutbound) {
        int nMaxSmartnodeOutbound = fSmartnodeMode ? MAX_OUTBOUND_SMARTNODE_CONNECTIONS_ON_MN : MAX_OUTBOUND_SMARTNODE_CONNECTIONS;
        for (int i = 0; i < nMaxSmartnodeOutbound; i++) {
            semSmartnodeOutbound->post();
        }
    }
}

void CConnman::Stop()
{
    if (threadMessageHandler.joinable())
        threadMessageHandler.join();
    if (threadOpenSmartnodeConnections.joinable())
        threadOpenSmartnodeConnections.join();
    if (threadOpenConnections.joinable())
        threadOpenConnections.join();
    if (threadOpenAddedConnections.joinable())
        threadOpenAddedConnections.join();
    if (threadDNSAddressSeed.joinable())
        threadDNSAddressSeed.join();
    if (threadSocketHandler.joinable())
        threadSocketHandler.join();

    if (fAddressesInitialized) {
        DumpData();
        fAddressesInitialized = false;
    }

    // Close sockets
    for (CNode* pnode : vNodes)
        pnode->CloseSocketDisconnect();
    for (ListenSocket& hListenSocket : vhListenSocket)
        if (hListenSocket.socket != INVALID_SOCKET)
            if (!CloseSocket(hListenSocket.socket))
                LogPrintf("CloseSocket(hListenSocket) failed with error %s\n", NetworkErrorString(WSAGetLastError()));

    // clean up some globals (to help leak detection)
    for (CNode* pnode : vNodes) {
        DeleteNode(pnode);
    }
    for (CNode* pnode : vNodesDisconnected) {
        DeleteNode(pnode);
    }
    vNodes.clear();
    vNodesDisconnected.clear();
    vhListenSocket.clear();
    delete semOutbound;
    semOutbound = nullptr;
    delete semAddnode;
    semAddnode = nullptr;
    delete semSmartnodeOutbound;
    semSmartnodeOutbound = nullptr;

#ifndef WIN32
    if (wakeupPipe[0] != -1) close(wakeupPipe[0]);
    if (wakeupPipe[1] != -1) close(wakeupPipe[1]);
    wakeupPipe[0] = wakeupPipe[1] = -1;
#endif
}

void CConnman::DeleteNode(CNode* pnode)
{
    assert(pnode);
    bool fUpdateConnectionTime = false;
    m_msgproc->FinalizeNode(pnode->GetId(), fUpdateConnectionTime);
    if (fUpdateConnectionTime) {
        addrman.Connected(pnode->addr);
    }
    delete pnode;
}

CConnman::~CConnman()
{
    Interrupt();
    Stop();
}

size_t CConnman::GetAddressCount() const
{
    return addrman.size();
}

void CConnman::SetServices(const CService& addr, ServiceFlags nServices)
{
    addrman.SetServices(addr, nServices);
}

void CConnman::MarkAddressGood(const CAddress& addr)
{
    addrman.Good(addr);
}

void CConnman::AddNewAddresses(const std::vector<CAddress>& vAddr, const CAddress& addrFrom, int64_t nTimePenalty)
{
    addrman.Add(vAddr, addrFrom, nTimePenalty);
}

std::vector<CAddress> CConnman::GetAddresses()
{
    return addrman.GetAddr();
}

bool CConnman::AddNode(const std::string& strNode)
{
    LOCK(cs_vAddedNodes);
    for (std::vector<std::string>::const_iterator it = vAddedNodes.begin(); it != vAddedNodes.end(); ++it) {
        if (strNode == *it)
            return false;
    }

    vAddedNodes.push_back(strNode);
    return true;
}

bool CConnman::RemoveAddedNode(const std::string& strNode)
{
    LOCK(cs_vAddedNodes);
    for (std::vector<std::string>::iterator it = vAddedNodes.begin(); it != vAddedNodes.end(); ++it) {
        if (strNode == *it) {
            vAddedNodes.erase(it);
            return true;
        }
    }
    return false;
}

bool CConnman::AddPendingSmartnode(const CService& service)
{
    LOCK(cs_vPendingSmartnodes);
    for (std::vector<CService>::const_iterator it = vPendingSmartnodes.begin(); it != vPendingSmartnodes.end(); ++it) {
        if (service == *it)
            return false;
    }

    vPendingSmartnodes.push_back(service);
    return true;
}

bool CConnman::AddSmartnodeQuorumNodes(Consensus::LLMQType llmqType, const uint256& quorumHash, const std::set<uint256>& proTxHashes)
{
    LOCK(cs_vPendingSmartnodes);
    auto it = smartnodeQuorumNodes.find(std::make_pair(llmqType, quorumHash));
    if (it != smartnodeQuorumNodes.end()) {
        return false;
    }
    smartnodeQuorumNodes.emplace(std::make_pair(llmqType, quorumHash), proTxHashes);
    return true;
}

bool CConnman::HasSmartnodeQuorumNodes(Consensus::LLMQType llmqType, const uint256& quorumHash)
{
    LOCK(cs_vPendingSmartnodes);
    return smartnodeQuorumNodes.count(std::make_pair(llmqType, quorumHash));
}

std::set<uint256> CConnman::GetSmartnodeQuorums(Consensus::LLMQType llmqType)
{
    LOCK(cs_vPendingSmartnodes);
    std::set<uint256> result;
    for (const auto& p : smartnodeQuorumNodes) {
        if (p.first.first != llmqType) {
            continue;
        }
        result.emplace(p.first.second);
    }
    return result;
}

std::set<NodeId> CConnman::GetSmartnodeQuorumNodes(Consensus::LLMQType llmqType, const uint256& quorumHash) const
{
    LOCK2(cs_vNodes, cs_vPendingSmartnodes);
    auto it = smartnodeQuorumNodes.find(std::make_pair(llmqType, quorumHash));
    if (it == smartnodeQuorumNodes.end()) {
        return {};
    }
    const auto& proRegTxHashes = it->second;

    std::set<NodeId> nodes;
    for (const auto pnode : vNodes) {
        if (pnode->fDisconnect) {
            continue;
        }
        if (!pnode->qwatch && (pnode->verifiedProRegTxHash.IsNull() || !proRegTxHashes.count(pnode->verifiedProRegTxHash))) {
            continue;
        }
        nodes.emplace(pnode->GetId());
    }
    return nodes;
}

void CConnman::RemoveSmartnodeQuorumNodes(Consensus::LLMQType llmqType, const uint256& quorumHash)
{
    LOCK(cs_vPendingSmartnodes);
    smartnodeQuorumNodes.erase(std::make_pair(llmqType, quorumHash));
}

bool CConnman::IsSmartnodeQuorumNode(const CNode* pnode)
{
    // Let's see if this is an outgoing connection to an address that is known to be a smartnode
    // We however only need to know this if the node did not authenticate itself as a MN yet
    uint256 assumedProTxHash;
    if (pnode->verifiedProRegTxHash.IsNull() && !pnode->fInbound) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        auto dmn = mnList.GetMNByService(pnode->addr);
        if (dmn == nullptr) {
            // This is definitely not a smartnode
            return false;
        }
        assumedProTxHash = dmn->proTxHash;
    }

    LOCK(cs_vPendingSmartnodes);
    for (const auto& p : smartnodeQuorumNodes) {
        if (!pnode->verifiedProRegTxHash.IsNull()) {
            if (p.second.count(pnode->verifiedProRegTxHash)) {
                return true;
            }
        } else if (!assumedProTxHash.IsNull()) {
            if (p.second.count(assumedProTxHash)) {
                return true;
            }
        }
    }
    return false;
}

size_t CConnman::GetNodeCount(NumConnections flags)
{
    LOCK(cs_vNodes);
    if (flags == CConnman::CONNECTIONS_ALL) // Shortcut if we want total
        return vNodes.size();

    int nNum = 0;
    for (std::vector<CNode*>::const_iterator it = vNodes.begin(); it != vNodes.end(); ++it)
        if (flags & ((*it)->fInbound ? CONNECTIONS_IN : CONNECTIONS_OUT))
            nNum++;

    return nNum;
}

size_t CConnman::GetMaxOutboundNodeCount()
{
    return nMaxOutbound;
}

void CConnman::GetNodeStats(std::vector<CNodeStats>& vstats)
{
    vstats.clear();
    LOCK(cs_vNodes);
    vstats.reserve(vNodes.size());
    for (std::vector<CNode*>::iterator it = vNodes.begin(); it != vNodes.end(); ++it) {
        CNode* pnode = *it;
        vstats.emplace_back();
        pnode->copyStats(vstats.back());
    }
}

bool CConnman::DisconnectNode(const std::string& strNode)
{
    LOCK(cs_vNodes);
    if (CNode* pnode = FindNode(strNode)) {
        pnode->fDisconnect = true;
        return true;
    }
    return false;
}
bool CConnman::DisconnectNode(NodeId id)
{
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes) {
        if (id == pnode->GetId()) {
            pnode->fDisconnect = true;
            return true;
        }
    }
    return false;
}

void CConnman::RelayTransaction(const CTransaction& tx)
{
    uint256 hash = tx.GetHash();
    int nInv = MSG_TX;
    if (CPrivateSend::GetDSTX(hash)) {
        nInv = MSG_DSTX;
    }
    CInv inv(nInv, hash);
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes) {
        pnode->PushInventory(inv);
    }
}

void CConnman::RelayInv(CInv& inv, const int minProtoVersion)
{
    LOCK(cs_vNodes);
    for (const auto& pnode : vNodes)
        if (pnode->nVersion >= minProtoVersion)
            pnode->PushInventory(inv);
}

void CConnman::RelayInvFiltered(CInv& inv, const CTransaction& relatedTx, const int minProtoVersion)
{
    LOCK(cs_vNodes);
    for (const auto& pnode : vNodes) {
        if (pnode->nVersion < minProtoVersion)
            continue;
        {
            LOCK(pnode->cs_filter);
            if (pnode->pfilter && !pnode->pfilter->IsRelevantAndUpdate(relatedTx))
                continue;
        }
        pnode->PushInventory(inv);
    }
}

void CConnman::RelayInvFiltered(CInv& inv, const uint256& relatedTxHash, const int minProtoVersion)
{
    LOCK(cs_vNodes);
    for (const auto& pnode : vNodes) {
        if (pnode->nVersion < minProtoVersion) continue;
        {
            LOCK(pnode->cs_filter);
            if (pnode->pfilter && !pnode->pfilter->contains(relatedTxHash)) continue;
        }
        pnode->PushInventory(inv);
    }
}

void CConnman::RemoveAskFor(const uint256& hash)
{
    mapAlreadyAskedFor.erase(hash);

    LOCK(cs_vNodes);
    for (const auto& pnode : vNodes) {
        pnode->RemoveAskFor(hash);
    }
}

void CConnman::RecordBytesRecv(uint64_t bytes)
{
    LOCK(cs_totalBytesRecv);
    nTotalBytesRecv += bytes;
}

void CConnman::RecordBytesSent(uint64_t bytes)
{
    LOCK(cs_totalBytesSent);
    nTotalBytesSent += bytes;

    uint64_t now = GetTime();
    if (nMaxOutboundCycleStartTime + nMaxOutboundTimeframe < now) {
        // timeframe expired, reset cycle
        nMaxOutboundCycleStartTime = now;
        nMaxOutboundTotalBytesSentInCycle = 0;
    }

    // TODO, exclude whitebind peers
    nMaxOutboundTotalBytesSentInCycle += bytes;
}

void CConnman::SetMaxOutboundTarget(uint64_t limit)
{
    LOCK(cs_totalBytesSent);
    nMaxOutboundLimit = limit;
}

uint64_t CConnman::GetMaxOutboundTarget()
{
    LOCK(cs_totalBytesSent);
    return nMaxOutboundLimit;
}

uint64_t CConnman::GetMaxOutboundTimeframe()
{
    LOCK(cs_totalBytesSent);
    return nMaxOutboundTimeframe;
}

uint64_t CConnman::GetMaxOutboundTimeLeftInCycle()
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return 0;

    if (nMaxOutboundCycleStartTime == 0)
        return nMaxOutboundTimeframe;

    uint64_t cycleEndTime = nMaxOutboundCycleStartTime + nMaxOutboundTimeframe;
    uint64_t now = GetTime();
    return (cycleEndTime < now) ? 0 : cycleEndTime - GetTime();
}

void CConnman::SetMaxOutboundTimeframe(uint64_t timeframe)
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundTimeframe != timeframe) {
        // reset measure-cycle in case of changing
        // the timeframe
        nMaxOutboundCycleStartTime = GetTime();
    }
    nMaxOutboundTimeframe = timeframe;
}

bool CConnman::OutboundTargetReached(bool historicalBlockServingLimit)
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return false;

    if (historicalBlockServingLimit) {
        // keep a large enough buffer to at least relay each block once
        uint64_t timeLeftInCycle = GetMaxOutboundTimeLeftInCycle();
        uint64_t buffer = timeLeftInCycle / 600 * MaxBlockSize(fDIP0001ActiveAtTip);
        if (buffer >= nMaxOutboundLimit || nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit - buffer)
            return true;
    } else if (nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit)
        return true;

    return false;
}

uint64_t CConnman::GetOutboundTargetBytesLeft()
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return 0;

    return (nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit) ? 0 : nMaxOutboundLimit - nMaxOutboundTotalBytesSentInCycle;
}

uint64_t CConnman::GetTotalBytesRecv()
{
    LOCK(cs_totalBytesRecv);
    return nTotalBytesRecv;
}

uint64_t CConnman::GetTotalBytesSent()
{
    LOCK(cs_totalBytesSent);
    return nTotalBytesSent;
}

ServiceFlags CConnman::GetLocalServices() const
{
    return nLocalServices;
}

void CConnman::SetBestHeight(int height)
{
    nBestHeight.store(height, std::memory_order_release);
}

int CConnman::GetBestHeight() const
{
    return nBestHeight.load(std::memory_order_acquire);
}

unsigned int CConnman::GetReceiveFloodSize() const { return nReceiveFloodSize; }

CNode::CNode(NodeId idIn, ServiceFlags nLocalServicesIn, int nMyStartingHeightIn, SOCKET hSocketIn, const CAddress& addrIn, uint64_t nKeyedNetGroupIn, uint64_t nLocalHostNonceIn, const CAddress& addrBindIn, const std::string& addrNameIn, bool fInboundIn) :
    nTimeConnected(GetSystemTimeInSeconds()),
    nTimeFirstMessageReceived(0),
    fFirstMessageIsMNAUTH(false),
    addr(addrIn),
    addrBind(addrBindIn),
    fInbound(fInboundIn),
    nKeyedNetGroup(nKeyedNetGroupIn),
    addrKnown(5000, 0.001),
    filterInventoryKnown(50000, 0.000001),
    id(idIn),
    nLocalHostNonce(nLocalHostNonceIn),
    nLocalServices(nLocalServicesIn),
    nMyStartingHeight(nMyStartingHeightIn),
    nSendVersion(0)
{
    nServices = NODE_NONE;
    hSocket = hSocketIn;
    nRecvVersion = INIT_PROTO_VERSION;
    nLastSend = 0;
    nLastRecv = 0;
    nSendBytes = 0;
    nRecvBytes = 0;
    nTimeOffset = 0;
    addrName = addrNameIn == "" ? addr.ToStringIPPort() : addrNameIn;
    nVersion = 0;
    nNumWarningsSkipped = 0;
    nLastWarningTime = 0;
    strSubVer = "";
    fWhitelisted = false;
    fOneShot = false;
    m_manual_connection = false;
    fClient = false; // set by version message
    fFeeler = false;
    fSuccessfullyConnected = false;
    fDisconnect = false;
    nRefCount = 0;
    nSendSize = 0;
    nSendOffset = 0;
    hashContinue = uint256();
    nStartingHeight = -1;
    filterInventoryKnown.reset();
    fSendMempool = false;
    fGetAddr = false;
    nNextLocalAddrSend = 0;
    nNextAddrSend = 0;
    nAddrTokenBucket = 1; // initialize to 1 to allow self-announcement
    nAddrTokenTimestamp = GetTimeMicros();
    nProcessedAddrs = 0;
    nRatelimitedAddrs = 0;
    nNextInvSend = 0;
    fRelayTxes = false;
    fSentAddr = false;
    pfilter = new CBloomFilter();
    timeLastMempoolReq = 0;
    nLastBlockTime = 0;
    nLastTXTime = 0;
    nPingNonceSent = 0;
    nPingUsecStart = 0;
    nPingUsecTime = 0;
    fPingQueued = false;
    fSmartnode = false;
    nMinPingUsecTime = std::numeric_limits<int64_t>::max();
    fPauseRecv = false;
    fPauseSend = false;
    nProcessQueueSize = 0;

    for (const std::string& msg : getAllNetMessageTypes())
        mapRecvBytesPerMsgCmd[msg] = 0;
    mapRecvBytesPerMsgCmd[NET_MESSAGE_COMMAND_OTHER] = 0;

    if (fLogIPs) {
        LogPrint(BCLog::NET, "Added connection to %s peer=%d\n", addrName, id);
    } else {
        LogPrint(BCLog::NET, "Added connection peer=%d\n", id);
    }
}

CNode::~CNode()
{
    CloseSocket(hSocket);

    if (pfilter)
        delete pfilter;
}

void CNode::AskFor(const CInv& inv, int64_t doubleRequestDelay)
{
    if (queueAskFor.size() > MAPASKFOR_MAX_SZ || setAskFor.size() > SETASKFOR_MAX_SZ) {
        int64_t nNow = GetTime();
        if (nNow - nLastWarningTime > WARNING_INTERVAL) {
            LogPrintf("CNode::AskFor -- WARNING: inventory message dropped: vecAskFor.size = %d, setAskFor.size = %d, MAPASKFOR_MAX_SZ = %d, SETASKFOR_MAX_SZ = %d, nSkipped = %d, peer=%d\n",
                queueAskFor.size(), setAskFor.size(), MAPASKFOR_MAX_SZ, SETASKFOR_MAX_SZ, nNumWarningsSkipped, id);
            nLastWarningTime = nNow;
            nNumWarningsSkipped = 0;
        } else {
            ++nNumWarningsSkipped;
        }
        return;
    }
    // a peer may not have multiple non-responded queue positions for a single inv item
    if (!setAskFor.emplace(inv.hash).second)
        return;

    // We're using queueAskFor as a priority queue,
    // the key is the earliest time the request can be sent
    int64_t nRequestTime;
    auto it = mapAlreadyAskedFor.find(inv.hash);
    if (it != mapAlreadyAskedFor.end())
        nRequestTime = it->second;
    else
        nRequestTime = 0;

    LogPrint(BCLog::NET, "askfor %s  %d (%s) peer=%d\n", inv.ToString(), nRequestTime, DateTimeStrFormat("%H:%M:%S", nRequestTime / 1000000), id);

    // Make sure not to reuse time indexes to keep things in the same order
    int64_t nNow = GetTimeMicros() - 1000000;
    static int64_t nLastTime;
    ++nLastTime;
    nNow = std::max(nNow, nLastTime);
    nLastTime = nNow;

    // Each retry is 2 minutes after the last
    nRequestTime = std::max(nRequestTime + doubleRequestDelay, nNow);
    if (it != mapAlreadyAskedFor.end())
        mapAlreadyAskedFor.update(it, nRequestTime);
    else
        mapAlreadyAskedFor.insert(std::make_pair(inv.hash, nRequestTime));

    queueAskFor.emplace(nRequestTime, inv);
    setAskForInQueue.emplace(inv.hash);
}

void CNode::RemoveAskFor(const uint256& hash)
{
    setAskFor.erase(hash);
    // we don't really remove it from queueAskFor as it would be too expensive to rebuild the heap
    // instead, we're ignoring the entry later as it won't be found in setAskForInQueue anymore
    setAskForInQueue.erase(hash);
}

bool CConnman::NodeFullyConnected(const CNode* pnode)
{
    return pnode && pnode->fSuccessfullyConnected && !pnode->fDisconnect;
}

void CConnman::PushMessage(CNode* pnode, CSerializedNetMsg&& msg, bool allowOptimisticSend)
{
    size_t nMessageSize = msg.data.size();
    size_t nTotalSize = nMessageSize + CMessageHeader::HEADER_SIZE;
    LogPrint(BCLog::NET, "sending %s (%d bytes) peer=%d\n", SanitizeString(msg.command.c_str()), nMessageSize, pnode->GetId());

    std::vector<unsigned char> serializedHeader;
    serializedHeader.reserve(CMessageHeader::HEADER_SIZE);
    uint256 hash = Hash(msg.data.data(), msg.data.data() + nMessageSize);
    CMessageHeader hdr(Params().MessageStart(), msg.command.c_str(), nMessageSize);
    memcpy(hdr.pchChecksum, hash.begin(), CMessageHeader::CHECKSUM_SIZE);

    CVectorWriter{SER_NETWORK, INIT_PROTO_VERSION, serializedHeader, 0, hdr};

    size_t nBytesSent = 0;
    {
        LOCK(pnode->cs_vSend);
        bool hasPendingData = !pnode->vSendMsg.empty();
        bool optimisticSend(allowOptimisticSend && pnode->vSendMsg.empty());

        // log total amount of bytes per command
        pnode->mapSendBytesPerMsgCmd[msg.command] += nTotalSize;
        pnode->nSendSize += nTotalSize;

        if (pnode->nSendSize > nSendBufferMaxSize)
            pnode->fPauseSend = true;
        pnode->vSendMsg.push_back(std::move(serializedHeader));
        if (nMessageSize)
            pnode->vSendMsg.push_back(std::move(msg.data));

        // If write queue empty, attempt "optimistic write"
        if (optimisticSend == true)
            nBytesSent = SocketSendData(pnode);
        // wake up select() call in case there was no pending data before (so it was not selecting this socket for sending)
        else if (!hasPendingData && wakeupSelectNeeded)
            WakeSelect();
    }
    if (nBytesSent)
        RecordBytesSent(nBytesSent);
}

bool CConnman::ForNode(const CService& addr, std::function<bool(const CNode* pnode)> cond, std::function<bool(CNode* pnode)> func)
{
    CNode* found = nullptr;
    LOCK(cs_vNodes);
    for (auto&& pnode : vNodes) {
        if ((CService)pnode->addr == addr) {
            found = pnode;
            break;
        }
    }
    return found != nullptr && cond(found) && func(found);
}

bool CConnman::ForNode(NodeId id, std::function<bool(const CNode* pnode)> cond, std::function<bool(CNode* pnode)> func)
{
    CNode* found = nullptr;
    LOCK(cs_vNodes);
    for (auto&& pnode : vNodes) {
        if (pnode->GetId() == id) {
            found = pnode;
            break;
        }
    }
    return found != nullptr && cond(found) && func(found);
}

bool CConnman::IsSmartnodeOrDisconnectRequested(const CService& addr)
{
    return ForNode(addr, AllNodes, [](CNode* pnode) {
        return pnode->fSmartnode || pnode->fDisconnect;
    });
}

int64_t PoissonNextSend(int64_t nNow, int average_interval_seconds)
{
    return nNow + (int64_t)(log1p(GetRand(1ULL << 48) * -0.0000000000000035527136788 /* -1/2^48 */) * average_interval_seconds * -1000000.0 + 0.5);
}

std::vector<CNode*> CConnman::CopyNodeVector(std::function<bool(const CNode* pnode)> cond)
{
    std::vector<CNode*> vecNodesCopy;
    LOCK(cs_vNodes);
    for (size_t i = 0; i < vNodes.size(); ++i) {
        CNode* pnode = vNodes[i];
        if (!cond(pnode))
            continue;
        pnode->AddRef();
        vecNodesCopy.push_back(pnode);
    }
    return vecNodesCopy;
}

std::vector<CNode*> CConnman::CopyNodeVector()
{
    return CopyNodeVector(AllNodes);
}

void CConnman::ReleaseNodeVector(const std::vector<CNode*>& vecNodes)
{
    LOCK(cs_vNodes);
    for (size_t i = 0; i < vecNodes.size(); ++i) {
        CNode* pnode = vecNodes[i];
        pnode->Release();
    }
}

CSipHasher CConnman::GetDeterministicRandomizer(uint64_t id) const
{
    return CSipHasher(nSeed0, nSeed1).Write(id);
}

uint64_t CConnman::CalculateKeyedNetGroup(const CAddress& ad) const
{
    std::vector<unsigned char> vchNetGroup(ad.GetGroup());

    return GetDeterministicRandomizer(RANDOMIZER_ID_NETGROUP).Write(vchNetGroup.data(), vchNetGroup.size()).Finalize();
}
