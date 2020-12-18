// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <miner.h>

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <timedata.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <util/translation.h>
#include <kernel.h>
#include <net.h>
#include <node/context.h>
#include <node/ui_interface.h>
#include <validation.h>
#include <wallet/wallet.h>
#include <wallet/coincontrol.h>
#include <warnings.h>

#include <algorithm>
#include <utility>

#include <boost/thread.hpp>

int64_t nLastCoinStakeSearchInterval = 0;
int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    AssertLockHeld(cs_main);
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks)
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);

    return nNewTime - nOldTime;
}

void RegenerateCommitments(CBlock& block)
{
    CMutableTransaction tx{*block.vtx.at(0)};
    tx.vout.erase(tx.vout.begin() + GetWitnessCommitmentIndex(block));
    block.vtx.at(0) = MakeTransactionRef(tx);

    GenerateCoinbaseCommitment(block, WITH_LOCK(cs_main, return LookupBlockIndex(block.hashPrevBlock)), Params().GetConsensus());

    block.hashMerkleRoot = BlockMerkleRoot(block);
}

BlockAssembler::Options::Options() {
    blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    nBlockMaxWeight = DEFAULT_BLOCK_MAX_WEIGHT;
}

BlockAssembler::BlockAssembler(const CTxMemPool& mempool, const CChainParams& params, const Options& options)
    : chainparams(params),
      m_mempool(mempool)
{
    blockMinFeeRate = options.blockMinFeeRate;
    // Limit weight to between 4K and MAX_BLOCK_WEIGHT-4K for sanity:
    nBlockMaxWeight = std::max<size_t>(4000, std::min<size_t>(MAX_BLOCK_WEIGHT - 4000, options.nBlockMaxWeight));
}

static BlockAssembler::Options DefaultOptions()
{
    // Block resource limits
    // If -blockmaxweight is not given, limit to DEFAULT_BLOCK_MAX_WEIGHT
    BlockAssembler::Options options;
    options.nBlockMaxWeight = gArgs.GetArg("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT);
    CAmount n = 0;
    if (gArgs.IsArgSet("-blockmintxfee") && ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n)) {
        options.blockMinFeeRate = CFeeRate(n);
    } else {
        options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    }
    return options;
}

BlockAssembler::BlockAssembler(const CTxMemPool& mempool, const CChainParams& params)
    : BlockAssembler(mempool, params, DefaultOptions()) {}

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockWeight = 4000;
    nBlockSigOpsCost = 400;
    fIncludeWitness = false;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}

static inline void FillTreasuryPayee(CMutableTransaction& txNew, const int nHeight, const Consensus::Params& consensusParams)
{
    const CAmount nTreasuryPayment = GetTreasuryPayment(nHeight, consensusParams);

    if (nTreasuryPayment > 0) {
        const std::map<CScript, int>& treasuryPayees = consensusParams.mTreasuryPayees;

        for (const std::pair<CScript, int>& payee : treasuryPayees)
            txNew.vout.emplace_back(nTreasuryPayment * payee.second / 100, payee.first);
    }
}

Optional<int64_t> BlockAssembler::m_last_block_num_txs{nullopt};
Optional<int64_t> BlockAssembler::m_last_block_weight{nullopt};

