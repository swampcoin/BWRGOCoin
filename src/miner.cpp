// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017-2018 The XDNA Core developers
// Copyright (c) 2018-2019 The NWO Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "miner.h"

#include "amount.h"
#include "hash.h"
#include "main.h"
#include "masternode-sync.h"
#include "net.h"
#include "pow.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "timedata.h"
#include "util.h"
#include "utilmoneystr.h"
#ifdef ENABLE_WALLET
#include "wallet.h"
#endif
#include "masternode-payments.h"

#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>

using namespace std;

//////////////////////////////////////////////////////////////////////////////
//
// NWOMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest priority or fee rate, so we might consider
// transactions that depend on transactions that aren't yet in the block.
// The COrphan class keeps track of these 'temporary orphans' while
// CreateBlock is figuring out which transactions to include.
//
class COrphan
{
public:
    const CTransaction* ptx;
    set<uint256> setDependsOn;
    CFeeRate feeRate;
    double dPriority;

    COrphan(const CTransaction* ptxIn) : ptx(ptxIn), feeRate(0), dPriority(0)
    {
    }
};

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;
int64_t nLastCoinStakeSearchInterval = 0;

// We want to sort transactions by priority and fee rate, so:
typedef boost::tuple<double, CFeeRate, const CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;

public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) {}

    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee) {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        } else {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();
            return a.get<0>() < b.get<0>();
        }
    }
};

void UpdateTime(CBlockHeader* pblock, const CBlockIndex* pindexPrev)
{
    pblock->nTime = std::max(pindexPrev->GetMedianTimePast() + 1, GetAdjustedTime());
}

