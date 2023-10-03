// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/bitcoin-config.h"
#endif

#include "net.h"

#include "addrman.h"
#include "chainparams.h"
#include "clientversion.h"
#include "primitives/transaction.h"
#include "scheduler.h"
#include "ui_interface.h"
#include "crypto/common.h"
#include "zen/utiltls.h"
#include "zen/tlsmanager.h"

#ifdef WIN32
#include <string.h>
#else
#include <fcntl.h>
#endif

#include <openssl/conf.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

// Dump addresses to peers.dat every 15 minutes (900s)
#define DUMP_ADDRESSES_INTERVAL 900

#if !defined(HAVE_MSG_NOSIGNAL) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
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

#define USE_TLS

#if defined(USE_TLS) && !defined(TLS1_2_VERSION)
    // minimum secure protocol is 1.2
    // TLS1_2_VERSION is defined in openssl/tls1.h
    #error "ERROR: Your OpenSSL version does not support TLS v1.2"
#endif

using namespace std;
//
// Global state variables
//
bool fDiscover = true;
bool fListen = true;
CCriticalSection cs_mapLocalHost;
map<CNetAddr, LocalServiceInfo> mapLocalHost;
static bool vfLimited[NET_MAX] = {};
uint64_t nLocalHostNonce = 0;  //// This is part of CNode
CAddrMan addrman;
std::map<CInv, CDataStream> mapRelay;
std::deque<std::pair<int64_t, CInv> > vRelayExpiration;
CCriticalSection cs_mapRelay;

// Signals for message handling
static CNodeSignals g_signals;
CNodeSignals& GetNodeSignals() { return g_signals; }

// OpenSSL server and client contexts
SSL_CTX *tls_ctx_server, *tls_ctx_client;

static bool operator==(_NODE_ADDR a, _NODE_ADDR b)
{
    return (a.ipAddr == b.ipAddr);
}


void CConnman::AddOneShot(const std::string& strDest)
{
    LOCK(cs_vOneShots);
    vOneShots.push_back(strDest);
}

unsigned short GetListenPort()
{
    return (unsigned short)(GetArg("-port", Params().GetDefaultPort()));
}

// find 'best' local address for a particular peer
bool GetLocal(CService& addr, const CNetAddr *paddrPeer)
{
    if (!fListen)
        return false;

    int nBestScore = -1;
    int nBestReachability = -1;
    {
        LOCK(cs_mapLocalHost);
        for (map<CNetAddr, LocalServiceInfo>::iterator it = mapLocalHost.begin(); it != mapLocalHost.end(); ++it)
        {
            int nScore = (*it).second.nScore;
            int nReachability = (*it).first.GetReachabilityFrom(paddrPeer);
            if (nReachability > nBestReachability || (nReachability == nBestReachability && nScore > nBestScore))
            {
                addr = CService((*it).first, (*it).second.nPort);
                nBestReachability = nReachability;
                nBestScore = nScore;
            }
        }
    }
    return nBestScore >= 0;
}

//! Convert the pnSeeds6 array into usable address objects.
static std::vector<CAddress> convertSeed6(const std::vector<SeedSpec6> &vSeedsIn)
{
    // It'll only connect to one or two seed nodes because once it connects,
    // it'll get a pile of addresses with newer timestamps.
    // Seed nodes are given a random 'last seen time' of between one and two
    // weeks ago.
    const int64_t nOneWeek = 7*24*60*60;
    std::vector<CAddress> vSeedsOut;
    vSeedsOut.reserve(vSeedsIn.size());
    for (std::vector<SeedSpec6>::const_iterator i(vSeedsIn.begin()); i != vSeedsIn.end(); ++i)
    {
        struct in6_addr ip;
        memcpy(&ip, i->addr, sizeof(ip));
        CAddress addr(CService(ip, i->port));
        addr.nTime = GetTime() - GetRand(nOneWeek) - nOneWeek;
        vSeedsOut.push_back(addr);
    }
    return vSeedsOut;
}

// get best local address for a particular peer as a CAddress
// Otherwise, return the unroutable 0.0.0.0 but filled in with
// the normal parameters, since the IP may be changed to a useful
// one by discovery.
CAddress GetLocalAddress(const CNetAddr *paddrPeer)
{
    CAddress ret(CService("0.0.0.0",GetListenPort()),0);
    CService addr;
    if (GetLocal(addr, paddrPeer))
    {
        ret = CAddress(addr);
    }
    ret.nServices = connman->GetLocalServices();
    ret.nTime = GetTime();
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
bool IsPeerAddrLocalGood(CNode *pnode)
{
    return fDiscover && pnode->addr.IsRoutable() && pnode->addrLocal.IsRoutable() &&
           !IsLimited(pnode->addrLocal.GetNetwork());
}

// pushes our own address to a peer
void AdvertizeLocal(CNode *pnode)
{
    if (fListen && pnode->fSuccessfullyConnected)
    {
        CAddress addrLocal = GetLocalAddress(&pnode->addr);
        // If discovery is enabled, sometimes give our peer the address it
        // tells us that it sees us as in case it has a better idea of our
        // address than we do.
        if (IsPeerAddrLocalGood(pnode) && (!addrLocal.IsRoutable() ||
             GetRand((GetnScore(addrLocal) > LOCAL_MANUAL) ? 8:2) == 0))
        {
            addrLocal.SetIP(pnode->addrLocal);
        }
        if (addrLocal.IsRoutable())
        {
            LogPrintf("AdvertizeLocal: advertizing address %s\n", addrLocal.ToString());
            pnode->PushAddress(addrLocal);
        }
    }
}

// learn a new local address
bool AddLocal(const CService& addr, int nScore)
{
    if (!addr.IsRoutable())
        return false;

    if (!fDiscover && nScore < LOCAL_MANUAL)
        return false;

    if (IsLimited(addr))
        return false;

    LogPrintf("AddLocal(%s,%i)\n", addr.ToString(), nScore);

    {
        LOCK(cs_mapLocalHost);
        bool fAlready = mapLocalHost.count(addr) > 0;
        LocalServiceInfo &info = mapLocalHost[addr];
        if (!fAlready || nScore >= info.nScore) {
            info.nScore = nScore + (fAlready ? 1 : 0);
            info.nPort = addr.GetPort();
        }
    }

    return true;
}

bool AddLocal(const CNetAddr &addr, int nScore)
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
    if (net == NET_UNROUTABLE)
        return;
    LOCK(cs_mapLocalHost);
    vfLimited[net] = fLimited;
}

bool IsLimited(enum Network net)
{
    LOCK(cs_mapLocalHost);
    return vfLimited[net];
}

bool IsLimited(const CNetAddr &addr)
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

void AddressCurrentlyConnected(const CService& addr)
{
    addrman.Connected(addr);
}


CNode::eTlsOption CNode::tlsFallbackNonTls = CNode::eTlsOption::FALLBACK_UNSET;
CNode::eTlsOption CNode::tlsValidate       = CNode::eTlsOption::FALLBACK_UNSET;

CNode* CConnman::FindNode(const CNetAddr& ip)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        if ((CNetAddr)pnode->addr == ip)
            return (pnode);
    return NULL;
}

CNode* CConnman::FindNode(const CSubNet& subNet)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    if (subNet.Match((CNetAddr)pnode->addr))
        return (pnode);
    return NULL;
}

CNode* CConnman::FindNode(const std::string& addrName)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        if (pnode->addrName == addrName)
            return (pnode);
    return NULL;
}

CNode* CConnman::FindNode(const CService& addr)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        if ((CService)pnode->addr == addr)
            return (pnode);
    return NULL;
}