// peercoin: if pwallet != NULL it will attempt to create coinstake
std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn, CWallet* pwallet, bool* pfPoSCancel)
{
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if(!pblocktemplate.get())
        return nullptr;
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience

    LOCK2(cs_main, m_mempool.cs);
    CBlockIndex* pindexPrev = ::ChainActive().Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    const Consensus::Params &consensusParams = chainparams.GetConsensus();
    const bool fProofOfStake = pwallet != nullptr;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vout.resize(1);
    coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;

    if (!fProofOfStake) {
        coinbaseTx.vout[0].nValue = /*nFees +*/ GetBlockSubsidy(nHeight, false, 0, consensusParams);
        FillTreasuryPayee(coinbaseTx, nHeight, consensusParams);
    }

    // Add dummy coinbase tx as first transaction
    pblocktemplate->entries.emplace_back(CTransactionRef(), -1, -1); // updated at end

    // peercoin: if coinstake available add coinstake tx
    if (fProofOfStake)
        pblocktemplate->entries.emplace_back(CTransactionRef(), -1, -1); // updated at end
    static int64_t nLastCoinStakeSearchTime = GetAdjustedTime(); // only initialized at startup

    pblock->nVersion = ComputeBlockVersion(pindexPrev, fProofOfStake ? CBlockHeader::ALGO_POS : CBlockHeader::ALGO_POW_SHA256, consensusParams);
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = gArgs.GetArg("-blockversion", pblock->nVersion);

    pblock->nTime = GetAdjustedTime();
    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);

    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                       ? nMedianTimePast
                       : pblock->GetBlockTime();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization).
    // Note that the mempool would accept transactions with witness data before
    // IsWitnessEnabled, but we would only ever mine blocks after IsWitnessEnabled
    // unless there is a massive block reorganization with the witness softfork
    // not activated.
    // TODO: replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = IsWitnessEnabled(pindexPrev, consensusParams);

    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    addPackageTxs(nPackagesSelected, nDescendantsUpdated);

    // Ensure that transactions are canonically ordered
    std::sort(std::begin(pblocktemplate->entries) + (fProofOfStake ? 2 : 1),
            std::end(pblocktemplate->entries),
            [](const CBlockTemplateEntry &a, const CBlockTemplateEntry &b) -> bool {
                const uint256 &txbHash = b.tx->GetHash();
                bool txbIsInput = false;
                for (const CTxIn &vin : a.tx->vin) {
                    if (vin.prevout.hash == txbHash) {
                        txbIsInput = true; // one of a.tx's inputs is b.tx, so we cannot put a.tx before b.tx in the block (topological order must be kept)
                        break;
                    }
                }
                return !txbIsInput && a.tx->GetWitnessHash() < b.tx->GetWitnessHash();
            });

    // Copy all the transactions refs into the block
    pblock->vtx.reserve(pblocktemplate->entries.size());
    for (const CBlockTemplateEntry &entry : pblocktemplate->entries) {
        pblock->vtx.push_back(entry.tx);
    }

    if (fProofOfStake) { // attempt to find a coinstake
        *pfPoSCancel = true;
        CMutableTransaction coinstakeTx;
        int64_t nSearchTime = GetAdjustedTime(); // search to current time
        if (nSearchTime > nLastCoinStakeSearchTime) {
            if (CreateCoinStake(coinstakeTx, pblock, pwallet, nHeight, pindexPrev, consensusParams)) {
                if (pblock->nTime > pindexPrev->GetMedianTimePast() && (pblock->nTime & consensusParams.nStakeTimestampMask) == 0) {
                    // make sure coinstake would meet timestamp protocol
                    // as it would be the same as the block timestamp
                    coinbaseTx.vout[0].SetEmpty();
                    pblocktemplate->entries[1].tx = MakeTransactionRef(std::move(coinstakeTx));
                    pblock->vtx[1] = pblocktemplate->entries[1].tx;
                    *pfPoSCancel = false;
                }
            }
            nLastCoinStakeSearchInterval = nSearchTime - nLastCoinStakeSearchTime;
            nLastCoinStakeSearchTime = nSearchTime;
        }
        if (*pfPoSCancel)
            return nullptr; // peercoin: there is no point to continue if we failed to create coinstake
    }

    int64_t nTime1 = GetTimeMicros();

    m_last_block_num_txs = nBlockTx;
    m_last_block_weight = nBlockWeight;

    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    pblocktemplate->entries[0].tx = MakeTransactionRef(std::move(coinbaseTx));
    pblock->vtx[0] = pblocktemplate->entries[0].tx;
    pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, consensusParams);
    pblocktemplate->entries[0].fees = -nFees;

    LogPrintf("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, nFees, nBlockSigOpsCost);

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    if (!fProofOfStake)
        UpdateTime(pblock, consensusParams, pindexPrev);
    pblock->nNonce         = 0;
    pblocktemplate->entries[0].sigOpsCost = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);

    BlockValidationState state;
    if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, state.ToString()));
    }
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCH, "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)\n", 0.001 * (nTime1 - nTimeStart), nPackagesSelected, nDescendantsUpdated, 0.001 * (nTime2 - nTime1), 0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        }
        else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= nBlockMaxWeight)
        return false;
    if (nBlockSigOpsCost + packageSigOpsCost >= MAX_BLOCK_SIGOPS_COST)
        return false;
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - premature witness (in case segwit transactions are added to mempool before
//   segwit activation)
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries& package)
{
    for (CTxMemPool::txiter it : package) {
        if (!IsFinalTx(it->GetTx(), nHeight, nLockTimeCutoff))
            return false;
        if (!fIncludeWitness && it->GetTx().HasWitness())
            return false;
    }
    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblocktemplate->entries.emplace_back(iter->GetSharedTx(), iter->GetFee(), iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("fee %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

int BlockAssembler::UpdatePackagesForAdded(const CTxMemPool::setEntries& alreadyAdded,
        indexed_modified_transaction_set &mapModifiedTx)
{
    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        m_mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc))
                continue;
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCostWithAncestors -= it->GetSigOpCost();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }
    return nDescendantsUpdated;
}