CBlockTemplate* CreateNewBlock(const CScript& scriptPubKeyIn, CWallet* pwallet, bool fProofOfStake)
{
    // Create new block
    unique_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());

    if (!pblocktemplate.get())
        return NULL;

    CBlock* pblock = &pblocktemplate->block; // pointer for convenience

    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (Params().MineBlocksOnDemand())
        pblock->nVersion = GetArg("-blockversion", pblock->nVersion);

    // Create coinbase tx
    CMutableTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);
    txNew.vout[0].SetEmpty();

    //LogPrintf("CreateNewBlock() : chainActive.Height() = %s \n", chainActive.Height());
    if (chainActive.Height() >= Params().LAST_POW_BLOCK()) {
      txNew.vout[0].scriptPubKey = scriptPubKeyIn;

    }

    pblock->vtx.push_back(txNew);

    pblocktemplate->vTxFees.push_back(-1);   // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    // ppcoin: if coinstake available add coinstake tx
    static int64_t nLastCoinStakeSearchTime = GetAdjustedTime(); // only initialized at startup

    CMutableTransaction txCoinStake;
    if (fProofOfStake) {
        pblock->vtx[0].vout[0].SetEmpty();
        pblock->vtx.push_back(CTransaction(txCoinStake));
    }

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    unsigned int nBlockMaxSizeNetwork = MAX_BLOCK_SIZE;
    nBlockMaxSize = std::max((unsigned int)1000, std::min((nBlockMaxSizeNetwork - 1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", DEFAULT_BLOCK_MIN_SIZE);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Collect memory pool transactions into the block
    CAmount nFees = 0;

    int nHeight;
    uint64_t nBlockSize = 1000;
    uint64_t nBlockTx = 0;
    int nBlockSigOps = 100;
    bool fSortedByFee = (nBlockPrioritySize <= 0);
    CBlockIndex* pindexPrev;
    {
        LOCK2(cs_main, mempool.cs);

        pindexPrev = chainActive.Tip();
        nHeight = pindexPrev->nHeight + 1;
        CCoinsViewCache view(pcoinsTip);

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;
        bool fPrintPriority = GetBoolArg("-printpriority", false);

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());
        for (map<uint256, CTxMemPoolEntry>::iterator mi = mempool.mapTx.begin();
             mi != mempool.mapTx.end(); ++mi) {
            const CTransaction& tx = mi->second.GetTx();
            if (tx.IsCoinBase() || tx.IsCoinStake() || !IsFinalTx(tx, nHeight))
                continue;

            COrphan* porphan = NULL;
            double dPriority = 0;
            CAmount nTotalIn = 0;
            bool fMissingInputs = false;
            uint256 txid = tx.GetHash();
            for (const CTxIn& txin : tx.vin) {
                // Read prev transaction
                if (!view.HaveCoins(txin.prevout.hash)) {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if (!mempool.mapTx.count(txin.prevout.hash)) {
                        LogPrintf("ERROR: mempool transaction missing input\n");
                        if (fDebug) assert("mempool transaction missing input" == 0);
                        fMissingInputs = true;
                        if (porphan)
                            vOrphan.pop_back();
                        break;
                    }

                    // Has to wait for dependencies
                    if (!porphan) {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }
                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].GetTx().vout[txin.prevout.n].nValue;
                    continue;
                }
                const CCoins* coins = view.AccessCoins(txin.prevout.hash);
                assert(coins);

                CAmount nValueIn = coins->vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = nHeight - coins->nHeight;

                dPriority = double_safe_addition(dPriority, ((double)nValueIn * nConf));

            }
            if (fMissingInputs) continue;

            // Priority is sum(valuein * age) / modified_txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority = tx.ComputePriority(dPriority, nTxSize);

            uint256 hash = tx.GetHash();
            mempool.ApplyDeltas(hash, dPriority, nTotalIn);

            CFeeRate feeRate(nTotalIn - tx.GetValueOut(), nTxSize);

            if (porphan) {
                porphan->dPriority = dPriority;
                porphan->feeRate = feeRate;
            } else
                vecPriority.push_back(TxPriority(dPriority, feeRate, &mi->second.GetTx()));
        }

        // Collect transactions into block
        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty()) {
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            CFeeRate feeRate = vecPriority.front().get<1>();
            const CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
                continue;

            // Legacy limits on sigOps:
            unsigned int nMaxBlockSigOps = MAX_BLOCK_SIGOPS;
            unsigned int nTxSigOps = GetLegacySigOpCount(tx);
            if (nBlockSigOps + nTxSigOps >= nMaxBlockSigOps)
                continue;

            // Skip free transactions if we're past the minimum block size:
            const uint256& hash = tx.GetHash();
            double dPriorityDelta = 0;
            CAmount nFeeDelta = 0;
            mempool.ApplyDeltas(hash, dPriorityDelta, nFeeDelta);
            if (fSortedByFee && (dPriorityDelta <= 0) && (nFeeDelta <= 0) && (feeRate < ::minRelayTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
                continue;

            // Prioritise by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || !AllowFree(dPriority))) {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            if (!view.HaveInputs(tx))
                continue;

            CAmount nTxFees = view.GetValueIn(tx) - tx.GetValueOut();

            nTxSigOps += GetP2SHSigOpCount(tx, view);
            if (nBlockSigOps + nTxSigOps >= nMaxBlockSigOps)
                continue;

            // Note that flags: we don't want to set mempool/IsStandard()
            // policy here, but we still have to ensure that the block we
            // create only contains transactions that are valid in new blocks.
            CValidationState state;
            if (!CheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS, true))
                continue;

            CTxUndo txundo;
            UpdateCoins(tx, state, view, txundo, nHeight);

            // Added
            pblock->vtx.push_back(tx);
            pblocktemplate->vTxFees.push_back(nTxFees);
            pblocktemplate->vTxSigOps.push_back(nTxSigOps);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fPrintPriority) {
                LogPrintf("priority %.1f fee %s txid %s\n",
                    dPriority, feeRate.ToString(), tx.GetHash().ToString());
            }

            // Add transactions that depend on this one to the priority queue
            if (mapDependers.count(hash)) {
                BOOST_FOREACH (COrphan* porphan, mapDependers[hash]) {
                    if (!porphan->setDependsOn.empty()) {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty()) {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->feeRate, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }
    }

    // Compute final transaction.
    if (fProofOfStake) {
        boost::this_thread::interruption_point();
        pblock->nTime = GetAdjustedTime();
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock);
        int64_t nSearchTime = pblock->nTime; // search to current time
        bool fStakeFound = false;
        if (nSearchTime >= nLastCoinStakeSearchTime) {
            unsigned int nTxNewTime = 0;
            if (pwallet->CreateCoinStake(*pwallet, pblock->nTime, pblock->nBits, nSearchTime - nLastCoinStakeSearchTime, txCoinStake, nTxNewTime, nFees)) {
                pblock->nTime = nTxNewTime;

                LogPrintf("CreateNewBlock() if fProofOfStake: chainActive.Height() = %s \n", chainActive.Height());
                pblock->vtx[0].vin[0].scriptSig = CScript() << nHeight << OP_0;  // fixup empty coinbase tx
                pblock->vtx[1] = CTransaction(txCoinStake);
                fStakeFound = true;
            }
            nLastCoinStakeSearchInterval = nSearchTime - nLastCoinStakeSearchTime;
            nLastCoinStakeSearchTime = nSearchTime;
        }

        if (!fStakeFound)
            return nullptr;

    } else {
        txNew.vin[0].scriptSig = CScript() << nHeight << OP_0;
        CAmount block_value = GetBlockValue(nHeight);

        txNew.vout[0].nValue       = block_value + nFees;
        txNew.vout[0].scriptPubKey = scriptPubKeyIn;
        pblocktemplate->vTxFees[0] = -nFees;

        if(nHeight > 1) { // exclude premine
	    // Add NWO Dev and Fund Payments
            CScript scriptDevPubKeyIn  = CScript{} << Params().xNWODevKey() << OP_CHECKSIG;
            CScript scriptFundPubKeyIn = CScript{} << Params().xNWOFundKey() << OP_CHECKSIG;

            auto vDevReward  = block_value * Params().GetDevFee() / 100;
            auto vFundReward = block_value * Params().GetFundFee() / 100;

            txNew.vout.emplace_back(vDevReward, scriptDevPubKeyIn);
            txNew.vout.emplace_back(vFundReward, scriptFundPubKeyIn);

            // Add Masternode Payments
            auto mn_reward = masternodePayments.FillBlockPayee(txNew, block_value, false);

            // Deduct from the mining reward
            txNew.vout[0].nValue -= (vDevReward + vFundReward + mn_reward);
        }
        // Replace the coinbase TX with the modified one
        pblock->vtx[0] = txNew;
    }

    nLastBlockTx = nBlockTx;
    nLastBlockSize = nBlockSize;
    LogPrintf("CreateNewBlock(): total size %u\n", nBlockSize);

    // Fill in header
    pblock->hashPrevBlock = pindexPrev->GetBlockHash();
    if (!fProofOfStake)
        UpdateTime(pblock, pindexPrev);
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock);
    pblock->nNonce = 0;
    pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(pblock->vtx[0]);

    CValidationState state;
    //LogPrintf("CreateNewBlock() if CValidationState: chainActive.Height() = %s \n", chainActive.Height());
      if (!TestBlockValidity(state, *pblock, pindexPrev, false, false)) {
          LogPrintf("CreateNewBlock() : TestBlockValidity failed\n");
          mempool.clear();
          return NULL;
      }

    return pblocktemplate.release();
}