CNode* CConnman::ConnectNode(CAddress addrConnect, const char *pszDest)
{
    if (pszDest == NULL) {
        if (IsLocal(addrConnect))
            return NULL;

        // Look for an existing connection
        CNode* pnode = FindNode((CService)addrConnect);
        if (pnode)
        {
            pnode->AddRef();
            return pnode;
        }
    }

    /// debug print
    LogPrint("net", "trying connection %s lastseen=%.1fhrs\n",
        pszDest ? pszDest : addrConnect.ToString(),
        pszDest ? 0.0 : (double)(GetTime() - addrConnect.nTime)/3600.0);

    // Connect
    std::unique_ptr<Sock> sock = CreateSock(addrConnect);
    if (!sock) {
        return nullptr;
    }
    bool proxyConnectionFailed = false;
    if (
        pszDest ? ConnectSocketByName(addrConnect, *sock, pszDest, Params().GetDefaultPort(), nConnectTimeout, &proxyConnectionFailed) :
                  ConnectSocket(addrConnect, *sock, nConnectTimeout, &proxyConnectionFailed))
    {
        if (!sock->IsSelectable()) {
            LogPrintf("Cannot create connection: non-selectable socket created (fd >= FD_SETSIZE ?)\n");
            return NULL;
        }

        addrman.Attempt(addrConnect);

        SSL *ssl = nullptr;
        
#ifdef USE_TLS
        /* TCP connection is ready. Do client side SSL. */
        if (CNode::GetTlsFallbackNonTls())
        {
            {
                LOCK(cs_vNonTLSNodesOutbound);
            
                LogPrint("tls", "%s():%d - handling connection to %s\n", __func__, __LINE__,  addrConnect.ToString());
 
                NODE_ADDR nodeAddr(addrConnect.ToStringIP());
            
                bool bUseTLS = (find(vNonTLSNodesOutbound.begin(),
                                     vNonTLSNodesOutbound.end(),
                                     nodeAddr) == vNonTLSNodesOutbound.end());
                unsigned long err_code = 0;
                if (bUseTLS)
                {
                    ssl = TLSManager::connect(*sock, addrConnect, err_code);
                    assert(ssl == sock->GetSSL());
                    if (!ssl)
                    {
                        if (err_code == TLSManager::SELECT_TIMEDOUT)
                        {
                            // can fail for timeout in select on fd, that is not a ssl error and we should not
                            // consider this node as non TLS
                            LogPrint("tls", "%s():%d - Connection to %s timedout\n",
                                __func__, __LINE__, addrConnect.ToStringIP());
                        }
                        else
                        {
                            // Further reconnection will be made in non-TLS (unencrypted) mode
                            vNonTLSNodesOutbound.push_back(NODE_ADDR(addrConnect.ToStringIP(), GetTimeMillis()));
                            LogPrint("tls", "%s():%d - err_code %x, adding connection to %s vNonTLSNodesOutbound list (sz=%d)\n",
                                __func__, __LINE__, err_code, addrConnect.ToStringIP(), vNonTLSNodesOutbound.size());
                        }
                        return NULL;
                    }
                }
                else
                {
                    LogPrintf ("Connection to %s will be unencrypted\n", addrConnect.ToString());
            
                    vNonTLSNodesOutbound.erase(
                            remove(
                                    vNonTLSNodesOutbound.begin(),
                                    vNonTLSNodesOutbound.end(),
                                    nodeAddr),
                            vNonTLSNodesOutbound.end());
                }
            }
        }
        else
        {
            unsigned long err_code = 0;
            ssl = TLSManager::connect(*sock, addrConnect, err_code);
            if (!ssl)
            {
                LogPrint("tls", "%s():%d - err_code %x, connection to %s failed)\n",
                    __func__, __LINE__, err_code, addrConnect.ToStringIP());
                return NULL;
            }
        }
        
        // certificate validation is disabled by default    
        if (CNode::GetTlsValidate())
        {
            if (ssl && !ValidatePeerCertificate(ssl))
            {
                LogPrintf ("TLS: ERROR: Wrong server certificate from %s. Connection will be closed.\n", addrConnect.ToString());
        
                //SSL_shutdown(ssl);
                //SSL_free(ssl);
                return NULL;
            }
        }
#endif  // USE_TLS

        // Add node
        CNode* pnode = new CNode(std::move(sock), addrConnect, pszDest ? pszDest : "", false);
        pnode->AddRef();

        {
            LOCK(cs_vNodes);
            vNodes.push_back(pnode);
        }

        pnode->nTimeConnected = GetTime();

        return pnode;
    } else if (!proxyConnectionFailed) {
        // If connecting to the node failed, and failure is not caused by a problem connecting to
        // the proxy, mark this as an attempt.
        addrman.Attempt(addrConnect);
    }

    return nullptr;
}

void CNode::CloseSocketDisconnect()
{
    fDisconnect = true;
    
    {
        LOCK(cs_hSocket);
        
        if (hSocket)
        {
            try
            {
                LogPrint("net", "disconnecting peer=%d\n", id);
            }
            catch(std::bad_alloc&)
            {
                // when the node is shutting down, the call above might use invalid memory resulting in a 
                // std::bad_alloc exception when instantiating internal objs for handling log category
                LogPrintf("(node is probably shutting down) disconnecting peer=%d\n", id);
            }

            if (hSocket->GetSSL())
            {
                unsigned long err_code = 0;
                TLSManager::waitFor(SSL_SHUTDOWN, addr, *hSocket, 100 /*double of avg roundtrip on decent connection*/, err_code);
            }

            hSocket.reset();
        }
    }

    // in case this fails, we'll empty the recv buffer when the CNode is deleted
    TRY_LOCK(cs_vRecvMsg, lockRecv);
    if (lockRecv)
        vRecvMsg.clear();
}

void CNode::PushVersion()
{
    int nBestHeight = g_signals.GetHeight().get_value_or(0);

    int64_t nTime = GetTime();
    CAddress addrYou = (addr.IsRoutable() && !IsProxy(addr) ? addr : CAddress(CService("0.0.0.0",0)));
    CAddress addrMe = GetLocalAddress(&addr);
    GetRandBytes((unsigned char*)&nLocalHostNonce, sizeof(nLocalHostNonce));
    if (fLogIPs)
        LogPrint("net", "send version message: version %d, blocks=%d, us=%s, them=%s, peer=%d\n", PROTOCOL_VERSION, nBestHeight, addrMe.ToString(), addrYou.ToString(), id);
    else
        LogPrint("net", "send version message: version %d, blocks=%d, us=%s, peer=%d\n", PROTOCOL_VERSION, nBestHeight, addrMe.ToString(), id);
    PushMessage(NetMsgType::VERSION, PROTOCOL_VERSION, connman->GetLocalServices(), nTime, addrYou, addrMe,
                nLocalHostNonce, FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, std::vector<string>()), nBestHeight, true);
}




//// Bitcoin encapsulates all the following functions and members into a 
////   ban manager, included in the CConnman class
std::map<CSubNet, int64_t> CNode::setBanned;
CCriticalSection CNode::cs_setBanned;

void CNode::ClearBanned()
{
    LOCK(cs_setBanned);
    setBanned.clear();
}

bool CNode::IsBanned(const CNetAddr &ip)
{
    bool fResult = false;
    {
        LOCK(cs_setBanned);
        for (std::map<CSubNet, int64_t>::iterator it = setBanned.begin(); it != setBanned.end(); ++it)
        {
            CSubNet subNet = (*it).first;
            int64_t t = (*it).second;

            if(subNet.Match(ip) && GetTime() < t)
                fResult = true;
        }
    }
    return fResult;
}

bool CNode::IsBanned(const CSubNet &subnet)
{
    bool fResult = false;
    {
        LOCK(cs_setBanned);
        std::map<CSubNet, int64_t>::iterator i = setBanned.find(subnet);
        if (i != setBanned.end())
        {
            int64_t t = (*i).second;
            if (GetTime() < t)
                fResult = true;
        }
    }
    return fResult;
}

void CNode::Ban(const CNetAddr& addr, int64_t bantimeoffset, bool sinceUnixEpoch) {
    CSubNet subNet(addr.ToString()+(addr.IsIPv4() ? "/32" : "/128"));
    Ban(subNet, bantimeoffset, sinceUnixEpoch);
}

void CNode::Ban(const CSubNet& subNet, int64_t bantimeoffset, bool sinceUnixEpoch) {
    int64_t banTime = GetTime()+GetArg("-bantime", 60*60*24);  // Default 24-hour ban
    if (bantimeoffset > 0)
        banTime = (sinceUnixEpoch ? 0 : GetTime() )+bantimeoffset;

    LOCK(cs_setBanned);
    if (setBanned[subNet] < banTime)
        setBanned[subNet] = banTime;
}

bool CNode::Unban(const CNetAddr &addr) {
    CSubNet subNet(addr.ToString()+(addr.IsIPv4() ? "/32" : "/128"));
    return Unban(subNet);
}

bool CNode::Unban(const CSubNet &subNet) {
    LOCK(cs_setBanned);
    if (setBanned.erase(subNet))
        return true;
    return false;
}

void CNode::GetBanned(std::map<CSubNet, int64_t> &banMap)
{
    LOCK(cs_setBanned);
    banMap = setBanned; //create a thread safe copy
}

bool CConnman::IsWhitelistedRange(const CNetAddr &addr) {
    LOCK(cs_vWhitelistedRange);
    BOOST_FOREACH(const CSubNet& subnet, vWhitelistedRange) {
        if (subnet.Match(addr))
            return true;
    }
    return false;
}


#undef X
#define X(name) stats.name = name
void CNode::copyStats(CNodeStats &stats)
{
    stats.nodeid = this->GetId();
    X(nServices);
    X(nLastSend);
    X(nLastRecv);
    X(nTimeConnected);
    X(nTimeOffset);
    X(addrName);
    X(nVersion);
    X(cleanSubVer);
    X(fInbound);
    X(nStartingHeight);
    X(nSendBytes);
    X(mapSendBytesPerMsgType);
    X(nRecvBytes);
    X(mapRecvBytesPerMsgType);
    X(fWhitelisted);
    X(m_addr_rate_limited);
    X(m_addr_processed);

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

    // Raw ping time is in microseconds, but show it to user as whole seconds (Bitcoin users should be well used to small numbers with many decimal places by now :)
    stats.dPingTime = (((double)nPingUsecTime) / 1e6);
    stats.dPingWait = (((double)nPingUsecWait) / 1e6);

    // Leave string empty if addrLocal invalid (not filled in yet)
    stats.addrLocal = addrLocal.IsValid() ? addrLocal.ToString() : "";

    // If ssl != NULL it means TLS connection was established successfully
    {
        LOCK(cs_hSocket);
        SSL* ssl = hSocket->GetSSL();
        stats.fTLSEstablished = (ssl != nullptr) && (SSL_get_state(ssl) == TLS_ST_OK);
        stats.fTLSVerified = (ssl != nullptr) && ValidatePeerCertificate(ssl);
    }
}
#undef X

