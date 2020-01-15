// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/thread.hpp>

#include "alert.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "darksend.h"
#include "db.h"
#include "init.h"
#include "instantx.h"
#include "kernel.h"
#include "masternode.h"
#include "merkleblock.h"
#include "net.h"
#include "spork.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "undo.h"
#include "utilmoneystr.h"

using namespace std;

#if defined(NDEBUG)
#error "Bitcoin cannot be compiled without assertions."
#endif

/**
 * Global state
 */

CCriticalSection cs_setpwalletRegistered;
set<CWallet*> setpwalletRegistered;

CCriticalSection cs_main;

BlockMap mapBlockIndex;
set<pair<COutPoint, unsigned int> > setStakeSeen;

uint256 bnProofOfStakeLimit(~uint256(0) >> 20);
uint256 bnProofOfStakeLimitV2(~uint256(0) >> 48);
uint256 bnProofOfStakeLimitV2fork(~uint256(0) >> 32);

unsigned int nStakeMinAge = 24 * 60 * 60;      //! 24 hours
unsigned int nStakeMaxAge = 30 * 24 * 60 * 60; //! 30 days
unsigned int nModifierInterval = 10 * 60;      //! time to elapse before new modifier is computed

int nCoinbaseMaturity = 10;

CChain chainActive;
CBlockIndex *pindexBestHeader = NULL;
int64_t nTimeBestReceived = 0;
CWaitableCriticalSection csBestBlock;
CConditionVariable cvBlockChange;
int nScriptCheckThreads = 0;
bool fImporting = false;
bool fReindex = false;
bool fIsBareMultisigStd = true;
bool fCheckBlockIndex = false;
unsigned int nCoinCacheSize = 5000;
bool fAlerts = DEFAULT_ALERTS;

/** Fees smaller than this (in satoshi) are considered zero fee (for relaying) */
CFeeRate minRelayTxFee = CFeeRate(10000);

CTxMemPool mempool(::minRelayTxFee);

struct COrphanTx {
    CTransaction tx;
    NodeId fromPeer;
};

map<uint256, COrphanTx> mapOrphanTransactions;
map<uint256, set<uint256> > mapOrphanTransactionsByPrev;

struct COrphanBlock {
    uint256 hashBlock;
    uint256 hashPrev;
    std::pair<COutPoint, unsigned int> stake;
    std::vector<unsigned char> vchBlock;
};

map<uint256, COrphanBlock*> mapOrphanBlocks;
multimap<uint256, COrphanBlock*> mapOrphanBlocksByPrev;
set<pair<COutPoint, unsigned int> > setStakeSeenOrphan;
size_t nOrphanBlocksSize = 0;

void EraseOrphansFor(NodeId peer);

static void CheckBlockIndex();

//! Constant stuff for coinbase transactions we create:
CScript COINBASE_FLAGS;

const string strMessageMagic = "Linda Signed Message:\n";

std::set<uint256> setValidatedTx;

//! Internal stuff
namespace
{
struct CBlockIndexWorkComparator {
    bool operator()(CBlockIndex* pa, CBlockIndex* pb) const
    {
        //! First sort by most total work, ...
        if (pa->nChainTrust > pb->nChainTrust)
            return false;
        if (pa->nChainTrust < pb->nChainTrust)
            return true;

        //! ... then by earliest time received, ...
        if (pa->nSequenceId < pb->nSequenceId)
            return false;
        if (pa->nSequenceId > pb->nSequenceId)
            return true;
        
        /**
         * Use pointer address as tie breaker (should only happen with blocks
         * loaded from disk, as those all have id 0).
         */
        if (pa < pb)
            return false;
        if (pa > pb)
            return true;

        //! Identical blocks.
        return false;
    }
};

CBlockIndex* pindexBestInvalid;
/**
 * The set of all CBlockIndex entries with BLOCK_VALID_TRANSACTIONS (for itself and all ancestors) and
 * as good as our current tip or better. Entries may be failed, though.
 */
set<CBlockIndex*, CBlockIndexWorkComparator> setBlockIndexCandidates;
//! Number of nodes with fSyncStarted.
int nSyncStarted = 0;
//! All pairs A->B, where A (or one if its ancestors) misses transactions, but B has transactions.
multimap<CBlockIndex*, CBlockIndex*> mapBlocksUnlinked;
CCriticalSection cs_LastBlockFile;
std::vector<CBlockFileInfo> vinfoBlockFile;
int nLastBlockFile = 0;

/** 
 * Every received block is assigned a unique and increasing identifier, so we
 * know which one to give priority in case of a fork.
 */
CCriticalSection cs_nBlockSequenceId;
//! Blocks loaded from disk are assigned id 0, so start the counter at 1.
uint32_t nBlockSequenceId = 1;

/**
 * Sources of received blocks, to be able to send them reject messages or ban
 * them, if processing happens afterwards. Protected by cs_main.
 */
map<uint256, NodeId> mapBlockSource;

struct QueuedBlock {
    uint256 hash;
    CBlockIndex *pindex;  //! Optional.
    int64_t nTime;     //! Time of "getdata" request in microseconds.
    int nValidatedQueuedBefore;  //! Number of blocks queued with validated headers (globally) at the time this one is requested.
    bool fValidatedHeaders;  //! Whether this block has validated headers at the time of request.
};

/**
 * Blocks that are in flight, and that are in the queue to be downloaded.
 * Protected by cs_main.
 */
map<uint256, pair<NodeId, list<QueuedBlock>::iterator> > mapBlocksInFlight;

/** Number of blocks in flight with validated headers. */
int nQueuedValidatedHeaders = 0;

// Dirty block index entries.
set<CBlockIndex*> setDirtyBlockIndex;

// Dirty block file entries.
set<int> setDirtyFileInfo;
} // namespace


//////////////////////////////////////////////////////////////////////////////
/**
 *
 * dispatching functions
 *
 *
 * These functions dispatch to one or all registered wallets
 */
namespace
{
struct CMainSignals {
    //! Notifies listeners of updated transaction data (transaction, and optionally the block it is found in.
    boost::signals2::signal<void(const CTransaction&, const CBlock*)> SyncTransaction;
    //! Notifies listeners of an erased transaction (currently disabled, requires transaction replacement).
    boost::signals2::signal<void(const uint256&)> EraseTransaction;
    //! Notifies listeners of an updated transaction without new data (for now: a coinbase potentially becoming visible).
    boost::signals2::signal<void(const uint256&)> UpdatedTransaction;
    //! Notifies listeners of a new active block chain.
    boost::signals2::signal<void(const CBlockLocator&)> SetBestChain;
    //! Notifies listeners about an inventory item being seen on the network.
    boost::signals2::signal<void(const uint256&)> Inventory;
    //! Tells listeners to broadcast their data.
    boost::signals2::signal<void(bool)> Broadcast;
    // Notifies listeners of a block validation result
    boost::signals2::signal<void (const CBlock&, const CValidationState&)> BlockChecked;
} g_signals;

} // namespace

void RegisterValidationInterface(CValidationInterface* pwalletIn) {
    g_signals.SyncTransaction.connect(boost::bind(&CValidationInterface::SyncTransaction, pwalletIn, _1, _2));
    g_signals.EraseTransaction.connect(boost::bind(&CValidationInterface::EraseFromWallet, pwalletIn, _1));
    g_signals.UpdatedTransaction.connect(boost::bind(&CValidationInterface::UpdatedTransaction, pwalletIn, _1));
    g_signals.SetBestChain.connect(boost::bind(&CValidationInterface::SetBestChain, pwalletIn, _1));
    g_signals.Inventory.connect(boost::bind(&CValidationInterface::Inventory, pwalletIn, _1));
    g_signals.Broadcast.connect(boost::bind(&CValidationInterface::ResendWalletTransactions, pwalletIn, _1));
    g_signals.BlockChecked.connect(boost::bind(&CValidationInterface::BlockChecked, pwalletIn, _1, _2));
}

void UnregisterValidationInterface(CValidationInterface* pwalletIn) {
    g_signals.BlockChecked.disconnect(boost::bind(&CValidationInterface::BlockChecked, pwalletIn, _1, _2));
    g_signals.Broadcast.disconnect(boost::bind(&CValidationInterface::ResendWalletTransactions, pwalletIn, _1));
    g_signals.Inventory.disconnect(boost::bind(&CValidationInterface::Inventory, pwalletIn, _1));
    g_signals.SetBestChain.disconnect(boost::bind(&CValidationInterface::SetBestChain, pwalletIn, _1));
    g_signals.UpdatedTransaction.disconnect(boost::bind(&CValidationInterface::UpdatedTransaction, pwalletIn, _1));
    g_signals.EraseTransaction.disconnect(boost::bind(&CValidationInterface::EraseFromWallet, pwalletIn, _1));
    g_signals.SyncTransaction.disconnect(boost::bind(&CValidationInterface::SyncTransaction, pwalletIn, _1, _2));
}

void UnregisterAllValidationInterfaces()
{
    g_signals.BlockChecked.disconnect_all_slots();
    g_signals.Broadcast.disconnect_all_slots();
    g_signals.Inventory.disconnect_all_slots();
    g_signals.SetBestChain.disconnect_all_slots();
    g_signals.UpdatedTransaction.disconnect_all_slots();
    g_signals.EraseTransaction.disconnect_all_slots();
    g_signals.SyncTransaction.disconnect_all_slots();
}

void SyncWithWallets(const CTransaction& tx, const CBlock* pblock)
{
    g_signals.SyncTransaction(tx, pblock);
}

void ResendWalletTransactions(bool fForce)
{
    g_signals.Broadcast(fForce);
}


//////////////////////////////////////////////////////////////////////////////
/**
 * Registration of network node signals.
 */

namespace
{

    struct CBlockReject {
        unsigned char chRejectCode;
        string strRejectReason;
        uint256 hashBlock;
    };

    /**
     * Maintain validation-specific state about nodes, protected by cs_main, instead
     * by CNode's own locks. This simplifies asynchronous operation, where
     * processing of incoming data is done after the ProcessMessage call returns,
     * and we're no longer holding the node's locks.
     */
    struct CNodeState {
        //! The peer's address
        CService address;
        //! Whether we have a fully established connection.
        bool fCurrentlyConnected;
        //! Accumulated misbehaviour score for this peer.
        int nMisbehavior;
        //! Whether this peer should be disconnected and banned (unless whitelisted).
        bool fShouldBan;
        //! String name of this peer (debugging/logging purposes).
        std::string name;
        //! List of asynchronously-determined block rejections to notify this peer about.
        std::vector<CBlockReject> rejects;
        //! The best known block we know this peer has announced.
        CBlockIndex* pindexBestKnownBlock;
        //! The hash of the last unknown block this peer has announced.
        uint256 hashLastUnknownBlock;
        //! The last full block we both have.
        CBlockIndex *pindexLastCommonBlock;
        //! Whether we've started headers synchronization with this peer.
        bool fSyncStarted;
        //! Since when we're stalling block download progress (in microseconds), or 0.
        int64_t nStallingSince;
        list<QueuedBlock> vBlocksInFlight;
        int nBlocksInFlight;

        CNodeState()
        {
            fCurrentlyConnected = false;
            nMisbehavior = 0;
            fShouldBan = false;
            pindexBestKnownBlock = NULL;
            hashLastUnknownBlock = uint256(0);
            pindexLastCommonBlock = NULL;
            fSyncStarted = false;
            nStallingSince = 0;        
            nBlocksInFlight = 0;
        }
    };
    //! Map maintaining per-node state. Requires cs_main.
    map<NodeId, CNodeState> mapNodeState;

    //! Requires cs_main.
    CNodeState* State(NodeId pnode)
    {
        map<NodeId, CNodeState>::iterator it = mapNodeState.find(pnode);
        if (it == mapNodeState.end())
            return NULL;
        return &it->second;
    }

    int static GetHeight()
    {
        LOCK(cs_main);
        return chainActive.Height();
    }

    void InitializeNode(NodeId nodeid, const CNode* pnode)
    {
        LOCK(cs_main);
        CNodeState& state = mapNodeState.insert(std::make_pair(nodeid, CNodeState())).first->second;
        state.name = pnode->addrName;
        state.address = pnode->addr;
    }

    void FinalizeNode(NodeId nodeid)
    {
        LOCK(cs_main);
        CNodeState* state = State(nodeid);

        if (state->fSyncStarted)
            nSyncStarted--;

        if (state->nMisbehavior == 0 && state->fCurrentlyConnected) {
            AddressCurrentlyConnected(state->address);
        }

        BOOST_FOREACH (const QueuedBlock& entry, state->vBlocksInFlight)
           mapBlocksInFlight.erase(entry.hash);
        EraseOrphansFor(nodeid);

        mapNodeState.erase(nodeid);
    }

    //! Requires cs_main.
    void MarkBlockAsReceived(const uint256& hash)
    {
        map<uint256, pair<NodeId, list<QueuedBlock>::iterator> >::iterator itInFlight = mapBlocksInFlight.find(hash);
        if (itInFlight != mapBlocksInFlight.end()) {
            CNodeState* state = State(itInFlight->second.first);
            nQueuedValidatedHeaders -= itInFlight->second.second->fValidatedHeaders;
            state->vBlocksInFlight.erase(itInFlight->second.second);
            state->nBlocksInFlight--;
            state->nStallingSince = 0;
            mapBlocksInFlight.erase(itInFlight);
        }
    }

    //! Requires cs_main.
    void MarkBlockAsInFlight(NodeId nodeid, const uint256& hash, CBlockIndex *pindex = NULL) 
    {
        CNodeState* state = State(nodeid);
        assert(state != NULL);

        // Make sure it's not listed somewhere already.
        MarkBlockAsReceived(hash);

        QueuedBlock newentry = {hash, pindex, GetTimeMicros(), nQueuedValidatedHeaders, pindex != NULL};
         nQueuedValidatedHeaders += newentry.fValidatedHeaders;
        list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(), newentry);
        state->nBlocksInFlight++;
        mapBlocksInFlight[hash] = std::make_pair(nodeid, it);
    }

    /** Check whether the last unknown block a peer advertized is not yet known. */
    void ProcessBlockAvailability(NodeId nodeid)
    {
        CNodeState* state = State(nodeid);
        assert(state != NULL);

        if (state->hashLastUnknownBlock != 0) {
            BlockMap::iterator itOld = mapBlockIndex.find(state->hashLastUnknownBlock);
            if (itOld != mapBlockIndex.end() && itOld->second->nChainTrust > 0) {
                if (state->pindexBestKnownBlock == NULL || itOld->second->nChainTrust >= state->pindexBestKnownBlock->nChainTrust)
                    state->pindexBestKnownBlock = itOld->second;
                state->hashLastUnknownBlock = uint256(0);
            }
        }
    }

    /** Update tracking information about which blocks a peer is assumed to have. */
    void UpdateBlockAvailability(NodeId nodeid, const uint256& hash)
    {
        CNodeState* state = State(nodeid);
        assert(state != NULL);

        ProcessBlockAvailability(nodeid);

        BlockMap::iterator it = mapBlockIndex.find(hash);
        if (it != mapBlockIndex.end() && it->second->nChainTrust > 0) {
            //! An actually better block was announced.
            if (state->pindexBestKnownBlock == NULL || it->second->nChainTrust >= state->pindexBestKnownBlock->nChainTrust)
                state->pindexBestKnownBlock = it->second;
        } else {
            //! An unknown block was announced; just assume that the latest one is the best one.
            state->hashLastUnknownBlock = hash;
        }
    }

    /** Find the last common ancestor two blocks have.
     *  Both pa and pb must be non-NULL. */
    CBlockIndex* LastCommonAncestor(CBlockIndex* pa, CBlockIndex* pb) {
        if (pa->nHeight > pb->nHeight) {
            pa = pa->GetAncestor(pb->nHeight);
        } else if (pb->nHeight > pa->nHeight) {
            pb = pb->GetAncestor(pa->nHeight);
        }

        while (pa != pb && pa && pb) {
            pa = pa->pprev;
            pb = pb->pprev;
        }

        //! Eventually all chain branches meet at the genesis block.
        assert(pa == pb);
        return pa;
    }

    /** Update pindexLastCommonBlock and add not-in-flight missing successors to vBlocks, until it has
     *  at most count entries. */
    void FindNextBlocksToDownload(NodeId nodeid, unsigned int count, std::vector<CBlockIndex*>& vBlocks, NodeId& nodeStaller) {
        if (count == 0)
            return;

        vBlocks.reserve(vBlocks.size() + count);
        CNodeState *state = State(nodeid);
        assert(state != NULL);

        //! Make sure pindexBestKnownBlock is up to date, we'll need it.
        ProcessBlockAvailability(nodeid);

        if (state->pindexBestKnownBlock == NULL || state->pindexBestKnownBlock->nChainTrust < chainActive.Tip()->nChainTrust) {
            //! This peer has nothing interesting.
            return;
        }

        if (state->pindexLastCommonBlock == NULL) {
            /**
             * Bootstrap quickly by guessing a parent of our best tip is the forking point.
             * Guessing wrong in either direction is not a problem.
             */
            state->pindexLastCommonBlock = chainActive[std::min(state->pindexBestKnownBlock->nHeight, chainActive.Height())];
        }

        /**
         * If the peer reorganized, our previous pindexLastCommonBlock may not be an ancestor
         * of their current tip anymore. Go back enough to fix that.
         */
        state->pindexLastCommonBlock = LastCommonAncestor(state->pindexLastCommonBlock, state->pindexBestKnownBlock);
        if (state->pindexLastCommonBlock == state->pindexBestKnownBlock)
            return;

        std::vector<CBlockIndex*> vToFetch;
        CBlockIndex *pindexWalk = state->pindexLastCommonBlock;
        /**
         * Never fetch further than the best block we know the peer has, or more than BLOCK_DOWNLOAD_WINDOW + 1 beyond our
         * current tip. The +1 is so we can detect stalling, namely if we would be able to download that next block if the
         * window were 1 larger.
         */
        int nMaxHeight = std::min<int>(state->pindexBestKnownBlock->nHeight, chainActive.Height() + BLOCK_DOWNLOAD_WINDOW + 1);
        NodeId waitingfor = -1;
        while (pindexWalk->nHeight < nMaxHeight) {
            /**
             * Read up to 128 (or more, if more blocks than that are needed) successors of pindexWalk (towards
             * pindexBestKnownBlock) into vToFetch. We fetch 128, because CBlockIndex::GetAncestor may be as expensive
             * as iterating over ~100 CBlockIndex* entries anyway.
             */
            int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight, std::max<int>(count - vBlocks.size(), 128));
            vToFetch.resize(nToFetch);
            pindexWalk = state->pindexBestKnownBlock->GetAncestor(pindexWalk->nHeight + nToFetch);
            vToFetch[nToFetch - 1] = pindexWalk;
            for (unsigned int i = nToFetch - 1; i > 0; i--) {
                vToFetch[i - 1] = vToFetch[i]->pprev;
            }

            /**
             * Iterate over those blocks in vToFetch (in forward direction), adding the ones that
             * are not yet downloaded and not in flight to vBlocks. In the mean time, update
             * pindexLastCommonBlock as long as all ancestors are already downloaded.
             */
            BOOST_FOREACH(CBlockIndex* pindex, vToFetch) {
                if (!pindex->IsValid(BLOCK_VALID_TREE)) {
                    //! We consider the chain that this peer is on invalid.
                    return;
                }
                if (pindex->nStatus & BLOCK_HAVE_DATA) {
                    if (pindex->nChainTx)
                        state->pindexLastCommonBlock = pindex;
                } else if (mapBlocksInFlight.count(pindex->GetBlockHash()) == 0) {
                    //! The block is not already downloaded, and not yet in flight.
                    if (pindex->nHeight > chainActive.Height() + (int)BLOCK_DOWNLOAD_WINDOW) {
                        //! We reached the end of the window.
                        if (vBlocks.size() == 0 && waitingfor != nodeid) {
                            //! We aren't able to fetch anything, but we would be if the download window was one larger.
                            nodeStaller = waitingfor;
                        }
                        return;
                    }
                    vBlocks.push_back(pindex);
                    if (vBlocks.size() == count) {
                        return;
                    }
                } else if (waitingfor == -1) {
                    //! This is the first already-in-flight block.
                    waitingfor = mapBlocksInFlight[pindex->GetBlockHash()].first;
                }
            }
        }
    }

} //! anon namespace

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats)
{
    LOCK(cs_main);
    CNodeState* state = State(nodeid);
    if (state == NULL)
        return false;
    stats.nMisbehavior = state->nMisbehavior;
    stats.nSyncHeight = state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
    stats.nCommonHeight = state->pindexLastCommonBlock ? state->pindexLastCommonBlock->nHeight : -1;
    BOOST_FOREACH(const QueuedBlock& queue, state->vBlocksInFlight) {
        if (queue.pindex)
            stats.vHeightInFlight.push_back(queue.pindex->nHeight);
    }
    return true;
}

CBlockIndex* FindForkInGlobalIndex(const CChain& chain, const CBlockLocator& locator)
{
    //! Find the first block the caller has in the main chain
    BOOST_FOREACH(const uint256& hash, locator.vHave) {
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end()) {
            CBlockIndex* pindex = (*mi).second;
            if (chain.Contains(pindex))
                return pindex;
        }
    }
    return chain.Genesis();
}

void RegisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.connect(&GetHeight);
    nodeSignals.ProcessMessages.connect(&ProcessMessages);
    nodeSignals.SendMessages.connect(&SendMessages);
    nodeSignals.InitializeNode.connect(&InitializeNode);
    nodeSignals.FinalizeNode.connect(&FinalizeNode);
}

void UnregisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.disconnect(&GetHeight);
    nodeSignals.ProcessMessages.disconnect(&ProcessMessages);
    nodeSignals.SendMessages.disconnect(&SendMessages);
    nodeSignals.InitializeNode.disconnect(&InitializeNode);
    nodeSignals.FinalizeNode.disconnect(&FinalizeNode);
}

CCoinsViewCache* pcoinsTip = NULL;
CBlockTreeDB* pblocktree = NULL;

//////////////////////////////////////////////////////////////////////////////
/**
 * mapOrphanTransactions
 */

bool AddOrphanTx(const CTransaction& tx, NodeId peer)
{
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    /**
     * Ignore big transactions, to avoid a
     * send-big-orphans memory exhaustion attack. If a peer has a legitimate
     * large transaction with a missing parent then we assume
     * it will rebroadcast it later, after the parent transaction(s)
     * have been mined or received.
     * 10,000 orphans, each of which is at most 5,000 bytes big is
     * at most 500 megabytes of orphans:
     */

    size_t nSize = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);

    if (nSize > 5000) {
        LogPrint("mempool", "ignoring large orphan tx (size: %u, hash: %s)\n", nSize, hash.ToString());
        return false;
    }

    mapOrphanTransactions[hash].tx = tx;
    mapOrphanTransactions[hash].fromPeer = peer;
    BOOST_FOREACH (const CTxIn& txin, tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    LogPrint("mempool", "stored orphan tx %s (mapsz %u)\n", hash.ToString(),
             mapOrphanTransactions.size());
    return true;
}

void static EraseOrphanTx(uint256 hash)
{
    map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.find(hash);
    if (it == mapOrphanTransactions.end())
        return;
    BOOST_FOREACH (const CTxIn& txin, it->second.tx.vin) {
        map<uint256, set<uint256> >::iterator itPrev = mapOrphanTransactionsByPrev.find(txin.prevout.hash);
        if (itPrev == mapOrphanTransactionsByPrev.end())
            continue;
        itPrev->second.erase(hash);
        if (itPrev->second.empty())
            mapOrphanTransactionsByPrev.erase(itPrev);
    }
    mapOrphanTransactions.erase(it);
}


void EraseOrphansFor(NodeId peer)
{
    int nErased = 0;
    map<uint256, COrphanTx>::iterator iter = mapOrphanTransactions.begin();
    while (iter != mapOrphanTransactions.end()) {
        map<uint256, COrphanTx>::iterator maybeErase = iter++; //! increment to avoid iterator becoming invalid
        if (maybeErase->second.fromPeer == peer) {
            EraseOrphanTx(maybeErase->second.tx.GetHash());
            ++nErased;
        }
    }
    if (nErased > 0)
        LogPrint("mempool", "Erased %d orphan tx from peer %d\n", nErased, peer);
}

unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    unsigned int nEvicted = 0;
    while (mapOrphanTransactions.size() > nMaxOrphans) {
        //! Evict a random orphan:
        uint256 randomhash = GetRandHash();
        map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}


//////////////////////////////////////////////////////////////////////////////
/**
 * CTransaction
 */

bool IsStandardTx(const CTransaction& tx, string& reason)
{
    if (tx.nVersion > CTransaction::CURRENT_VERSION || tx.nVersion < 1) {
        reason = "version";
        return false;
    }

    //! nTime has different purpose from nLockTime but can be used in similar attacks
    if (tx.nTime > FutureDrift(GetAdjustedTime())) {
        reason = "time-too-new";
        return false;
    }

    /**
     * Extremely large transactions with lots of inputs can cost the network
     * almost as much to process as they cost the sender in fees, because
     * computing signature hashes is O(ninputs*txsize). Limiting transactions
     * to MAX_STANDARD_TX_SIZE mitigates CPU exhaustion attacks.
     */
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    if (sz >= MAX_STANDARD_TX_SIZE) {
        reason = "tx-size";
        return false;
    }

    BOOST_FOREACH (const CTxIn& txin, tx.vin) {
        /**
         * Biggest 'standard' txin is a 15-of-15 P2SH multisig with compressed
         * keys. (remember the 520 byte limit on redeemScript size) That works
         * out to a (15*(33+1))+3=513 byte redeemScript, 513+1+15*(73+1)+3=1627
         * bytes of scriptSig, which we round off to 1650 bytes for some minor
         * future-proofing. That's also enough to spend a 20-of-20
         * CHECKMULTISIG scriptPubKey, though such a scriptPubKey is not
         * considered standard)
         */
        if (txin.scriptSig.size() > 1650) {
            reason = "scriptsig-size";
            return false;
        }
        if (!txin.scriptSig.IsPushOnly()) {
            reason = "scriptsig-not-pushonly";
            return false;
        }
    }

    unsigned int nDataOut = 0;
    txnouttype whichType;
    BOOST_FOREACH (const CTxOut& txout, tx.vout) {
        if (!::IsStandard(txout.scriptPubKey, whichType)) {
            reason = "scriptpubkey";
            return false;
        }

        if (whichType == TX_NULL_DATA)
            nDataOut++;
        else if ((whichType == TX_MULTISIG) && (!fIsBareMultisigStd)) {
            reason = "bare-multisig";
            return false;
        } else if (txout.IsDust(::minRelayTxFee)) {
            reason = "dust";
            return false;
        }
    }

    /**
     * not more than one data txout per non-data txout is permitted
     * only one data txout is permitted too
     */
    if (nDataOut > 1 && nDataOut > tx.vout.size() / 2) {
        reason = "multi-op-return";
        return false;
    }

    return true;
}