// Skip entries in mapTx that are already in a block or are present
// in mapModifiedTx (which implies that the mapTx ancestor state is
// stale due to ancestor inclusion in the block)
// Also skip transactions that we've already failed to add. This can happen if
// we consider a transaction in mapModifiedTx and it fails: we can then
// potentially consider it again while walking mapTx.  It's currently
// guaranteed to fail again, but as a belt-and-suspenders check we put it in
// failedTx and avoid re-evaluation, since the re-evaluation would be using
// cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx, CTxMemPool::setEntries &failedTx)
{
    assert(it != m_mempool.mapTx.end());
    return mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it);
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs(int &nPackagesSelected, int &nDescendantsUpdated)
{
    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator mi = m_mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != m_mempool.mapTx.get<ancestor_score>().end() || !mapModifiedTx.empty()) {
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != m_mempool.mapTx.get<ancestor_score>().end() &&
            SkipMapTxEntry(m_mempool.mapTx.project<0>(mi), mapModifiedTx, failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == m_mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = m_mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                    CompareTxMemPoolEntryByAncestorFee()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOpsCost = iter->GetSigOpCostWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOpsCost = modit->nSigOpCostWithAncestors;
        }

        if (packageFees < blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOpsCost)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight >
                    nBlockMaxWeight - 4000) {
                // Give up if we're close to full and haven't succeeded in a while
                break;
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        m_mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final
        if (!TestPackageTransactions(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, sortedEntries);

        for (size_t i=0; i<sortedEntries.size(); ++i) {
            AddToBlock(sortedEntries[i]);
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
}

void IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce));
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}


bool CreateCoinStake(CMutableTransaction& coinstakeTx, CBlock* pblock, CWallet* pwallet, const int& nHeight, const CBlockIndex* pindexPrev, const Consensus::Params& consensusParams)
{
    AssertLockHeld(pwallet->cs_wallet);

    bool fKernelFound = false;
    std::set<CInputCoin> setCoins;
    if (pwallet->SelectStakeCoins(setCoins)) {
        while ((pblock->nTime & consensusParams.nStakeTimestampMask) != 0)
            pblock->nTime++;

        CAmount nCredit = 0;
        CScript scriptPubKeyKernel;
        for (const auto& pcoin : setCoins) {
            if (::ChainActive().Height() != pindexPrev->nHeight)
                break;

            CCoinsViewCache view(&::ChainstateActive().CoinsTip());
            const COutPoint& prevout = pcoin.outpoint;
            Coin coin;

            if (!view.GetCoin(prevout, coin)) {
                LogPrintf("%s : failed to find stake input %s in UTXO set\n", __func__, pcoin.outpoint.hash.ToString());
                continue;
            }

            const CBlockIndex* pindexFrom = ::ChainActive()[coin.nHeight];
            if (!pindexFrom) {
                LogPrintf("%s : block index not found\n", __func__);
                continue;
            }

            if (pindexFrom->GetBlockTime() + consensusParams.nStakeMinAge > pblock->nTime || nHeight - pindexFrom->nHeight < consensusParams.nStakeMinDepth)
                continue; // only count coins meeting min age/depth requirement

            unsigned int nInterval = 0;
            uint256 hashProofOfStake;
            if (CheckStakeKernelHash(pblock->nBits, pindexPrev, pindexFrom, pcoin.txout, pindexFrom->GetBlockTime(), prevout, pblock->nTime, nInterval, false, hashProofOfStake, gArgs.GetBoolArg("-debug", false))) {
                // Found a kernel
                if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printcoinstake", false))
                    LogPrintf("CreateCoinStake : kernel found\n");
                //LogPrintf("proof-of-stake found\n   hash: %s\n target: %s\n   bits: %08x\n", hashProofOfStake.ToString(), (arith_uint256().SetCompact(pblock->nBits) * arith_uint256(pcoin.txout.nValue)).ToString(), pblock->nBits);
                std::vector<std::vector<unsigned char>> vSolutions;
                CScript scriptPubKeyOut;
                scriptPubKeyKernel = pcoin.txout.scriptPubKey;
                TxoutType whichType = Solver(scriptPubKeyKernel, vSolutions);

                if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printcoinstake", false))
                    LogPrintf("CreateCoinStake : parsed kernel type=%s\n", GetTxnOutputType(whichType));
                if (whichType == TxoutType::PUBKEYHASH || whichType == TxoutType::WITNESS_V0_KEYHASH) { // pay to address type or witness keyhash
                    // convert to pay to public key type
                    CKey key;
                    if (!pwallet->GetLegacyScriptPubKeyMan()->GetKey(CKeyID(uint160(vSolutions[0])), key)) {
                        if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printcoinstake", false))
                            LogPrintf("CreateCoinStake : failed to get key for kernel type=%s\n", GetTxnOutputType(whichType));
                        continue;  // unable to find corresponding public key
                    }
                    scriptPubKeyOut << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;
                } else if (whichType == TxoutType::PUBKEY) {
                    scriptPubKeyOut = scriptPubKeyKernel;
                } else {
                    if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printcoinstake", false))
                        LogPrintf("CreateCoinStake : no support for kernel type=%s\n", GetTxnOutputType(whichType));
                    continue;  // only support pay to public key and pay to address and pay to witness keyhash
                }

                coinstakeTx.vin.push_back(CTxIn(pcoin.outpoint.hash, pcoin.outpoint.n));
                nCredit += pcoin.txout.nValue;
                coinstakeTx.vout.push_back(CTxOut(0, CScript()));
                if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printcoinstake", false))
                    LogPrintf("CreateCoinStake : added kernel type=%s\n", GetTxnOutputType(whichType));

                uint64_t nCoinAge = 0;
                if (!GetCoinAge((const CTransaction)coinstakeTx, view, pblock->nTime, nHeight, nCoinAge))
                    return error("%s : failed to calculate coin age", __func__);

                CAmount nReward = GetBlockSubsidy(nHeight, true, nCoinAge, consensusParams);
                // Refuse to create mint that has zero or negative reward
                if (nReward <= 0)
                    return false;
                nCredit += nReward;
                coinstakeTx.vout.push_back(CTxOut(nCredit, scriptPubKeyOut));

                // Add treasury payment
                FillTreasuryPayee(coinstakeTx, nHeight, consensusParams);

                // Sign
                int nIn = 0;
                if (!SignSignature(*pwallet->GetLegacyScriptPubKeyMan(), scriptPubKeyKernel, coinstakeTx, nIn++, pcoin.effective_value, SIGHASH_ALL))
                    return error("CreateCoinStake : failed to sign coinstake");

                fKernelFound = true;
            }
            if (fKernelFound)
                break; // if kernel is found stop searching
        }
    }

    return fKernelFound;
}