// requires LOCK(cs_vRecvMsg)
bool CNode::ReceiveMsgBytes(const char *pch, unsigned int nBytes)
{
    while (nBytes > 0) {

        // get current incomplete message, or create a new one
        if (vRecvMsg.empty() ||
            vRecvMsg.back().complete())
            vRecvMsg.push_back(CNetMessage(Params().MessageStart(), SER_NETWORK, nRecvVersion));

        CNetMessage& msg = vRecvMsg.back();

        // absorb network data
        int handled;
        if (!msg.in_data)
            handled = msg.readHeader(pch, nBytes);
        else
            handled = msg.readData(pch, nBytes);

        if (handled < 0)
                return false;

        if (msg.in_data && msg.hdr.nMessageSize > MAX_PROTOCOL_MESSAGE_LENGTH) {
            LogPrint("net", "Oversized message from peer=%i, disconnecting\n", GetId());
            return false;
        }

        pch += handled;
        nBytes -= handled;

        if (msg.complete()) {
            msg.nTime = GetTimeMicros();
            AccountForRecvBytes(msg.hdr.pchCommand, msg.hdr.nMessageSize + CMessageHeader::HEADER_SIZE);
            connman->condMsgProc.notify_one();
        }
    }

    return true;
}

int CNetMessage::readHeader(const char *pch, unsigned int nBytes)
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
    }
    catch (const std::exception&) {
        return -1;
    }

    // reject messages larger than MAX_SERIALIZED_COMPACT_SIZE
    if (hdr.nMessageSize > MAX_SERIALIZED_COMPACT_SIZE)
            return -1;

    // switch state to reading message data
    in_data = true;

    return nCopy;
}

int CNetMessage::readData(const char *pch, unsigned int nBytes)
{
    unsigned int nRemaining = hdr.nMessageSize - nDataPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    if (vRecv.size() < nDataPos + nCopy) {
        // Allocate up to 256 KiB ahead, but never more than the total message size.
        vRecv.resize(std::min(hdr.nMessageSize, nDataPos + nCopy + 256 * 1024));
    }

    memcpy(&vRecv[nDataPos], pch, nCopy);
    nDataPos += nCopy;

    return nCopy;
}






//////////////////////
///    CConnman    ///
//////////////////////

CConnman::CConnman() {
    Options connOptions;
    Init(connOptions);
}

void CConnman::Stop()
{
    if (threadMessageHandler.joinable())
        threadMessageHandler.join();
    if (threadOpenConnections.joinable())
        threadOpenConnections.join();
    if (threadOpenAddedConnections.joinable())
        threadOpenAddedConnections.join();
    if (threadDNSAddressSeed.joinable())
        threadDNSAddressSeed.join();
    if (threadSocketHandler.joinable())
        threadSocketHandler.join();
    if (threadNonTLSPoolsCleaner.joinable())
        threadNonTLSPoolsCleaner.join();
    NetCleanup();
};

CConnman::~CConnman()
{
    try
    {
        LogPrintf("CConnman destruction");
        StopNode();
        Stop();
        LogPrintf("CConnman destruction - done");
    }
    catch (...)
    {
        LogPrintf("CConnman destructor exception\n");
    }
}


// In Bitcoin this is called CConnman::Interrupt()
bool CConnman::StopNode()
{
    LogPrintf("CConnman: StopNode()\n");

    flagInterruptMsgProc = true;

    condMsgProc.notify_all();

    interruptNet();
    InterruptSocks5(true);
    InterruptLookup(true);

    if (semOutbound)
        for (int i=0; i<MAX_OUTBOUND_CONNECTIONS; i++)
            semOutbound->post();

    if (fAddressesInitialized)
    {
        DumpAddresses();
        fAddressesInitialized = false;
    }

    return true;
}

void CConnman::NetCleanup()
{
    // Close sockets
    for (CNode* pnode: vNodes) {
        pnode->CloseSocketDisconnect();
    }

    for (ListenSocket& hListenSocket: vhListenSocket) {
        if (!hListenSocket.sock->Reset()) {
            LogPrintf("CloseSocket(hListenSocket) failed with error %s\n", NetworkErrorString(WSAGetLastError()));
        }
    }

    // clean up some globals (to help leak detection)
    for (CNode *pnode: vNodes) {
        delete pnode;
    }
    for (CNode *pnode: vNodesDisconnected) {
        delete pnode;
    }
    vNodes.clear();
    vNodesDisconnected.clear();
    vhListenSocket.clear();
    semOutbound.reset(nullptr);
    pnodeLocalHost.reset(nullptr);

#ifdef WIN32
    // Shutdown Windows Sockets
    WSACleanup();
#endif
}

// requires LOCK(cs_vSend)
void CConnman::SocketSendData(CNode *pnode)
{
    std::deque<CSerializeData>::iterator it = pnode->vSendMsg.begin();

    while (it != pnode->vSendMsg.end())
    {
        const CSerializeData &data = *it;
        assert(data.size() > pnode->nSendOffset);

        int nBytes = 0;
        {
            LOCK(pnode->cs_hSocket);
            
            if (!pnode->hSocket)
            {
                LogPrint("net", "Send: connection with %s is already closed\n", pnode->addr.ToString());
                break;
            }
    
            nBytes = pnode->hSocket->Send(&data[pnode->nSendOffset], data.size() - pnode->nSendOffset, MSG_NOSIGNAL | MSG_DONTWAIT);
        }
        if (nBytes > 0)
        {
            pnode->nLastSend = GetTime();
            pnode->nSendBytes += nBytes;
            pnode->nSendOffset += nBytes;
            RecordBytesSent(nBytes);

            if (pnode->nSendOffset == data.size())
            {
                pnode->nSendOffset = 0;
                pnode->nSendSize -= data.size();
                ++it;
            }
            else
            {
                // could not send full message; stop sending more
                break;
            }
        }
        else
        {
            if (nBytes <= 0)
            {
                // error
                //
                if (pnode->GetSSL() != nullptr)
                {
                    const int nRet = SSL_get_error(pnode->GetSSL(), nBytes);
                    if (nRet != SSL_ERROR_WANT_READ && nRet != SSL_ERROR_WANT_WRITE)
                    {
                        LogPrintf("ERROR: SSL_write %s; closing connection\n", ERR_error_string(nRet, NULL));
                        pnode->CloseSocketDisconnect();
                    }
                    else
                    {
                        // preventive measure from exhausting CPU usage
                        //
                        MilliSleep(1);    // 1 msec
                    }
                }
                else
                {
                    const int nRet = WSAGetLastError();
                    if (nRet != WSAEWOULDBLOCK && nRet != WSAEMSGSIZE && nRet != WSAEINTR && nRet != WSAEINPROGRESS)
                    {
                        LogPrintf("ERROR: send %s; closing connection\n", NetworkErrorString(nRet));
                        pnode->CloseSocketDisconnect();
                    }
                }
            }

            // couldn't send anything at all
            break;
        }
    }

    if (it == pnode->vSendMsg.end())
    {
        assert(pnode->nSendOffset == 0);
        assert(pnode->nSendSize == 0);
    }
    pnode->vSendMsg.erase(pnode->vSendMsg.begin(), it);
}

class CNodeRef {
public:
    CNodeRef(CNode *pnode) : _pnode(pnode) {
        LOCK(connman->cs_vNodes);
        _pnode->AddRef();
    }

    ~CNodeRef() {
        LOCK(connman->cs_vNodes);
        _pnode->Release();
    }

    CNode& operator *() const {return *_pnode;};
    CNode* operator ->() const {return _pnode;};

    CNodeRef& operator =(const CNodeRef& other)
    {
        if (this != &other) {
            LOCK(connman->cs_vNodes);

            _pnode->Release();
            _pnode = other._pnode;
            _pnode->AddRef();
        }
        return *this;
    }

    CNodeRef(const CNodeRef& other):
        _pnode(other._pnode)
    {
        LOCK(connman->cs_vNodes);
        _pnode->AddRef();
    }
private:
    CNode *_pnode;
};

static bool ReverseCompareNodeMinPingTime(const CNodeRef &a, const CNodeRef &b)
{
    return a->nMinPingUsecTime > b->nMinPingUsecTime;
}

static bool ReverseCompareNodeTimeConnected(const CNodeRef &a, const CNodeRef &b)
{
    return a->nTimeConnected > b->nTimeConnected;
}

class CompareNetGroupKeyed
{
    std::vector<unsigned char> vchSecretKey;
public:
    CompareNetGroupKeyed()
    {
        vchSecretKey.resize(32, 0);
        GetRandBytes(vchSecretKey.data(), vchSecretKey.size());
    }

    bool operator()(const CNodeRef &a, const CNodeRef &b)
    {
        std::vector<unsigned char> vchGroupA, vchGroupB;
        CSHA256 hashA, hashB;
        std::vector<unsigned char> vchA(32), vchB(32);

        vchGroupA = a->addr.GetGroup();
        vchGroupB = b->addr.GetGroup();

        hashA.Write(begin_ptr(vchGroupA), vchGroupA.size());
        hashB.Write(begin_ptr(vchGroupB), vchGroupB.size());

        hashA.Write(begin_ptr(vchSecretKey), vchSecretKey.size());
        hashB.Write(begin_ptr(vchSecretKey), vchSecretKey.size());

        hashA.Finalize(begin_ptr(vchA));
        hashB.Finalize(begin_ptr(vchB));

        return vchA < vchB;
    }
};