bool IsFinalTx(const CTransaction& tx, int nBlockHeight, int64_t nBlockTime)
{
    AssertLockHeld(cs_main);
    //! Time based nLockTime implemented in 0.1.6
    if (tx.nLockTime == 0)
        return true;
    if (nBlockHeight == 0)
        nBlockHeight = chainActive.Height();
    if (nBlockTime == 0)
        nBlockTime = GetAdjustedTime();
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    BOOST_FOREACH (const CTxIn& txin, tx.vin)
        if (!txin.IsFinal())
            return false;
    return true;
}

/**
 * Check transaction inputs to mitigate two
 * potential denial-of-service attacks:
 *
 * 1. scriptSigs with extra data stuffed into them,
 *    not consumed by scriptPubKey (or P2SH script)
 * 2. P2SH scripts with a crazy number of expensive
 *    CHECKSIG/CHECKMULTISIG operations
 */
bool AreInputsStandard(const CTransaction& tx, const CCoinsViewCache& mapInputs)
{
    if (tx.IsCoinBase())
        return true; //! Coinbases don't use vin normally

    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        const CTxOut& prev = mapInputs.GetOutputFor(tx.vin[i]);

        vector<vector<unsigned char> > vSolutions;
        txnouttype whichType;
        //! get the scriptPubKey corresponding to this input:
        const CScript& prevScript = prev.scriptPubKey;
        if (!Solver(prevScript, whichType, vSolutions))
            return false;
        int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);
        if (nArgsExpected < 0)
            return false;

        /**
         * Transactions with extra stuff in their scriptSigs are
         * non-standard. Note that this EvalScript() call will
         * be quick, because if there are any operations
         * beside "push data" in the scriptSig
         * IsStandard() will have already returned false
         * and this method isn't called.
         */
        vector<vector<unsigned char> > stack;
        if (!EvalScript(stack, tx.vin[i].scriptSig, false, BaseSignatureChecker()))
            return false;

        if (whichType == TX_SCRIPTHASH) {
            if (stack.empty())
                return false;
            CScript subscript(stack.back().begin(), stack.back().end());
            vector<vector<unsigned char> > vSolutions2;
            txnouttype whichType2;
            if (Solver(subscript, whichType2, vSolutions2)) {
                int tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);
                if (tmpExpected < 0)
                    return false;
                nArgsExpected += tmpExpected;
            } else {
                //! Any other Script with less than 15 sigops OK:
                unsigned int sigops = subscript.GetSigOpCount(true);
                //! ... extra data left on the stack after execution is OK, too:
                return (sigops <= MAX_P2SH_SIGOPS);
            }
        }

        if (stack.size() != (unsigned int)nArgsExpected)
            return false;
    }

    return true;
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    BOOST_FOREACH (const CTxIn& txin, tx.vin) {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    BOOST_FOREACH (const CTxOut& txout, tx.vout) {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const CCoinsViewCache& inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        const CTxOut& prevout = inputs.GetOutputFor(tx.vin[i]);
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

bool CheckTransaction(const CTransaction& tx, CValidationState& state)
{
    //! Basic checks that don't depend on any context
    if (tx.vin.empty())
        return state.DoS(10, error("CheckTransaction() : vin empty"),
                         REJECT_INVALID, "bad-txns-vin-empty");
    if (tx.vout.empty())
        return state.DoS(10, error("CheckTransaction() : vout empty"),
                         REJECT_INVALID, "bad-txns-vout-empty");
    //! Size limits
    if (::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return state.DoS(100, error("CheckTransaction() : size limits failed"),
                         REJECT_INVALID, "bad-txns-oversize");

    //! Check for negative or overflow output values
    CAmount nValueOut = 0;
    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const CTxOut& txout = tx.vout[i];
        if (txout.IsEmpty() && !tx.IsCoinBase() && !tx.IsCoinStake())
            return state.DoS(100, error("CheckTransaction() : txout empty for user transaction"),
                             REJECT_INVALID, "bad-txns-vout-empty");
        if (txout.nValue < 0)
            return state.DoS(100, error("CheckTransaction() : txout.nValue negative"),
                             REJECT_INVALID, "bad-txns-vout-negative");
        if (txout.nValue > MAX_MONEY)
            return state.DoS(100, error("CheckTransaction() : txout.nValue too high"),
                             REJECT_INVALID, "bad-txns-vout-toolarge");
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return state.DoS(100, error("CheckTransaction() : txout total out of range"),
                             REJECT_INVALID, "bad-txns-txouttotal-toolarge");
    }

    //! Check for duplicate inputs
    set<COutPoint> vInOutPoints;
    BOOST_FOREACH (const CTxIn& txin, tx.vin) {
        if (vInOutPoints.count(txin.prevout))
            return state.DoS(100, error("CheckTransaction() : duplicate inputs"),
                             REJECT_INVALID, "bad-txns-inputs-duplicate");
        vInOutPoints.insert(txin.prevout);
    }

    if (tx.IsCoinBase()) {
        if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() > 100)
            return state.DoS(100, error("CheckTransaction() : coinbase script size"),
                             REJECT_INVALID, "bad-cb-length");
    } else {
        BOOST_FOREACH (const CTxIn& txin, tx.vin)
            if (txin.prevout.IsNull())
                return state.DoS(10, error("CheckTransaction() : prevout is null"),
                                 REJECT_INVALID, "bad-txns-prevout-null");
    }

    return true;
}


CAmount GetMinRelayFee(const CTransaction& tx, unsigned int nBytes)
{
    {
        LOCK(mempool.cs);
        uint256 hash = tx.GetHash();
        double dPriorityDelta = 0;
        CAmount nFeeDelta = 0;
        mempool.ApplyDeltas(hash, dPriorityDelta, nFeeDelta);
        if (dPriorityDelta > 0 || nFeeDelta > 0)
            return 0;
    }

    CAmount nMinFee;

    nMinFee = ::minRelayTxFee.GetFee(nBytes);

    if (!MoneyRange(nMinFee))
        nMinFee = MAX_MONEY;
    return nMinFee;
}

bool AcceptToMemoryPool(CTxMemPool& pool, CValidationState& state, const CTransaction& tx, bool fLimitFree, bool* pfMissingInputs, bool fRejectInsaneFee)
{
    try {
        std::string errorMessage;
        return AcceptToMemoryPool(pool, state, tx, fLimitFree, pfMissingInputs, fRejectInsaneFee, errorMessage);
    } catch (std::runtime_error& e) {
        return state.Abort(_("System error: ") + e.what());
    }
}

bool AcceptToMemoryPool(CTxMemPool& pool, CValidationState& state, const CTransaction& tx, bool fLimitFree, bool* pfMissingInputs, bool fRejectInsaneFee, std::string& errorMessage)
{
    AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;

    if (!CheckTransaction(tx, state)) {
        errorMessage = "CheckTransaction failed";
        return error("AcceptToMemoryPool : CheckTransaction failed");
    }

    //! Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase()) {
        errorMessage = "coinbase as individual tx";
        return state.DoS(100, error("AcceptToMemoryPool: : coinbase as individual tx"),
                         REJECT_INVALID, "coinbase");
    }

    //! ppcoin: coinstake is also only valid in a block, not as a loose transaction
    if (tx.IsCoinStake()) {
        errorMessage = "coinstake as individual tx";
        return state.DoS(100, error("AcceptToMemoryPool : coinstake as individual tx"),
                         REJECT_INVALID, "coinstake");
    }

    //! Rather not work on nonstandard transactions (unless -testnet)
    string reason;
    if (Params().RequireStandard() && !IsStandardTx(tx, reason)) {
        errorMessage = "nonstandard transaction: " + reason;
        return state.DoS(0,
                         error("AcceptToMemoryPool : nonstandard transaction: %s", reason),
                         REJECT_NONSTANDARD, reason);
    }

    //! Only accept nLockTime-using transactions that can be mined in the next
    //! block; we don't want our mempool filled up with transactions that can't
    //! be mined yet.
    //!
    //! However, IsFinalTx() is confusing... Without arguments, it uses
    //! chainActive.Height() to evaluate nLockTime; when a block is accepted,
    //! chainActive.Height() is set to the value of nHeight in the block.
    //! However, when IsFinalTx() is called within CBlock::AcceptBlock(), the
    //! height of the block *being* evaluated is what is used. Thus if we want
    //! to know if a transaction can be part of the *next* block, we need to
    //! call IsFinalTx() with one more than chainActive.Height().
    //!
    //! Timestamps on the other hand don't get any special treatment, because we
    //! can't know what timestamp the next block will have, and there aren't
    //! timestamp applications where it matters.
    if (!IsFinalTx(tx, chainActive.Height() + 1))
        return state.DoS(0,
                         error("AcceptToMemoryPool : non-final"),
                         REJECT_NONSTANDARD, "non-final");

    //! is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash)) {
        errorMessage = "transaction already in memory pool";
        return false;
    }

    //! Check for conflicts with in-memory transactions
    {
        LOCK(pool.cs); //! protect pool.mapNextTx
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            COutPoint outpoint = tx.vin[i].prevout;
            if (pool.mapNextTx.count(outpoint)) {
                //! Disable replacement feature for now
                return false;
            }
        }
    }

    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        CAmount nValueIn = 0;
        {
            LOCK(pool.cs);
            CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
            view.SetBackend(viewMemPool);

            //! do we already have it?
            if (view.HaveCoins(hash)) {
                errorMessage = "transaction already exists";
                return false;
            }

            /**
             * do all inputs exist?
             * Note that this does not check for the presence of actual outputs (see the next check for that),
             * only helps filling in pfMissingInputs (to determine missing vs spent).
             */
            BOOST_FOREACH (const CTxIn txin, tx.vin) {
                if (!view.HaveCoins(txin.prevout.hash)) {
                    if (pfMissingInputs)
                        *pfMissingInputs = true;
                    errorMessage = "FetchInputs failed";
                    return false;
                }
            }

            //! are the actual inputs available?
            if (!view.HaveInputs(tx)) {
                errorMessage = "inputs already spent";
                return state.Invalid(error("AcceptToMemoryPool : inputs already spent"),
                                     REJECT_DUPLICATE, "bad-txns-inputs-spent");
            }

            //! Bring the best block into scope
            view.GetBestBlock();

            nValueIn = view.GetValueIn(tx);

            //! we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
            view.SetBackend(dummy);
        }

        //! Check for non-standard pay-to-script-hash in inputs
        if (Params().RequireStandard() && !AreInputsStandard(tx, view)) {
            errorMessage = "nonstandard transaction input";
            return error("AcceptToMemoryPool : nonstandard transaction input");
        }

        /**
         * Check that the transaction doesn't have an excessive number of
         * sigops, making it impossible to mine. Since the coinbase transaction
         * itself can contain sigops MAX_TX_SIGOPS is less than
         * MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
         * merely non-standard transaction.
         */
        unsigned int nSigOps = GetLegacySigOpCount(tx);
        nSigOps += GetP2SHSigOpCount(tx, view);
        if (nSigOps > MAX_TX_SIGOPS) {
            errorMessage = "too many sigops " + hash.ToString() + ", " + boost::lexical_cast<string>(nSigOps) + " > " + boost::lexical_cast<string>(MAX_TX_SIGOPS);
            return state.DoS(0,
                             error("AcceptToMemoryPool : too many sigops %s, %d > %d",
                                   hash.ToString(), nSigOps, MAX_TX_SIGOPS));
        }

        CAmount nValueOut = tx.GetValueOut();
        CAmount nFees = nValueIn - nValueOut;
        double dPriority = view.GetPriority(tx, chainActive.Height());

        CTxMemPoolEntry entry(tx, nFees, GetTime(), dPriority, chainActive.Height());
        unsigned int nSize = entry.GetTxSize();

        //! Don't accept it if it can't get into a block
        CAmount txMinFee = GetMinRelayFee(tx, nSize);
        if (fLimitFree && nFees < txMinFee) {
            errorMessage = "not enough fees " + hash.ToString() + ", " + boost::lexical_cast<string>(nFees) + " < " + boost::lexical_cast<string>(txMinFee);
            return error("AcceptToMemoryPool : not enough fees %s, %d < %d",
                         hash.ToString(),
                         nFees, txMinFee);
        }

        //! Require that free transactions have sufficient priority to be mined in the next block.
        if (GetBoolArg("-relaypriority", true) && nFees < ::minRelayTxFee.GetFee(nSize) && !AllowFree(view.GetPriority(tx, chainActive.Height() + 1))) {
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "insufficient priority");
        }
     
        /**
         * Continuously rate-limit free (really, very-low-fee) transactions
         * This mitigates 'penny-flooding' -- sending thousands of free transactions just to
         * be annoying or make others' transactions take longer to confirm.
         */
        if (fLimitFree && nFees < ::minRelayTxFee.GetFee(nSize)) {
            static CCriticalSection csFreeLimiter;
            static double dFreeCount;
            static int64_t nLastTime;
            int64_t nNow = GetTime();

            LOCK(csFreeLimiter);

            //! Use an exponentially decaying ~10-minute window:
            dFreeCount *= pow(1.0 - 1.0 / 600.0, (double)(nNow - nLastTime));
            nLastTime = nNow;
            /**
             * -limitfreerelay unit is thousand-bytes-per-minute
             * At default rate it would take over a month to fill 1GB
             */
            if (dFreeCount >= GetArg("-limitfreerelay", 15)*10*1000)
                return state.DoS(0, error("AcceptToMemoryPool : free transaction rejected by rate limiter"),
                                 REJECT_INSUFFICIENTFEE, "rate limited free transaction");
            LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount + nSize);
            dFreeCount += nSize;
        }

        if (fRejectInsaneFee && nFees > ::minRelayTxFee.GetFee(nSize) * 10000)
            return error("CTxMemPool::accept() : insane fees %s, %d > %d",
                         hash.ToString(),
                         nFees, ::minRelayTxFee.GetFee(nSize) * 10000);

        /**
         * Check against previous transactions
         * This is done last to help prevent CPU exhaustion denial-of-service attacks.
         */
         if (!CheckInputs(tx, state, view, true, STANDARD_SCRIPT_VERIFY_FLAGS, true))
        {
            errorMessage = "ConnectInputs failed " + hash.ToString();
            return error("AcceptToMemoryPool : ConnectInputs failed %s", hash.ToString());
        }
        
        // Check again against just the consensus-critical mandatory script
        // verification flags, in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks, however allowing such transactions into the mempool
        // can be exploited as a DoS attack.
        if (!CheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS, true))
        {
            return error("AcceptToMemoryPool: : BUG! PLEASE REPORT THIS! ConnectInputs failed against MANDATORY but not STANDARD flags %s", hash.ToString());
        }

        //! Store transaction in memory
        pool.addUnchecked(hash, entry);
    }

    setValidatedTx.insert(hash);

    SyncWithWallets(tx, NULL);
    return true;
}

bool AcceptableInputs(CTxMemPool& pool, CValidationState& state, const CTransaction& txo, bool fLimitFree, bool* pfMissingInputs)
{
    AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;

    CTransaction tx(txo);

    if (!CheckTransaction(tx, state))
        return error("AcceptableInputs : CheckTransaction failed");

    //! Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, error("AcceptableInputs : coinbase as individual tx"));

    //! ppcoin: coinstake is also only valid in a block, not as a loose transaction
    if (tx.IsCoinStake())
        return state.DoS(100, error("AcceptableInputs : coinstake as individual tx"));

    //! Rather not work on nonstandard transactions (unless -testnet)
    //!alot of Metrix transactions seem non standard, its a bug so we have to accept these, the transactions have still been checekd to be valid and unspent.
    string reason;
    if (false && !(Params().TestnetToBeDeprecatedFieldRPC()) && !IsStandardTx(tx, reason))
        return error("AcceptableInputs : nonstandard transaction: %s",
                     reason);

    //! is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash))
        return false;

    //! Check for conflicts with in-memory transactions
    {
        LOCK(pool.cs); //! protect pool.mapNextTx
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            COutPoint outpoint = tx.vin[i].prevout;
            if (pool.mapNextTx.count(outpoint)) {
                //! Disable replacement feature for now
                return false;
            }
        }
    }

    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        CAmount nValueIn = 0;
        {
            LOCK(pool.cs);
            CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
            view.SetBackend(viewMemPool);

            //! do we already have it?
            if (view.HaveCoins(hash))
                return false;

            /**
             * do all inputs exist?
             * Note that this does not check for the presence of actual outputs (see the next check for that),
             * only helps filling in pfMissingInputs (to determine missing vs spent).
             */
            BOOST_FOREACH (const CTxIn txin, tx.vin) {
                if (!view.HaveCoins(txin.prevout.hash)) {
                    return false;
                }
            }

            //! are the actual inputs available?
            if (!view.HaveInputs(tx))
                return state.Invalid(error("AcceptToMemoryPool : inputs already spent"),
                                     REJECT_DUPLICATE, "inputs spent");

            //! Bring the best block into scope
            view.GetBestBlock();

            nValueIn = view.GetValueIn(tx);

            //! we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
            view.SetBackend(dummy);
        }

        CAmount nValueOut = tx.GetValueOut();
        CAmount nFees = nValueIn - nValueOut;
        double dPriority = view.GetPriority(tx, chainActive.Height());

        CTxMemPoolEntry entry(tx, nFees, GetTime(), dPriority, chainActive.Height());
        unsigned int nSize = entry.GetTxSize();

        //! Don't accept it if it can't get into a block
        CAmount txMinFee = GetMinRelayFee(tx, nSize);
        if (fLimitFree && nFees < txMinFee)
            return state.DoS(0, error("AcceptableInputs : not enough fees %s, %d < %d", hash.ToString(), nFees, txMinFee),
                             REJECT_INSUFFICIENTFEE, "insufficient fee");

        /**
         * Continuously rate-limit free transactions
         * This mitigates 'penny-flooding' -- sending thousands of free transactions just to
         * be annoying or make others' transactions take longer to confirm.
         */
        if (fLimitFree && nFees < ::minRelayTxFee.GetFee(nSize)) {
            static CCriticalSection csFreeLimiter;
            static double dFreeCount;
            static int64_t nLastTime;
            int64_t nNow = GetTime();

            LOCK(csFreeLimiter);

            //! Use an exponentially decaying ~10-minute window:
            dFreeCount *= pow(1.0 - 1.0 / 600.0, (double)(nNow - nLastTime));
            nLastTime = nNow;
            /**
             * -limitfreerelay unit is thousand-bytes-per-minute
             * At default rate it would take over a month to fill 1GB
             */
            if (dFreeCount > GetArg("-limitfreerelay", 15) * 10 * 1000)
                return state.DoS(0, error("AcceptToMemoryPool : free transaction rejected by rate limiter"),
                                 REJECT_INSUFFICIENTFEE, "insufficient priority");
            LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount + nSize);
            dFreeCount += nSize;
        }

        /**
         * Check against previous transactions
         * This is done last to help prevent CPU exhaustion denial-of-service attacks.
         */
        if (!CheckInputs(tx, state, view, false, STANDARD_SCRIPT_VERIFY_FLAGS, true)) {
            return error("AcceptableInputs : ConnectInputs failed %s", hash.ToString());
        }
    }


    LogPrint("mempool", "AcceptableInputs : accepted %s (poolsz %u)\n",
             hash.ToString(),
             pool.mapTx.size());
    return true;
}

int GetInputAge(CTxIn& vin)
{
    const uint256& prevHash = vin.prevout.hash;
    CTransaction tx;
    uint256 hashBlock;
    bool fFound = GetTransaction(prevHash, tx, hashBlock, true);
    if (fFound) {
        if (mapBlockIndex.find(hashBlock) != mapBlockIndex.end()) {
            return chainActive.Height() - mapBlockIndex[hashBlock]->nHeight;
        } else
            return 0;
    } else
        return 0;
}

//! Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool GetTransaction(const uint256& hash, CTransaction& txOut, uint256& hashBlock, bool fAllowSlow)
{
    CBlockIndex* pindexSlow = NULL;

    LOCK(cs_main);

    if (mempool.lookup(hash, txOut)) {
        return true;
    }

    CDiskTxPos postx;
    if (pblocktree->ReadTxIndex(hash, postx)) {
        CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
        if (file.IsNull())
            return error("%s: OpenBlockFile failed", __func__);
        CBlockHeader header;
        try {
            file >> header;
            fseek(file, postx.nTxOffset, SEEK_CUR);
            file >> txOut;
        } catch (const std::exception& e) {
            return error("%s() : Deserialize or I/O error", __func__);
        }
        hashBlock = header.GetHash();
        if (txOut.GetHash() != hash)
            return error("%s() : txid mismatch", __func__);
        return true;
    }

    if (fAllowSlow) { //! use coin database to locate block that contains transaction, and scan it
        int nHeight = -1;
        {
            CCoinsViewCache& view = *pcoinsTip;
            const CCoins* coins = view.AccessCoins(hash);
            if (coins)
                nHeight = coins->nHeight;
        }
        if (nHeight > 0)
            pindexSlow = chainActive[nHeight];
    }

    if (pindexSlow) {
        CBlock block;
        if (ReadBlockFromDisk(block, pindexSlow)) {
            BOOST_FOREACH (const CTransaction& tx, block.vtx) {
                if (tx.GetHash() == hash) {
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
/**
 * CBlock and CBlockIndex
 */

bool WriteBlockToDisk(CBlock& block, CDiskBlockPos& pos)
{
    //! Open history file to append
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("CBlock::WriteToDisk() : OpenBlockFile failed");

    //! Write index header
    unsigned int nSize = fileout.GetSerializeSize(block);
    fileout << FLATDATA(Params().MessageStart()) << nSize;

    //! Write block
    long fileOutPos = ftell(fileout);
    if (fileOutPos < 0)
        return error("CBlock::WriteToDisk() : ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CDiskBlockPos& pos)
{
    block.SetNull();

    //! Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("CBlock::ReadFromDisk() : OpenBlockFile failed");

    //! Read block
    try {
        filein >> block;
    } catch (const std::exception& e) {
        return error("%s() : Deserialize or I/O error", __func__);
    }

    //! Check the header
    if (block.IsProofOfWork() && !CheckProofOfWork(block.GetPoWHash(), block.nBits))
        return error("CBlock::ReadFromDisk() : errors in block header");

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex)
{
    if (!ReadBlockFromDisk(block, pindex->GetBlockPos()))
        return false;
    if (block.GetHash() != pindex->GetBlockHash())
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*) : GetHash() doesn't match index");
    return true;
}

static uint256 GetProofOfStakeLimit(int nHeight)
{
    if (nHeight < DIFF_FORK_BLOCK) {
        return bnProofOfStakeLimitV2;
    } else {
        return bnProofOfStakeLimitV2fork;
    }
}

//! miner's coin base reward
CAmount GetProofOfWorkReward(const CAmount& nFees)
{
    int64_t nSubsidy = 0;

    if (chainActive.Height() < PREMINE_BLOCK) {
        nSubsidy = 500000000 * COIN; //!  PREMINE 10 BLOCKS
    } else if (chainActive.Height() < FAIR_LAUNCH_BLOCK) {
        nSubsidy = 0; //! No reward block to prevent an instamine
    } else if (chainActive.Height() >= REWARD_START && chainActive.Height() < POW_REWARD_V2_START_BLOCK) {
        nSubsidy = POW_REWARD_V1_FULL * COIN;
    } else if (chainActive.Height() >= POW_REWARD_V2_START_BLOCK && chainActive.Height() <= Params().LastPOWBlock()) {
        nSubsidy = POW_REWARD_V2_FULL * COIN;
    } else {
        nSubsidy = 0;
    }

    LogPrint("creation", "GetProofOfWorkReward() : create=%d(%s)\n", nSubsidy, FormatMoney(nSubsidy));

    return nSubsidy + nFees;
}

CAmount GetProofOfStakeReward(int64_t nCoinAge, const CAmount& nFees, unsigned int nHeight)
{
    CAmount nSubsidy = 0;
    if (chainActive.Tip()->nMoneySupply < MAX_MONEY) {
        CAmount nCoinYearReward = COIN_YEAR_REWARD;

        if (nHeight >= V3_START_BLOCK)
            nCoinYearReward = COIN_YEAR_REWARD_V3;

        nSubsidy = nCoinAge * nCoinYearReward * 33 / (365 * 33 + 8);
    }

    LogPrint("creation", "GetProofOfStakeReward(): create=%s nCoinAge=%d\n", FormatMoney(nSubsidy), nCoinAge);
    return nSubsidy + nFees;
}

/**
 * maximum nBits value could possible be required nTime after
 */
unsigned int ComputeMaxBits(uint256 bnTargetLimit, unsigned int nBase, int64_t nTime)
{
    uint256 bnResult;
    bnResult.SetCompact(nBase);
    bnResult *= 2;
    while (nTime > 0 && bnResult < bnTargetLimit) {
        //! Maximum 200% adjustment per day...
        bnResult *= 2;
        nTime -= 24 * 60 * 60;
    }
    if (bnResult > bnTargetLimit)
        bnResult = bnTargetLimit;
    return bnResult.GetCompact();
}

/**
 * minimum amount of work that could possibly be required nTime after
 * minimum proof-of-work required was nBase
 */
unsigned int ComputeMinWork(unsigned int nBase, int64_t nTime)
{
    return ComputeMaxBits(Params().ProofOfWorkLimit(), nBase, nTime);
}

/**
 * minimum amount of stake that could possibly be required nTime after
 * minimum proof-of-stake required was nBase
 */
unsigned int ComputeMinStake(unsigned int nBase, int64_t nTime, unsigned int nBlockTime)
{
    return ComputeMaxBits(bnProofOfStakeLimit, nBase, nTime);
}


//! ppcoin: find last block index up to pindex
const CBlockIndex* GetLastBlockIndex(const CBlockIndex* pindex, bool fProofOfStake)
{
    while (pindex && pindex->pprev && (pindex->IsProofOfStake() != fProofOfStake))
        pindex = pindex->pprev;
    return pindex;
}

unsigned int GetNextTargetRequired(const CBlockIndex* pindexLast, bool fProofOfStake)
{
    uint256 bnTargetLimit = fProofOfStake ? GetProofOfStakeLimit(pindexLast->nHeight) : Params().ProofOfWorkLimit();
    
    if (pindexLast == NULL)
        return bnTargetLimit.GetCompact(); //! genesis block

    const CBlockIndex* pindexPrev = GetLastBlockIndex(pindexLast, fProofOfStake);
    if (pindexPrev->pprev == NULL)
        return bnTargetLimit.GetCompact(); //! first block
    const CBlockIndex* pindexPrevPrev = GetLastBlockIndex(pindexPrev->pprev, fProofOfStake);
    if (pindexPrevPrev->pprev == NULL)
        return bnTargetLimit.GetCompact(); //! second block

    int64_t nTargetSpacing = Params().TargetSpacing();
    int64_t nActualSpacing = pindexPrev->GetBlockTime() - pindexPrevPrev->GetBlockTime();
    if (nActualSpacing < 0)
        nActualSpacing = nTargetSpacing;

    //! ppcoin: target change every block
    //! ppcoin: retarget with exponential moving toward target spacing
    uint256 bnNew;
    bnNew.SetCompact(pindexPrev->nBits);
    int64_t nInterval = Params().Interval();
    bnNew *= ((nInterval - 1) * nTargetSpacing + nActualSpacing + nActualSpacing);
    bnNew /= ((nInterval + 1) * nTargetSpacing);

    if (bnNew <= 0 || bnNew > bnTargetLimit)
        bnNew = bnTargetLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits)
{
    bool fNegative;
    bool fOverflow;
    uint256 bnTarget;
    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    //! Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > Params().ProofOfWorkLimit())
        return error("CheckProofOfWork() : nBits below minimum work");

    //! Check proof of work matches claimed amount
    if (hash > bnTarget)
        return error("CheckProofOfWork() : hash doesn't match nBits");

    return true;
}

bool CaughtUp()
{
    return ((chainActive.Height() >= Checkpoints::GetTotalBlocksEstimate()) && nTimeBestReceived > GetTime() - 90 * 60);
}

bool CanRequestMoreHeaders()
{
    return ((pindexBestHeader ? pindexBestHeader->nHeight : -1) - chainActive.Height()) < MAX_HEADERS_PENDING;
}

bool IsInitialBlockDownload()
{
    LOCK(cs_main);
    if (fReindex || fImporting || chainActive.Height() < Checkpoints::GetTotalBlocksEstimate())
        return true;
    static int64_t nLastUpdate;
    static CBlockIndex* pindexLastBest;
    if (chainActive.Tip() != pindexLastBest) {
        pindexLastBest = chainActive.Tip();
        nLastUpdate = GetTime();
    }
    return (GetTime() - nLastUpdate < 15 &&
            chainActive.Tip()->GetBlockTime() < GetTime() - 8 * 60 * 60);
}

bool fLargeWorkForkFound = false;
bool fLargeWorkInvalidChainFound = false;
CBlockIndex *pindexBestForkTip = NULL, *pindexBestForkBase = NULL;

void CheckForkWarningConditions()
{
    AssertLockHeld(cs_main);
    /**
     * Before we get past initial download, we cannot reliably alert about forks
     * (we assume we don't get stuck on a fork before the last checkpoint)
     */
    if (IsInitialBlockDownload())
        return;
    /**
     * If our best fork is no longer within 72 blocks (+/- 12 hours if no one mines it)
     * of our head, drop it
     */
    if (pindexBestForkTip && chainActive.Height() - pindexBestForkTip->nHeight >= 72)
        pindexBestForkTip = NULL;

    if (pindexBestForkTip || (pindexBestInvalid && pindexBestInvalid->nChainTrust > chainActive.Tip()->nChainTrust + (chainActive.Tip()->GetBlockTrust() * 6))) {
        if (!fLargeWorkForkFound && pindexBestForkBase) {
            std::string warning = std::string("'Warning: Large-work fork detected, forking after block ") +
                pindexBestForkBase->phashBlock->ToString() + std::string("'");
            CAlert::Notify(warning, true);
        }
        if (pindexBestForkTip && pindexBestForkBase) {
            LogPrintf("CheckForkWarningConditions: Warning: Large valid fork found\n  forking the chain at height %d (%s)\n  lasting to height %d (%s).\nChain state database corruption likely.\n",
                      pindexBestForkBase->nHeight, pindexBestForkBase->phashBlock->ToString(),
                      pindexBestForkTip->nHeight, pindexBestForkTip->phashBlock->ToString());
            fLargeWorkForkFound = true;
        } else {
            LogPrintf("CheckForkWarningConditions: Warning: Found invalid chain at least ~6 blocks longer than our best chain.\nChain state database corruption likely.\n");
            fLargeWorkInvalidChainFound = true;
        }
    } else {
        fLargeWorkForkFound = false;
        fLargeWorkInvalidChainFound = false;
    }
}

void CheckForkWarningConditionsOnNewFork(CBlockIndex* pindexNewForkTip)
{
    AssertLockHeld(cs_main);
    //! If we are on a fork that is sufficiently large, set a warning flag
    CBlockIndex* pfork = pindexNewForkTip;
    CBlockIndex* plonger = chainActive.Tip();
    while (pfork && pfork != plonger) {
        while (plonger && plonger->nHeight > pfork->nHeight)
            plonger = plonger->pprev;
        if (pfork == plonger)
            break;
        pfork = pfork->pprev;
    }
    /**
     * We define a condition which we should warn the user about as a fork of at least 7 blocks
     * who's tip is within 72 blocks (+/- 12 hours if no one mines it) of ours
     * We use 7 blocks rather arbitrarily as it represents just under 10% of sustained network
     * hash rate operating on the fork.
     * or a chain that is entirely longer than ours and invalid (note that this should be detected by both)
     * We define it this way because it allows us to only store the highest fork tip (+ base) which meets
     * the 7-block condition and from this always have the most-likely-to-cause-warning fork
     */
    if (pfork && (!pindexBestForkTip || (pindexBestForkTip && pindexNewForkTip->nHeight > pindexBestForkTip->nHeight)) &&
        pindexNewForkTip->nChainTrust - pfork->nChainTrust > (pfork->GetBlockTrust() * 7) &&
        chainActive.Height() - pindexNewForkTip->nHeight < 72) {
        pindexBestForkTip = pindexNewForkTip;
        pindexBestForkBase = pfork;
    }
    CheckForkWarningConditions();
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (!pindexBestInvalid || pindexNew->nChainTrust > pindexBestInvalid->nChainTrust) {
        pindexBestInvalid = pindexNew;
    }

    uint256 nBestInvalidBlockTrust = pindexNew->nChainTrust - pindexNew->pprev->nChainTrust;
    uint256 nBestBlockTrust = chainActive.Height() != 0 ? (chainActive.Tip()->nChainTrust - chainActive.Tip()->pprev->nChainTrust) : chainActive.Tip()->nChainTrust;

    LogPrintf("InvalidChainFound: invalid block=%s  height=%d  trust=%s  blocktrust=%d  date=%s\n",
              pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
              (pindexNew->nChainTrust).ToString(), nBestInvalidBlockTrust.GetLow64(),
              DateTimeStrFormat("%x %H:%M:%S", pindexNew->GetBlockTime()));
    LogPrintf("InvalidChainFound:  current best=%s  height=%d  trust=%s  blocktrust=%d  date=%s\n",
              chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(),
              (chainActive.Tip()->nChainTrust).ToString(),
              nBestBlockTrust.GetLow64(),
              DateTimeStrFormat("%x %H:%M:%S", chainActive.Tip()->GetBlockTime()));
    CheckForkWarningConditions();
}


void static InvalidBlockFound(CBlockIndex* pindex, const CValidationState& state)
{
    int nDoS = 0;
    if (state.IsInvalid(nDoS)) {
        std::map<uint256, NodeId>::iterator it = mapBlockSource.find(pindex->GetBlockHash());
        if (it != mapBlockSource.end() && State(it->second)) {
            CBlockReject reject = {state.GetRejectCode(), state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), pindex->GetBlockHash()};
            State(it->second)->rejects.push_back(reject);
            if (nDoS > 0)
                Misbehaving(it->second, nDoS);
        }
    }
    if (!state.CorruptionPossible()) {
        pindex->nStatus |= BLOCK_FAILED_VALID;
        setDirtyBlockIndex.insert(pindex);
        setBlockIndexCandidates.erase(pindex);
        InvalidChainFound(pindex);
    }
}

void UpdateTime(CBlockHeader& block, const CBlockIndex* pindexPrev)
{
    block.nTime = max(block.GetBlockTime(), GetAdjustedTime());
}

void UpdateCoins(const CTransaction& tx, CValidationState& state, CCoinsViewCache& inputs, CTxUndo& txundo, int nHeight)
{
    //! mark inputs spent
    if (!tx.IsCoinBase()) {
        txundo.vprevout.reserve(tx.vin.size());
        BOOST_FOREACH(const CTxIn &txin, tx.vin) {
            CCoinsModifier coins = inputs.ModifyCoins(txin.prevout.hash);
            unsigned nPos = txin.prevout.n;

            if (nPos >= coins->vout.size() || coins->vout[nPos].IsNull())
                assert(false);
            // mark an outpoint spent, and construct undo information
            txundo.vprevout.push_back(CTxInUndo(coins->vout[nPos]));
            coins->Spend(nPos);
            if (coins->vout.size() == 0) {
                CTxInUndo& undo = txundo.vprevout.back();
                undo.nHeight = coins->nHeight;
                undo.fCoinBase = coins->fCoinBase;
                undo.nVersion = coins->nVersion;
                undo.fCoinStake = coins->fCoinStake; //! ppcoin
                undo.nTime = coins->nTime;           //! ppcoin
            }
        }
    }

    //! add outputs
    inputs.ModifyCoins(tx.GetHash())->FromTx(tx, nHeight);
}

void UpdateCoins(const CTransaction& tx, CValidationState &state, CCoinsViewCache &inputs, int nHeight)
{
    CTxUndo txundo;
    UpdateCoins(tx, state, inputs, txundo, nHeight);
}

bool CScriptCheck::operator()()
{
    const CScript& scriptSig = ptxTo->vin[nIn].scriptSig;
    if (!VerifyScript(scriptSig, scriptPubKey, nFlags, CachingTransactionSignatureChecker(ptxTo, nIn, cacheStore), &error)) {
        return ::error("CScriptCheck() : %s:%d VerifyScript failed %s", ptxTo->GetHash().ToString(), nIn, ScriptErrorString(error));
    }
    return true;
}

bool DoBurnVout(const CTransaction& tx, unsigned int nVout)
{
    //! if it's not a coin stake then mark it as burnt
    if (!tx.IsCoinStake())
        return false;
    //! if it is a coin stake don't burn the coinstake marker or masternode payment
    if (nVout == 0)
        return false;
    if (tx.vout.size() == 4 && nVout == 3)
        return false;
    if (tx.vout.size() == 3 && tx.vout[1].scriptPubKey != tx.vout[2].scriptPubKey && nVout == 2)
        return false;

    return true;
}

bool CheckInputs(const CTransaction& tx, CValidationState& state, const CCoinsViewCache& inputs, bool fScriptChecks, unsigned int flags, bool cacheStore, std::vector<CScriptCheck>* pvChecks)
{
    if (!tx.IsCoinBase()) {
        if (pvChecks)
            pvChecks->reserve(tx.vin.size());
        /**
         * This doesn't trigger the DoS code on purpose; if it did, it would make it easier
         * for an attacker to attempt to split the network.
         */
        if (!inputs.HaveInputs(tx))
            return state.Invalid(error("CheckInputs() : %s inputs unavailable", tx.GetHash().ToString().substr(0, 10).c_str()));
        /**
         * While checking, GetBestBlock() refers to the parent block.
         * This is also true for mempool checks.
         */
        CBlockIndex* pindexPrev = mapBlockIndex.find(inputs.GetBestBlock())->second;
        int nSpendHeight = pindexPrev->nHeight + 1;
        CAmount nValueIn = 0;
        CAmount nFees = 0;
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            const COutPoint& prevout = tx.vin[i].prevout;
            const CCoins* coins = inputs.AccessCoins(prevout.hash);
            assert(coins);

            //! If prev is coinbase or coinstake, check that it's matured
            if (coins->IsCoinBase() || coins->IsCoinStake())
                if (nSpendHeight - coins->nHeight < nCoinbaseMaturity)
                    return state.Invalid(
                        error("CheckInputs() : tried to spend %s at depth %d", coins->IsCoinBase() ? "coinbase" : "coinstake", nSpendHeight - coins->nHeight),
                        REJECT_INVALID, "bad-txns-premature-spend-of-coinbase");

            //! ppcoin: check transaction timestamp
            if (coins->nTime > tx.nTime)
                return state.DoS(100, error("CheckInputs() : transaction timestamp earlier than input transaction"),
                                 REJECT_INVALID, "timestamp earlier than input transaction");

            //! Check for negative or overflow input values
            nValueIn += coins->vout[prevout.n].nValue;
            if (!MoneyRange(coins->vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                return state.DoS(100, error("CheckInputs() : txin values out of range"),
                                 REJECT_INVALID, "bad-txns-inputvalues-outofrange");
        }

        if (!tx.IsCoinStake()) {
            if (nValueIn < tx.GetValueOut())
            return state.DoS(100, error("CheckInputs() : %s value in (%s) < value out (%s)",
                                        tx.GetHash().ToString(), FormatMoney(nValueIn), FormatMoney(tx.GetValueOut())),                                 REJECT_INVALID, "bad-txns-in-belowout");

            //! Tally transaction fees
            CAmount nTxFee = nValueIn - tx.GetValueOut();
            if (nTxFee < 0)
                return state.DoS(100, error("CheckInputs() : %s nTxFee < 0", tx.GetHash().ToString()),
                                 REJECT_INVALID, "bad-txns-fee-negative");

            nFees += nTxFee;
            if (!MoneyRange(nFees))
                return state.DoS(100, error("CheckInputs() : nFees out of range"),
                                 REJECT_INVALID, "bad-txns-fee-outofrange");
        }

        /**
         * The first loop above does all the inexpensive checks.
         * Only if ALL inputs pass do we perform expensive ECDSA signature checks.
         * Helps prevent CPU exhaustion attacks.
         *
         * Skip ECDSA signature verification when connecting blocks
         * before the last block chain checkpoint. This is safe because block merkle hashes are
         * still computed and checked, and any change will be caught at the next checkpoint.
         */
        if (fScriptChecks) {
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                const COutPoint& prevout = tx.vin[i].prevout;
                const CCoins* coins = inputs.AccessCoins(prevout.hash);
                assert(coins);

                //! Verify signature
                CScriptCheck check(*coins, tx, i, flags, cacheStore);
                if (pvChecks) {
                    pvChecks->push_back(CScriptCheck());
                    check.swap(pvChecks->back());
                } else if (!check()) {
                    if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                        /**
                         * Check whether the failure was caused by a
                         * non-mandatory script verification check, such as
                         * non-standard DER encodings or non-null dummy
                         * arguments; if so, don't trigger DoS protection to
                         * avoid splitting the network between upgraded and
                         * non-upgraded nodes.
                         */
                        CScriptCheck check(*coins, tx, i,
                                flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, cacheStore);
                        if (check())
                            return state.Invalid(false, REJECT_NONSTANDARD, strprintf("non-mandatory-script-verify-flag (%s)", ScriptErrorString(check.GetScriptError())));
                    }
                    /**
                     * Failures of other flags indicate a transaction that is
                     * invalid in new blocks, e.g. a invalid P2SH. We DoS ban
                     * such nodes as they are not following the protocol. That
                     * said during an upgrade careful thought should be taken
                     * as to the correct behavior - we may want to continue
                     * peering with non-upgraded nodes even after a soft-fork
                     * super-majority vote has passed.
                     */
                    return state.DoS(100, false, REJECT_INVALID, strprintf("mandatory-script-verify-flag-failed (%s)", ScriptErrorString(check.GetScriptError())));
                }
            }
        }
    }

    return true;
}

namespace {

bool UndoWriteToDisk(const CBlockUndo& blockundo, CDiskBlockPos& pos, const uint256& hashBlock)
{
    //! Open history file to append
    CAutoFile fileout(OpenUndoFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : OpenUndoFile failed", __func__);

    //! Write index header
    unsigned int nSize = fileout.GetSerializeSize(blockundo);
    fileout << FLATDATA(Params().MessageStart()) << nSize;

    //! Write undo data
    long fileOutPos = ftell(fileout);
    if (fileOutPos < 0)
        return error("%s : ftell failed", __func__);
    pos.nPos = (unsigned int)fileOutPos;
    fileout << blockundo;

    //! calculate & write checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << blockundo;
    fileout << hasher.GetHash();

    return true;
}

bool UndoReadFromDisk(CBlockUndo& blockundo, const CDiskBlockPos& pos, const uint256& hashBlock)
{
    //! Open history file to read
    CAutoFile filein(OpenUndoFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s : OpenBlockFile failed", __func__);

    //! Read block
    uint256 hashChecksum;
    try {
        filein >> blockundo;
        filein >> hashChecksum;
    } catch (const std::exception& e) {
        return error("%s() : Deserialize or I/O error", __func__);
    }

    //! Verify checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << blockundo;
    if (hashChecksum != hasher.GetHash())
        return error("%s : Checksum mismatch", __func__);

    return true;
}

} // anon namespace

bool DisconnectBlock(CBlock& block, CValidationState& state, CBlockIndex* pindex, CCoinsViewCache& view, bool* pfClean)
{
    assert(pindex->GetBlockHash() == view.GetBestBlock());

    if (pfClean)
        *pfClean = false;

    bool fClean = true;

    CBlockUndo blockUndo;
    CDiskBlockPos pos = pindex->GetUndoPos();
    if (pos.IsNull())
        return error("DisconnectBlock() : no undo data available");
    if (!UndoReadFromDisk(blockUndo, pos, pindex->pprev->GetBlockHash()))
        return error("DisconnectBlock() : failure reading undo data");

    if (blockUndo.vtxundo.size() + 1 != block.vtx.size())
        return error("DisconnectBlock() : block and undo data inconsistent");

    //! undo transactions in reverse order
    for (int i = block.vtx.size() - 1; i >= 0; i--) {
        const CTransaction& tx = block.vtx[i];
        uint256 hash = tx.GetHash();

        /**
         * Check that all outputs are available and match the outputs in the block itself
         * exactly. Note that transactions with only provably unspendable outputs won't
         * have outputs available even in the block itself, so we handle that case
         * specially with outsEmpty.
         */
        {
        CCoins outsEmpty;
        CCoinsModifier outs = view.ModifyCoins(hash);
        outs->ClearUnspendable();

        CCoins outsBlock(tx, pindex->nHeight);
        /**
         * The CCoins serialization does not serialize negative numbers.
         * No network rules currently depend on the version here, so an inconsistency is harmless
         * but it must be corrected before txout nversion ever influences a network rule.
         */
        if (outsBlock.nVersion < 0)
            outs->nVersion = outsBlock.nVersion;
        if (*outs != outsBlock)
            fClean = fClean && error("DisconnectBlock() : added transaction mismatch? database corrupted");

        //! remove outputs
         outs->Clear();
        }

        //! restore inputs
        if (i > 0) { //! not coinbases
            const CTxUndo& txundo = blockUndo.vtxundo[i - 1];
            if (txundo.vprevout.size() != tx.vin.size())
                return error("DisconnectBlock() : transaction and undo data inconsistent");
            for (unsigned int j = tx.vin.size(); j-- > 0;) {
                const COutPoint& out = tx.vin[j].prevout;
                const CTxInUndo& undo = txundo.vprevout[j];
                CCoinsModifier coins = view.ModifyCoins(out.hash);
                if (undo.nHeight != 0) {
                    //! undo data contains height: this is the last output of the prevout tx being spent
                    if (!coins->IsPruned())
                        fClean = fClean && error("DisconnectBlock() : undo data overwriting existing transaction");
                    coins->Clear();
                    coins->fCoinBase = undo.fCoinBase;
                    coins->nHeight = undo.nHeight;
                    coins->nVersion = undo.nVersion;
                } else {
                    if (coins->IsPruned())
                        fClean = fClean && error("DisconnectBlock() : undo data adding output to missing transaction");
                }
                if (coins->IsAvailable(out.n))
                    fClean = fClean && error("DisconnectBlock() : undo data overwriting existing output");
                if (coins->vout.size() < out.n+1)
                    coins->vout.resize(out.n+1);
                coins->vout[out.n] = undo.txout;
            }
        }
    }

    if (block.IsProofOfStake())
        setStakeSeen.erase(block.GetProofOfStake());

    //! move best block pointer to prevout block
    view.SetBestBlock(pindex->pprev->GetBlockHash());

    //! ppcoin: clean up wallet after disconnecting coinstake
    BOOST_FOREACH (CTransaction& tx, block.vtx)
        SyncWithWallets(tx, NULL);

    if (pfClean) {
        *pfClean = fClean;
        return true;
    } else {
        return fClean;
    }
}

bool FindUndoPos(CValidationState& state, int nFile, CDiskBlockPos& pos, unsigned int nAddSize);

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);

void ThreadScriptCheck()
{
    RenameThread("Metrix-scriptch");
    scriptcheckqueue.Thread();
}

static int64_t nTimeVerify = 0;
static int64_t nTimeConnect = 0;
static int64_t nTimeIndex = 0;
static int64_t nTimeCallbacks = 0;
static int64_t nTimeTotal = 0;

bool ConnectBlock(CBlock& block, CValidationState& state, CBlockIndex* pindex, CCoinsViewCache& view, bool fJustCheck)
{
    AssertLockHeld(cs_main);
    //! Check it again in case a previous version let a bad block in, but skip BlockSig checking
    if (!CheckBlock(block, state, !fJustCheck, !fJustCheck, false)){
        LogPrintf("ERROR ConnectBlock() : CheckBlock failed\n");
        return false;
    }

    //! verify that the view's current state corresponds to the previous block
    uint256 hashPrevBlock = pindex->pprev == NULL ? uint256(0) : pindex->pprev->GetBlockHash();
    assert(hashPrevBlock == view.GetBestBlock());

    //! Check block POS/POW work
    if (!fJustCheck && !UpdateHashProof(block, state, pindex))
        return false;

    //! Special case for the genesis block, skipping connection of its transactions
    //! (its coinbase is unspendable)
    if (block.GetHash() == Params().HashGenesisBlock()) {
        view.SetBestBlock(pindex->GetBlockHash());
        return true;
    }

    //! BIP30
    BOOST_FOREACH (const CTransaction& tx, block.vtx) {
        const CCoins* coins = view.AccessCoins(tx.GetHash());
        if (coins && !coins->IsPruned())
            return state.DoS(100, error("ConnectBlock() : tried to overwrite transaction"),
                             REJECT_INVALID, "bad-txns-BIP30");
    }

    bool fScriptChecks = pindex->nHeight >= Checkpoints::GetTotalBlocksEstimate();

    unsigned int flags = SCRIPT_VERIFY_P2SH;
    // Start enforcing the DERSIG (BIP66) rules
    // For Metrix the chain has always been BIP66 compliant so there is no need for a soft fork
    flags |= SCRIPT_VERIFY_DERSIG;

    // Start enforcing CHECKLOCKTIMEVERIFY, (BIP65) for block.nVersion=8
    // blocks, when 75% of the network has upgraded:
    if (block.nVersion >= 7 && IsSuperMajority(8, pindex->pprev, Params().EnforceBlockUpgradeMajority())) {
        flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    }

    CBlockUndo blockundo;

    CCheckQueueControl<CScriptCheck> control(fScriptChecks && nScriptCheckThreads ? &scriptcheckqueue : NULL);

    int64_t nTimeStart = GetTimeMicros();
    CAmount nFees = 0;
    CAmount nValueIn = 0;
    CAmount nValueOut = 0;
    CAmount nStakeReward = 0;
    unsigned int nSigOps = 0;
    int nInputs = 0;
    CDiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(block.vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos> > vPos;
    vPos.reserve(block.vtx.size());
    blockundo.vtxundo.reserve(block.vtx.size() - 1);
    for (unsigned int i = 0; i < block.vtx.size(); i++) {
        const CTransaction& tx = block.vtx[i];
        nInputs += tx.vin.size();
        nSigOps += GetLegacySigOpCount(tx);
        if (nSigOps > MAX_BLOCK_SIGOPS)
            return state.DoS(100, error("ConnectBlock() : too many sigops"),
                             REJECT_INVALID, "bad-blk-sigops");

        if (tx.IsCoinBase())
            nValueOut += tx.GetValueOut();
        else {
            if (!view.HaveInputs(tx))
                return state.DoS(100, error("ConnectBlock() : inputs missing/spent"),
                                 REJECT_INVALID, "bad-txns-inputs-missingorspent");

            /**
             * Add in sigops done by pay-to-script-hash inputs;
             * this is to prevent a "rogue miner" from creating
             * an incredibly-expensive-to-validate block.
             */
            nSigOps += GetP2SHSigOpCount(tx, view);
            if (nSigOps > MAX_BLOCK_SIGOPS)
                return state.DoS(100, error("ConnectBlock() : too many sigops"),
                                 REJECT_INVALID, "bad-blk-sigops");

            CAmount nTxValueIn = view.GetValueIn(tx);
            CAmount nTxValueOut = tx.GetValueOut();
            nValueIn += nTxValueIn;
            nValueOut += nTxValueOut;
            if (!tx.IsCoinStake())
                nFees += nTxValueIn - nTxValueOut;
            if (tx.IsCoinStake())
                nStakeReward = nTxValueOut - nTxValueIn;

            std::vector<CScriptCheck> vChecks;
            if (!CheckInputs(tx, state, view, fScriptChecks, flags, false, nScriptCheckThreads ? &vChecks : NULL))
                return false;
            control.Add(vChecks);
        }
        CTxUndo undoDummy;
        if (i > 0) {
            blockundo.vtxundo.push_back(CTxUndo());
        }
        UpdateCoins(tx, state, view, i == 0 ? undoDummy : blockundo.vtxundo.back(), pindex->nHeight);

        vPos.push_back(std::make_pair(tx.GetHash(), pos));
        pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
    }

    int64_t nTime1 = GetTimeMicros();
    nTimeConnect += nTime1 - nTimeStart;
    LogPrint("bench", "      - Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin) [%.2fs]\n", (unsigned)block.vtx.size(), 0.001 * (nTime1 - nTimeStart), 0.001 * (nTime1 - nTimeStart) / block.vtx.size(), nInputs <= 1 ? 0 : 0.001 * (nTime1 - nTimeStart) / (nInputs - 1), nTimeConnect * 0.000001);


    if (block.IsProofOfWork()) {
        CAmount nReward = GetProofOfWorkReward(nFees);

        //! Check coinbase reward
        if (block.vtx[0].GetValueOut() > nReward)
            return state.DoS(50, error("ConnectBlock() : coinbase reward exceeded (actual=%d vs calculated=%d)", block.vtx[0].GetValueOut(), nReward),
                             REJECT_INVALID, "bad-cb-amount");
    }

    if (block.IsProofOfStake()) {
        //! ppcoin: coin stake tx earns reward instead of paying fee
        uint64_t nCoinAge;
        if (!GetCoinAge(block.vtx[1], state, view, nCoinAge, pindex->nHeight))
            return error("ConnectBlock() : %s unable to get coin age for coinstake", block.vtx[1].GetHash().ToString());

        CAmount nCalculatedStakeReward = GetProofOfStakeReward(nCoinAge, nFees, pindex->nHeight);

        if (pindex->nHeight >= V3_START_BLOCK) {
            //! if we are paying a masternode we need to validate this reward
            //! and adjust the calculated stake reward
            if (block.HasMasternodePayment())
            {
                CAmount nMasternodePayment = block.GetMasternodePayment();
                LogPrint("connectblock", "ConnectBlock(): HasMasternodePayment=%i amount=%d\n", pindex->nHeight, nMasternodePayment);
                if (!IsBlockMasternodePaymentValid(pindex, nMasternodePayment))
                {
                    return state.DoS(50, error("ConnectBlock() : masternode reward exceeded (actual=%d)", nMasternodePayment),
                             REJECT_INVALID, "bad-mn-amount");
                }
                nCalculatedStakeReward += nMasternodePayment;
            }
        }

        //! account for a rounding discrepancy in an old version
        if (pindex->nHeight == 476205)
            nCalculatedStakeReward += 100000;

        if (nStakeReward > nCalculatedStakeReward)
            return state.DoS(100, error("ConnectBlock() : coinstake pays too much(actual=%d vs calculated=%d)", nStakeReward, nCalculatedStakeReward),
                             REJECT_INVALID, "bad-cb-amount");
    }

    //! ppcoin: track money supply and mint amount info
    pindex->nMint = nValueOut - nValueIn + nFees;
    pindex->nMoneySupply = (pindex->pprev ? pindex->pprev->nMoneySupply : 0) + nValueOut - nValueIn;

    int64_t nTime2 = GetTimeMicros();
    nTimeVerify += nTime2 - nTimeStart;
    LogPrint("bench", "    - Verify %u txins: %.2fms (%.3fms/txin) [%.2fs]\n", nInputs - 1, 0.001 * (nTime2 - nTimeStart), nInputs <= 1 ? 0 : 0.001 * (nTime2 - nTimeStart) / (nInputs - 1), nTimeVerify * 0.000001);

    if (fJustCheck)
        return true;

    //! Write undo information to disk
    if (pindex->GetUndoPos().IsNull() || !pindex->IsValid(BLOCK_VALID_SCRIPTS)) {
        if (pindex->GetUndoPos().IsNull()) {
            CDiskBlockPos pos;
            if (!FindUndoPos(state, pindex->nFile, pos, ::GetSerializeSize(blockundo, SER_DISK, CLIENT_VERSION) + 40))
                return error("ConnectBlock() : FindUndoPos failed");
            if (!UndoWriteToDisk(blockundo, pos, pindex->pprev->GetBlockHash()))
                return state.Abort("Failed to write undo data");

            //! update nUndoPos in block index
            pindex->nUndoPos = pos.nPos;
            pindex->nStatus |= BLOCK_HAVE_UNDO;
        }

        pindex->RaiseValidity(BLOCK_VALID_SCRIPTS);
        setDirtyBlockIndex.insert(pindex);
    }

    if (!pblocktree->WriteTxIndex(vPos))
        return state.Abort(_("Failed to write transaction index"));
    //! add this block to the view's block chain
    view.SetBestBlock(pindex->GetBlockHash());

    int64_t nTime3 = GetTimeMicros();
    nTimeIndex += nTime3 - nTime2;
    LogPrint("bench", "    - Index writing: %.2fms [%.2fs]\n", 0.001 * (nTime3 - nTime2), nTimeIndex * 0.000001);


    BOOST_FOREACH (const CTransaction& tx, block.vtx)
        SyncWithWallets(tx, &block);

    //! Watch for changes to the previous coinbase transaction.
    static uint256 hashPrevBestCoinBase;
    g_signals.UpdatedTransaction(hashPrevBestCoinBase);
    hashPrevBestCoinBase = block.vtx[0].GetHash();

    int64_t nTime4 = GetTimeMicros();
    nTimeCallbacks += nTime4 - nTime3;
    LogPrint("bench", "    - Callbacks: %.2fms [%.2fs]\n", 0.001 * (nTime4 - nTime3), nTimeCallbacks * 0.000001);


    return true;
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
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }
}

enum FlushStateMode {
    FLUSH_STATE_IF_NEEDED,
    FLUSH_STATE_PERIODIC,
    FLUSH_STATE_ALWAYS
};

/**
 * Update the on-disk chain state.
 * The caches and indexes are flushed if either they're too large, forceWrite is set, or
 * fast is not set and it's been a while since the last write.
 */
bool static FlushStateToDisk(CValidationState &state, FlushStateMode mode)
{
    LOCK(cs_main);
    static int64_t nLastWrite = 0;
    try {
    if ((mode == FLUSH_STATE_ALWAYS) ||
        ((mode == FLUSH_STATE_PERIODIC || mode == FLUSH_STATE_IF_NEEDED) && pcoinsTip->GetCacheSize() > nCoinCacheSize) ||
        (mode == FLUSH_STATE_PERIODIC && GetTimeMicros() > nLastWrite + DATABASE_WRITE_INTERVAL * 1000000)) {
        /**
         * Typical CCoins structures on disk are around 100 bytes in size.
         * Pushing a new one to the database can cause it to be written
         * twice (once in the log, and once in the tables). This is already
         * an overestimation, as most will delete an existing entry or
         * overwrite one. Still, use a conservative safety factor of 2.
         */
        if (!CheckDiskSpace(100 * 2 * 2 * pcoinsTip->GetCacheSize()))
            return state.Error("out of disk space");
        // First make sure all block and undo data is flushed to disk.
        FlushBlockFile();
        // Then update all block file information (which may refer to block and undo files).
        {
            std::vector<std::pair<int, const CBlockFileInfo*> > vFiles;
            vFiles.reserve(setDirtyFileInfo.size());
            for (set<int>::iterator it = setDirtyFileInfo.begin(); it != setDirtyFileInfo.end(); ) {
                vFiles.push_back(make_pair(*it, &vinfoBlockFile[*it]));
                setDirtyFileInfo.erase(it++);
            }
            std::vector<const CBlockIndex*> vBlocks;
            vBlocks.reserve(setDirtyBlockIndex.size());
            for (set<CBlockIndex*>::iterator it = setDirtyBlockIndex.begin(); it != setDirtyBlockIndex.end(); ) {
                vBlocks.push_back(*it);
                setDirtyBlockIndex.erase(it++);
            }
            if (!pblocktree->WriteBatchSync(vFiles, nLastBlockFile, vBlocks)) {
                return state.Abort("Files to write to block index database");
            }
        }
        // Finally flush the chainstate (which may refer to block index entries).
        if (!pcoinsTip->Flush())
            return state.Abort("Failed to write to coin database");
        // Update best block in wallet (so we can detect restored wallets).
        if (mode != FLUSH_STATE_IF_NEEDED) {
            g_signals.SetBestChain(chainActive.GetLocator());
        }
        nLastWrite = GetTimeMicros();
    }
    } catch (const std::runtime_error& e) {
        return state.Abort(std::string("System error while flushing: ") + e.what());
    }
    return true;
}

void FlushStateToDisk() {
    CValidationState state;
    FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
}

//! Update chainActive and related internal data structures.
void static UpdateTip(CBlockIndex* pindexNew)
{
    chainActive.SetTip(pindexNew);

    //! New best block
    nTimeBestReceived = GetTime();
    mempool.AddTransactionsUpdated(1);

    uint256 nBestBlockTrust = chainActive.Height() != 0 ? (chainActive.Tip()->nChainTrust - chainActive.Tip()->pprev->nChainTrust) : chainActive.Tip()->nChainTrust;

    LogPrintf("UpdateTip: new best=%s  height=%d  trust=%s  blocktrust=%d  tx=%lu  date=%s cache=%u\n",
              chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(),
              (chainActive.Tip()->nChainTrust).ToString(),
              nBestBlockTrust.GetLow64(),
              (unsigned long)pindexNew->nChainTx,
              DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()), (unsigned int)pcoinsTip->GetCacheSize());

    cvBlockChange.notify_all();

    //! Check the version of the last 100 blocks to see if we need to upgrade:
    static bool fWarned = false;
    if (!IsInitialBlockDownload() && !fWarned)    
    {
        int nUpgraded = 0;
        const CBlockIndex* pindex = chainActive.Tip();
        int nblockCurrentVersion = CBlock::CURRENT_VERSION;
        if (pindex->nHeight > V8_START_BLOCK)
        {
            nblockCurrentVersion = 8;
        }
        for (int i = 0; i < 100 && pindex != NULL; i++) {
            if (pindex->nVersion > nblockCurrentVersion)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            LogPrintf("SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, nblockCurrentVersion);
        if (nUpgraded > 100 / 2)
        {
                    //! strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: This version is obsolete, upgrade required!");
            CAlert::Notify(strMiscWarning, true);
            fWarned = true;
        }
    }
}

//! Disconnect chainActive's tip.
bool static DisconnectTip(CValidationState& state)
{
    CBlockIndex* pindexDelete = chainActive.Tip();
    assert(pindexDelete);
    mempool.check(pcoinsTip);
    //! Read block from disk.
    CBlock block;
    if (!ReadBlockFromDisk(block, pindexDelete))
        return state.Abort("Failed to read block");
    //! Apply the block atomically to the chain state.
    int64_t nStart = GetTimeMicros();
    {
        CCoinsViewCache view(pcoinsTip);
        if (!DisconnectBlock(block, state, pindexDelete, view))
            return error("DisconnectTip() : DisconnectBlock %s failed", pindexDelete->GetBlockHash().ToString());
        assert(view.Flush());
    }
    LogPrint("bench", "- Disconnect block: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
    //! Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(state, FLUSH_STATE_IF_NEEDED))
        return false;
    //! Resurrect mempool transactions from the disconnected block.
    BOOST_FOREACH (const CTransaction& tx, block.vtx) {
        //! ignore validation errors in resurrected transactions
        list<CTransaction> removed;
        CValidationState stateDummy;
        if (tx.IsCoinBase() || !AcceptToMemoryPool(mempool, stateDummy, tx, false, NULL))
            mempool.remove(tx, removed, true);
    }
    mempool.removeCoinbaseSpends(pcoinsTip, pindexDelete->nHeight);
    mempool.check(pcoinsTip);
    //! Update chainActive and related variables.
    UpdateTip(pindexDelete->pprev);
    //! Let wallets know transactions went from 1-confirmed to
    //! 0-confirmed or conflicted:
    BOOST_FOREACH (const CTransaction& tx, block.vtx) {
        SyncWithWallets(tx, NULL);
    }
    return true;
}

static int64_t nTimeReadFromDisk = 0;
static int64_t nTimeConnectTotal = 0;
static int64_t nTimeFlush = 0;
static int64_t nTimeChainState = 0;
static int64_t nTimePostConnect = 0;

//! Connect a new block to chainActive. pblock is either NULL or a pointer to a CBlock
//! corresponding to pindexNew, to bypass loading it again from disk.
bool static ConnectTip(CValidationState& state, CBlockIndex* pindexNew, CBlock* pblock)
{
    assert(pindexNew->pprev == chainActive.Tip());
    mempool.check(pcoinsTip);

    //! Read block from disk.
    int64_t nTime1 = GetTimeMicros();
    CBlock block;
    if (!pblock) {
        if (!ReadBlockFromDisk(block, pindexNew))
            return state.Abort("Failed to read block");
        pblock = &block;
    }
    //! Apply the block atomically to the chain state.
    int64_t nTime2 = GetTimeMicros();
    nTimeReadFromDisk += nTime2 - nTime1;
    int64_t nTime3;
    LogPrint("bench", "  - Load block from disk: %.2fms [%.2fs]\n", (nTime2 - nTime1) * 0.001, nTimeReadFromDisk * 0.000001);
    {
        CCoinsViewCache view(pcoinsTip);
        CInv inv(MSG_BLOCK, pindexNew->GetBlockHash());
        bool rv = ConnectBlock(*pblock, state, pindexNew, view);
        g_signals.BlockChecked(*pblock, state);
        if (!rv) {
            if (state.IsInvalid())
                InvalidBlockFound(pindexNew, state);
            return error("ConnectTip() : ConnectBlock %s failed", pindexNew->GetBlockHash().ToString());
        }
        mapBlockSource.erase(inv.hash);
        nTime3 = GetTimeMicros();
        nTimeConnectTotal += nTime3 - nTime2;
        LogPrint("bench", "  - Connect total: %.2fms [%.2fs]\n", (nTime3 - nTime2) * 0.001, nTimeConnectTotal * 0.000001);
        assert(view.Flush());
    }

    int64_t nTime4 = GetTimeMicros();
    nTimeFlush += nTime4 - nTime3;
    LogPrint("bench", "  - Flush: %.2fms [%.2fs]\n", (nTime4 - nTime3) * 0.001, nTimeFlush * 0.000001);
    //! Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(state, FLUSH_STATE_IF_NEEDED))
        return false;
    int64_t nTime5 = GetTimeMicros();
    nTimeChainState += nTime5 - nTime4;
    LogPrint("bench", "  - Writing chainstate: %.2fms [%.2fs]\n", (nTime5 - nTime4) * 0.001, nTimeChainState * 0.000001);
    //! Remove conflicting transactions from the mempool.
    list<CTransaction> txConflicted;
    mempool.removeForBlock(pblock->vtx, pindexNew->nHeight, txConflicted);
    mempool.check(pcoinsTip);
    //! Update chainActive & related variables.
    UpdateTip(pindexNew);
    //! Tell wallet about transactions that went from mempool
    //! to conflicted:
    BOOST_FOREACH (const CTransaction& tx, txConflicted) {
        SyncWithWallets(tx, NULL);
    }
    //! ... and about transactions that got confirmed:
    BOOST_FOREACH (const CTransaction& tx, pblock->vtx) {
        SyncWithWallets(tx, pblock);
    }

    int64_t nTime6 = GetTimeMicros();
    nTimePostConnect += nTime6 - nTime5;
    nTimeTotal += nTime6 - nTime1;
    LogPrint("bench", "  - Connect postprocess: %.2fms [%.2fs]\n", (nTime6 - nTime5) * 0.001, nTimePostConnect * 0.000001);
    LogPrint("bench", "- Connect block: %.2fms [%.2fs]\n", (nTime6 - nTime1) * 0.001, nTimeTotal * 0.000001);
    return true;
}

//! Return the tip of the chain with the most work in it, that isn't
//! known to be invalid (it's however far from certain to be valid).
static CBlockIndex* FindMostWorkChain()
{
    do {
        CBlockIndex* pindexNew = NULL;

        //! Find the best candidate header.
        {
            std::set<CBlockIndex*, CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexCandidates.rbegin();
            if (it == setBlockIndexCandidates.rend())
                return NULL;
            pindexNew = *it;
        }

        //! Check whether all blocks on the path between the currently active chain and the candidate are valid.
        //! Just going until the active chain is an optimization, as we know all blocks in it are valid already.
        CBlockIndex* pindexTest = pindexNew;
        bool fInvalidAncestor = false;
        while (pindexTest && !chainActive.Contains(pindexTest)) {
            assert(pindexTest->nStatus & BLOCK_HAVE_DATA);
            assert(pindexTest->nChainTx || pindexTest->nHeight == 0);
            if (pindexTest->nStatus & BLOCK_FAILED_MASK) {
                //! Candidate has an invalid ancestor, remove entire chain from the set.
                if (pindexBestInvalid == NULL || pindexNew->nChainTrust > pindexBestInvalid->nChainTrust)
                    pindexBestInvalid = pindexNew;
                CBlockIndex* pindexFailed = pindexNew;
                while (pindexTest != pindexFailed) {
                    pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
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
static void PruneBlockIndexCandidates() {
    //! Note that we can't delete the current block itself, as we may need to return to it later in case a
    //! reorganization to a better block fails.
    std::set<CBlockIndex*, CBlockIndexWorkComparator>::iterator it = setBlockIndexCandidates.begin();
    while (it != setBlockIndexCandidates.end() && setBlockIndexCandidates.value_comp()(*it, chainActive.Tip())) {
        setBlockIndexCandidates.erase(it++);
    }
    //! Either the current tip or a successor of it we're working towards is left in setBlockIndexCandidates.
    assert(!setBlockIndexCandidates.empty());
}

//! Try to make some progress towards making pindexMostWork the active block.
//! pblock is either NULL or a pointer to a CBlock corresponding to pindexMostWork.
static bool ActivateBestChainStep(CValidationState& state, CBlockIndex* pindexMostWork, CBlock* pblock)
{
    AssertLockHeld(cs_main);
    bool fInvalidFound = false;
    const CBlockIndex *pindexOldTip = chainActive.Tip();
    const CBlockIndex *pindexFork = chainActive.FindFork(pindexMostWork);

    //! Disconnect active blocks which are no longer in the best chain.
    while (chainActive.Tip() && chainActive.Tip() != pindexFork) {
        if (!DisconnectTip(state))
            return false;
    }

    //! Build list of new blocks to connect.
    std::vector<CBlockIndex*> vpindexToConnect;
    bool fContinue = true;
    int nHeight = pindexFork ? pindexFork->nHeight : -1;
    while (fContinue && nHeight != pindexMostWork->nHeight) {
    //! Don't iterate the entire list of potential improvements toward the best tip, as we likely only need
    //! a few blocks along the way.
    int nTargetHeight = std::min(nHeight + 32, pindexMostWork->nHeight);
    vpindexToConnect.clear();
    vpindexToConnect.reserve(nTargetHeight - nHeight);
    CBlockIndex *pindexIter = pindexMostWork->GetAncestor(nTargetHeight);
    while (pindexIter && pindexIter->nHeight != nHeight) {
        vpindexToConnect.push_back(pindexIter);
        pindexIter = pindexIter->pprev;
    }
    nHeight = nTargetHeight;

    //! Connect new blocks.
    BOOST_REVERSE_FOREACH(CBlockIndex *pindexConnect, vpindexToConnect) {
        if (!ConnectTip(state, pindexConnect, pindexConnect == pindexMostWork ? pblock : NULL)) {
            if (state.IsInvalid()) {
                //! The block violates a consensus rule.
                if (!state.CorruptionPossible())
                    InvalidChainFound(vpindexToConnect.back());
                state = CValidationState();
                fInvalidFound = true;
                fContinue = false;
                break;
            } else {
                //! A system error occurred (disk space, database error, ...).
                return false;
            }
        } else {
            PruneBlockIndexCandidates();
            if (!pindexOldTip || chainActive.Tip()->nChainTrust > pindexOldTip->nChainTrust) {
                //! We're in a better position than we were. Return temporarily to release the lock.
                fContinue = false;
                break;
            }
        }
    }
    }

    //! Callbacks/notifications for a new best chain.
    if (fInvalidFound)
        CheckForkWarningConditionsOnNewFork(vpindexToConnect.back());
    else
        CheckForkWarningConditions();

    return true;
}

/**
 * Make the best chain active, in multiple steps. The result is either failure
 * or an activated best chain. pblock is either NULL or a pointer to a block
 * that is already loaded (to avoid loading it again from disk).
 */
bool ActivateBestChain(CValidationState& state, CBlock* pblock)
{
    LOCK(cs_main);
    CBlockIndex* pindexNewTip = NULL;
    CBlockIndex* pindexMostWork = NULL;
    do {
        boost::this_thread::interruption_point();

        bool fInitialDownload;
        {
            LOCK(cs_main);
            pindexMostWork = FindMostWorkChain();

            //! Whether we have anything to do at all.
            if (pindexMostWork == NULL || pindexMostWork == chainActive.Tip())
                return true;

            if (!ActivateBestChainStep(state, pindexMostWork, pblock && pblock->GetHash() == pindexMostWork->GetBlockHash() ? pblock : NULL))
                return false;

            pindexNewTip = chainActive.Tip();
            fInitialDownload = IsInitialBlockDownload();
        }
        //! When we reach this point, we switched to a new tip (stored in pindexNewTip).

        //! Notifications/callbacks that can run without cs_main
        if (!fInitialDownload) {
            uint256 hashNewTip = pindexNewTip->GetBlockHash();
            //! Relay inventory, but don't relay old inventory during initial block download.
            int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate();
            {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                    if (chainActive.Height() > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate))
                        pnode->PushInventory(CInv(MSG_BLOCK, hashNewTip));
            }
            
            // Notify external listeners about the new tip.
            uiInterface.NotifyBlockTip(hashNewTip);
        }

        uiInterface.NotifyBlocksChanged();
    } while (pindexMostWork != chainActive.Tip());
    CheckBlockIndex();

    // Write changes periodically to disk, after relay.
    if (!FlushStateToDisk(state, FLUSH_STATE_PERIODIC)) {
        return false;
    }

    return true;
}

/**
 * ppcoin: total coin age spent in transaction, in the unit of coin-days.
 * Only those coins meeting minimum age requirement counts. As those
 * transactions not in main chain are not currently indexed so we
 * might not find out about their coin age. Older transactions are
 * guaranteed to be in main chain by sync-checkpoint. This rule is
 * introduced to help nodes establish a consistent view of the coin
 * age (trust score) of competing branches.
 */
bool GetCoinAge(const CTransaction& tx, CValidationState& state, CCoinsViewCache& view, uint64_t& nCoinAge, unsigned int nHeight)
{
    uint256 bnCentSecond = 0; //! coin age in the unit of cent-seconds
    nCoinAge = 0;

    if (tx.IsCoinBase())
        return true;

    BOOST_FOREACH (const CTxIn& txin, tx.vin) {
        //! First try finding the previous transaction in database
        const COutPoint& prevout = txin.prevout;
        CCoins coins;

        if (!view.GetCoins(prevout.hash, coins))
            continue; //! previous transaction not in main chain
        if (tx.nTime < coins.nTime)
            return false; //! Transaction timestamp violation

        CDiskTxPos postx;
        CTransaction txPrev;
        if (pblocktree->ReadTxIndex(prevout.hash, postx)) {
            CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
            if (file.IsNull())
                return error("%s: OpenBlockFile failed", __func__);
            CBlockHeader header;
            try {
                file >> header;
                fseek(file, postx.nTxOffset, SEEK_CUR);
                file >> txPrev;
            } catch (const std::exception& e) {
                return error("%s() : Deserialize or I/O error in GetCoinAge()", __func__);
            }
            if (txPrev.GetHash() != prevout.hash)
                return error("%s() : txid mismatch in GetCoinAge()", __func__);

            if (header.GetBlockTime() + nStakeMinAge > tx.nTime)
                continue; //! only count coins meeting min age requirement

            CAmount nValueIn = 0;
            int64_t nTimeWeight = 0;

            if (nHeight < V3_START_BLOCK) {
                nValueIn = txPrev.vout[txin.prevout.n].nValue;
                nTimeWeight = tx.nTime - txPrev.nTime;
            } else {
                nValueIn = min(txPrev.vout[txin.prevout.n].nValue, MAX_STAKE_VALUE);
                nTimeWeight = min(tx.nTime - txPrev.nTime, nStakeMaxAge);
            }

            bnCentSecond += uint256(nValueIn) * nTimeWeight / CENT;

            if (fDebug && GetBoolArg("-printcoinage", false)) {
                LogPrint("getcoinage", "GetCoinAge::RAW  nValueIn=%d nTimeDiff=%d\n", txPrev.vout[txin.prevout.n].nValue, tx.nTime - txPrev.nTime);
                LogPrint("getcoinage", "GetCoinAge::CALC nValueIn=%d nTimeDiff=%d\n", nValueIn, nTimeWeight);
                LogPrint("getcoinage", "GetCoinAge bnCentSecond=%s\n", bnCentSecond.ToString());
            }
        } else
            return error("%s() : tx missing in tx index in GetCoinAge()", __func__);
    }

    uint256 bnCoinDay = bnCentSecond * CENT / COIN / (24 * 60 * 60);
    if (fDebug && GetBoolArg("-printcoinage", false))
        LogPrintf("coin age bnCoinDay=%s\n", bnCoinDay.ToString());
    nCoinAge = bnCoinDay.GetLow64();
    return true;
}

bool InvalidateBlock(CValidationState& state, CBlockIndex *pindex) {
    AssertLockHeld(cs_main);

    // Mark the block itself as invalid.
    pindex->nStatus |= BLOCK_FAILED_VALID;
    setDirtyBlockIndex.insert(pindex);
    setBlockIndexCandidates.erase(pindex);

    while (chainActive.Contains(pindex)) {
        CBlockIndex *pindexWalk = chainActive.Tip();
        pindexWalk->nStatus |= BLOCK_FAILED_CHILD;
        setDirtyBlockIndex.insert(pindexWalk);
        setBlockIndexCandidates.erase(pindexWalk);
        // ActivateBestChain considers blocks already in chainActive
        // unconditionally valid already, so force disconnect away from it.
        if (!DisconnectTip(state)) {
            return false;
        }
    }

    // The resulting new best tip may not be in setBlockIndexCandidates anymore, so
    // add them again.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && !setBlockIndexCandidates.value_comp()(it->second, chainActive.Tip())) {
            setBlockIndexCandidates.insert(it->second);
        }
        it++;
    }

    InvalidChainFound(pindex);
    return true;
}

bool ReconsiderBlock(CValidationState& state, CBlockIndex *pindex) {
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
                pindexBestInvalid = NULL;
            }
        }
        it++;
    }

    // Remove the invalidity flag from all ancestors too.
    while (pindex != NULL) {
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            pindex->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(pindex);
        }
        pindex = pindex->pprev;
    }
    return true;
}


CBlockIndex* AddToBlockIndex(const CBlockHeader& block)
{
    //! Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end())
        return it->second;

    //! Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(block);
    assert(pindexNew);
    /**
     * We assign the sequence id to blocks only when the full data is available,
     * to avoid miners withholding blocks but broadcasting headers, to get a
     * competitive advantage.
     */
    pindexNew->nSequenceId = 0;
    BlockMap::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = mapBlockIndex.find(block.hashPrevBlock);
    if (miPrev != mapBlockIndex.end()) {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();
    }
    //! ppcoin: compute chain trust score
    pindexNew->nChainTrust = (pindexNew->pprev ? pindexNew->pprev->nChainTrust : 0) + pindexNew->GetBlockTrust();
    pindexNew->RaiseValidity(BLOCK_VALID_TREE);
    if (pindexBestHeader == NULL || pindexBestHeader->nChainTrust < pindexNew->nChainTrust)
        pindexBestHeader = pindexNew;

    setDirtyBlockIndex.insert(pindexNew);

    return pindexNew;
}

//! Mark a block as having its data received and checked (up to BLOCK_VALID_TRANSACTIONS).
bool ReceivedBlockTransactions(const CBlock& block, CValidationState& state, CBlockIndex* pindexNew, const CDiskBlockPos& pos, const uint256& hashProof)
{
    pindexNew->nTx = block.vtx.size();
    pindexNew->nChainTx = 0;
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus |= BLOCK_HAVE_DATA;
    pindexNew->RaiseValidity(BLOCK_VALID_TRANSACTIONS);
    setDirtyBlockIndex.insert(pindexNew);

    //! metrix: SetExtractionSource
    pindexNew->SetPOSDetail(block);

    //! ppcoin: compute stake entropy bit for stake modifier
    if (!pindexNew->SetStakeEntropyBit(block.GetStakeEntropyBit()))
        return error("AddToBlockIndex() : SetStakeEntropyBit() failed");

    //! Record proof hash value
    pindexNew->hashProof = hashProof;

    if (pindexNew->IsProofOfStake())
        setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));

    if (pindexNew->pprev == NULL || pindexNew->pprev->nChainTx) {
        //! If pindexNew is the genesis block or all parents are BLOCK_VALID_TRANSACTIONS.
        deque<CBlockIndex*> queue;
        queue.push_back(pindexNew);

        //! Recursively process any descendant blocks that now may be eligible to be connected.
        while (!queue.empty()) {
            CBlockIndex *pindex = queue.front();
            queue.pop_front();
            pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
            {
                LOCK(cs_nBlockSequenceId);
                pindex->nSequenceId = nBlockSequenceId++;
            }
            if (chainActive.Tip() == NULL || !setBlockIndexCandidates.value_comp()(pindex, chainActive.Tip())) {
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

bool FindBlockPos(CValidationState& state, CDiskBlockPos& pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown = false)
{
    LOCK(cs_LastBlockFile);

    unsigned int nFile = fKnown ? pos.nFile : nLastBlockFile;
    if (vinfoBlockFile.size() <= nFile) {
        vinfoBlockFile.resize(nFile + 1);
    }
    if (!fKnown) {
        while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            LogPrintf("Leaving block file %i: %s\n", nFile, vinfoBlockFile[nFile].ToString());
            FlushBlockFile(true);
            nFile++;
            if (vinfoBlockFile.size() <= nFile) {
                vinfoBlockFile.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = vinfoBlockFile[nFile].nSize;
    }
    nLastBlockFile = nFile;
    vinfoBlockFile[nFile].AddBlock(nHeight, nTime);

    if (fKnown)
        vinfoBlockFile[nFile].nSize = std::max(pos.nPos + nAddSize, vinfoBlockFile[nFile].nSize);
    else
        vinfoBlockFile[nFile].nSize += nAddSize;
    
    if (!fKnown) {
        unsigned int nOldChunks = (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks = (vinfoBlockFile[nFile].nSize + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        if (nNewChunks > nOldChunks) {
            if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos)) {
                FILE *file = OpenBlockFile(pos);
                if (file) {
                    LogPrintf("Pre-allocating up to position 0x%x in blk%05u.dat\n", nNewChunks * BLOCKFILE_CHUNK_SIZE, pos.nFile);
                    AllocateFileRange(file, pos.nPos, nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos);
                    fclose(file);
                }
            }
            else
                return state.Error("out of disk space");
        }
    }

    setDirtyFileInfo.insert(nFile);
    return true;
}

bool FindUndoPos(CValidationState& state, int nFile, CDiskBlockPos& pos, unsigned int nAddSize)
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


bool CheckBlockHeader(const CBlockHeader& block, CValidationState& state, bool fCheckPOW)
{
    if (block.GetHash() != Params().HashGenesisBlock() && block.nVersion < 7)
        return state.DoS(100, error("CheckBlockHeader() : reject too old nVersion = %d", block.nVersion),
                         REJECT_INVALID, "old nVersion");

    //! check POW block is before cutoff
    if (fCheckPOW && chainActive.Height() >= Params().LastPOWBlock())
        return state.DoS(100, error("CheckBlockHeader() : reject proof-of-work at height %d", chainActive.Height()),
                         REJECT_INVALID, "rejected pow");

    //! Check proof of work matches claimed amount
    if (fCheckPOW && !CheckProofOfWork(block.GetPoWHash(), block.nBits))
        return state.DoS(50, error("CheckBlockHeader() : proof of work failed"),
                         REJECT_INVALID, "high-hash");

    //! Check timestamp
    if (block.GetBlockTime() > FutureDrift(GetAdjustedTime()) && !fCheckPOW)
        return state.Invalid(error("CheckBlockHeader() : block timestamp too far in the future"),
                             REJECT_INVALID, "time-too-new");

    //! Get prev block index
    CBlockIndex* pindexPrev = NULL;
    uint256 hash = block.GetHash();
    if (hash != Params().HashGenesisBlock()) {
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end())
            return state.DoS(0, error("%s : prev block %s not found", __func__, block.hashPrevBlock.ToString().c_str()), 0, "bad-prevblk");
        pindexPrev = (*mi).second;
        if (pindexPrev->nStatus & BLOCK_FAILED_MASK)
            return state.DoS(100, error("%s : prev block invalid", __func__), REJECT_INVALID, "bad-prevblk");
    }

    //! Reject block.nVersion=8 blocks when 95% (75% on testnet) of the network has upgraded:
    if (block.nVersion < 8 && IsSuperMajority(8, pindexPrev, Params().RejectBlockOutdatedMajority()))
    {
        return state.Invalid(error("%s : rejected nVersion=7 block", __func__),
                             REJECT_OBSOLETE, "bad-version");
    }

    return true;
}

bool CheckBlock(const CBlock& block, CValidationState& state, bool fCheckPOW, bool fCheckMerkleRoot, bool fCheckSig)
{
    //! These are checks that are independent of context.

    //! Check that the header is valid (particularly PoW).  This is mostly
    //! redundant with the call in AcceptBlockHeader.
    if (!CheckBlockHeader(block, state, block.IsProofOfWork() && fCheckPOW))
        return state.DoS(100, error("CheckBlock() : CheckBlockHeader failed"),
                         REJECT_INVALID, "bad-header", true);

    //! Check the merkle root.
    if (fCheckMerkleRoot) {
        bool mutated;
        uint256 hashMerkleRoot2 = block.BuildMerkleTree(&mutated);
        if (block.hashMerkleRoot != hashMerkleRoot2)
            return state.DoS(100, error("CheckBlock() : hashMerkleRoot mismatch"),
                             REJECT_INVALID, "bad-txnmrklroot", true);

        /**
         * Check for merkle tree malleability (CVE-2012-2459): repeating sequences
         * of transactions in a block without affecting the merkle root of a block,
         * while still invalidating it.
         */
        if (mutated)
            return state.DoS(100, error("CheckBlock() : duplicate transaction"),
                             REJECT_INVALID, "bad-txns-duplicate", true);
    }

    /**
     * All potential-corruption validation must be done before we do any
     * transaction validation, as otherwise we may mark the header as invalid
     * because we receive the wrong transactions for it.
     */

    //! Size limits
    if (block.vtx.empty() || block.vtx.size() > MAX_BLOCK_SIZE || ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return state.DoS(100, error("CheckBlock() : size limits failed"),
                         REJECT_INVALID, "bad-blk-length");

    //! First transaction must be coinbase, the rest must not be
    if (block.vtx.empty() || !block.vtx[0].IsCoinBase())
        return state.DoS(100, error("CheckBlock() : first tx is not coinbase"),
                         REJECT_INVALID, "bad-cb-missing");
    for (unsigned int i = 1; i < block.vtx.size(); i++)
        if (block.vtx[i].IsCoinBase())
            return state.DoS(100, error("CheckBlock() : more than one coinbase"),
                             REJECT_INVALID, "bad-cb-multiple");

    if (block.IsProofOfStake()) {
        //! Coinbase output should be empty if proof-of-stake block
        if (block.vtx[0].vout.size() != 1 || !block.vtx[0].vout[0].IsEmpty())
            return state.DoS(100, error("CheckBlock() : coinbase output not empty for proof-of-stake block"),
                             REJECT_INVALID, "pos coinbase output not empty");

        //! Second transaction must be coinstake, the rest must not be
        if (block.vtx.empty() || !block.vtx[1].IsCoinStake())
            return state.DoS(100, error("CheckBlock() : second tx is not coinstake"),
                             REJECT_INVALID, "tx not coinstake");
        for (unsigned int i = 2; i < block.vtx.size(); i++)
            if (block.vtx[i].IsCoinStake())
                return state.DoS(100, error("CheckBlock() : more than one coinstake"),
                                 REJECT_INVALID, "duplicate coinstake");
    }

    //! Check proof-of-stake block signature
    if (fCheckSig && !CheckBlockSignature(block))
        return state.DoS(100, error("CheckBlock() : bad proof-of-stake block signature"),
                         REJECT_INVALID, "bad pos signature");


    //! ----------- instantX transaction scanning -----------

    if (IsSporkActive(SPORK_1_MASTERNODE_PAYMENTS_ENFORCEMENT_DEFAULT)) {
        BOOST_FOREACH (const CTransaction& tx, block.vtx) {
            if (!tx.IsCoinBase()) {
                //!only reject blocks when it's based on complete consensus
                BOOST_FOREACH (const CTxIn& in, tx.vin) {
                    if (mapLockedInputs.count(in.prevout)) {
                        if (mapLockedInputs[in.prevout] != tx.GetHash()) {
                            if (fDebug) {
                                LogPrintf("CheckBlock() : found conflicting transaction with transaction lock %s %s\n", mapLockedInputs[in.prevout].ToString().c_str(), tx.GetHash().ToString().c_str());
                            }
                            return state.DoS(0, error("CheckBlock() : found conflicting transaction with transaction lock"),
                                             REJECT_INVALID, "conflicting tx with lock");
                        }
                    }
                }
            }
        }
    } else {
        if (fDebug) {
            LogPrintf("CheckBlock() : skipping transaction locking checks\n");
        }
    }


    //! ----------- masternode payments -----------
    //! Start enforcing for block.nVersion=8 blocks 
    //! when 75% of the network has upgraded
    if (block.HasMasternodePayment())
    {
        LOCK2(cs_main, mempool.cs);
        CBlockIndex* pindex = chainActive.Tip();
        if (pindex != NULL)
        {
            if (IsSuperMajority(8, pindex, Params().EnforceBlockUpgradeMajority()))
            {
                if (block.nTime > GetTime() - MASTERNODE_MIN_DSEEP_SECONDS)
                {
                    if (vecMasternodes.size() == 0)
                    {
                        if (!IsValidMasternodePayment(pindex->nHeight + 1, block))
                        {
                            LogPrint("masternode", "CheckBlock() : Invalid masternode payment at block %i\n", chainActive.Height() + 1);
                            return state.DoS(100, error("CheckBlock() : Invalid masternode payment"),
                                            REJECT_INVALID, "invalid masternode payment");
                        }
                        else
                        {
                            LogPrint("masternode", "CheckBlock() : Valid masternode payment at block %i\n", chainActive.Height() + 1); 
                        }
                    }
                    else
                    {
                        LogPrint("masternode", "CheckBlock() : Have not synced masternode list \n");
                    }
                }
                else
                {
                    LogPrint("masternode", "CheckBlock() : Block is older than dseep time, skipping masternode payment check %d\n", chainActive.Height() + 1);
                }
            }
            else
            {
                LogPrint("masternode", "CheckBlock() : Skipping masternode payment check - is not super majority nHeight %d Hash %s\n", chainActive.Height() + 1, block.GetHash().ToString().c_str());
            }
        }
        else
        {
            LogPrint("masternode", "CheckBlock() : pindex is null, skipping masternode payment check\n");
        }
    }
    else
    {
        LogPrint("masternode", "CheckBlock() : skipping masternode payment checks\n");
    }


    //! Check transactions
    BOOST_FOREACH (const CTransaction& tx, block.vtx) {
        if (!CheckTransaction(tx, state))
            return error("CheckBlock() : CheckTransaction failed");

        //! ppcoin: check transaction timestamp
        if (block.GetBlockTime() < (int64_t)tx.nTime)
            return state.DoS(50, error("CheckBlock() : block timestamp earlier than transaction timestamp"),
                             REJECT_INVALID, "timestamp earlier than tx");
    }

    unsigned int nSigOps = 0;
    BOOST_FOREACH (const CTransaction& tx, block.vtx) {
        nSigOps += GetLegacySigOpCount(tx);
    }
    if (nSigOps > MAX_BLOCK_SIGOPS)
        return state.DoS(100, error("CheckBlock() : out-of-bounds SigOpCount"),
                         REJECT_INVALID, "bad-blk-sigops", true);

    return true;
}

bool AcceptBlockHeader(const CBlockHeader& block, CValidationState& state, CBlockIndex** ppindex)
{
    AssertLockHeld(cs_main);

    //! Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator miSelf = mapBlockIndex.find(hash);
    CBlockIndex* pindex = NULL;
    if (miSelf != mapBlockIndex.end()) {
        //! Block header is already known.
        pindex = miSelf->second;
        if (ppindex)
            *ppindex = pindex;
        if (pindex->nStatus & BLOCK_FAILED_MASK)
            return state.Invalid(error("%s : block is marked invalid", __func__), 0, "duplicate");
            return true;
    }

    CBlockIndex* pindexPrev = NULL;
    int nHeight = 0;
    if (hash != Params().HashGenesisBlock()) {
        //! Get prev block index
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end())
            return state.DoS(10, error("%s : prev block not found", __func__), 0, "bad-prevblk");
        pindexPrev = (*mi).second;
        if (pindexPrev->nStatus & BLOCK_FAILED_MASK)
            return state.DoS(100, error("%s : prev block invalid", __func__), REJECT_INVALID, "bad-prevblk");
        nHeight = pindexPrev->nHeight + 1;

        if (!CheckBlockHeader(block, state, block.nNonce > 0 && nHeight < Params().LastPOWBlock()))
           return false;

        //! Check timestamp against prev
        if (block.GetBlockTime() <= pindexPrev->GetPastTimeLimit() || FutureDrift(block.GetBlockTime()) < pindexPrev->GetBlockTime())
            return state.Invalid(error("%s : block's timestamp is too early", __func__),
                                 REJECT_INVALID, "time-too-old");

        //! Check that the block chain matches the known block chain up to a checkpoint
        if (!Checkpoints::CheckBlock(nHeight, hash))
            return state.DoS(100, error("%s : rejected by hardened checkpoint lock-in at %d", __func__, nHeight),
                             REJECT_CHECKPOINT, "checkpoint mismatch");

        //! Don't accept any forks from the main chain prior to last checkpoint
        CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint();
        if (pcheckpoint && nHeight < pcheckpoint->nHeight)
            return state.DoS(100, error("%s : forked chain older than last checkpoint (height %d)", __func__, nHeight));
    }
    if (pindex == NULL)
        pindex = AddToBlockIndex(block);

    if (ppindex)
        *ppindex = pindex;

    return true;
}

bool AcceptBlock(CBlock& block, CValidationState& state, CBlockIndex** ppindex, CDiskBlockPos* dbp)
{
    AssertLockHeld(cs_main);

    CBlockIndex*& pindex = *ppindex;

    if (!AcceptBlockHeader(block, state, &pindex))
        return false;

    if (pindex->nStatus & BLOCK_HAVE_DATA) {
        //! TODO: deal better with duplicate blocks.
        //! return state.DoS(20, error("AcceptBlock() : already have block %d %s", pindex->nHeight, pindex->GetBlockHash().ToString()), REJECT_DUPLICATE, "duplicate");
        return true;
    }

    if (!CheckBlock(block, state)) {
        if (state.IsInvalid() && !state.CorruptionPossible()) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
            setDirtyBlockIndex.insert(pindex);
        }
        return false;
    }

    int nHeight = pindex->nHeight;
    uint256 hash = pindex->GetBlockHash();

    if (hash != Params().HashGenesisBlock()) {
        //! Check coinbase timestamp
        if (block.GetBlockTime() > FutureDrift((int64_t)block.vtx[0].nTime) && !block.IsProofOfWork())
            return state.DoS(50, error("AcceptBlock() : coinbase timestamp is too early"),
                             REJECT_INVALID, "coinbase timestamp too early");

        //! Check coinstake timestamp
        if (block.IsProofOfStake() && !CheckCoinStakeTimestamp(block.GetBlockTime(), (int64_t)block.vtx[1].nTime))
            return state.DoS(50, error("AcceptBlock() : coinstake timestamp violation nTimeBlock=%d nTimeTx=%u", block.GetBlockTime(), block.vtx[1].nTime),
                             REJECT_INVALID, "coinbase timestamp violation");

        //! Check that all transactions are finalized
        BOOST_FOREACH (const CTransaction& tx, block.vtx)
            if (!IsFinalTx(tx, nHeight, block.GetBlockTime())) {
                pindex->nStatus |= BLOCK_FAILED_VALID;
                return state.DoS(10, error("AcceptBlock() : contains a non-final transaction"),
                                 REJECT_INVALID, "bad-txns-nonfinal");
            }
       
        //! Enforce rule that the coinbase starts with serialized block height
        {
            CScript expect = CScript() << nHeight;
            if (block.vtx[0].vin[0].scriptSig.size() < expect.size() ||
                !std::equal(expect.begin(), expect.end(), block.vtx[0].vin[0].scriptSig.begin())) {
                pindex->nStatus |= BLOCK_FAILED_VALID;
                return state.DoS(100, error("AcceptBlock() : block height mismatch in coinbase"), REJECT_INVALID, "bad-cb-height");
            }
        }
    }
   

    //! Write block to history file
    try {
        unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
        CDiskBlockPos blockPos;
        if (dbp != NULL)
            blockPos = *dbp;
        if (!FindBlockPos(state, blockPos, nBlockSize + 8, nHeight, block.GetBlockTime(), dbp != NULL))
            return error("AcceptBlock() : FindBlockPos failed");
        if (dbp == NULL)
            if (!WriteBlockToDisk(block, blockPos))
                return state.Abort("Failed to write block");
        if (!ReceivedBlockTransactions(block, state, pindex, blockPos, NULL))
            return error("AcceptBlock() : ReceivedBlockTransactions failed");
    } catch (const std::runtime_error& e) {
        return state.Abort(std::string("System error: ") + e.what());
    }

    return true;
}

/**
 * Metrix perform POS checks on block during connect tip
 * to ensure we have all the transactions and prev blocks
 */
bool UpdateHashProof(CBlock& block, CValidationState& state, CBlockIndex* pindex)
{
    CBlockIndex* pindexPrev = pindex->pprev;
    uint256 hash = pindex->GetBlockHash();

    if (hash != Params().HashGenesisBlock())
    {
        uint256 hashProof;
        //! Check proof-of-work or proof-of-stake
        if (block.nBits != GetNextTargetRequired(pindexPrev, block.IsProofOfStake()))
            return state.DoS(100, error("UpdateHashProof() : incorrect %s", block.IsProofOfWork() ? "proof-of-work" : "proof-of-stake"),
                             REJECT_INVALID, "incorrect pow/pos");

        //! Verify hash target and signature of coinstake tx
        if (block.IsProofOfStake()) {
            uint256 targetProofOfStake;
            if (!CheckProofOfStake(state, pindexPrev, block.vtx[1], block.nBits, hashProof, targetProofOfStake))
                return state.Invalid(error("UpdateHashProof() : check proof-of-stake failed for block %s", hash.ToString()),
                                     REJECT_INVALID, "pos check failed");
        }
        //! PoW is checked in CheckBlock()
        //! Metrix adds POW block hashes to hash proof when confirming POS blocks
        if (block.IsProofOfWork())
            hashProof = block.GetPoWHash();
            
        pindex->hashProof = hashProof;
    }

    if (IsSuperMajority(8, pindex->pprev, Params().EnforceBlockUpgradeMajority()))
    {
        // compute v2 stake modifier
        pindex->nStakeModifierV2 = ComputeStakeModifier(pindex->pprev,block.IsProofOfWork() ? hash : block.vtx[1].vin[0].prevout.hash);
    }
    else
    {
        //! ppcoin: compute stake modifier
        uint64_t nStakeModifier = 0;
        bool fGeneratedStakeModifier = false;
        if (!ComputeNextStakeModifier(pindexPrev, nStakeModifier, fGeneratedStakeModifier))
            return error("UpdateHashProof() : ComputeNextStakeModifier() failed");
        pindex->SetStakeModifier(nStakeModifier, fGeneratedStakeModifier);
    } 

    return true;
}

uint256 CBlockIndex::GetBlockTrust() const
{
    uint256 bnTarget;
    bool fNegative;
    bool fOverflow;
    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);
    if (fNegative || fOverflow || bnTarget == 0)
        return 0;
    /**
     * We need to compute 2**256 / (bnTarget+1), but we can't represent 2**256
     * as it's too large for a arith_uint256. However, as 2**256 is at least as large
     * as bnTarget+1, it is equal to ((2**256 - bnTarget - 1) / (bnTarget+1)) + 1,
     * or ~bnTarget / (nTarget+1) + 1.
     */
    return (~bnTarget / (bnTarget + 1)) + 1;
}

bool IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired)
{
    unsigned int nToCheck = Params().ToCheckBlockUpgradeMajority();
    unsigned int nFound = 0;
    for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; i++) {
        if (pstart->nVersion >= minVersion)
            ++nFound;
        pstart = pstart->pprev;
    }
    return (nFound >= nRequired);
}

void PushGetBlocks(CNode* pnode, CBlockIndex* pindexBegin, uint256 hashEnd)
{
    AssertLockHeld(cs_main);
    // Filter out duplicate requests
    if (pindexBegin == pnode->pindexLastGetBlocksBegin && hashEnd == pnode->hashLastGetBlocksEnd)
        return;
    pnode->pindexLastGetBlocksBegin = pindexBegin;
    pnode->hashLastGetBlocksEnd = hashEnd;

    pnode->PushMessage("getblocks", chainActive.GetLocator(pindexBegin), hashEnd);
}

bool static ReserealizeBlockSignature(CBlock* pblock)
{
    if (pblock->IsProofOfWork()) {
        pblock->vchBlockSig.clear();
        return true;
    }

    return CKey::ReserealizeSignature(pblock->vchBlockSig);
}

bool static IsCanonicalBlockSignature(CBlock* pblock)
{
    if (pblock->IsProofOfWork()) {
        return pblock->vchBlockSig.empty();
    }

    return IsValidSignatureEncoding(pblock->vchBlockSig);
}

uint256 static GetOrphanRoot(const uint256& hash)
{
    map<uint256, COrphanBlock*>::iterator it = mapOrphanBlocks.find(hash);
    if (it == mapOrphanBlocks.end())
        return hash;
	
    // Work back to the first block in the orphan chain
    do {
        map<uint256, COrphanBlock*>::iterator it2 = mapOrphanBlocks.find(it->second->hashPrev);
        if (it2 == mapOrphanBlocks.end())
            return it->first;
        it = it2;
    } while(true);
}

uint256 WantedByOrphan(const COrphanBlock* pblockOrphan)
{
    while (mapOrphanBlocks.count(pblockOrphan->hashPrev))
        pblockOrphan = mapOrphanBlocks[pblockOrphan->hashPrev];
    return pblockOrphan->hashPrev;
}

// Remove a random orphan block (which does not have any dependent orphans).
void static PruneOrphanBlocks()
{
    size_t nMaxOrphanBlocksSize = GetArg("-maxorphanblocks", DEFAULT_MAX_ORPHAN_BLOCKS) * ((size_t) 1 << 20);
    while (nOrphanBlocksSize > nMaxOrphanBlocksSize)
    {
        // Pick a random orphan block.
        uint256 randomhash = GetRandHash();
        std::multimap<uint256, COrphanBlock*>::iterator it = mapOrphanBlocksByPrev.lower_bound(randomhash);
        if (it == mapOrphanBlocksByPrev.end())
            it = mapOrphanBlocksByPrev.begin();

        // As long as this block has other orphans depending on it, move to one of those successors.
        do {
            std::multimap<uint256, COrphanBlock*>::iterator it2 = mapOrphanBlocksByPrev.find(it->second->hashBlock);
            if (it2 == mapOrphanBlocksByPrev.end())
                break;
            it = it2;
        } while(1);

        setStakeSeenOrphan.erase(it->second->stake);
        uint256 hash = it->second->hashBlock;
        nOrphanBlocksSize -= it->second->vchBlock.size();
        delete it->second;
        mapOrphanBlocksByPrev.erase(it);
        mapOrphanBlocks.erase(hash);
    }
}

bool ProcessNewBlock(CValidationState& state, CNode* pfrom, CBlock* pblock, CDiskBlockPos* dbp)
{

    // Check for duplicate orphan block
    // Duplicate stake allowed only when there is orphan child block
    // if the block header is already known, allow it (to account for headers being sent before the block itself)
    uint256 hash = pblock->GetHash();
    if (!fReindex && !fImporting && pblock->IsProofOfStake() && setStakeSeen.count(pblock->GetProofOfStake()) > 1 && !mapOrphanBlocksByPrev.count(hash))
        return error("ProcessNewBlock() : duplicate proof-of-stake (%s, %d) for block %s", pblock->GetProofOfStake().first.ToString(), pblock->GetProofOfStake().second, hash.ToString());


    if (mapOrphanBlocks.count(hash))
        return error("ProcessNewBlock() : already have block (orphan) %s", hash.ToString());

    //! Preliminary checks
    bool checked = CheckBlock(*pblock, state);

    //! Block signature can be malleated in such a way that it increases block size up to maximum allowed by protocol
    //! For now we just strip garbage from newly received blocks
    if (!IsCanonicalBlockSignature(pblock)) {
        if (!ReserealizeBlockSignature(pblock))
            LogPrintf("WARNING: ProcesNewBlock() : ReserealizeBlockSignature FAILED\n");
    }

    LOCK(cs_main);
    MarkBlockAsReceived(pblock->GetHash());
    if (!checked) {
        return error("%s : CheckBlock FAILED", __func__);
    }

    // If we don't already have its previous block, shunt it off to holding area until we get it
    if (!mapBlockIndex.count(pblock->hashPrevBlock))
    {
        LogPrintf("ProcessNewBlock: ORPHAN BLOCK %lu, prev=%s\n", (unsigned long)mapOrphanBlocks.size(), pblock->hashPrevBlock.ToString());

        // Accept orphans as long as there is a node to request its parents from
        if (pfrom) {
            //! ppcoin: check proof-of-stake
            if (pblock->IsProofOfStake())
            {
                /**
                 * Limited duplicity on stake: prevents block flood attack
                 * Duplicate stake allowed only when there is orphan child block
                 */
                if (setStakeSeenOrphan.count(pblock->GetProofOfStake()) && !mapOrphanBlocksByPrev.count(hash))
                    return state.Invalid(error("ProcessNewBlock() : duplicate proof-of-stake (%s, %d) for orphan block %s", pblock->GetProofOfStake().first.ToString(), pblock->GetProofOfStake().second, hash.ToString()));
            }

            PruneOrphanBlocks();
            COrphanBlock* pblock2 = new COrphanBlock();
            {
                CDataStream ss(SER_DISK, CLIENT_VERSION);
                ss << *pblock;
                pblock2->vchBlock = std::vector<unsigned char>(ss.begin(), ss.end());
            }
            pblock2->hashBlock = hash;
            pblock2->hashPrev = pblock->hashPrevBlock;
            nOrphanBlocksSize += pblock2->vchBlock.size();
            mapOrphanBlocks.insert(std::make_pair(hash, pblock2));
            mapOrphanBlocksByPrev.insert(std::make_pair(pblock2->hashPrev, pblock2));
            if (pblock->IsProofOfStake())
                setStakeSeenOrphan.insert(pblock->GetProofOfStake());

            // Ask this guy to fill in what we're missing
            PushGetBlocks(pfrom, pindexBestHeader, GetOrphanRoot(hash));
            // ppcoin: getblocks may not obtain the ancestor block rejected
            // earlier by duplicate-stake check so we ask for it again directly
            if (!IsInitialBlockDownload())
                pfrom->AskFor(CInv(MSG_BLOCK, WantedByOrphan(pblock2)));
        }
        return true;
    }
    
    //! Store to disk
    CBlockIndex *pindex = NULL;
    bool ret = AcceptBlock(*pblock, state, &pindex, dbp);
    if (pindex && pfrom) {
        mapBlockSource[pindex->GetBlockHash()] = pfrom->GetId();
    }
    CheckBlockIndex();
    if (!ret)
        return error("%s : AcceptBlock FAILED", __func__);

    // Recursively process any orphan blocks that depended on this one
    vector<uint256> vWorkQueue;
    vWorkQueue.push_back(hash);
    for (unsigned int i = 0; i < vWorkQueue.size(); i++)
    {
        uint256 hashPrev = vWorkQueue[i];
        for (multimap<uint256, COrphanBlock*>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
            mi != mapOrphanBlocksByPrev.upper_bound(hashPrev);
            ++mi)
        {
            CBlock block;
            {
                CDataStream ss(mi->second->vchBlock, SER_DISK, CLIENT_VERSION);
                ss >> block;
            }
            block.BuildMerkleTree();
            // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan resolution (that is, feeding people an invalid block based on LegitBlockX in order to get anyone relaying LegitBlockX banned)
            CValidationState stateDummy;
            if (AcceptBlock(block, stateDummy, &pindex))
                vWorkQueue.push_back(mi->second->hashBlock);
            mapOrphanBlocks.erase(mi->second->hashBlock);

            if (block.IsProofOfStake())
                setStakeSeenOrphan.erase(block.GetProofOfStake());

            delete mi->second;
        }
        mapOrphanBlocksByPrev.erase(hashPrev);
    }

    if (!ActivateBestChain(state, pblock))
        return error("%s : ActivateBestChain failed", __func__);

    return true;
}

bool AbortNode(const std::string &strMessage, const std::string &userMessage)
{
    fRequestShutdown = true;
    strMiscWarning = strMessage;
    LogPrintf("*** %s\n", strMessage.c_str());
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occured, see debug.log for details") : userMessage,
        "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

//!#ifdef ENABLE_WALLET
//! Metrix: attempt to generate suitable proof-of-stake
bool SignBlock(CBlock& block, CWallet& wallet, CAmount nFees)
{
    //! if we are trying to sign
    //!    something except proof-of-stake block template
    if (!block.vtx[0].vout[0].IsEmpty())
        return false;

    //! if we are trying to sign
    //!    a complete proof-of-stake block
    if (block.IsProofOfStake())
        return true;

    CKey key;
    CMutableTransaction txCoinStake;
    CTransaction txNew;
    txCoinStake.nTime &= ~STAKE_TIMESTAMP_MASK;

    int64_t nSearchTime = txCoinStake.nTime; //! search to current time
    if (fDebug)
        LogPrintf("DEBUG : nSearchTime=%i, nLastCoinStakeSearchTime=%i\n", nSearchTime, nLastCoinStakeSearchTime);
    if (nSearchTime > nLastCoinStakeSearchTime) {
        if (wallet.CreateCoinStake(wallet, block.nBits, nSearchTime-nLastCoinStakeSearchTime, nFees, txCoinStake, key)) {
            if (txCoinStake.nTime >= chainActive.Tip()->GetPastTimeLimit() + 1) {
                //! make sure coinstake would meet timestamp protocol
                //!    as it would be the same as the block timestamp
                block.vtx[0].nTime = block.nTime = txCoinStake.nTime;

                //! we have to make sure that we have no future timestamps in
                //!    our transactions set
                for (vector<CTransaction>::iterator it = block.vtx.begin(); it != block.vtx.end();)
                    if (it->nTime > block.nTime) {
                        it = block.vtx.erase(it);
                    } else {
                        ++it;
                    }

                *static_cast<CTransaction*>(&txNew) = CTransaction(txCoinStake);
                block.vtx.insert(block.vtx.begin() + 1, txCoinStake);
                block.hashMerkleRoot = block.BuildMerkleTree();

                //! append a signature to our block
                return key.Sign(block.GetHash(), block.vchBlockSig);
            }
        }
        nLastCoinStakeSearchInterval = nSearchTime - nLastCoinStakeSearchTime;
        nLastCoinStakeSearchTime = nSearchTime;
    }

    return false;
}
//!#endif

bool CheckBlockSignature(const CBlock& block)
{
    if (block.IsProofOfWork())
        return block.vchBlockSig.empty();

    if (block.vchBlockSig.empty())
        return false;

    vector<valtype> vSolutions;
    txnouttype whichType;

    const CTxOut& txout = block.vtx[1].vout[1];

    if (!Solver(txout.scriptPubKey, whichType, vSolutions))
        return false;

    if (whichType == TX_PUBKEY) {
        valtype& vchPubKey = vSolutions[0];
        return CPubKey(vchPubKey).Verify(block.GetHash(), block.vchBlockSig);
    }

    return false;
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
    uint64_t nFreeBytesAvailable = boost::filesystem::space(GetDataDir()).available;

    //! Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
        return AbortNode("Disk space is low!", _("Error: Disk space is low!"));
    return true;
}

FILE* OpenDiskFile(const CDiskBlockPos& pos, const char* prefix, bool fReadOnly)
{
    if (pos.IsNull())
        return NULL;
    boost::filesystem::path path = GetBlockPosFilename(pos, prefix);
    boost::filesystem::create_directories(path.parent_path());
    FILE* file = fopen(path.string().c_str(), "rb+");
    if (!file && !fReadOnly)
        file = fopen(path.string().c_str(), "wb+");
    if (!file) {
        LogPrintf("Unable to open file %s\n", path.string().c_str());
        return NULL;
    }
    if (pos.nPos) {
        if (fseek(file, pos.nPos, SEEK_SET)) {
            LogPrintf("Unable to seek to position %u of %s\n", pos.nPos, path.string().c_str());
            fclose(file);
            return NULL;
        }
    }
    return file;
}

FILE* OpenBlockFile(const CDiskBlockPos& pos, bool fReadOnly)
{
    return OpenDiskFile(pos, "blk", fReadOnly);
}

FILE* OpenUndoFile(const CDiskBlockPos& pos, bool fReadOnly)
{
    return OpenDiskFile(pos, "rev", fReadOnly);
}

boost::filesystem::path GetBlockPosFilename(const CDiskBlockPos& pos, const char* prefix)
{
    return GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, pos.nFile);
}

CBlockIndex* InsertBlockIndex(uint256 hash)
{
    if (hash == 0)
        return NULL;

    //! Return existing
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    //! Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw runtime_error("LoadBlockIndex() : new CBlockIndex failed");
    mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

bool static LoadBlockIndexDB()
{
    if (!pblocktree->LoadBlockIndexGuts())
        return false;

    boost::this_thread::interruption_point();

    //! Calculate nChainTrust
    vector<pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    BOOST_FOREACH (const PAIRTYPE(uint256, CBlockIndex*) & item, mapBlockIndex) {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    BOOST_FOREACH (const PAIRTYPE(int, CBlockIndex*) & item, vSortedByHeight) {
        CBlockIndex* pindex = item.second;
        pindex->nChainTrust = (pindex->pprev ? pindex->pprev->nChainTrust : 0) + pindex->GetBlockTrust();
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
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
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && (pindex->nChainTx || pindex->pprev == NULL))
            setBlockIndexCandidates.insert(pindex);
        if (pindex->nStatus & BLOCK_FAILED_MASK && (!pindexBestInvalid || pindex->nChainTrust > pindexBestInvalid->nChainTrust))
            pindexBestInvalid = pindex;
        if (pindex->pprev)
            pindex->BuildSkip();
        if (pindex->IsValid(BLOCK_VALID_TREE) && (pindexBestHeader == NULL || CBlockIndexWorkComparator()(pindexBestHeader, pindex)))
            pindexBestHeader = pindex;
    }

    //! Load block file info
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
    //! Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    set<int> setBlkDataFiles;
    BOOST_FOREACH (const PAIRTYPE(uint256, CBlockIndex*) & item, mapBlockIndex) {
        CBlockIndex* pindex = item.second;
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(pindex->nFile);
        }
    }
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++) {
        CDiskBlockPos pos(*it, 0);
        if (!CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION)) {
            return false;
        }
    }

    //! Check whether we need to continue reindexing
    bool fReindexing = false;
    pblocktree->ReadReindexing(fReindexing);
    fReindex |= fReindexing;

    //! Load pointer to end of best chain
    BlockMap::iterator it = mapBlockIndex.find(pcoinsTip->GetBestBlock());
    if (it == mapBlockIndex.end())
        return true;
    chainActive.SetTip(it->second);

    LogPrintf("LoadBlockIndex(): hashBestChain=%s  height=%d date=%s  progress=%f\n",
              chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(),
              DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()),
              Checkpoints::GuessVerificationProgress(chainActive.Tip()));
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

bool CVerifyDB::VerifyDB(CCoinsView* coinsview, int nCheckLevel, int nCheckDepth)
{
    LOCK(cs_main);
    if (chainActive.Tip() == NULL || chainActive.Tip()->pprev == NULL)
        return true;

    //! Verify blocks in the best chain
    if (nCheckDepth <= 0)
        nCheckDepth = 1000000000; //! suffices until the year 19000
    if (nCheckDepth > chainActive.Height())
        nCheckDepth = chainActive.Height();
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    LogPrintf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
    CCoinsViewCache coins(coinsview);
    CBlockIndex* pindexState = chainActive.Tip();
    CBlockIndex* pindexFailure = NULL;
    int nGoodTransactions = 0;
    CValidationState state;
    for (CBlockIndex* pindex = chainActive.Tip(); pindex && pindex->pprev; pindex = pindex->pprev) {
        boost::this_thread::interruption_point();
        uiInterface.ShowProgress(_("Verifying blocks..."), std::max(1, std::min(99, (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * (nCheckLevel >= 4 ? 50 : 100)))));
        if (pindex->nHeight < chainActive.Height() - nCheckDepth)
            break;
        CBlock block;
        //! check level 0: read from disk
        if (!ReadBlockFromDisk(block, pindex))
            return error("VerifyDB() : *** block.ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
        //! check level 1: verify block validity
        if (nCheckLevel >= 1 && !CheckBlock(block, state))
            return error("VerifyDB() : *** found bad block at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
        //! check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            CDiskBlockPos pos = pindex->GetUndoPos();
            if (!pos.IsNull()) {
                if (!UndoReadFromDisk(undo, pos, pindex->pprev->GetBlockHash()))
                    return error("VerifyDB() : *** found bad undo data at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
            }
        }
        //! check level 3: check for inconsistencies during memory-only disconnect of tip blocks
        if (nCheckLevel >= 3 && pindex == pindexState && (coins.GetCacheSize() + pcoinsTip->GetCacheSize()) <= nCoinCacheSize) {
            bool fClean = true;
            if (!DisconnectBlock(block, state, pindex, coins, &fClean))
                return error("VerifyDB() : *** irrecoverable inconsistency in block data at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
            pindexState = pindex->pprev;
            if (!fClean) {
                nGoodTransactions = 0;
                pindexFailure = pindex;
            } else
                nGoodTransactions += block.vtx.size();
        }
        if (ShutdownRequested())
            return true;
    }
    if (pindexFailure)
        return error("VerifyDB() : *** coin database inconsistencies found (last %i blocks, %i good transactions before that)\n", chainActive.Height() - pindexFailure->nHeight + 1, nGoodTransactions);

    //! check level 4: try reconnecting blocks
    if (nCheckLevel >= 4) {
        CBlockIndex* pindex = pindexState;
        while (pindex != chainActive.Tip()) {
            boost::this_thread::interruption_point();
            uiInterface.ShowProgress(_("Verifying blocks..."), std::max(1, std::min(99, 100 - (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * 50))));
            pindex = chainActive.Next(pindex);
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex))
                return error("VerifyDB() : *** ReadblockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
            if (!ConnectBlock(block, state, pindex, coins))
                return error("VerifyDB() : *** found unconnectable block at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString().c_str());
        }
    }

    LogPrintf("No coin database inconsistencies in last %i blocks (%i transactions)\n", chainActive.Height() - pindexState->nHeight, nGoodTransactions);

    return true;
}

void UnloadBlockIndex()
{
    mapBlockIndex.clear();
    setBlockIndexCandidates.clear();
    chainActive.SetTip(NULL);
    pindexBestInvalid = NULL;
}

bool LoadBlockIndex()
{
    LOCK(cs_main);

    if (Params().TestnetToBeDeprecatedFieldRPC()) {
        nStakeMinAge = 1 * 60 * 60; //! test net min age is 1 hour
        nCoinbaseMaturity = 10;     //! test maturity is 10 blocks
    }

    /**
     * Load block index from databases
     */
    if (!fReindex && !LoadBlockIndexDB())
        return false;

    return true;
}

bool InitBlockIndex()
{
    LOCK(cs_main);
    //! Check whether we're already initialized
    if (chainActive.Genesis() != NULL)
        return true;

    //! Use the provided setting for -txindex in the new database
    pblocktree->WriteFlag("txindex", true); //!we need to tx index to lookup transactions for POS
    LogPrintf("Initializing databases...\n");

    //! Only add the genesis block if not reindexing (in which case we reuse the one already on disk)
    if (!fReindex) {
        CBlock& block = const_cast<CBlock&>(Params().GenesisBlock());
        //! Start new block file
        try {
            unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
            CDiskBlockPos blockPos;
            CValidationState state;
            if (!FindBlockPos(state, blockPos, nBlockSize + 8, 0, block.GetBlockTime()))
                return error("AcceptBlock() : FindBlockPos failed");
            if (!WriteBlockToDisk(block, blockPos))
                return error("LoadBlockIndex() : writing genesis block to disk failed");
            CBlockIndex* pindex = AddToBlockIndex(block);
            if (!ReceivedBlockTransactions(block, state, pindex, blockPos, Params().HashGenesisBlock()))
                return error("LoadBlockIndex() : genesis block not accepted");
            if (!ActivateBestChain(state, &block))
                return error("LoadBlockIndex() : genesis block cannot be activated");
            //! Force a chainstate write so that when we VerifyDB in a moment, it doesn't check stale data
            return FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
        } catch (const std::runtime_error& e) {
            return error("LoadBlockIndex() : failed to initialize block database: %s", e.what());
        }
    }

    return true;
}

bool LoadExternalBlockFile(FILE* fileIn, CDiskBlockPos* dbp)
{
    //! Map of disk positions for blocks with unknown parent (only used for reindex)
    static std::multimap<uint256, CDiskBlockPos> mapBlocksUnknownParent;
    uint64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    try {
        //! This takes over fileIn and calls fclose() on it in the CBufferedFile destructor
        CBufferedFile blkdat(fileIn, 2 * MAX_BLOCK_SIZE, MAX_BLOCK_SIZE + 8, SER_DISK, CLIENT_VERSION);
        uint64_t nRewind = blkdat.GetPos();
        while (!blkdat.eof()) {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            nRewind++;         //! start one byte further next time, in case of failure
            blkdat.SetLimit(); //! remove former limit
            unsigned int nSize = 0;
            try {
                //! locate a header
                unsigned char buf[MESSAGE_START_SIZE];
                blkdat.FindByte(Params().MessageStart()[0]);
                nRewind = blkdat.GetPos() + 1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, Params().MessageStart(), MESSAGE_START_SIZE))
                    continue;
                //! read size
                blkdat >> nSize;
                if (nSize < 80 || nSize > MAX_BLOCK_SIZE)
                    continue;
            } catch (const std::exception&) {
                //! no valid block header found; don't complain
                break;
            }
            try {
                //! read block
                uint64_t nBlockPos = blkdat.GetPos();
                if (dbp)
                    dbp->nPos = nBlockPos;
                blkdat.SetLimit(nBlockPos + nSize);
                blkdat.SetPos(nBlockPos);
                CBlock block;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                //! detect out of order blocks, and store them for later
                uint256 hash = block.GetHash();
                if (hash != Params().HashGenesisBlock() && mapBlockIndex.find(block.hashPrevBlock) == mapBlockIndex.end()) {
                    LogPrint("reindex", "%s: Out of order block %s, parent %s not known\n", __func__, hash.ToString(),
                            block.hashPrevBlock.ToString());
                    if (dbp)
                        mapBlocksUnknownParent.insert(std::make_pair(block.hashPrevBlock, *dbp));
                    continue;
                }
                //! process in case the block isn't known yet
                if (mapBlockIndex.count(hash) == 0 || (mapBlockIndex[hash]->nStatus & BLOCK_HAVE_DATA) == 0) {
                    CValidationState state;
                    if (ProcessNewBlock(state, NULL, &block, dbp))
                        nLoaded++;
                    if (state.IsError())
                        break;
                }

                //! Recursively process earlier encountered successors of this block
                deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty()) {
                    uint256 head = queue.front();
                    queue.pop_front();
                    std::pair<std::multimap<uint256, CDiskBlockPos>::iterator, std::multimap<uint256, CDiskBlockPos>::iterator> range = mapBlocksUnknownParent.equal_range(head);
                    while (range.first != range.second) {
                        std::multimap<uint256, CDiskBlockPos>::iterator it = range.first;
                        if (ReadBlockFromDisk(block, it->second))
                        {
                            LogPrintf("%s: Processing out of order child %s of %s\n", __func__, block.GetHash().ToString(),
                                    head.ToString());
                            CValidationState dummy;
                            if (ProcessNewBlock(dummy, NULL, &block, &it->second))
                            {
                                nLoaded++;
                                queue.push_back(block.GetHash());
                            }
                        }
                        range.first++;
                        mapBlocksUnknownParent.erase(it);
                    }
                }
            } catch (const std::exception& e) {
                LogPrintf("%s() : Deserialize or I/O error caught during load:%s\n", __func__, e.what());
            }
        }
    } catch (const std::runtime_error& e) {
        AbortNode(std::string("System error: ") + e.what());
    }
    if (nLoaded > 0)
        LogPrintf("Loaded %i blocks from external file in %15dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

void static CheckBlockIndex()
{
    if (!fCheckBlockIndex) {
        return;
    }

    LOCK(cs_main);

    //! During a reindex, we read the genesis block and call CheckBlockIndex before ActivateBestChain,
    //! so we have the genesis block in mapBlockIndex but no active chain.  (A few of the tests when
    //! iterating the block tree require that chainActive has been initialized.)
    if (chainActive.Height() < 0) {
        assert(mapBlockIndex.size() <= 1);
        return;
    }

    //! Build forward-pointing map of the entire block tree.
    std::multimap<CBlockIndex*,CBlockIndex*> forward;
    for (BlockMap::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); it++) {
        forward.insert(std::make_pair(it->second->pprev, it->second));
    }

    assert(forward.size() == mapBlockIndex.size());

    std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> rangeGenesis = forward.equal_range(NULL);
    CBlockIndex *pindex = rangeGenesis.first->second;
    rangeGenesis.first++;
    assert(rangeGenesis.first == rangeGenesis.second); //! There is only one index entry with parent NULL.

    //! Iterate over the entire block tree, using depth-first search.
    //! Along the way, remember whether there are blocks on the path from genesis
    //! block being explored which are the first to have certain properties.
    size_t nNodes = 0;
    int nHeight = 0;
    CBlockIndex* pindexFirstInvalid = NULL; //! Oldest ancestor of pindex which is invalid.
    CBlockIndex* pindexFirstMissing = NULL; //! Oldest ancestor of pindex which does not have BLOCK_HAVE_DATA.
    CBlockIndex* pindexFirstNotTreeValid = NULL; //! Oldest ancestor of pindex which does not have BLOCK_VALID_TREE (regardless of being valid or not).
    CBlockIndex* pindexFirstNotChainValid = NULL; //! Oldest ancestor of pindex which does not have BLOCK_VALID_CHAIN (regardless of being valid or not).
    CBlockIndex* pindexFirstNotScriptsValid = NULL; //! Oldest ancestor of pindex which does not have BLOCK_VALID_SCRIPTS (regardless of being valid or not).
    while (pindex != NULL) {
        nNodes++;
        if (pindexFirstInvalid == NULL && pindex->nStatus & BLOCK_FAILED_VALID) pindexFirstInvalid = pindex;
        if (pindexFirstMissing == NULL && !(pindex->nStatus & BLOCK_HAVE_DATA)) pindexFirstMissing = pindex;
        if (pindex->pprev != NULL && pindexFirstNotTreeValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE) pindexFirstNotTreeValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotChainValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_CHAIN) pindexFirstNotChainValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotScriptsValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS) pindexFirstNotScriptsValid = pindex;

        //! Begin: actual consistency checks.
        if (pindex->pprev == NULL) {
            //! Genesis block checks.
            assert(pindex->GetBlockHash() == Params().HashGenesisBlock()); //! Genesis block's hash must match.
            assert(pindex == chainActive.Genesis()); //! The current active chain's genesis block must be this block.
        }
        //! HAVE_DATA is equivalent to VALID_TRANSACTIONS and equivalent to nTx > 0 (we stored the number of transactions in the block)
        assert(!(pindex->nStatus & BLOCK_HAVE_DATA) == (pindex->nTx == 0));
        assert(((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TRANSACTIONS) == (pindex->nTx > 0));
        if (pindex->nChainTx == 0) assert(pindex->nSequenceId == 0);  //! nSequenceId can't be set for blocks that aren't linked
        //! All parents having data is equivalent to all parents being VALID_TRANSACTIONS, which is equivalent to nChainTx being set.
        assert((pindexFirstMissing != NULL) == (pindex->nChainTx == 0)); //! nChainTx == 0 is used to signal that all parent block's transaction data is available.
        assert(pindex->nHeight == nHeight); //! nHeight must be consistent.
        assert(pindex->pprev == NULL || pindex->nChainTrust >= pindex->pprev->nChainTrust); //! For every block except the genesis block, the chainwork must be larger than the parent's.
        assert(nHeight < 2 || (pindex->pskip && (pindex->pskip->nHeight < nHeight))); //! The pskip pointer must point back for all but the first 2 blocks.
        assert(pindexFirstNotTreeValid == NULL); //! All mapBlockIndex entries must at least be TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TREE) assert(pindexFirstNotTreeValid == NULL); //! TREE valid implies all parents are TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_CHAIN) assert(pindexFirstNotChainValid == NULL); //! CHAIN valid implies all parents are CHAIN valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_SCRIPTS) assert(pindexFirstNotScriptsValid == NULL); //! SCRIPTS valid implies all parents are SCRIPTS valid
        if (pindexFirstInvalid == NULL) {
            //! Checks for not-invalid blocks.
            assert((pindex->nStatus & BLOCK_FAILED_MASK) == 0); //! The failed mask cannot be set for blocks without invalid parents.
        }
        if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) && pindexFirstMissing == NULL) {
            if (pindexFirstInvalid == NULL) { //! If this block sorts at least as good as the current tip and is valid, it must be in setBlockIndexCandidates.
                 assert(setBlockIndexCandidates.count(pindex));
            }
        } else { //! If this block sorts worse than the current tip, it cannot be in setBlockIndexCandidates.
            assert(setBlockIndexCandidates.count(pindex) == 0);
        }
        //! Check whether this block is in mapBlocksUnlinked.
        std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> rangeUnlinked = mapBlocksUnlinked.equal_range(pindex->pprev);
        bool foundInUnlinked = false;
        while (rangeUnlinked.first != rangeUnlinked.second) {
            assert(rangeUnlinked.first->first == pindex->pprev);
            if (rangeUnlinked.first->second == pindex) {
                foundInUnlinked = true;
                break;
            }
            rangeUnlinked.first++;
        }
        if (pindex->pprev && pindex->nStatus & BLOCK_HAVE_DATA && pindexFirstMissing != NULL) {
            if (pindexFirstInvalid == NULL) { //! If this block has block data available, some parent doesn't, and has no invalid parents, it must be in mapBlocksUnlinked.
                assert(foundInUnlinked);
            }
        } else { //! If this block does not have block data available, or all parents do, it cannot be in mapBlocksUnlinked.
            assert(!foundInUnlinked);
        }
        //! assert(pindex->GetBlockHash() == pindex->GetBlockHeader().GetHash()); // Perhaps too slow
        //! End: actual consistency checks.

        //! Try descending into the first subnode.
        std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> range = forward.equal_range(pindex);
        if (range.first != range.second) {
            //! A subnode was found.
            pindex = range.first->second;
            nHeight++;
            continue;
        }
        //! This is a leaf node.
        //! Move upwards until we reach a node of which we have not yet visited the last child.
        while (pindex) {
            //! We are going to either move to a parent or a sibling of pindex.
            //! If pindex was the first with a certain property, unset the corresponding variable.
            if (pindex == pindexFirstInvalid) pindexFirstInvalid = NULL;
            if (pindex == pindexFirstMissing) pindexFirstMissing = NULL;
            if (pindex == pindexFirstNotTreeValid) pindexFirstNotTreeValid = NULL;
            if (pindex == pindexFirstNotChainValid) pindexFirstNotChainValid = NULL;
            if (pindex == pindexFirstNotScriptsValid) pindexFirstNotScriptsValid = NULL;
            // Find our parent.
            CBlockIndex* pindexPar = pindex->pprev;
            //! Find which child we just visited.
            std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> rangePar = forward.equal_range(pindexPar);
            while (rangePar.first->second != pindex) {
                assert(rangePar.first != rangePar.second); //! Our parent must have at least the node we're coming from as child.
                rangePar.first++;
            }
            //! Proceed to the next one.
            rangePar.first++;
            if (rangePar.first != rangePar.second) {
                //! Move to the sibling.
                pindex = rangePar.first->second;
                break;
            } else {
                //! Move up further.
                pindex = pindexPar;
                nHeight--;
                continue;
            }
        }
    }

    //! Check that we actually traversed the entire map.
    assert(nNodes == forward.size());
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