void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock) {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight + 1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = txCoinbase;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}

#ifdef ENABLE_WALLET
//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//
double dHashesPerSec = 0.0;
int64_t nHPSTimerStart = 0;

CBlockTemplate* CreateNewBlockWithKey(CReserveKey& reservekey, CWallet* pwallet, bool fProofOfStake)
{
    CPubKey pubkey;
    if (!reservekey.GetReservedKey(pubkey))
        return NULL;

    const int nHeightNext = chainActive.Tip()->nHeight + 1;
    static int nLastPOWBlock = Params().LAST_POW_BLOCK();
    
    // If we're building a premature PoS block, abort.
    if ((nHeightNext <= nLastPOWBlock) && fProofOfStake) {
        LogPrintf("CreateNewBlockWithKey(): Aborting PoS block creation during PoW phase\n");
        // We will sleep in BitcoinMiner() anyway, if this even gets to this point
        return NULL;
    }

    // If we're building a late PoW block, abort.
    if ((nHeightNext > nLastPOWBlock) && !fProofOfStake) {
        LogPrintf("CreateNewBlockWithKey(): Aborting PoW block creation during PoS phase\n");
        MilliSleep((Params().TargetSpacing() * 1000) >> 1);  // sleep 1/2 a block time so we don't go into a tight loop.
        return NULL;
    }
    
    CScript scriptPubKey = CScript() << ToByteVector(pubkey) << OP_CHECKSIG;
    return CreateNewBlock(scriptPubKey, pwallet, fProofOfStake);
}