bool CConnman::AttemptToEvictConnection(bool fPreferNewConnection) {
    std::vector<CNodeRef> vEvictionCandidates;
    {
        LOCK(cs_vNodes);

        BOOST_FOREACH(CNode *node, vNodes) {
            if (node->fWhitelisted)
                continue;
            if (!node->fInbound)
                continue;
            if (node->fDisconnect)
                continue;
            vEvictionCandidates.push_back(CNodeRef(node));
        }
    }

    if (vEvictionCandidates.empty()) return false;

    // Protect connections with certain characteristics

    // Deterministically select 4 peers to protect by netgroup.
    // An attacker cannot predict which netgroups will be protected.
    static CompareNetGroupKeyed comparerNetGroupKeyed;
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), comparerNetGroupKeyed);
    vEvictionCandidates.erase(vEvictionCandidates.end() - std::min(4, static_cast<int>(vEvictionCandidates.size())), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Protect the 8 nodes with the best ping times.
    // An attacker cannot manipulate this metric without physically moving nodes closer to the target.
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), ReverseCompareNodeMinPingTime);
    vEvictionCandidates.erase(vEvictionCandidates.end() - std::min(8, static_cast<int>(vEvictionCandidates.size())), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Protect the half of the remaining nodes which have been connected the longest.
    // This replicates the existing implicit behavior.
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), ReverseCompareNodeTimeConnected);
    vEvictionCandidates.erase(vEvictionCandidates.end() - static_cast<int>(vEvictionCandidates.size() / 2), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Identify the network group with the most connections and youngest member.
    // (vEvictionCandidates is already sorted by reverse connect time)
    std::vector<unsigned char> naMostConnections;
    unsigned int nMostConnections = 0;
    int64_t nMostConnectionsTime = 0;
    std::map<std::vector<unsigned char>, std::vector<CNodeRef> > mapAddrCounts;
    BOOST_FOREACH(const CNodeRef &node, vEvictionCandidates) {
        mapAddrCounts[node->addr.GetGroup()].push_back(node);
        int64_t grouptime = mapAddrCounts[node->addr.GetGroup()][0]->nTimeConnected;
        size_t groupsize = mapAddrCounts[node->addr.GetGroup()].size();

        if (groupsize > nMostConnections || (groupsize == nMostConnections && grouptime > nMostConnectionsTime)) {
            nMostConnections = groupsize;
            nMostConnectionsTime = grouptime;
            naMostConnections = node->addr.GetGroup();
        }
    }

    // Reduce to the network group with the most connections
    vEvictionCandidates = mapAddrCounts[naMostConnections];

    // Do not disconnect peers if there is only one unprotected connection from their network group.
    if (vEvictionCandidates.size() <= 1)
        // unless we prefer the new connection (for whitelisted peers)
        if (!fPreferNewConnection)
            return false;

    // Disconnect from the network group with the most connections
    vEvictionCandidates[0]->fDisconnect = true;

    return true;
}


void CConnman::AcceptConnection(ListenSocket& hListenSocket) {
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    //SOCKET hSocket = accept(hListenSocket.sock->Get(), (struct sockaddr*)&sockaddr, &len);
    //hListenSocket.sock.reset(new Sock(accept(hListenSocket.sock->Get(), (struct sockaddr*)&sockaddr, &len)));
    std::unique_ptr<Sock> sock = hListenSocket.sock->Accept((struct sockaddr*)&sockaddr, &len);
    CAddress addr;
    int nInbound = 0;
    int nMaxInbound = nMaxConnections - MAX_OUTBOUND_CONNECTIONS;

    if (!sock)
    {
        int nErr = WSAGetLastError();
        if (nErr != WSAEWOULDBLOCK)
            LogPrintf("socket error accept failed: %s\n", NetworkErrorString(nErr));
        return;
    }

    if (sock)
        if (!addr.SetSockAddr((const struct sockaddr*)&sockaddr))
            LogPrintf("Warning: Unknown socket family\n");

    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            if (pnode->fInbound)
                nInbound++;
    }

    if (!sock->IsSelectable())
    {
        LogPrintf("connection from %s dropped: non-selectable socket\n", addr.ToString());
        return;
    }

    bool whitelisted = hListenSocket.whitelisted || CConnman::IsWhitelistedRange(addr);
    if (CNode::IsBanned(addr) && !whitelisted)
    {
        LogPrintf("connection from %s dropped (banned)\n", addr.ToString());
        return;
    }

    if (nInbound >= nMaxInbound)
    {
        if (!AttemptToEvictConnection(whitelisted)) {
            // No connection to evict, disconnect the new connection
            LogPrint("net", "failed to find an eviction candidate - connection dropped (full)\n");
            return;
        }
    }

    // According to the internet TCP_NODELAY is not carried into accepted sockets
    // on all platforms.  Set it again here just to be sure.
    int set = 1;
#ifdef WIN32
    sock->SetSockOpt(IPPROTO_TCP, TCP_NODELAY, (const char*)&set, sizeof(int));
#else
    sock->SetSockOpt(IPPROTO_TCP, TCP_NODELAY, (void*)&set, sizeof(int));
#endif


    SSL *ssl = nullptr;
    
    sock->SetNonBlocking();
    
#ifdef USE_TLS
    /* TCP connection is ready. Do server side SSL. */
    if (CNode::GetTlsFallbackNonTls())
    {
        LOCK(cs_vNonTLSNodesInbound);
    
        LogPrint("tls", "%s():%d - handling connection from %s\n", __func__, __LINE__,  addr.ToString());

        NODE_ADDR nodeAddr(addr.ToStringIP());
        
        bool bUseTLS = (find(vNonTLSNodesInbound.begin(),
                             vNonTLSNodesInbound.end(),
                             nodeAddr) == vNonTLSNodesInbound.end());
        unsigned long err_code = 0;
        if (bUseTLS)
        {
            ssl = TLSManager::accept(*sock, addr, err_code);
            if(!ssl)
            {
                if (err_code == TLSManager::SELECT_TIMEDOUT)
                {
                    // can fail also for timeout in select on fd, that is not a ssl error and we should not
                    // consider this node as non TLS
                    LogPrint("tls", "%s():%d - Connection from %s timedout\n", __func__, __LINE__, addr.ToStringIP());
                }
                else
                {
                    // Further reconnection will be made in non-TLS (unencrypted) mode
                    vNonTLSNodesInbound.push_back(NODE_ADDR(addr.ToStringIP(), GetTimeMillis()));
                    LogPrint("tls", "%s():%d - err_code %x, adding connection from %s vNonTLSNodesInbound list (sz=%d)\n",
                        __func__, __LINE__, err_code, addr.ToStringIP(), vNonTLSNodesInbound.size());
                }
                return;
            }
        }
        else
        {
            LogPrintf ("TLS: Connection from %s will be unencrypted\n", addr.ToStringIP());
            
            vNonTLSNodesInbound.erase(
                    remove(
                            vNonTLSNodesInbound.begin(),
                            vNonTLSNodesInbound.end(),
                            nodeAddr
                    ),
                    vNonTLSNodesInbound.end());
        }
    }
    else
    {
        unsigned long err_code = 0;
        ssl = TLSManager::accept(*sock, addr, err_code);
        if (!ssl)
        {
            LogPrint("tls", "%s():%d - err_code %x, failure accepting connection from %s\n",
                __func__, __LINE__, err_code, addr.ToStringIP());
            return;
        }
    }
    
    // certificate validation is disabled by default    
    if (CNode::GetTlsValidate())
    {
        if (ssl && !ValidatePeerCertificate(ssl))
        {
            LogPrintf ("TLS: ERROR: Wrong client certificate from %s. Connection will be closed.\n", addr.ToString());
        
            SSL_shutdown(ssl);
            return;
        }
    }
#endif // USE_TLS

    CNode* pnode = new CNode(std::move(sock), addr, "", true);
    pnode->AddRef();
    pnode->fWhitelisted = whitelisted;

    {
        LOCK(cs_vNodes);
        vNodes.push_back(pnode);
    }
}

#if defined(USE_TLS)
void CConnman::ThreadNonTLSPoolsCleaner()
{
    while (!interruptNet)
    {
        TLSManager::cleanNonTLSPool(connman->vNonTLSNodesInbound,  connman->cs_vNonTLSNodesInbound);
        TLSManager::cleanNonTLSPool(connman->vNonTLSNodesOutbound, connman->cs_vNonTLSNodesOutbound);
        if (!interruptNet.sleep_for(std::chrono::milliseconds(DEFAULT_CONNECT_TIMEOUT)))
            return;
    }
}

#endif // USE_TLS 