//////////////////////////////////////////////////////////////////////////////
/**
 * CAlert
 */

extern map<uint256, CAlert> mapAlerts;
extern CCriticalSection cs_mapAlerts;

string GetWarnings(string strFor)
{
    int nPriority = 0;
    string strStatusBar;
    string strRPC;

    if (!CLIENT_VERSION_IS_RELEASE)
        strStatusBar = _("This is a pre-release test build - use at your own risk - do not use for mining or merchant applications");

    if (GetBoolArg("-testsafemode", false))
        strStatusBar = strRPC = "testsafemode enabled";

    //! Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "") {
        nPriority = 1000;
        strStatusBar = strMiscWarning;
    }

    //! Longer invalid proof-of-work/proof-of-stake chain
    if (fLargeWorkForkFound) {
        nPriority = 2000;
        strStatusBar = strRPC = _("Warning: The network does not appear to fully agree! Some peers appear to be experiencing issues.");
    } else if (fLargeWorkInvalidChainFound) {
        nPriority = 2000;
        strStatusBar = strRPC = _("Warning: We do not appear to fully agree with our peers! You may need to upgrade, or other nodes may need to upgrade.");
    }
    //! Alerts
    {
        LOCK(cs_mapAlerts);
        BOOST_FOREACH (PAIRTYPE(const uint256, CAlert) & item, mapAlerts) {
            const CAlert& alert = item.second;
            if (alert.AppliesToMe() && alert.nPriority > nPriority) {
                nPriority = alert.nPriority;
                strStatusBar = alert.strStatusBar;
                if (nPriority > 1000)
                    strRPC = strStatusBar;
            }
        }
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings() : invalid parameter");
    return "error";
}