bool ProcessBlockFound(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue));

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
            return error("NWOMiner : generated block is stale");
    }

    // Remove key from key pool
    reservekey.KeepKey();

    // Track how many getdata requests this block gets
    {
        LOCK(wallet.cs_wallet);
        wallet.mapRequestCount[pblock->GetHash()] = 0;
    }

    // Process this block the same as if we had received it from another node
    CValidationState state;
    if (!ProcessNewBlock(state, NULL, pblock))
        return error("NWOMiner : ProcessNewBlock, block not accepted");

    for (CNode* node : vNodes) {
        node->PushInventory(CInv(MSG_BLOCK, pblock->GetHash()));
    }

    return true;
}

bool fGenerateBitcoins = false;
bool fMintableCoins = false;
int nMintableLastCheck = 0;

// ***TODO*** that part changed in bitcoin, we are using a mix with old one here for now

void BitcoinMiner(CWallet* pwallet, bool fProofOfStake)
{
    LogPrintf("NWOMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("bwrgocoin-miner");

    // Each thread has its own key and counter
    CReserveKey reservekey(pwallet);
    unsigned int nExtraNonce = 0;

    // do an initial check
    if (fProofOfStake && !nMintableLastCheck)
    {
        nMintableLastCheck = GetTime();
        fMintableCoins = pwallet->MintableCoins();
    }

    while (fGenerateBitcoins || fProofOfStake) {
        if (fProofOfStake) {
            if (chainActive.Tip()->nHeight < Params().LAST_POW_BLOCK()) {
                // The last POW block hasn't even been mined yet.
                MilliSleep(Params().TargetSpacing() * 1000);
                continue;
            }

            // Periodically check if we have mintable coins
            if (GetTime() - nMintableLastCheck > 1 * 60) // 1 minute check time
            {
                nMintableLastCheck = GetTime();
                fMintableCoins = pwallet->MintableCoins();
            }

            while (vNodes.empty() || pwallet->IsLocked() || !fMintableCoins || (pwallet->GetBalance() > 0 && nReserveBalance >= pwallet->GetBalance()) || !masternodeSync.IsSynced()) {
                nLastCoinStakeSearchInterval = 0;
                MilliSleep(5000);
                // recheck if we have mintable coins, while waiting to be stakable.
                if (GetTime() - nMintableLastCheck > 1 * 60) // 1 minute check time
                {
                    nMintableLastCheck = GetTime();
                    fMintableCoins = pwallet->MintableCoins();
                }
            }

            if (mapHashedBlocks.count(chainActive.Tip()->nHeight)) //search our map of hashed blocks, see if bestblock has been hashed yet
            {
                if (GetTime() - mapHashedBlocks[chainActive.Tip()->nHeight] < max(pwallet->nHashInterval, (unsigned int)1)) // wait half of the nHashDrift with max wait of 3 minutes
                {
                    MilliSleep(5000);
                    continue;
                }
            }
        } else { // PoW
            if ((chainActive.Tip()->nHeight - 6) > Params().LAST_POW_BLOCK()) {
                // Run for a little while longer, just in case there is a rewind on the chain.
                LogPrintf("BitcoinMiner(): Exiting Proof of Work Mining Thread at height: %d\n",
                          chainActive.Tip()->nHeight );
                return;
            }
        }

        //
        // Create new block
        //
        unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
        CBlockIndex* pindexPrev = chainActive.Tip();
        if (!pindexPrev)
            continue;

        unique_ptr<CBlockTemplate> pblocktemplate(CreateNewBlockWithKey(reservekey, pwallet, fProofOfStake));
        if (!pblocktemplate.get())
            continue;

        CBlock* pblock = &pblocktemplate->block;
        IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

        //Stake miner main
        if (fProofOfStake) {
            LogPrintf("CPUMiner : proof-of-stake block found %s \n", pblock->GetHash().ToString().c_str());

            if (!pblock->SignBlock(*pwallet)) {
                LogPrintf("BitcoinMiner(): Signing new block failed \n");
                continue;
            }

            LogPrintf("CPUMiner : proof-of-stake block was signed %s \n", pblock->GetHash().ToString().c_str());
            SetThreadPriority(THREAD_PRIORITY_NORMAL);
            ProcessBlockFound(pblock, *pwallet, reservekey);
            SetThreadPriority(THREAD_PRIORITY_LOWEST);

            continue;
        }

        LogPrintf("Running NWOMiner with %u transactions in block (%u bytes)\n", pblock->vtx.size(),
            ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));

        //
        // Search
        //
        int64_t nStart = GetTime();
        uint256 hashTarget = uint256().SetCompact(pblock->nBits);
        while (true) {
            unsigned int nHashesDone = 0;

            uint256 hash;
            while (true) {
                hash = pblock->GetHash();
                if (hash <= hashTarget) {
                    // Found a solution
                    SetThreadPriority(THREAD_PRIORITY_NORMAL);
                    LogPrintf("BitcoinMiner:\n");
                    LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", hash.GetHex(), hashTarget.GetHex());
                    ProcessBlockFound(pblock, *pwallet, reservekey);
                    SetThreadPriority(THREAD_PRIORITY_LOWEST);

                    // In regression test mode, stop mining after a block is found. This
                    // allows developers to controllably generate a block on demand.
                    if (Params().MineBlocksOnDemand())
                        throw boost::thread_interrupted();

                    break;
                }
                pblock->nNonce += 1;
                nHashesDone += 1;
                if ((pblock->nNonce & 0xFF) == 0)
                    break;
            }

            // Meter hashes/sec
            static int64_t nHashCounter;
            if (nHPSTimerStart == 0) {
                nHPSTimerStart = GetTimeMillis();
                nHashCounter = 0;
            } else
                nHashCounter += nHashesDone;
            if (GetTimeMillis() - nHPSTimerStart > 4000) {
                static CCriticalSection cs;
                {
                    LOCK(cs);
                    if (GetTimeMillis() - nHPSTimerStart > 4000) {
                        dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                        nHPSTimerStart = GetTimeMillis();
                        nHashCounter = 0;
                        static int64_t nLogTime;
                        if (GetTime() - nLogTime > 30 * 60) {
                            nLogTime = GetTime();
                            LogPrintf("hashmeter %6.0f khash/s\n", dHashesPerSec / 1000.0);
                        }
                    }
                }
            }

            // Check for stop or if block needs to be rebuilt
            boost::this_thread::interruption_point();
            // Regtest mode doesn't require peers
            if (vNodes.empty() && Params().MiningRequiresPeers())
                break;
            if (pblock->nNonce >= 0xffff0000)
                break;
            if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                break;
            if (pindexPrev != chainActive.Tip())
                break;

            // Update nTime every few seconds
            UpdateTime(pblock, pindexPrev);
        }
    }
}

void static ThreadBitcoinMiner(void* parg)
{
    boost::this_thread::interruption_point();
    CWallet* pwallet = (CWallet*)parg;
    try {
        BitcoinMiner(pwallet, false);
        boost::this_thread::interruption_point();
    } catch (std::exception& e) {
        LogPrintf("ThreadBitcoinMiner() exception");
    } catch (...) {
        LogPrintf("ThreadBitcoinMiner() exception");
    }

    LogPrintf("ThreadBitcoinMiner exiting\n");
}

void GenerateBitcoins(bool fGenerate, CWallet* pwallet, int nThreads)
{
    static boost::thread_group* minerThreads = NULL;
    fGenerateBitcoins = fGenerate;

    if (nThreads < 0) {
        // In regtest threads defaults to 1
        if (Params().DefaultMinerThreads())
            nThreads = Params().DefaultMinerThreads();
        else
            nThreads = boost::thread::hardware_concurrency();
    }

    if (minerThreads != NULL) {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++)
        minerThreads->create_thread(boost::bind(&ThreadBitcoinMiner, pwallet));
}

#endif // ENABLE_WALLET