void CConnman::ThreadSocketHandler()
{
    unsigned int nPrevNodeCount = 0;
    while (!interruptNet)
    {
        //
        // Disconnect nodes
        //
        {
            LOCK(cs_vNodes);
            // Disconnect unused nodes
            vector<CNode*> vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
            {
                if (pnode->fDisconnect ||
                    (pnode->GetRefCount() <= 0 && pnode->vRecvMsg.empty() && pnode->nSendSize == 0 && pnode->ssSend.empty()))
                {
                    // remove from vNodes
                    vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());

                    // release outbound grant (if any)
                    pnode->grantOutbound.Release();

                    // close socket and cleanup
                    pnode->CloseSocketDisconnect();

                    // hold in disconnected pool until all refs are released
                    if (pnode->fNetworkNode || pnode->fInbound)
                        pnode->Release();
                    vNodesDisconnected.push_back(pnode);
                }
            }
        }
        {
            // Delete disconnected nodes
            list<CNode*> vNodesDisconnectedCopy = vNodesDisconnected;
            BOOST_FOREACH(CNode* pnode, vNodesDisconnectedCopy)
            {
                // Destroy the object only after other threads have stopped using it
                if (pnode->GetRefCount() == 0)
                {
                    bool fDelete = false;
                    {
                        TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                        if (lockRecv)
                            fDelete = true;
                    }
                    if (fDelete)
                    {
                        vNodesDisconnected.remove(pnode);
                        delete pnode;
                    }
                }
            }
        }
        if(vNodes.size() != nPrevNodeCount) {
            nPrevNodeCount = vNodes.size();
            uiInterface.NotifyNumConnectionsChanged(nPrevNodeCount);
        }

        //
        // Find which sockets have data to receive
        //
        struct timeval timeout;
        timeout.tv_sec  = 0;
        timeout.tv_usec = 50000; // frequency to poll pnode->vSend

        fd_set fdsetRecv;
        fd_set fdsetSend;
        fd_set fdsetError;
        FD_ZERO(&fdsetRecv);
        FD_ZERO(&fdsetSend);
        FD_ZERO(&fdsetError);
        SOCKET hSocketMax = 0;
        bool have_fds = false;

        BOOST_FOREACH(const ListenSocket& hListenSocket, connman->vhListenSocket) {
            FD_SET(hListenSocket.sock->Get(), &fdsetRecv);
            hSocketMax = max(hSocketMax, hListenSocket.sock->Get());
            have_fds = true;
        }

        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
            {
                LOCK(pnode->cs_hSocket);
                
                SOCKET socket = pnode->GetSocketFd();
                if (socket == INVALID_SOCKET)
                    continue;
                FD_SET(socket, &fdsetError);
                hSocketMax = max(hSocketMax, socket);
                have_fds = true;

                // Implement the following logic:
                // * If there is data to send, select() for sending data. As this only
                //   happens when optimistic write failed, we choose to first drain the
                //   write buffer in this case before receiving more. This avoids
                //   needlessly queueing received data, if the remote peer is not themselves
                //   receiving data. This means properly utilizing TCP flow control signalling.
                // * Otherwise, if there is no (complete) message in the receive buffer,
                //   or there is space left in the buffer, select() for receiving data.
                // * (if neither of the above applies, there is certainly one message
                //   in the receiver buffer ready to be processed).
                // Together, that means that at least one of the following is always possible,
                // so we don't deadlock:
                // * We send some data.
                // * We wait for data to be received (and disconnect after timeout).
                // * We process a message in the buffer (message handler thread).

                {
                    TRY_LOCK(pnode->cs_vSend, lockSend);
                    if (lockSend && !pnode->vSendMsg.empty()) {
                        FD_SET(socket, &fdsetSend);
                        continue;
                    }
                }
                {
                    TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                    if (lockRecv && (
                        pnode->vRecvMsg.empty() || !pnode->vRecvMsg.front().complete() ||
                        pnode->GetTotalRecvSize() <= GetReceiveFloodSize()))
                        FD_SET(socket, &fdsetRecv);
                }
            }
        }

        int nSelect = select(have_fds ? hSocketMax + 1 : 0,
                             &fdsetRecv, &fdsetSend, &fdsetError, &timeout);
        if (interruptNet)
            return;

        if (nSelect == SOCKET_ERROR)
        {
            if (have_fds)
            {
                int nErr = WSAGetLastError();
                LogPrintf("socket select error %s\n", NetworkErrorString(nErr));
                for (unsigned int i = 0; i <= hSocketMax; i++)
                    FD_SET(i, &fdsetRecv);
            }
            FD_ZERO(&fdsetSend);
            FD_ZERO(&fdsetError);
            if (!interruptNet.sleep_for(std::chrono::microseconds(timeout.tv_usec)))
                return;
        }

        //
        // Accept new connections
        //
        BOOST_FOREACH(const ListenSocket& hListenSocket, vhListenSocket)
        {
            if (hListenSocket.sock->Get() != INVALID_SOCKET && FD_ISSET(hListenSocket.sock->Get(), &fdsetRecv))
            {
                AcceptConnection(hListenSocket);
            }
        }

        //
        // Service each socket
        //
        vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->AddRef();
        }
        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            if (interruptNet)
                return;

            if (TLSManager::threadSocketHandler(pnode,fdsetRecv,fdsetSend,fdsetError)==-1){
                continue;
            }

            //
            // Inactivity checking
            //
            int64_t nTime = GetTime();
            if (nTime - pnode->nTimeConnected > 60)
            {
                if (pnode->nLastRecv == 0 || pnode->nLastSend == 0)
                {
                    LogPrint("net", "socket no bytes in first 60 seconds, %d %d from %d\n", pnode->nLastRecv != 0, pnode->nLastSend != 0, pnode->id);
                    pnode->fDisconnect = true;
                }
                else if (nTime - pnode->nLastSend > TIMEOUT_INTERVAL)
                {
                    LogPrintf("socket sending timeout: %is\n", nTime - pnode->nLastSend);
                    pnode->fDisconnect = true;
                }
                else if (nTime - pnode->nLastRecv > TIMEOUT_INTERVAL)
                {
                    LogPrintf("socket receive timeout: %is\n", nTime - pnode->nLastRecv);
                    pnode->fDisconnect = true;
                }
                else if (pnode->nPingNonceSent && pnode->nPingUsecStart + TIMEOUT_INTERVAL * 1000000 < GetTimeMicros())
                {
                    LogPrintf("ping timeout: %fs\n", 0.000001 * (GetTimeMicros() - pnode->nPingUsecStart));
                    pnode->fDisconnect = true;
                }
            }
        }
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }
    }
}


void CConnman::ThreadDNSAddressSeed()
{
    // goal: only query DNS seeds if address need is acute
    if ((addrman.size() > 0) &&
        (!GetBoolArg("-forcednsseed", false))) {
        if (!interruptNet.sleep_for(std::chrono::seconds(11)))
            return;

        LOCK(cs_vNodes);
        if (vNodes.size() >= 2) {
            LogPrintf("P2P peers available. Skipped DNS seeding.\n");
            return;
        }
    }

    const vector<CDNSSeedData> &vSeeds = Params().DNSSeeds();
    const unsigned int nMaxIPs = 256;
    const int nThreeDays = 3 * 24 * 3600;
    const int nFourDays = 4 * 24 * 3600;
    int found = 0;

    LogPrintf("Loading addresses from DNS seeds (could take a while)\n");

    for(const CDNSSeedData &seed : vSeeds)
    {
        if (HaveNameProxy()) {
            AddOneShot(seed.host);
            continue;
        } 
    
        vector<CNetAddr> vIPs;
        if (!LookupHost(seed.host.c_str(), vIPs, nMaxIPs))
        {
            continue;
        }

        vector<CAddress> vAdd;
        for(const CNetAddr& ip : vIPs)
        {
            vAdd.emplace_back(CService(ip, Params().GetDefaultPort()));
            vAdd.back().nTime = GetTime() - nThreeDays - GetRand(nFourDays); // use a random age between 3 and 7 days old
        }
        
        addrman.Add(vAdd, CNetAddr(seed.name, true));
        found += vAdd.size();
    }

    LogPrintf("%d addresses found from DNS seeds\n", found);
}




/// To be moved to CConnman after boost::thread refactoring
void CConnman::DumpAddresses()
{
    int64_t nStart = GetTimeMillis();

    CAddrDB adb;
    adb.Write(addrman);

    LogPrint("net", "Flushed %d addresses to peers.dat  %dms\n",
           addrman.size(), GetTimeMillis() - nStart);
}

void CConnman::ProcessOneShot()
{
    string strDest;
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
        if (!OpenNetworkConnection(addr, &grant, strDest.c_str(), true))
            AddOneShot(strDest);
    }
}

void CConnman::ThreadOpenConnections()
{
    // Connect to specific addresses
    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0)
    {
        for (int64_t nLoop = 0;; nLoop++)
        {
            ProcessOneShot();
            BOOST_FOREACH(const std::string& strAddr, mapMultiArgs["-connect"])
            {
                CAddress addr;
                OpenNetworkConnection(addr, NULL, strAddr.c_str());
                
                for (int i = 0; i < 10 && i < nLoop; i++)
                {
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
    while (!interruptNet)
    {
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
                addrman.Add(convertSeed6(Params().FixedSeeds()), CNetAddr("127.0.0.1"));
                done = true;
            }
        }

        //
        // Choose an address to connect to based on most recently seen
        //
        CAddress addrConnect;

        // Only connect out to one peer per network group (/16 for IPv4).
        // Do this here so we don't have to critsect vNodes inside mapAddresses critsect.
        int nOutbound = 0;
        set<vector<unsigned char> > setConnected;
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes) {
                if (!pnode->fInbound) {
                    setConnected.insert(pnode->addr.GetGroup());
                    nOutbound++;
                }
            }
        }

        int64_t nANow = GetTime();

        int nTries = 0;
        while (true)
        {
            CAddrInfo addr = addrman.Select();

            // if we selected an invalid address, restart
            if (!addr.IsValid() || setConnected.count(addr.GetGroup()) || IsLocal(addr))
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

            // do not allow non-default ports, unless after 50 invalid addresses selected already
            if (addr.GetPort() != Params().GetDefaultPort() && nTries < 50)
                continue;

            addrConnect = addr;
            break;
        }

        if (addrConnect.IsValid())
            OpenNetworkConnection(addrConnect, &grant);
    }
}