//////////////////////////////////////////////////////////////////////////////
/**
 * Messages
 */


bool static AlreadyHave(const CInv& inv)
{
    switch (inv.type) {
    case MSG_TX: {
        bool txInMap = false;
        txInMap = mempool.exists(inv.hash);
        return txInMap ||
               mapOrphanTransactions.count(inv.hash) ||
               pcoinsTip->HaveCoins(inv.hash);
    }

    case MSG_BLOCK:
        return mapBlockIndex.count(inv.hash);
    case MSG_TXLOCK_REQUEST:
        return mapTxLockReq.count(inv.hash) ||
               mapTxLockReqRejected.count(inv.hash);
    case MSG_TXLOCK_VOTE:
        return mapTxLockVote.count(inv.hash);
    case MSG_SPORK:
        return mapSporks.count(inv.hash);
    case MSG_MASTERNODE_WINNER:
        return mapSeenMasternodeVotes.count(inv.hash);
    }
    //! Don't know what it is, just say we already got one
    return true;
}


void Misbehaving(NodeId pnode, int howmuch)
{
    if (howmuch == 0)
        return;
    CNodeState* state = State(pnode);
    if (state == NULL)
        return;
    state->nMisbehavior += howmuch;
    int banscore = GetArg("-banscore", 100);
    if (state->nMisbehavior >= banscore && state->nMisbehavior - howmuch < banscore) {
        LogPrintf("Misbehaving: %s (%d -> %d) BAN THRESHOLD EXCEEDED\n", state->name.c_str(), state->nMisbehavior - howmuch, state->nMisbehavior);
        state->fShouldBan = true;
    } else
        LogPrintf("Misbehaving: %s (%d -> %d)\n", state->name.c_str(), state->nMisbehavior - howmuch, state->nMisbehavior);
}