static inline bool ProcessBlockFound(const CBlock* pblock, const CChainParams& chainparams, ChainstateManager* chainman)
{
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("generated %s\n", FormatMoney(pblock->IsProofOfStake() ? pblock->vtx[1]->GetValueOut() : pblock->vtx[0]->GetValueOut()));

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != ::ChainActive().Tip()->GetBlockHash())
            return error("BitcoinMiner: generated block is stale");
    }

    // Process this block the same as if we had received it from another node
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(*pblock);
    if (!chainman->ProcessNewBlock(chainparams, shared_pblock, true, nullptr))
        return error("ProcessNewBlock, block not accepted");

    return true;
}

static inline void PoSMiner(std::shared_ptr<CWallet> pwallet, ChainstateManager* chainman, CConnman* connman, CTxMemPool* mempool)
{
    LogPrintf("CPUMiner started for proof-of-stake\n");
    util::ThreadRename("bitcoin-stake-minter");

    unsigned int nExtraNonce = 0;

    // Compute timeout for pos as sqrt(numUTXO)
    unsigned int pos_timio;
    {
        LOCK(pwallet->cs_wallet);

        std::vector<COutput> vCoins;
        CCoinControl coincontrol;
        pwallet->AvailableCoins(vCoins, false, &coincontrol);
        pos_timio = gArgs.GetArg("-staketimio", 500) + 30 * sqrt(vCoins.size());
        LogPrintf("Set proof-of-stake timeout: %ums for %u UTXOs\n", pos_timio, vCoins.size());
    }

    std::string strMintMessage = _("Info: Minting suspended due to locked wallet.").translated;
    std::string strMintSyncMessage = _("Info: Minting suspended while synchronizing wallet.").translated;
    std::string strMintDisabledMessage = _("Info: Minting disabled by 'nominting' option.").translated;
    std::string strMintBlockMessage = _("Info: Minting suspended due to block creation failure.").translated;
    std::string strMintEmpty = "";
    if (!gArgs.GetBoolArg("-minting", true) || !gArgs.GetBoolArg("-staking", true)) {
        strMintWarning = strMintDisabledMessage;
        LogPrintf("proof-of-stake minter disabled\n");
        return;
    }

    try {
        bool fNeedToClear = false;
        while (true) {
            while (pwallet->IsLocked()) {
                if (strMintWarning != strMintMessage) {
                    strMintWarning = strMintMessage;
                    uiInterface.NotifyAlertChanged();
                }
                fNeedToClear = true;
                if (!connman->interruptNet.sleep_for(std::chrono::seconds(3)))
                    return;
            }

            if (Params().NetworkIDString() != CBaseChainParams::REGTEST) { // Params().MiningRequiresPeers()
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                while (connman == nullptr || connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0 || ::ChainstateActive().IsInitialBlockDownload()) {
                    if (!connman->interruptNet.sleep_for(std::chrono::seconds(10)))
                        return;
                }
            }

            while (GuessVerificationProgress(Params().TxData(), ::ChainActive().Tip()) < 0.996) {
                LogPrintf("Minter thread sleeps while sync at %f\n", GuessVerificationProgress(Params().TxData(), ::ChainActive().Tip()));
                if (strMintWarning != strMintSyncMessage) {
                    strMintWarning = strMintSyncMessage;
                    uiInterface.NotifyAlertChanged();
                }
                fNeedToClear = true;
                if (!connman->interruptNet.sleep_for(std::chrono::seconds(10)))
                    return;
            }
            if (fNeedToClear) {
                strMintWarning = strMintEmpty;
                uiInterface.NotifyAlertChanged();
                fNeedToClear = false;
            }

            //
            // Create new block
            //
            CBlockIndex* pindexPrev = ::ChainActive().Tip();
            bool fPoSCancel = false;
            CBlock *pblock;
            std::unique_ptr<CBlockTemplate> pblocktemplate;

            {
                LOCK(pwallet->cs_wallet);

                pblocktemplate = BlockAssembler(*mempool, Params()).CreateNewBlock(CScript(), pwallet.get(), &fPoSCancel);
            }

            if (!pblocktemplate.get()) {
                if (fPoSCancel == true) {
                    if (!connman->interruptNet.sleep_for(std::chrono::milliseconds(pos_timio)))
                        return;
                    continue;
                }
                strMintWarning = strMintBlockMessage;
                uiInterface.NotifyAlertChanged();
                LogPrintf("Error in BitcoinMiner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                if (!connman->interruptNet.sleep_for(std::chrono::seconds(10)))
                   return;

                return;
            }
            pblock = &pblocktemplate->block;
            IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

            // peercoin: if proof-of-stake block found then process block
            if (pblock->IsProofOfStake()) {
                {
                    LOCK(pwallet->cs_wallet);
                    if (!pwallet->SignBlock(*pblock)) {
                        LogPrintf("PoSMiner(): failed to sign PoS block");
                        continue;
                    }
                }
                LogPrintf("CPUMiner : proof-of-stake block found %s\n", pblock->GetHash().ToString());
                ProcessBlockFound(pblock, Params(), chainman);
                // Rest for ~3 minutes after successful block to preserve close quick
                if (!connman->interruptNet.sleep_for(std::chrono::seconds(60 + GetRand(4))))
                    return;
            }
            if (!connman->interruptNet.sleep_for(std::chrono::milliseconds(pos_timio)))
                return;

            continue;
        }
    } catch (boost::thread_interrupted) {
        LogPrintf("BitcoinMiner terminated\n");
        return;
        // throw;
    } catch (const std::runtime_error &e) {
        LogPrintf("BitcoinMiner runtime error: %s\n", e.what());
        return;
    }
}

// peercoin: stake minter thread
static void ThreadStakeMinter(std::shared_ptr<CWallet> pwallet, ChainstateManager* chainman, CConnman* connman, CTxMemPool* mempool)
{
    LogPrintf("ThreadStakeMinter started\n");
    try {
        PoSMiner(pwallet, chainman, connman, mempool);
    } catch (std::exception& e) {
        PrintExceptionContinue(&e, "ThreadStakeMinter()");
    } catch (...) {
        PrintExceptionContinue(NULL, "ThreadStakeMinter()");
    }
    LogPrintf("ThreadStakeMinter exiting\n");
}

// peercoin: stake minter
void MintStake(boost::thread_group& threadGroup, std::shared_ptr<CWallet> pwallet, ChainstateManager* chainman, CConnman* connman, CTxMemPool* mempool)
{
    // peercoin: mint proof-of-stake blocks in the background
    threadGroup.create_thread(boost::bind(&ThreadStakeMinter, pwallet, chainman, connman, mempool));
}