void CConnman::ThreadOpenAddedConnections()
{
    {
        LOCK(cs_vAddedNodes);
        vAddedNodes = mapMultiArgs["-addnode"];
    }

    if (HaveNameProxy()) {
        while(!interruptNet) {
            list<string> lAddresses(0);
            {
                LOCK(cs_vAddedNodes);
                BOOST_FOREACH(const std::string& strAddNode, vAddedNodes)
                    lAddresses.push_back(strAddNode);
            }
            BOOST_FOREACH(const std::string& strAddNode, lAddresses) {
                CAddress addr;
                CSemaphoreGrant grant(*semOutbound);
                OpenNetworkConnection(addr, &grant, strAddNode.c_str());
                if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                    return;
            }
            if (!interruptNet.sleep_for(std::chrono::minutes(2))) // Retry every 2 minutes
                return;
        }
    }

    for (unsigned int i = 0; true; i++)
    {
        list<string> lAddresses(0);
        {
            LOCK(cs_vAddedNodes);
            BOOST_FOREACH(const std::string& strAddNode, vAddedNodes)
                lAddresses.push_back(strAddNode);
        }

        list<vector<CService> > lservAddressesToAdd(0);
        BOOST_FOREACH(const std::string& strAddNode, lAddresses) {
            vector<CService> vservNode(0);
            if(Lookup(strAddNode.c_str(), vservNode, Params().GetDefaultPort(), fNameLookup, 0))
            {
                lservAddressesToAdd.push_back(vservNode);
            }
        }
        // Attempt to connect to each IP for each addnode entry until at least one is successful per addnode entry
        // (keeping in mind that addnode entries can have many IPs if fNameLookup)
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
                for (list<vector<CService> >::iterator it = lservAddressesToAdd.begin(); it != lservAddressesToAdd.end(); ++it)
                    BOOST_FOREACH(const CService& addrNode, *(it))
                        if (pnode->addr == addrNode)
                        {
                            it = lservAddressesToAdd.erase(it);
                            --it;
                            break;
                        }
        }
        BOOST_FOREACH(vector<CService>& vserv, lservAddressesToAdd)
        {
            CSemaphoreGrant grant(*semOutbound);
            OpenNetworkConnection(CAddress(vserv[i % vserv.size()]), &grant);
            if (!interruptNet.sleep_for(std::chrono::milliseconds(500)))
                return;
        }
        if (!interruptNet.sleep_for(std::chrono::minutes(2))) // Retry every 2 minutes
            return;
    }
}

// if successful, this moves the passed grant to the constructed node
bool CConnman::OpenNetworkConnection(const CAddress& addrConnect, CSemaphoreGrant *grantOutbound, const char *pszDest, bool fOneShot)
{
    //
    // Initiate outbound network connection
    //
    if (interruptNet)
        return false;

    if (!pszDest) {
        if (IsLocal(addrConnect) ||
            FindNode((CNetAddr)addrConnect) || CNode::IsBanned(addrConnect) ||
            FindNode(addrConnect.ToStringIPPort()))
            return false;
    } else if (FindNode(std::string(pszDest)))
        return false;
    
    CNode* pnode = ConnectNode(addrConnect, pszDest);
    if (interruptNet)
        return false;

#if defined(USE_TLS)
    if (CNode::GetTlsFallbackNonTls())
    {
        if (!pnode)
        {
            string strDest;
            int port;
        
            if (!pszDest)
                strDest = addrConnect.ToStringIP();
            else
                SplitHostPort(string(pszDest), port, strDest);
        
            if (TLSManager::isNonTLSAddr(strDest, vNonTLSNodesOutbound, cs_vNonTLSNodesOutbound))
            {
                // Attempt to reconnect in non-TLS mode
                pnode = ConnectNode(addrConnect, pszDest);
                if (interruptNet)
                    return false;
            }
        }
    }
    
#endif


    if (!pnode)
        return false;
    if (grantOutbound)
        grantOutbound->MoveTo(pnode->grantOutbound);
    pnode->fNetworkNode = true;
    if (fOneShot)
        pnode->fOneShot = true;

    return true;
}

void CConnman::ThreadMessageHandler()
{
    SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL);
    while (!flagInterruptMsgProc)
    {
        vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy) {
                pnode->AddRef();
            }
        }

        // Poll the connected nodes for messages
        CNode* pnodeTrickle = nullptr;
        if (!vNodesCopy.empty())
            pnodeTrickle = vNodesCopy[GetRand(vNodesCopy.size())];

        bool fSleep = true;

        for(CNode* pnode: vNodesCopy)
        {
            if (pnode->fDisconnect)
                continue;

            // Receive messages
            {
                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                if (lockRecv)
                {
                    if (!g_signals.ProcessMessages(pnode, flagInterruptMsgProc))
                        pnode->CloseSocketDisconnect();

                    if (pnode->nSendSize < GetSendBufferSize())
                    {
                        if (!pnode->vRecvGetData.empty() || (!pnode->vRecvMsg.empty() && pnode->vRecvMsg[0].complete()))
                        {
                            fSleep = false;
                        }
                    }
                }
            }
            if (flagInterruptMsgProc)
                return;

            // Send messages
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);
                if (lockSend)
                    g_signals.SendMessages(pnode, pnode == pnodeTrickle || pnode->fWhitelisted, flagInterruptMsgProc);
            }
            if (flagInterruptMsgProc)
                return;
        }

        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }

        if (fSleep) {
            std::unique_lock<std::mutex> lock(mutexMsgProc);
            condMsgProc.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
        }
    }
}

bool InitError(const std::string &str);

bool CConnman::Bind(const CService &addr, unsigned int flags) {
    if (!(flags & BF_EXPLICIT) && IsLimited(addr))
        return false;
    std::string strError;
    if (!BindListenPort(addr, strError, (flags & BF_WHITELIST) != 0)) {
        if (flags & BF_REPORT_ERROR)
            return InitError(strError);
        return false;
    }
    return true;
}