void static ProcessGetData(CNode* pfrom)
{
    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();

    vector<CInv> vNotFound;

    LOCK(cs_main);

    while (it != pfrom->vRecvGetData.end()) {
        //! Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        const CInv& inv = *it;
        {
            boost::this_thread::interruption_point();
            it++;

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK) {
                bool send = false;
                BlockMap::iterator mi = mapBlockIndex.find(inv.hash);
                if (mi != mapBlockIndex.end()) {
                    if (chainActive.Contains(mi->second)) {
                        send = true;
                    } else {
                        // To prevent fingerprinting attacks, only send blocks outside of the active
                        // chain if they are valid, and no more than a month older than the best header
                        // chain we know about.
                        send = mi->second->IsValid(BLOCK_VALID_SCRIPTS) && (pindexBestHeader != NULL) &&
                            (mi->second->GetBlockTime() > pindexBestHeader->GetBlockTime() - 30 * 24 * 60 * 60);
                        if (!send) {
                            LogPrintf("ProcessGetData(): ignoring request from peer=%i for old block that isn't in the main chain\n", pfrom->GetId());
                        }
                    }
                }
                if (send) {
                    //! Send block from disk
                    CBlock block;
                    if (!ReadBlockFromDisk(block, (*mi).second))
                        assert(!"cannot load block from disk");
                    if (inv.type == MSG_BLOCK)
                        pfrom->PushMessage("block", block);
                    else //! MSG_FILTERED_BLOCK)
                    {
                        LOCK(pfrom->cs_filter);
                        if (pfrom->pfilter) {
                            CMerkleBlock merkleBlock(block, *pfrom->pfilter);
                            pfrom->PushMessage("merkleblock", merkleBlock);
                            /**
                             * CMerkleBlock just contains hashes, so also push any transactions in the block the client did not see
                             * This avoids hurting performance by pointlessly requiring a round-trip
                             * Note that there is currently no way for a node to request any single transactions we didnt send here -
                             * they must either disconnect and retry or request the full block.
                             * Thus, the protocol spec specified allows for us to provide duplicate txn here,
                             * however we MUST always provide at least what the remote peer needs
                             */
                            typedef std::pair<unsigned int, uint256> PairType;
                            BOOST_FOREACH (PairType& pair, merkleBlock.vMatchedTxn)
                                if (!pfrom->setInventoryKnown.count(CInv(MSG_TX, pair.second)))
                                    pfrom->PushMessage("tx", block.vtx[pair.first]);
                        }
                        //! else
                        //! no response
                    }

                    //! Trigger them to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue) {
                        /**
                         * Bypass PushInventory, this must send even if redundant,
                         * and we want it right after the last block so they don't
                         * wait for other stuff first.
                         */
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, chainActive.Tip()->GetBlockHash()));
                        pfrom->PushMessage("inv", vInv);
                        pfrom->hashContinue = 0;
                    }
                }
            } else if (inv.IsKnownType()) {
                //! Send stream from relay memory
                bool pushed = false;
                {
                    LOCK(cs_mapRelay);
                    map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    if (mi != mapRelay.end()) {
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TX) {
                    if (mapDarksendBroadcastTxes.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapDarksendBroadcastTxes[inv.hash].tx << mapDarksendBroadcastTxes[inv.hash].vin << mapDarksendBroadcastTxes[inv.hash].vchSig << mapDarksendBroadcastTxes[inv.hash].sigTime;

                        pfrom->PushMessage("dstx", ss);
                        pushed = true;
                    } else {
                        CTransaction tx;
                        if (mempool.lookup(inv.hash, tx)) {
                            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                            ss.reserve(1000);
                            ss << tx;
                            pfrom->PushMessage("tx", ss);
                            pushed = true;
                        }
                    }
                }
                if (!pushed && inv.type == MSG_TXLOCK_VOTE) {
                    if (mapTxLockVote.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapTxLockVote[inv.hash];
                        pfrom->PushMessage("txlvote", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TXLOCK_REQUEST) {
                    if (mapTxLockReq.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapTxLockReq[inv.hash];
                        pfrom->PushMessage("txlreq", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_SPORK) {
                    if (mapSporks.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapSporks[inv.hash];
                        pfrom->PushMessage("spork", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_MASTERNODE_WINNER) {
                    if (mapSeenMasternodeVotes.count(inv.hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        int a = 0;
                        ss.reserve(1000);
                        ss << mapSeenMasternodeVotes[inv.hash] << a;
                        pfrom->PushMessage("mnw", ss);
                        pushed = true;
                    }
                }
                if (!pushed) {
                    vNotFound.push_back(inv);
                }
            }

            //! Track requests for our stuff.
            g_signals.Inventory(inv.hash);

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
                break;
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    if (!vNotFound.empty()) {
        /**
         * Let the peer know that we didn't find what it asked for, so it doesn't
         * have to wait around forever. Currently only SPV clients actually care
         * about this message: it's needed when they are recursively walking the
         * dependencies of relevant unconfirmed transactions. SPV clients want to
         * do that because they want to know about (and store and rebroadcast and
         * risk analyze) the dependencies of transactions relevant to them, without
         * having to download the entire memory pool.
         */
        pfrom->PushMessage("notfound", vNotFound);
    }
}

bool static ProcessMessage(CNode* pfrom, string strCommand, CDataStream& vRecv, int64_t nTimeReceived)
{
    RandAddSeedPerfmon();
    LogPrint("net", "received: %s (%u bytes) peer=%d\n", SanitizeString(strCommand), vRecv.size(), pfrom->id);
    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0) {
        LogPrintf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }

    if (strCommand == "version") {
        //! Each connection can only send one version message
        if (pfrom->nVersion != 0) {
            pfrom->PushMessage("reject", strCommand, REJECT_DUPLICATE, string("Duplicate version message"));
            Misbehaving(pfrom->GetId(), 1);
            return false;
        }

        int64_t nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64_t nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;
        if (pfrom->nVersion < MIN_PEER_PROTO_VERSION) {
            //! disconnect from peers older than this proto version
            LogPrintf("peer=%d using obsolete version %i; disconnecting\n", pfrom->id, pfrom->nVersion);
            pfrom->PushMessage("reject", strCommand, REJECT_OBSOLETE,
                               strprintf("Version must be %d or greater", MIN_PEER_PROTO_VERSION));
            pfrom->fDisconnect = true;
            Misbehaving(pfrom->GetId(), 100);
            return false;
        }

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty())
            vRecv >> LIMITED_STRING(pfrom->strSubVer, 256);
        if (!vRecv.empty())
            vRecv >> pfrom->nStartingHeight;
        if (!vRecv.empty())
            vRecv >> pfrom->fRelayTxes; //! set to true after we get the first filter* message
        else
            pfrom->fRelayTxes = true;

        pfrom->cleanSubVer = SanitizeString(pfrom->strSubVer);

        //! Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1) {
            LogPrintf("connected to self at %s, disconnecting\n", pfrom->addr.ToString());
            pfrom->fDisconnect = true;
            return true;
        }

        pfrom->addrLocal = addrMe;
        if (pfrom->fInbound && addrMe.IsRoutable()) {
            SeenLocal(addrMe);
        }

        //! Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        //! Change version
        pfrom->PushMessage("verack");
        pfrom->ssSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound) {
            //! Advertise our address
            if (fListen && !IsInitialBlockDownload()) {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                if (addr.IsRoutable()) {
                    pfrom->PushAddress(addr);
                } else if (IsPeerAddrLocalGood(pfrom)) {
                    addr.SetIP(pfrom->addrLocal);
                    pfrom->PushAddress(addr);
                }
            }

            //! Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000) {
                pfrom->PushMessage("getaddr");
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        } else {
            if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom) {
                addrman.Add(addrFrom, addrFrom);
                addrman.Good(addrFrom);
            }
        }

        //! Relay alerts
        {
            LOCK(cs_mapAlerts);
            BOOST_FOREACH (PAIRTYPE(const uint256, CAlert) & item, mapAlerts)
                item.second.RelayTo(pfrom);
        }

        pfrom->fSuccessfullyConnected = true;

        string remoteAddr;
        if (fLogIPs)
            remoteAddr = ", peeraddr=" + pfrom->addr.ToString();

        LogPrintf("receive version message: %s: version %d, blocks=%d, us=%s, peer=%d%s\n",
                  pfrom->cleanSubVer, pfrom->nVersion,
                  pfrom->nStartingHeight, addrMe.ToString(), pfrom->id,
                  remoteAddr);

        if (GetBoolArg("-synctime", true))
            AddTimeData(pfrom->addr, nTime);
    }


    else if (pfrom->nVersion == 0) {
        //! Must have a version message before anything else
        Misbehaving(pfrom->GetId(), 1);
        return false;
    }


    else if (strCommand == "verack") {
        pfrom->SetRecvVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        //! Mark this node as currently connected, so we update its timestamp later.
        if (pfrom->fNetworkNode) {
            LOCK(cs_main);
            State(pfrom->GetId())->fCurrentlyConnected = true;
        }
    }


    else if (strCommand == "addr") {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        //! Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
            return true;
        if (vAddr.size() > 1000) {
            Misbehaving(pfrom->GetId(), 20);
            return error("message addr size() = %u", vAddr.size());
        }

        //! Store the new addresses
        vector<CAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;
        BOOST_FOREACH (CAddress& addr, vAddr) {
            boost::this_thread::interruption_point();

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable()) {
                //! Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);
                    //! Use deterministic randomness to send to the same nodes for 24 hours
                    //! at a time so the setAddrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint64_t hashAddr = addr.GetHash();
                    uint256 hashRand = hashSalt ^ (hashAddr << 32) ^ ((GetTime() + hashAddr) / (24 * 60 * 60));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    multimap<uint256, CNode*> mapMix;
                    BOOST_FOREACH (CNode* pnode, vNodes) {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; //! limited relaying of addresses outside our network(s)
                    for (multimap<uint256, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr);
                }
            }
            //! Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot)
            pfrom->fDisconnect = true;
    }

    else if (strCommand == "inv") {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ) {
            Misbehaving(pfrom->GetId(), 20);
            return error("message inv size() = %u", vInv.size());
        }

        LOCK(cs_main);
        int nBlocksGet = 0;
        std::vector<CInv> vToFetch;

        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
            const CInv& inv = vInv[nInv];

            boost::this_thread::interruption_point();
            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = AlreadyHave(inv);
            LogPrint("net", "got inv: %s  %s peer=%d\n", inv.ToString(), fAlreadyHave ? "have" : "new", pfrom->id);

        if (!fAlreadyHave && !fImporting && !fReindex && inv.type != MSG_BLOCK)
                pfrom->AskFor(inv);

            if (inv.type == MSG_BLOCK) {
                UpdateBlockAvailability(pfrom->GetId(), inv.hash);
                 if (!fAlreadyHave && !fImporting && !fReindex && !mapBlocksInFlight.count(inv.hash) && CanRequestMoreHeaders()) {
                    /**
                     * First request the headers preceeding the announced block. In the normal fully-synced
                     * case where a new block is announced that succeeds the current tip (no reorganization),
                     * there are no such headers.
                     * Secondly, and only when we are close to being synced, we request the announced block directly,
                     * to avoid an extra round-trip. Note that we must *first* ask for the headers, so by the
                     * time the block arrives, the header chain leading up to it is already validated. Not
                     * doing this will result in the received block being rejected as an orphan in case it is
                     * not a direct successor.
                     */
                    pfrom->PushMessage("getheaders", chainActive.GetLocator(pindexBestHeader), inv.hash);
                    CNodeState *nodestate = State(pfrom->GetId());
                    if (chainActive.Tip()->GetBlockTime() > GetAdjustedTime() - Params().TargetSpacing() * 20 &&
                        nodestate->nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
                            vToFetch.push_back(inv);
                            /**
                             * Mark block as in flight already, even though the actual "getdata" message only goes out
                             * later (within the same cs_main lock, though).
                             */
                            MarkBlockAsInFlight(pfrom->GetId(), inv.hash);
                    }
                }
            }

            //! Track requests for our stuff
            g_signals.Inventory(inv.hash);

            if (pfrom->nSendSize > (SendBufferSize() * 2)) {
                Misbehaving(pfrom->GetId(), 20);
                return error("send buffer size() = %u", pfrom->nSendSize);
            }
        }

        if (!vToFetch.empty())
            pfrom->PushMessage("getdata", vToFetch);

        if (nBlocksGet)
            pfrom->tBlockInvs = GetTimeMillis();
    }


    else if (strCommand == "getdata") {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ) {
            Misbehaving(pfrom->GetId(), 20);
            return error("message getdata size() = %u", vInv.size());
        }

        if (fDebug || (vInv.size() != 1))
            LogPrint("net", "received getdata (%u invsz) peer=%d\n", vInv.size(), pfrom->id);

        if ((fDebug && vInv.size() > 0) || (vInv.size() == 1))
            LogPrint("net", "received getdata for: %s peer=%d\n", vInv[0].ToString(), pfrom->id);

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        ProcessGetData(pfrom);
    }


    else if (strCommand == "getblocks") {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        //! Find the last block the caller has in the main chain
        CBlockIndex* pindex = FindForkInGlobalIndex(chainActive, locator);

        //! Send the rest of the chain
        if (pindex)
            pindex = chainActive.Next(pindex);
        int nLimit = 500;
        LogPrint("net", "getblocks %d to %s limit %d from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop == uint256(0) ? "end" : hashStop.ToString(), nLimit, pfrom->id);
        for (; pindex; pindex = chainActive.Next(pindex)) {
            if (pindex->GetBlockHash() == hashStop) {
                LogPrint("net", "  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0) {
                //! When this block is requested, we'll send an inv that'll make them
                //! getblocks the next batch of inventory.
                LogPrint("net", "  getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }

    else if (strCommand == "getheaders") {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        CBlockIndex* pindex = NULL;
        if (locator.IsNull()) {
            //! If locator is null, return the hashStop block
            BlockMap::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        } else {
            //! Find the last block the caller has in the main chain
            pindex = FindForkInGlobalIndex(chainActive, locator);
            if (pindex)
                pindex = chainActive.Next(pindex);
        }

        //! we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
        vector<CBlock> vHeaders;
        int nLimit = MAX_HEADERS_RESULTS;
        LogPrint("net", "getheaders %d to %s from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString(), pfrom->id);
        for (; pindex; pindex = chainActive.Next(pindex)) {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        pfrom->PushMessage("headers", vHeaders);
    }


    else if (strCommand == "tx") {
        vector<uint256> vWorkQueue;
        vector<uint256> vEraseQueue;
        CTransaction tx;
        vRecv >> tx;

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        LOCK(cs_main);

        bool fMissingInputs = false;

        mapAlreadyAskedFor.erase(inv);
        CValidationState state;
        if (AcceptToMemoryPool(mempool, state, tx, true, &fMissingInputs)) {
            mempool.check(pcoinsTip);
            RelayTransaction(tx);
            vWorkQueue.push_back(inv.hash);
            vEraseQueue.push_back(inv.hash);

            LogPrint("mempool", "AcceptToMemoryPool: peer=%d %s : accepted %s (poolsz %u)\n",
                     pfrom->id, pfrom->strSubVer.c_str(),
                     tx.GetHash().ToString().c_str(),
                     mempool.mapTx.size());

            //! Recursively process any orphan transactions that depended on this one
            set<NodeId> setMisbehaving;
            for (unsigned int i = 0; i < vWorkQueue.size(); i++) {
                map<uint256, set<uint256> >::iterator itByPrev = mapOrphanTransactionsByPrev.find(vWorkQueue[i]);
                if (itByPrev == mapOrphanTransactionsByPrev.end())
                    continue;
                for (set<uint256>::iterator mi = itByPrev->second.begin();
                     mi != itByPrev->second.end();
                     ++mi) {
                    const uint256& orphanHash = *mi;
                    const CTransaction& orphanTx = mapOrphanTransactions[orphanHash].tx;
                    NodeId fromPeer = mapOrphanTransactions[orphanHash].fromPeer;
                    bool fMissingInputs2 = false;
                    // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan
                    // resolution (that is, feeding people an invalid transaction based on LegitTxX in order to get
                    // anyone relaying LegitTxX banned)
                    CValidationState stateDummy;

                    vEraseQueue.push_back(orphanHash);

                    if (setMisbehaving.count(fromPeer))
                        continue;
                    if (AcceptToMemoryPool(mempool, stateDummy, orphanTx, true, &fMissingInputs2))
                    {
                        LogPrint("mempool", "   accepted orphan tx %s\n", orphanHash.ToString());
                        RelayTransaction(orphanTx);
                        vWorkQueue.push_back(orphanHash);
                    }
                    else if (!fMissingInputs2)
                    {
                        int nDos = 0;
                        if (stateDummy.IsInvalid(nDos) && nDos > 0)
                        {
                            // Punish peer that gave us an invalid orphan tx
                            Misbehaving(fromPeer, nDos);
                            setMisbehaving.insert(fromPeer);
                            LogPrint("mempool", "   invalid orphan tx %s\n", orphanHash.ToString());
                        }
                        // too-little-fee orphan
                        LogPrint("mempool", "   removed orphan tx %s\n", orphanHash.ToString());
                    }
                    mempool.check(pcoinsTip);
                }
            }

            BOOST_FOREACH (uint256 hash, vEraseQueue)
                EraseOrphanTx(hash);
        } else if (fMissingInputs) {
            AddOrphanTx(tx, pfrom->GetId());

            //! DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nMaxOrphanTx = (unsigned int)std::max((int64_t)0, GetArg("-maxorphantx", DEFAULT_MAX_ORPHAN_TRANSACTIONS));
            unsigned int nEvicted = LimitOrphanTxSize(nMaxOrphanTx);
            if (nEvicted > 0)
                LogPrintf("mempool", "mapOrphan overflow, removed %u tx\n", nEvicted);
        } else if (pfrom->fWhitelisted) {
            /**
             * Always relay transactions received from whitelisted peers, even
             * if they are already in the mempool (allowing the node to function
             * as a gateway for nodes hidden behind it).
             */
            RelayTransaction(tx);
        }
        int nDoS = 0;
        if (state.IsInvalid(nDoS)) {
            LogPrint("mempool", "%s from peer=%d %s was not accepted into the memory pool: %s\n", tx.GetHash().ToString(),
                pfrom->id, pfrom->cleanSubVer, state.GetRejectReason());
            pfrom->PushMessage("reject", strCommand, state.GetRejectCode(),
                    state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), inv.hash);
            if (nDoS > 0)
                Misbehaving(pfrom->GetId(), nDoS);
        }
    }


    else if (strCommand == "headers" && !fImporting && !fReindex) //! Ignore headers received while importing
    {
        //! Ignore headers received if we have enough and didn't ask for more
        CNodeState *nodestate = State(pfrom->GetId());
        if (!CanRequestMoreHeaders() && !nodestate->fSyncStarted)
            return error("ignoring headers we did not request");

        std::vector<CBlockHeader> headers;

        //! Bypass the normal CBlock deserialization, as we don't want to risk deserializing 2000 full blocks.
        unsigned int nCount = ReadCompactSize(vRecv);
        if (nCount > MAX_HEADERS_RESULTS) {
            Misbehaving(pfrom->GetId(), 20);
            return error("headers message size = %u", nCount);
        }
        headers.resize(nCount);       
        for (unsigned int n = 0; n < nCount; n++) {
            vRecv >> headers[n];
            ReadCompactSize(vRecv); //! ignore tx count; assume it is 0.
            //! metrix: ignore vchBlockSig this shouldn't be sent and should be removed in the future
            ReadCompactSize(vRecv); 
        }

        LOCK(cs_main);

        if (nCount == 0) {
            //! Nothing interesting. Stop asking this peers for more headers.
            return true;
        }

        CBlockIndex *pindexLast = NULL;
        BOOST_FOREACH(const CBlockHeader& header, headers) {
            CValidationState state;
            if (pindexLast != NULL && header.hashPrevBlock != pindexLast->GetBlockHash()) {
                Misbehaving(pfrom->GetId(), 20);
                return error("non-continuous headers sequence");
            }
            if (!AcceptBlockHeader(header, state, &pindexLast)) {
                int nDoS;
                if (state.IsInvalid(nDoS)) {
                    if (nDoS > 0)
                        Misbehaving(pfrom->GetId(), nDoS);
                    return error("invalid header received");
                }
            }
        }

        if (pindexLast)
            UpdateBlockAvailability(pfrom->GetId(), pindexLast->GetBlockHash());

        if (nCount == MAX_HEADERS_RESULTS && pindexLast && CanRequestMoreHeaders()) {
            /**
             * Headers message had its maximum size; the peer may have more headers.
             * TODO: optimize: if pindexLast is an ancestor of chainActive.Tip or pindexBestHeader, continue
             * from there instead.
             */
            LogPrint("net", "more getheaders (%d) to end to peer=%d (startheight:%d)\n", pindexLast->nHeight, pfrom->id, pfrom->nStartingHeight);
            pfrom->PushMessage("getheaders", chainActive.GetLocator(pindexLast), uint256(0));
        }

        CheckBlockIndex();
    }

    else if (strCommand == "block" && !fImporting && !fReindex) //! Ignore blocks received while importing
    {
        int size = vRecv.size();
        CBlock block;
        vRecv >> block;
        uint256 hashBlock = block.GetHash();

        CInv inv(MSG_BLOCK, hashBlock);
        LogPrint("net", "received block %s peer=%d\n", inv.hash.ToString(), pfrom->id);
        pfrom->AddInventoryKnown(inv);
        CValidationState state;
        ProcessNewBlock(state, pfrom, &block);
        int nDoS;
        if (state.IsInvalid(nDoS)) {
            pfrom->PushMessage("reject", strCommand, state.GetRejectCode(),
                               state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), inv.hash);
            if (nDoS > 0) {
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), nDoS);
            }
        }
    }


    // This asymmetric behavior for inbound and outbound connections was introduced
    // to prevent a fingerprinting attack: an attacker can send specific fake addresses
    // to users' AddrMan and later request them by sending getaddr messages. 
    // Making users (which are behind NAT and can only make outgoing connections) ignore 
    // getaddr message mitigates the attack.
    else if ((strCommand == "getaddr") && (pfrom->fInbound)) {
        //! Don't return addresses older than nCutOff timestamp
        int64_t nCutOff = GetTime() - (nNodeLifespan * 24 * 60 * 60);
        pfrom->vAddrToSend.clear();
        vector<CAddress> vAddr = addrman.GetAddr();
        BOOST_FOREACH (const CAddress& addr, vAddr)
            if (addr.nTime > nCutOff)
                pfrom->PushAddress(addr);
    }


    else if (strCommand == "mempool") {
        LOCK2(cs_main, pfrom->cs_filter);

        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);
        vector<CInv> vInv;
        BOOST_FOREACH (uint256& hash, vtxid) {
            CInv inv(MSG_TX, hash);
            CTransaction tx;
            bool fInMemPool = mempool.lookup(hash, tx);
            if (!fInMemPool)
                continue; //! another thread removed since queryHashes, maybe...
            if ((pfrom->pfilter && pfrom->pfilter->IsRelevantAndUpdate(tx)) ||
                (!pfrom->pfilter))
                vInv.push_back(inv);
            if (vInv.size() == MAX_INV_SZ) {
                pfrom->PushMessage("inv", vInv);
                vInv.clear();
            }
        }
        if (vInv.size() > 0)
            pfrom->PushMessage("inv", vInv);
    }


    else if (strCommand == "ping") {
        if (pfrom->nVersion > BIP0031_VERSION) {
            uint64_t nonce = 0;
            vRecv >> nonce;
            /**
             * Echo the message back with the nonce. This allows for two useful features:
             *
             * 1) A remote node can quickly check if the connection is operational
             * 2) Remote nodes can measure the latency of the network thread. If this node
             *    is overloaded it won't respond to pings quickly and the remote node can
             *    avoid sending us more work, like chain download requests.
             *
             * The nonce stops the remote getting confused between different pings: without
             * it, if the remote node sends a ping once per second and this node takes 5
             * seconds to respond to each, the 5th ping the remote sends would appear to
             * return very quickly.
             */
            pfrom->PushMessage("pong", nonce);
        }
    }


    else if (strCommand == "pong") {
        int64_t pingUsecEnd = nTimeReceived;
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;

        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            //! Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (pfrom->nPingNonceSent != 0) {
                if (nonce == pfrom->nPingNonceSent) {
                    //! Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                    if (pingUsecTime > 0) {
                        //! Successful ping time measurement, replace previous
                        pfrom->nPingUsecTime = pingUsecTime;
                    } else {
                        //! This should never happen
                        sProblem = "Timing mishap";
                    }
                } else {
                    //! Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0) {
                        //! This is most likely a bug in another implementation somewhere, cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            } else {
                sProblem = "Unsolicited pong without ping";
            }
        } else {
            //! This is most likely a bug in another implementation somewhere, cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty())) {
            LogPrint("net", "pong peer=%d %s: %s, %x expected, %x received, %u bytes\n",
                     pfrom->id,
                     pfrom->strSubVer,
                     sProblem,
                     pfrom->nPingNonceSent,
                     nonce,
                     nAvail);
        }
        if (bPingFinished) {
            pfrom->nPingNonceSent = 0;
        }
    }


    else if (fAlerts && strCommand == "alert") {
        CAlert alert;
        vRecv >> alert;

        uint256 alertHash = alert.GetHash();
        if (pfrom->setKnown.count(alertHash) == 0) {
            if (alert.ProcessAlert()) {
                //! Relay
                pfrom->setKnown.insert(alertHash);
                {
                    LOCK(cs_vNodes);
                    BOOST_FOREACH (CNode* pnode, vNodes)
                        alert.RelayTo(pnode);
                }
            } else {
                /**
                 * Small DoS penalty so peers that send us lots of
                 * duplicate/expired/invalid-signature/whatever alerts
                 * eventually get banned.
                 * This isn't a Misbehaving(100) (immediate ban) because the
                 * peer might be an older or different implementation with
                 * a different signature key, etc.
                 */
                Misbehaving(pfrom->GetId(), 10);
            }
        }
    }

    else if (strCommand == "filterload") {
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints())
            //! There is no excuse for sending a too-large filter
            Misbehaving(pfrom->GetId(), 100);
        else {
            LOCK(pfrom->cs_filter);
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter(filter);
            pfrom->pfilter->UpdateEmptyFull();
        }
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == "filteradd") {
        vector<unsigned char> vData;
        vRecv >> vData;

        /**
         * Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
         * and thus, the maximum size any matched object can have) in a filteradd message
         */
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE) {
            Misbehaving(pfrom->GetId(), 100);
        } else {
            LOCK(pfrom->cs_filter);
            if (pfrom->pfilter)
                pfrom->pfilter->insert(vData);
            else
                Misbehaving(pfrom->GetId(), 100);
        }
    }

    else if (strCommand == "filterclear") {
        LOCK(pfrom->cs_filter);
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter();
        pfrom->fRelayTxes = true;
    }

    else if (strCommand == "reject") {
        if (fDebug) {
            try {
                string strMsg;
                unsigned char ccode;
                string strReason;
                vRecv >> LIMITED_STRING(strMsg, CMessageHeader::COMMAND_SIZE) >> ccode >> LIMITED_STRING(strReason, MAX_REJECT_MESSAGE_LENGTH);

                ostringstream ss;
                ss << strMsg << " code " << itostr(ccode) << ": " << strReason;

                if (strMsg == "block" || strMsg == "tx") {
                    uint256 hash;
                    vRecv >> hash;
                    ss << ": hash " << hash.ToString();
                }
                LogPrint("net", "Reject %s\n", SanitizeString(ss.str()));
            } catch (const std::ios_base::failure& e) {
                //! Avoid feedback loops by preventing reject messages from triggering a new reject message.
                LogPrint("net", "Unparseable reject message received\n");
            }
        }
    }

    else {
        ProcessMessageDarksend(pfrom, strCommand, vRecv);
        ProcessMessageMasternode(pfrom, strCommand, vRecv);
        ProcessMessageInstantX(pfrom, strCommand, vRecv);
        ProcessSpork(pfrom, strCommand, vRecv);

        //! Ignore unknown commands for extensibility
        LogPrint("net", "Unknown command \"%s\" from peer=%d\n", SanitizeString(strCommand), pfrom->id);
    }

    return true;
}


//! requires LOCK(cs_vRecvMsg)
bool ProcessMessages(CNode* pfrom)
{
    /**
     * Message format
     *  (4) message start
     *  (12) command
     *  (4) size
     *  (4) checksum
     *  (x) data
     */
    bool fOk = true;

    if (!pfrom->vRecvGetData.empty())
        ProcessGetData(pfrom);

    //! this maintains the order of responses
    if (!pfrom->vRecvGetData.empty())
        return fOk;
    int nBlocksInFlight = 0;
    {
        LOCK(cs_main);
        CNodeState* state = State(pfrom->id);
        nBlocksInFlight = state->nBlocksInFlight;
    }

    std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
    while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end()) {
        //! Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        //! get next message
        CNetMessage& msg = *it;
        CMessageHeader& hdr = msg.hdr;
        unsigned int nMessageSize = hdr.nMessageSize;
        string strCommand = hdr.GetCommand();


        if (msg.nDataPos != msg.nLastDataPos) {
            if (strCommand == "block") {
                if (msg.nLastDataPos == 0) {
                    pfrom->tBlockRecvStart = GetTimeMillis();
                    if (pfrom->tBlockRecved)
                        LogPrint("net", "%ums later, ", pfrom->tBlockRecvStart - pfrom->tBlockRecved);
                }
                pfrom->tBlockRecving = GetTimeMillis();
            }
            msg.nLastDataPos = msg.nDataPos;
        }
        //! end, if an incomplete message is found
        if (!msg.complete())
            break;

        //! at this point, any failure means we can delete the current message
        it++;

        //! Scan for message start
        if (memcmp(msg.hdr.pchMessageStart, Params().MessageStart(), MESSAGE_START_SIZE) != 0) {
            LogPrintf("PROCESSMESSAGE: INVALID MESSAGESTART %s peer=%d\n", SanitizeString(msg.hdr.GetCommand()), pfrom->id);
            fOk = false;
            break;
        }


        if (!hdr.IsValid()) {
            LogPrintf("PROCESSMESSAGE: ERRORS IN HEADER %s peer=%d\n", SanitizeString(strCommand), pfrom->id);
            continue;
        }

        //! Checksum
        CDataStream& vRecv = msg.vRecv;
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        if (nChecksum != hdr.nChecksum) {
            LogPrintf("ProcessMessages(%s, %u bytes) : CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n",
                      SanitizeString(strCommand), nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        //! Process message
        bool fRet = false;
        try {
            fRet = ProcessMessage(pfrom, strCommand, vRecv, msg.nTime);
            boost::this_thread::interruption_point();
        } catch (const std::ios_base::failure& e) {
            pfrom->PushMessage("reject", strCommand, REJECT_MALFORMED, string("error parsing message"));
            if (strstr(e.what(), "end of data")) {
                //! Allow exceptions from under-length message on vRecv
                LogPrintf("ProcessMessages(%s, %u bytes) : Exception '%s' caught, normally caused by a message being shorter than its stated length\n", SanitizeString(strCommand), nMessageSize, e.what());
            } else if (strstr(e.what(), "size too large")) {
                //! Allow exceptions from over-long size
                LogPrintf("ProcessMessages(%s, %u bytes) : Exception '%s' caught\n", SanitizeString(strCommand), nMessageSize, e.what());
            } else {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        } catch (const boost::thread_interrupted) {
            throw;
        } catch (const std::exception& e) {
            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
            LogPrintf("ProcessMessage(%s, %u bytes) FAILED peer=%d\n", SanitizeString(strCommand), nMessageSize, pfrom->id);

        break;
    }

    //! In case the connection got shut down, its receive buffer was wiped
    if (!pfrom->fDisconnect)
        pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);

    return fOk;
}


bool SendMessages(CNode* pto)
{
    {
        //! Don't send anything until we get their version message
        if (pto->nVersion == 0)
            return true;

        /**
         * Message: ping
         */
        bool pingSend = false;
        if (pto->fPingQueued) {
            //! RPC ping request by user
            pingSend = true;
        }
        if (pto->nPingNonceSent == 0 && pto->nPingUsecStart + PING_INTERVAL * 1000000 < GetTimeMicros()) {
            //! Ping automatically sent as a latency probe & keepalive.
            pingSend = true;
        }
        if (pingSend) {
            uint64_t nonce = 0;
            while (nonce == 0) {
                GetRandBytes((unsigned char*)&nonce, sizeof(nonce));
            }
            pto->fPingQueued = false;
            pto->nPingUsecStart = GetTimeMicros();
            if (pto->nVersion > BIP0031_VERSION) {
                pto->nPingNonceSent = nonce;
                pto->PushMessage("ping", nonce);
            } else {
                //! Peer is too old to support ping command with nonce, pong will never arrive.
                pto->nPingNonceSent = 0;
                pto->PushMessage("ping");
            }
        }

        TRY_LOCK(cs_main, lockMain); //! Acquire cs_main for IsInitialBlockDownload() and CNodeState()
        if (!lockMain)
            return true;

        CNodeState& state = *State(pto->GetId());
        if (state.fShouldBan) {
            if (pto->addr.IsLocal())
                LogPrintf("Warning: not banning local node %s!\n", pto->addr.ToString().c_str());
            else {
                pto->fDisconnect = true;
                CNode::Ban(pto->addr);
            }
            state.fShouldBan = false;
        }

        BOOST_FOREACH (const CBlockReject& reject, state.rejects)
            pto->PushMessage("reject", (string) "block", reject.chRejectCode, reject.strRejectReason, reject.hashBlock);
        state.rejects.clear();

        //! Start block sync
        if (pindexBestHeader == NULL)
            pindexBestHeader = chainActive.Tip();
        bool fFetch = !pto->fInbound || (pindexBestHeader && (state.pindexLastCommonBlock ? state.pindexLastCommonBlock->nHeight : 0) + 144 > pindexBestHeader->nHeight);
        if (!state.fSyncStarted && !pto->fClient && fFetch && !fImporting && !fReindex) {
            //! Only actively request headers from a single peer, unless we're close to today.
            if (nSyncStarted == 0 || pindexBestHeader->GetBlockTime() > GetAdjustedTime() - 24 * 60 * 60) {
                state.fSyncStarted = true;
                nSyncStarted++;
                CBlockIndex *pindexStart = pindexBestHeader->pprev ? pindexBestHeader->pprev : pindexBestHeader;
                LogPrint("net", "initial getheaders (%d) to peer=%d (startheight:%d)\n", pindexStart->nHeight, pto->id, pto->nStartingHeight);
                pto->PushMessage("getheaders", chainActive.GetLocator(pindexStart), uint256(0));
            }
        }

        /**
         * Resend wallet transactions that haven't gotten in a block yet
         * Except during reindex, importing and IBD, when old wallet
         * transactions become unconfirmed and spams other nodes.
         */
        if (!fReindex && !fImporting && !IsInitialBlockDownload()) {
            ResendWalletTransactions();
        }

        /**
         * Message: addr
         */
        int64_t nNow = GetTimeMicros();
        if (pto->nNextAddrSend < nNow) {
            pto->nNextAddrSend = PoissonNextSend(nNow, AVG_ADDRESS_BROADCAST_INTERVAL);
            vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            BOOST_FOREACH (const CAddress& addr, pto->vAddrToSend) {
                //! returns true if wasn't already contained in the set
                if (pto->setAddrKnown.insert(addr).second) {
                    vAddr.push_back(addr);
                    //! receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000) {
                        pto->PushMessage("addr", vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage("addr", vAddr);
        }

        if (!IsInitialBlockDownload() && pto->nNextLocalAddrSend < nNow) {
            AdvertizeLocal(pto);
            pto->nNextLocalAddrSend = PoissonNextSend(nNow, AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
        }

        /*
         * Message: inventory
         */
        vector<CInv> vInv;
        vector<CInv> vInvWait;
        {
            bool fSendTrickle = false;
            if (pto->nNextInvSend < nNow) {
                fSendTrickle = true;
                pto->nNextInvSend = PoissonNextSend(nNow, AVG_INVENTORY_BROADCAST_INTERVAL);
            }
            LOCK(pto->cs_inventory);
            vInv.reserve(pto->vInventoryToSend.size());
            vInv.reserve(std::min<size_t>(1000, pto->vInventoryToSend.size()));
            BOOST_FOREACH (const CInv& inv, pto->vInventoryToSend) {
                if (pto->setInventoryKnown.count(inv))
                    continue;

                //! trickle out tx inv to protect privacy
                if (inv.type == MSG_TX && !fSendTrickle) {
                    //! 1/4 of tx invs blast to all immediately
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint256 hashRand = inv.hash ^ hashSalt;
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    bool fTrickleWait = ((hashRand & 3) != 0);

                    if (fTrickleWait) {
                        vInvWait.push_back(inv);
                        continue;
                    }
                }

                //! returns true if wasn't already contained in the set
                if (pto->setInventoryKnown.insert(inv).second) {
                    vInv.push_back(inv);
                    if (vInv.size() >= 1000) {
                        pto->PushMessage("inv", vInv);
                        vInv.clear();
                    }
                }
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
            pto->PushMessage("inv", vInv);


        //! Detect whether we're stalling
        int64_t tNow = GetTimeMillis();
        if (!pto->fDisconnect && state.nStallingSince && state.nStallingSince < nNow - 1000000 * BLOCK_STALLING_TIMEOUT) {
            /**
             * Stalling only triggers when the block download window cannot move. During normal steady state,
             * the download window should be much larger than the to-be-downloaded set of blocks, so disconnection
             * should only happen during initial block download.
             */
            LogPrintf("Peer=%d is stalling block download, disconnecting\n", pto->id);
            pto->fDisconnect = true;
        }
        /**
         * In case there is a block that has been in flight from this peer for (2 + 0.5 * N) times the block interval
         * (with N the number of validated blocks that were in flight at the time it was requested), disconnect due to
         * timeout. We compensate for in-flight blocks to prevent killing off peers due to our own downstream link
         * being saturated. We only count validated in-flight blocks so peers can't advertize nonexisting block hashes
         * to unreasonably increase our timeout.
         */
        if (!pto->fDisconnect && state.vBlocksInFlight.size() > 0 && state.vBlocksInFlight.front().nTime < nNow - 500000 * Params().TargetSpacing() * (4 + state.vBlocksInFlight.front().nValidatedQueuedBefore)) {
            LogPrintf("Timeout downloading block %s from peer=%d, disconnecting\n", state.vBlocksInFlight.front().hash.ToString(), pto->id);
            pto->fDisconnect = true;
        }

        /**
         * Message: getdata (blocks)
         */
        std::vector<CInv> vGetData;
        if (!pto->fDisconnect && !pto->fClient && fFetch && state.nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            vector<CBlockIndex*> vToDownload;
            NodeId staller = -1;
            FindNextBlocksToDownload(pto->GetId(), MAX_BLOCKS_IN_TRANSIT_PER_PEER - state.nBlocksInFlight, vToDownload, staller);
            BOOST_FOREACH(CBlockIndex *pindex, vToDownload) {
                vGetData.push_back(CInv(MSG_BLOCK, pindex->GetBlockHash()));
                MarkBlockAsInFlight(pto->GetId(), pindex->GetBlockHash(), pindex);
                LogPrint("net", "Requesting block %s peer=%d\n", pindex->GetBlockHash().ToString(), pto->id);
            }
            if (state.nBlocksInFlight == 0 && staller != -1) {
                if (State(staller)->nStallingSince == 0)
                    State(staller)->nStallingSince = nNow;
            }
        }

        /**
         * Message: getdata (non-blocks)
         */
        nNow = GetTimeMicros();
        while (!pto->fDisconnect && !pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow) {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(inv)) {
                if (fDebug)
                    LogPrint("net", "Requesting %s peer=%d\n", inv.ToString(), pto->id);
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000) {
                    pto->PushMessage("getdata", vGetData);
                    vGetData.clear();
                }
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            pto->PushMessage("getdata", vGetData);
    }
    return true;
}

bool IsBlockMasternodePaymentValid(CBlockIndex *pindex, CAmount masternodePayment)
{
    std::vector<CAmount> vCollaterals;
    GetMasternodeCollaterals(vCollaterals, pindex);
    BOOST_FOREACH (CAmount collateral, vCollaterals)
    {
        int64_t calculatedPayment = GetMasternodePayment(pindex->nHeight, 0, collateral);
        if (calculatedPayment <= masternodePayment)
        {
            return true;
        }
    }
    return false;
}

int64_t GetMasternodePayment(int nHeight, int64_t blockValue, CAmount masternodeCollateral)
{
    if (chainActive.Tip()->nMoneySupply < MAX_MONEY) {
        if (nHeight < V3_START_BLOCK) {
            //! MBK: Set masternode reward phase
            return static_cast<int64_t>(blockValue * 0.677777777777777777); //! ~2/3 masternode stake reward
        } else {
            //! starting V3 masternodes will earn a constant block reward ~60% over the year
            return static_cast<int64_t>(masternodeCollateral * 0.0016);
        }
    }
    return 0;
}

std::string CBlockFileInfo::ToString() const
{
    return strprintf("CBlockFileInfo(blocks=%u, size=%u, heights=%u..%u, time=%s..%s)", nBlocks, nSize, nHeightFirst, nHeightLast, DateTimeStrFormat("%Y-%m-%d", nTimeFirst).c_str(), DateTimeStrFormat("%Y-%m-%d", nTimeLast).c_str());
}

class CMainCleanup
{
public:
    CMainCleanup() {}
    ~CMainCleanup()
    {
        //! block headers
        BlockMap::iterator it1 = mapBlockIndex.begin();
        for (; it1 != mapBlockIndex.end(); it1++)
            delete (*it1).second;
        mapBlockIndex.clear();
        //! orphan transactions
        mapOrphanTransactions.clear();
        mapOrphanBlocks.clear();
        mapOrphanBlocksByPrev.clear();
        setStakeSeenOrphan.clear();
    }
} instance_of_cmaincleanup;