bool CConnman::BindListenPort(const CService &addrBind, string& strError, bool fWhitelisted)
{
    strError = "";
    int nOne = 1;

    // Create socket for listening for incoming connections
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);

    if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len))
    {
        strError = strprintf("Error: Bind address family for %s not supported", addrBind.ToString());
        LogPrintf("%s\n", strError);
        return false;
    }

    //SOCKET hListenSocket = socket(((struct sockaddr*)&sockaddr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    std::unique_ptr<Sock> sock = CreateSock(addrBind);
    if (!sock)
    {
        strError = strprintf("Error: Couldn't open socket for incoming connections (socket returned error %s)", NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        return false;
    }
    if (!sock->IsSelectable())
    {
        strError = "Error: Couldn't create a listenable socket for incoming connections";
        LogPrintf("%s\n", strError);
        return false;
    }

#ifndef WIN32
#ifdef SO_NOSIGPIPE
    // Different way of disabling SIGPIPE on BSD
    sock->SetSockOpt(SOL_SOCKET, SO_NOSIGPIPE, (void*)&nOne, sizeof(int));
#endif
    // Allow binding if the port is still in TIME_WAIT state after
    // the program was closed and restarted.
    sock->SetSockOpt(SOL_SOCKET, SO_REUSEADDR, (void*)&nOne, sizeof(int));
    // Disable Nagle's algorithm
    sock->SetSockOpt(IPPROTO_TCP, TCP_NODELAY, (void*)&nOne, sizeof(int));
#else
    sock->SetSockOpt(SOL_SOCKET, SO_REUSEADDR, (const char*)&nOne, sizeof(int));
    sock->SetSockOpt(IPPROTO_TCP, TCP_NODELAY, (const char*)&nOne, sizeof(int));
#endif

    // Set to non-blocking, incoming connections will also inherit this
    //
    // WARNING!
    // On Linux, the new socket returned by accept() does not inherit file
    // status flags such as O_NONBLOCK and O_ASYNC from the listening
    // socket. http://man7.org/linux/man-pages/man2/accept.2.html
    if (!sock->SetNonBlocking()) {
        strError = strprintf("BindListenPort: Setting listening socket to non-blocking failed, error %s\n", NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        return false;
    }

    // some systems don't have IPV6_V6ONLY but are always v6only; others do have the option
    // and enable it by default or not. Try to enable it, if possible.
    if (addrBind.IsIPv6()) {
#ifdef IPV6_V6ONLY
#ifdef WIN32
        sock->SetSockOpt(IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&nOne, sizeof(int));
#else
        sock->SetSockOpt(IPPROTO_IPV6, IPV6_V6ONLY, (void*)&nOne, sizeof(int));
#endif
#endif
#ifdef WIN32
        int nProtLevel = PROTECTION_LEVEL_UNRESTRICTED;
        sock->SetSockOpt(IPPROTO_IPV6, IPV6_PROTECTION_LEVEL, (const char*)&nProtLevel, sizeof(int));
#endif
    }

    if (sock->Bind((struct sockaddr*)&sockaddr, len) == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE)
            strError = strprintf(_("Unable to bind to %s on this computer. Horizen is probably already running."), addrBind.ToString());
        else
            strError = strprintf(_("Unable to bind to %s on this computer (bind returned error %s)"), addrBind.ToString(), NetworkErrorString(nErr));
        LogPrintf("%s\n", strError);
        return false;
    }
    LogPrintf("Bound to %s on sock %d\n", addrBind.ToString(), sock->Get());

    // Listen for incoming connections
    if (sock->Listen(SOMAXCONN) == SOCKET_ERROR)
    {
        strError = strprintf(_("Error: Listening for incoming connections failed (listen returned error %s)"), NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        return false;
    }

    vhListenSocket.emplace_back(std::move(sock), fWhitelisted);

    if (addrBind.IsRoutable() && fDiscover && !fWhitelisted)
        AddLocal(addrBind, LOCAL_BIND);

    return true;
}

void static Discover()
{
    if (!fDiscover)
        return;

#ifdef WIN32
    // Get local host IP
    char pszHostName[256] = "";
    if (gethostname(pszHostName, sizeof(pszHostName)) != SOCKET_ERROR)
    {
        vector<CNetAddr> vaddr;
        if (LookupHost(pszHostName, vaddr))
        {
            BOOST_FOREACH (const CNetAddr &addr, vaddr)
            {
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: %s - %s\n", __func__, pszHostName, addr.ToString());
            }
        }
    }
#else
    // Get local host ip
    struct ifaddrs* myaddrs;
    if (getifaddrs(&myaddrs) == 0)
    {
        for (struct ifaddrs* ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == NULL) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0) continue;
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            if (strcmp(ifa->ifa_name, "lo0") == 0) continue;
            if (ifa->ifa_addr->sa_family == AF_INET)
            {
                struct sockaddr_in* s4 = (struct sockaddr_in*)(ifa->ifa_addr);
                CNetAddr addr(s4->sin_addr);
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: IPv4 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
            }
            else if (ifa->ifa_addr->sa_family == AF_INET6)
            {
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



void CConnman::StartNode(CScheduler& scheduler, const Options& connOptions)
{
    Init(connOptions);

    uiInterface.InitMessage(_("Loading addresses..."));
    // Load addresses for peers.dat
    int64_t nStart = GetTimeMillis();
    {
        CAddrDB adb;
        if (!adb.Read(addrman))
            LogPrintf("Invalid or missing peers.dat; recreating\n");
    }
    LogPrintf("Loaded %i addresses from peers.dat  %dms\n",
           addrman.size(), GetTimeMillis() - nStart);
    fAddressesInitialized = true;

    if (semOutbound == nullptr) {
        // initialize semaphore
        int nMaxOutbound = min(MAX_OUTBOUND_CONNECTIONS, nMaxConnections);
        semOutbound = std::make_unique<CSemaphore>(nMaxOutbound);
    }

    if (pnodeLocalHost == nullptr)
        pnodeLocalHost = std::make_unique<CNode>(nullptr, CAddress(CService("127.0.0.1", 0), nLocalServices));

    Discover();

#ifdef USE_TLS
    
    if (!TLSManager::prepareCredentials())
    {
        LogPrintf("TLS: ERROR: %s: %s: Credentials weren't loaded. Node can't be started.\n", __FILE__, __func__);
        return;
    }
    
    if (!TLSManager::initialize())
    {
        LogPrintf("TLS: ERROR: %s: %s: TLS initialization failed. Node can't be started.\n", __FILE__, __func__);
        return;
    }
#else
    LogPrintf("TLS is not used!\n");
#endif

    //
    // Start threads
    //

    InterruptSocks5(false);
    InterruptLookup(false);
    interruptNet.reset();
    flagInterruptMsgProc = false;

    if (!GetBoolArg("-dnsseed", true))
        LogPrintf("DNS seeding disabled\n");
    else
        threadDNSAddressSeed = std::thread(&TraceThread<std::function<void()> >, "dnsseed",
                std::function<void()>(std::bind(&CConnman::ThreadDNSAddressSeed, this)));

    // Send and receive from sockets, accept connections
    threadSocketHandler = std::thread(&TraceThread<std::function<void()> >, "net",
            std::function<void()>(std::bind(&CConnman::ThreadSocketHandler, this)));

    // Initiate outbound connections from -addnode
    threadOpenAddedConnections = std::thread(&TraceThread<std::function<void()> >, "addcon",
            std::function<void()>(std::bind(&CConnman::ThreadOpenAddedConnections, this)));

    // Initiate outbound connections
    threadOpenConnections = std::thread(&TraceThread<std::function<void()> >, "opencon",
            std::function<void()>(std::bind(&CConnman::ThreadOpenConnections, this)));

    // Process messages
    threadMessageHandler = std::thread(&TraceThread<std::function<void()> >, "msghand",
            std::function<void()>(std::bind(&CConnman::ThreadMessageHandler, this)));

#if defined(USE_TLS)
    if (CNode::GetTlsFallbackNonTls())
    {
        // Clean pools of addresses for non-TLS connections
        threadNonTLSPoolsCleaner = std::thread(&TraceThread<std::function<void()> >, "poolscleaner",
                std::function<void()>(std::bind(&CConnman::ThreadNonTLSPoolsCleaner, this)));
    }
#endif
    
    // Dump network addresses
    scheduler.scheduleEvery(std::function<void()>(std::bind(&CConnman::DumpAddresses, this)), DUMP_ADDRESSES_INTERVAL);
}

void Relay(const CTransactionBase& tx, const CDataStream& ss)
{
    CInv inv(MSG_TX, tx.GetHash());
    {
        LOCK(cs_mapRelay);
        // Expire old relay messages
        while (!vRelayExpiration.empty() && vRelayExpiration.front().first < GetTime())
        {
            mapRelay.erase(vRelayExpiration.front().second);
            vRelayExpiration.pop_front();
        }

        // Save original serialized message so newer versions are preserved
        mapRelay.insert(std::make_pair(inv, ss));
        vRelayExpiration.push_back(std::make_pair(GetTime() + 15 * 60, inv));
    }
    LOCK(connman->cs_vNodes);
    BOOST_FOREACH(CNode* pnode, connman->vNodes)
    {
        if(!pnode->fRelayTxes)
            continue;
        LOCK(pnode->cs_filter);
        if (pnode->pfilter)
        {
            if (pnode->pfilter->IsRelevantAndUpdate(tx))
                pnode->PushInventory(inv);
        } else
            pnode->PushInventory(inv);
    }
}

#if 0
void Relay(const CScCertificate& cert)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(10000);
    ss << cert;
    Relay(cert, ss);
}
#endif

void CConnman::RecordBytesRecv(uint64_t bytes)
{
    nTotalBytesRecv.fetch_add(bytes, std::memory_order_relaxed);
}

void CConnman::RecordBytesSent(uint64_t bytes)
{
    nTotalBytesSent.fetch_add(bytes, std::memory_order_relaxed);
}

uint64_t CConnman::GetTotalBytesRecv()
{
    return nTotalBytesRecv;
}

uint64_t CConnman::GetTotalBytesSent()
{
    return nTotalBytesSent;
}

uint64_t CConnman::GetLocalServices() const
{
    return nLocalServices;
}





void CNode::Fuzz(int nChance)
{
    if (!fSuccessfullyConnected) return; // Don't fuzz initial handshake
    if (GetRand(nChance) != 0) return; // Fuzz 1 of every nChance messages

    switch (GetRand(3))
    {
    case 0:
        // xor a random byte with a random value:
        if (!ssSend.empty()) {
            CDataStream::size_type pos = GetRand(ssSend.size());
            ssSend[pos] ^= (unsigned char)(GetRand(256));
        }
        break;
    case 1:
        // delete a random byte:
        if (!ssSend.empty()) {
            CDataStream::size_type pos = GetRand(ssSend.size());
            ssSend.erase(ssSend.begin()+pos);
        }
        break;
    case 2:
        // insert a random byte at a random position
        {
            CDataStream::size_type pos = GetRand(ssSend.size());
            char ch = (char)GetRand(256);
            ssSend.insert(ssSend.begin()+pos, ch);
        }
        break;
    }
    // Chance of more than one change half the time:
    // (more changes exponentially less likely):
    Fuzz(2);
}

//
// CAddrDB
//

CAddrDB::CAddrDB() :
    pathAddr {GetDataDir() / "peers.dat"}
{
}

bool CAddrDB::Write(const CAddrMan& addr)
{
    // Generate random temporary filename
    unsigned short randv = 0;
    GetRandBytes((unsigned char*)&randv, sizeof(randv));
    std::string tmpfn = strprintf("peers.dat.%04x", randv);

    // serialize addresses, checksum data up to that point, then append csum
    CDataStream ssPeers(SER_DISK, CLIENT_VERSION);
    ssPeers << FLATDATA(Params().MessageStart());
    ssPeers << addr;
    uint256 hash = Hash(ssPeers.begin(), ssPeers.end());
    ssPeers << hash;

    // open temp output file, and associate with CAutoFile
    boost::filesystem::path pathTmp = GetDataDir() / tmpfn;
    FILE *file = fopen(pathTmp.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s: Failed to open file %s", __func__, pathTmp.string());

    // Write and commit header, data
    try {
        fileout << ssPeers;
    }
    catch (const std::exception& e) {
        return error("%s: Serialize or I/O error - %s", __func__, e.what());
    }
    FileCommit(fileout.Get());
    fileout.fclose();

    // replace existing peers.dat, if any, with new peers.dat.XXXX
    if (!RenameOver(pathTmp, pathAddr))
        return error("%s: Rename-into-place failed", __func__);

    return true;
}

bool CAddrDB::Read(CAddrMan& addr)
{
    // open input file, and associate with CAutoFile
    FILE *file = fopen(pathAddr.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: Failed to open file %s", __func__, pathAddr.string());

    // use file size to size memory buffer
    int fileSize = boost::filesystem::file_size(pathAddr);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }
    filein.fclose();

    CDataStream ssPeers(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssPeers.begin(), ssPeers.end());
    if (hashIn != hashTmp)
        return error("%s: Checksum mismatch, data corrupted", __func__);

    unsigned char pchMsgTmp[4];
    try {
        // de-serialize file header (network specific magic number) and ..
        ssPeers >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
            return error("%s: Invalid network magic number", __func__);

        // de-serialize address data into one CAddrMan object
        ssPeers >> addr;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    return true;
}

unsigned int CConnman::GetReceiveFloodSize() {
    return nReceiveFloodSize;
}

unsigned int CConnman::GetSendBufferSize() {
    return nSendBufferMaxSize;
}

NodeId CConnman::GetNewNodeId()
{
    return nLastNodeId.fetch_add(1, std::memory_order_relaxed);
}

CNode::CNode(std::unique_ptr<Sock>&& sock, const CAddress& addrIn, const std::string& addrNameIn, bool fInboundIn) :
    ssSend{SER_NETWORK, INIT_PROTO_VERSION},
    addrKnown{5000, 0.001},
    setInventoryKnown{connman->GetSendBufferSize() / 1000},
    hSocket{std::move(sock)}
{
    nServices = 0;
    nRecvVersion = INIT_PROTO_VERSION;
    nLastSend = 0;
    nLastRecv = 0;
    nSendBytes = 0;
    nRecvBytes = 0;
    nTimeConnected = GetTime();
    nTimeOffset = 0;
    addr = addrIn;
    addrName = addrNameIn == "" ? addr.ToStringIPPort() : addrNameIn;
    nVersion = 0;
    strSubVer = "";
    fWhitelisted = false;
    fOneShot = false;
    fClient = false; // set by version message
    fInbound = fInboundIn;
    fNetworkNode = false;
    fSuccessfullyConnected = false;
    fDisconnect = false;
    nRefCount = 0;
    nSendSize = 0;
    nSendOffset = 0;
    hashContinue = uint256();
    nStartingHeight = -1;
    fGetAddr = false;
    fRelayTxes = false;
    fSentAddr = false;
    pfilter = new CBloomFilter();
    nPingNonceSent = 0;
    nPingUsecStart = 0;
    nPingUsecTime = 0;
    fPingQueued = false;
    nMinPingUsecTime = std::numeric_limits<int64_t>::max();
    m_addr_token_timestamp = GetTimeMicros();

    id = connman->GetNewNodeId();

    if (fLogIPs)
        LogPrint("net", "Added connection to %s peer=%d\n", addrName, id);
    else
        LogPrint("net", "Added connection peer=%d\n", id);

    for (size_t i = 0; i < allNetMessageTypesSize; i++) {
        mapSendBytesPerMsgType.insert({allNetMessageTypes[i], {0, 0}});
        mapRecvBytesPerMsgType.insert({allNetMessageTypes[i], {0, 0}});
    }

    // Be shy and don't send version until we hear
    if (hSocket && !fInbound)
        PushVersion();

    GetNodeSignals().InitializeNode(GetId(), this);
}

bool CNode::GetTlsFallbackNonTls()
{
    if (tlsFallbackNonTls == eTlsOption::FALLBACK_UNSET)
    {
        // one time only setting of static class attribute
        if ( GetBoolArg("-tlsfallbacknontls", true))
        {
            LogPrint("tls", "%s():%d - Non-TLS connections will be used in case of failure of TLS\n",
                __func__, __LINE__);
            tlsFallbackNonTls = eTlsOption::FALLBACK_TRUE;
        }
        else
        {
            LogPrint("tls", "%s():%d - Non-TLS connections will NOT be used in case of failure of TLS\n",
                __func__, __LINE__);
            tlsFallbackNonTls = eTlsOption::FALLBACK_FALSE;
        }
    }
    return (tlsFallbackNonTls == eTlsOption::FALLBACK_TRUE);
}

bool CNode::GetTlsValidate()
{
    if (tlsValidate == eTlsOption::FALLBACK_UNSET)
    {
        // one time only setting of static class attribute
        if ( GetBoolArg("-tlsvalidate", false))
        {
            LogPrint("tls", "%s():%d - TLS certificates will be validated\n",
                __func__, __LINE__);
            tlsValidate = eTlsOption::FALLBACK_TRUE;
        }
        else
        {
            LogPrint("tls", "%s():%d - TLS certificates will NOT be validated\n",
                __func__, __LINE__);
            tlsValidate = eTlsOption::FALLBACK_FALSE;
        }
    }
    return (tlsValidate == eTlsOption::FALLBACK_TRUE);
}

CNode::~CNode()
{
    // No need to make a lock on cs_hSocket, because before deletion CNode object is removed from the vNodes vector, so any other thread hasn't access to it.
    // Removal is synchronized with read and write routines, so all of them will be completed to this moment.
    
    if (hSocket)
    {
        if (GetSSL())
        {
            unsigned long err_code = 0;
            TLSManager::waitFor(SSL_SHUTDOWN, addr, *hSocket, 0 /*no retries here make no sense on destructor*/, err_code);
        }
    }

    if (pfilter)
        delete pfilter;

    GetNodeSignals().FinalizeNode(GetId());
}

void CNode::AskFor(const CInv& inv)
{
    if (mapAskFor.size() > MAPASKFOR_MAX_SZ || setAskFor.size() > SETASKFOR_MAX_SZ)
        return;
    // a peer may not have multiple non-responded queue positions for a single inv item
    if (!setAskFor.insert(inv.hash).second)
        return;

    // If we need to ask for this inv again (after it has already been received)
    // then pretend we never received it before so that the request is actually performed.
    // Otherwise, this request would be blocked in main::SendMessages.
    if (connman->mapAlreadyReceived.erase(inv)) {
        LogPrint("net", "%s():%d - askfor %s even though it was received already in the past\n", __func__, __LINE__, inv.ToString());
    }

    // We're using mapAskFor as a priority queue,
    // the key is the earliest time the request can be sent
    int64_t nRequestTime;
    LimitedMap<CInv, int64_t>::const_iterator it = connman->mapAlreadyAskedFor.find(inv);
    if (it != connman->mapAlreadyAskedFor.end())
        nRequestTime = it->second;
    else
        nRequestTime = 0;
    LogPrint("net", "askfor %s  %d (%s) peer=%d\n", inv.ToString(), nRequestTime, DateTimeStrFormat("%H:%M:%S", nRequestTime/1000000), id);

    // Make sure not to reuse time indexes to keep things in the same order
    int64_t nNow = GetTimeMicros() - 1000000;
    static int64_t nLastTime;
    ++nLastTime;
    nNow = std::max(nNow, nLastTime);
    nLastTime = nNow;

    // Each retry is 2 minutes after the last
    nRequestTime = std::max(nRequestTime + 2 * 60 * 1000000, nNow);
    if (it != connman->mapAlreadyAskedFor.end())
        connman->mapAlreadyAskedFor.update(it, nRequestTime);
    else
        connman->mapAlreadyAskedFor.insert(std::make_pair(inv, nRequestTime));
    mapAskFor.insert(std::make_pair(nRequestTime, inv));
}

void CNode::BeginMessage(const char* pszCommand) EXCLUSIVE_LOCK_FUNCTION(cs_vSend)
{
    ENTER_CRITICAL_SECTION(cs_vSend);
    assert(ssSend.size() == 0);
    ssSend << CMessageHeader(Params().MessageStart(), pszCommand, 0);
    LogPrint("net", "sending: %s ", SanitizeString(pszCommand));
}

void CNode::AbortMessage() UNLOCK_FUNCTION(cs_vSend)
{
    ssSend.clear();

    LEAVE_CRITICAL_SECTION(cs_vSend);

    LogPrint("net", "(aborted)\n");
}

void CNode::EndMessage(const char* pszCommand) UNLOCK_FUNCTION(cs_vSend)
{
    // The -*messagestest options are intentionally not documented in the help message,
    // since they are only used during development to debug the networking code and are
    // not intended for end-users.
    if (mapArgs.count("-dropmessagestest") && GetRand(GetArg("-dropmessagestest", 2)) == 0)
    {
        LogPrint("net", "dropmessages DROPPING SEND MESSAGE\n");
        AbortMessage();
        return;
    }
    if (mapArgs.count("-fuzzmessagestest"))
        Fuzz(GetArg("-fuzzmessagestest", 10));

    if (ssSend.size() == 0)
    {
        LEAVE_CRITICAL_SECTION(cs_vSend);
        return;
    }
    // Set the size
    unsigned int nSize = ssSend.size() - CMessageHeader::HEADER_SIZE;
    WriteLE32((uint8_t*)&ssSend[CMessageHeader::MESSAGE_SIZE_OFFSET], nSize);

    // Set the checksum
    uint256 hash = Hash(ssSend.begin() + CMessageHeader::HEADER_SIZE, ssSend.end());
    unsigned int nChecksum = 0;
    memcpy(&nChecksum, &hash, sizeof(nChecksum));
    assert(ssSend.size () >= CMessageHeader::CHECKSUM_OFFSET + sizeof(nChecksum));
    memcpy((char*)&ssSend[CMessageHeader::CHECKSUM_OFFSET], &nChecksum, sizeof(nChecksum));

    LogPrint("net", "(%d bytes) peer=%d\n", nSize, id);

    std::deque<CSerializeData>::iterator it = vSendMsg.insert(vSendMsg.end(), CSerializeData());
    ssSend.GetAndClear(*it);
    nSendSize += (*it).size();

    // If write queue empty, attempt "optimistic write"
    if (it == vSendMsg.begin())
        connman->SocketSendData(this);

    // Only now save stats on sent bytes
    AccountForSentBytes(pszCommand, nSize + CMessageHeader::HEADER_SIZE);

    LEAVE_CRITICAL_SECTION(cs_vSend);

    return;
}
