// Copyright (c) 2012-2020 The Peercoin developers
// Copyright (c) 2015-2019 The PIVX developers
// Copyright (c) 2020 ComputerCraftr
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/validation.h>
#include <hash.h>
#include <kernel.h>
#include <policy/policy.h>
#include <random.h>
#include <script/interpreter.h>
#include <streams.h>
#include <timedata.h>
#include <txdb.h>
#include <util/system.h>
#include <validation.h>

#include <boost/assign/list_of.hpp>

// Hard checkpoints of stake modifiers to ensure they are deterministic
static std::map<int, unsigned int> mapStakeModifierCheckpoints =
    boost::assign::map_list_of
    ( 0, 0xfd11f4e7u )
    ;

static std::map<int, unsigned int> mapStakeModifierTestnetCheckpoints =
    boost::assign::map_list_of
    ( 0, 0xfd11f4e7u )
    ;

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex)
        return error("GetLastStakeModifier: null pindex");
    while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier())
        pindex = pindex->pprev;
    if (!pindex->GeneratedStakeModifier())
        return error("GetLastStakeModifier: no generation at genesis block");
    nStakeModifier = pindex->nStakeModifier;
    nModifierTime = pindex->GetBlockTime();
    return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert(nSection >= 0 && nSection < 64);
    return (Params().GetConsensus().nModifierInterval * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1))));
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionInterval()
{
    int64_t nSelectionInterval = 0;
    for (int nSection=0; nSection<64; nSection++)
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    return nSelectionInterval;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(
    std::vector<std::pair<int64_t, uint256> >& vSortedByTimestamp,
    std::map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop, uint64_t nStakeModifierPrev,
    const CBlockIndex** pindexSelected)
{
    bool fSelected = false;
    arith_uint256 hashBest = 0;
    *pindexSelected = (const CBlockIndex*) 0;
    for (const auto& item : vSortedByTimestamp)
    {
        const CBlockIndex* pindex = LookupBlockIndex(item.second);
        if (!pindex)
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString());
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;
        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;

        // compute the selection hash by hashing an input that is unique to that block
        uint256 hashProof = pindex->GetBlockHash();

        CDataStream ss(SER_GETHASH, 0);
        ss << hashProof << nStakeModifierPrev;
        arith_uint256 hashSelection = UintToArith256(Hash(ss.begin(), ss.end()));
        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
            hashSelection >>= 32;
        if (fSelected && hashSelection < hashBest)
        {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
        else if (!fSelected)
        {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
    }
    if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printstakemodifier", false))
        LogPrintf("SelectBlockFromCandidates: selection hash=%s\n", hashBest.ToString());
    return fSelected;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex* pindexCurrent, uint64_t &nStakeModifier, bool& fGeneratedStakeModifier)
{
    const Consensus::Params& params = Params().GetConsensus();
    const CBlockIndex* pindexPrev = pindexCurrent->pprev;
    nStakeModifier = 0;
    fGeneratedStakeModifier = false;
    if (!pindexPrev)
    {
        fGeneratedStakeModifier = true;
        return true; // genesis block's modifier is 0
    }
    if (pindexPrev->nHeight == 0 || Params().NetworkIDString() == CBaseChainParams::REGTEST) {
        // Give a stake modifier to the first block - fixed stake modifier only for regtest
        fGeneratedStakeModifier = true;
        nStakeModifier = 0x7374616b656d6f64; // stakemod
        return true;
    }

    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
        return error("ComputeNextStakeModifier: unable to get last modifier");
    if (gArgs.GetBoolArg("-debug", false))
        LogPrintf("ComputeNextStakeModifier: prev modifier=0x%016x time=%s epoch=%u\n", nStakeModifier, FormatISO8601DateTime(nModifierTime), (unsigned int)nModifierTime);
    if (nModifierTime / params.nModifierInterval >= pindexPrev->GetBlockTime() / params.nModifierInterval)
    {
        if (gArgs.GetBoolArg("-debug", false))
            LogPrintf("ComputeNextStakeModifier: no new interval keep current modifier: pindexPrev nHeight=%d nTime=%u\n", pindexPrev->nHeight, (unsigned int)pindexPrev->GetBlockTime());
        return true;
    }
    /*if (nModifierTime / params.nModifierInterval >= pindexCurrent->GetBlockTime() / params.nModifierInterval)
    {
        // v0.4+ requires current block timestamp also be in a different modifier interval
        if (IsProtocolV04(pindexCurrent->nTime))
        {
            if (gArgs.GetBoolArg("-debug", false))
                LogPrintf("ComputeNextStakeModifier: (v0.4+) no new interval keep current modifier: pindexCurrent nHeight=%d nTime=%u\n", pindexCurrent->nHeight, (unsigned int)pindexCurrent->GetBlockTime());
            return true;
        }
        else
        {
            if (gArgs.GetBoolArg("-debug", false))
                LogPrintf("ComputeNextStakeModifier: v0.3 modifier at block %s not meeting v0.4+ protocol: pindexCurrent nHeight=%d nTime=%u\n", pindexCurrent->GetBlockHash().ToString(), pindexCurrent->nHeight, (unsigned int)pindexCurrent->GetBlockTime());
        }
    }*/

    // Sort candidate blocks by timestamp
    std::vector<std::pair<int64_t, uint256> > vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * params.nModifierInterval / (2 * params.nPowTargetSpacing)); // PoS spacing is 160 seconds
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / params.nModifierInterval) * params.nModifierInterval - nSelectionInterval;
    const CBlockIndex* pindex = pindexPrev;
    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart)
    {
        vSortedByTimestamp.push_back(std::make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }
    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;

    // Shuffle before sort
    for(int i = vSortedByTimestamp.size() - 1; i > 1; --i)
    std::swap(vSortedByTimestamp[i], vSortedByTimestamp[GetRand(i)]);

    sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end(), [] (const std::pair<int64_t, uint256> &a, const std::pair<int64_t, uint256> &b)
    {
        if (a.first != b.first)
            return a.first < b.first;
        // Timestamp equals - compare block hashes
        const uint32_t *pa = a.second.GetDataPtr();
        const uint32_t *pb = b.second.GetDataPtr();
        int cnt = 256 / 32;
        do {
            --cnt;
            if (pa[cnt] != pb[cnt])
                return pa[cnt] < pb[cnt];
        } while(cnt);
            return false; // Elements are equal
    });

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    std::map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound = 0; nRound < std::min(64, (int)vSortedByTimestamp.size()); nRound++)
    {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);
        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex))
            return error("ComputeNextStakeModifier: unable to select block at round %d", nRound);
        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);
        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(std::make_pair(pindex->GetBlockHash(), pindex));
        if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printstakemodifier", false))
            LogPrintf("ComputeNextStakeModifier: selected round %d stop=%s height=%d bit=%d\n",
                nRound, FormatISO8601DateTime(nSelectionIntervalStop), pindex->nHeight, pindex->GetStakeEntropyBit());
    }

    // Print selection map for visualization of the selected blocks
    if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printstakemodifier", false))
    {
        std::string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected
        strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
        pindex = pindexPrev;
        while (pindex && pindex->nHeight >= nHeightFirstCandidate)
        {
            // '=' indicates proof-of-stake blocks not selected
            if (pindex->IsProofOfStake())
                strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
            pindex = pindex->pprev;
        }
        for (const auto& item : mapSelectedBlocks)
        {
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake() ? "S" : "W");
        }
        LogPrintf("ComputeNextStakeModifier: selection height [%d, %d] map %s\n", nHeightFirstCandidate, pindexPrev->nHeight, strSelectionMap);
    }
    if (gArgs.GetBoolArg("-debug", false))
        LogPrintf("ComputeNextStakeModifier: new modifier=0x%016x time=%s\n", nStakeModifierNew, FormatISO8601DateTime(pindexPrev->GetBlockTime()));

    nStakeModifier = nStakeModifierNew;
    fGeneratedStakeModifier = true;
    return true;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint64_t ComputeStakeModifierV2(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return 0; // genesis block's modifier is 0
    if (pindexPrev->nHeight == 0 || Params().NetworkIDString() == CBaseChainParams::REGTEST) {
        // Give a stake modifier to the first block - fixed stake modifier only for regtest
        return 0x7374616b656d6f64; // stakemod
    }

    CHashWriter ss(SER_GETHASH, 0);
    ss << kernel;

    // switch with old modifier on upgrade block - PIVX
    //if (!Params().IsStakeModifierV2(pindexPrev->nHeight + 1))
        ss << pindexPrev->nStakeModifier;
    //else
        //ss << pindexPrev->nStakeModifierV2;

    return UintToArith256(ss.GetHash()).GetLow64();
}

// V0.5: Stake modifier used to hash for a stake kernel is chosen as the stake
// modifier that is (nStakeMinAge minus a selection interval) earlier than the
// stake, thus at least a selection interval later than the coin generating the
// kernel, as the generating coin is from at least nStakeMinAge ago.
static bool GetKernelStakeModifierV05(CBlockIndex* pindexPrev, unsigned int nTimeTx, const Consensus::Params& params, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    //const Consensus::Params& params = Params().GetConsensus();
    const CBlockIndex* pindex = pindexPrev;
    nStakeModifierHeight = pindex->nHeight;
    nStakeModifierTime = pindex->GetBlockTime();
    int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();

    if (nStakeModifierTime + params.nStakeMinAge[1] - nStakeModifierSelectionInterval <= (int64_t)nTimeTx)
    {
        // Best block is still more than
        // (nStakeMinAge minus a selection interval) older than kernel timestamp
        if (fPrintProofOfStake)
            return error("GetKernelStakeModifier() : best block %s at height %d too old for stake",
                pindex->GetBlockHash().ToString(), pindex->nHeight);
        else
            return false;
    }
    // loop to find the stake modifier earlier by
    // (nStakeMinAge minus a selection interval)
    while (nStakeModifierTime + params.nStakeMinAge[1] - nStakeModifierSelectionInterval > (int64_t)nTimeTx)
    {
        if (!pindex->pprev)
        {   // reached genesis block; should not happen
            return error("GetKernelStakeModifier() : reached genesis block");
        }
        pindex = pindex->pprev;
        if (pindex->GeneratedStakeModifier())
        {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
    }
    nStakeModifier = pindex->nStakeModifier;
    return true;
}

// V0.3: Stake modifier used to hash for a stake kernel is chosen as the stake
// modifier about a selection interval later than the coin generating the kernel
//
// This stake kernel is vulnerable to grinding because the selected stake modifier for a given input will never change, so
// the input can be resent in an attempt to get a more favorable kernel if it is determined that the input will not produce
// a stake (generate a small enough hashProofOfStake) within a reasonable amount of time (nTimeTx not too far in the future)
static bool GetKernelStakeModifierV03(CBlockIndex* pindexPrev, uint256 hashBlockFrom, const Consensus::Params& params, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    //const Consensus::Params& params = Params().GetConsensus();
    nStakeModifier = 0;
    const CBlockIndex* pindexFrom = LookupBlockIndex(hashBlockFrom);
    if (!pindexFrom)
        return error("GetKernelStakeModifier() : block not indexed");
    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->GetBlockTime();
    int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();

    // we need to iterate index forward but we cannot depend on ::ChainActive().Next()
    // because there is no guarantee that we are checking blocks in active chain.
    // So, we construct a temporary chain that we will iterate over.
    // pindexFrom - this block contains coins that are used to generate PoS
    // pindexPrev - this is a block that is previous to PoS block that we are checking, you can think of it as tip of our chain
    std::vector<CBlockIndex*> tmpChain;
    int32_t nDepth = pindexPrev->nHeight - (pindexFrom->nHeight-1); // -1 is used to also include pindexFrom
    tmpChain.reserve(nDepth);
    CBlockIndex* it = pindexPrev;
    for (int i=1; i<=nDepth && !::ChainActive().Contains(it); i++) {
        tmpChain.push_back(it);
        it = it->pprev;
    }
    std::reverse(tmpChain.begin(), tmpChain.end());
    size_t n = 0;

    const CBlockIndex* pindex = pindexFrom;
    // loop to find the stake modifier later by a selection interval
    while (nStakeModifierTime < pindexFrom->GetBlockTime() + nStakeModifierSelectionInterval)
    {
        const CBlockIndex* old_pindex = pindex;
        pindex = (!tmpChain.empty() && pindex->nHeight >= tmpChain[0]->nHeight - 1) ? tmpChain[n++] : ::ChainActive().Next(pindex);
        if (n > tmpChain.size() || pindex == NULL) // check if tmpChain[n+1] exists
        {   // reached best block; may happen if node is behind on block chain
            if (fPrintProofOfStake || (old_pindex->GetBlockTime() + params.nStakeMinAge[1] - nStakeModifierSelectionInterval > GetAdjustedTime()))
                return error("GetKernelStakeModifier() : reached best block %s at height %d from block %s",
                    old_pindex->GetBlockHash().ToString(), old_pindex->nHeight, hashBlockFrom.ToString());
            else
                return false;
        }
        if (pindex->GeneratedStakeModifier())
        {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
    }
    nStakeModifier = pindex->nStakeModifier;
    return true;
}

uint256 stakeHash(unsigned int nTimeTx, CDataStream ss, unsigned int prevoutIndex, uint256 prevoutHash, unsigned int nTimeBlockFrom)
{
    //Pivx will hash in the transaction hash and the index number in order to make sure each hash is unique
    ss << nTimeBlockFrom << prevoutIndex << prevoutHash << nTimeTx;
    return Hash(ss.begin(), ss.end());
}

// Test hash vs target
bool stakeTargetHit(const uint256& hashProofOfStake, int64_t nValueIn, const arith_uint256& bnTargetPerCoinDay, bool fNewWeight)
{
    // Get the stake weight - weight is equal to coin amount
    arith_uint256 bnCoinDayWeight = fNewWeight ? arith_uint256(nValueIn) : (arith_uint256(nValueIn) / 100);

    // Now check if proof-of-stake hash meets target protocol
    return (UintToArith256(hashProofOfStake) <= bnCoinDayWeight * bnTargetPerCoinDay);
}

// Get the stake modifier specified by the protocol to hash for a stake kernel
bool GetKernelStakeModifier(CBlockIndex* pindexPrev, uint256 hashBlockFrom, unsigned int nTimeTx, const Consensus::Params& params, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    // Hash the modifier - PIVX
    /*if (Params().IsStakeModifierV2(pindexPrev->nHeight + 1)) {
        // Modifier v2
        //modifier_ss << pindexPrev->nStakeModifierV2;
        nStakeModifier = pindexPrev->nStakeModifier;
        nStakeModifierHeight = pindexPrev->nHeight;
        nStakeModifierTime = pindexPrev->GetBlockTime();
        return true;
    }*/

    // Peercoin stake modifier selection for kernel
    if (pindexPrev->nHeight + 1 >= params.nMandatoryUpgradeBlock[1]) //IsProtocolV05(nTimeTx)
        return GetKernelStakeModifierV05(pindexPrev, nTimeTx, params, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake);
    else {
        // This is only here for backwards compatibility with very old PIVX forks; it should not be used in new production code due to the
        // stake grinding vulnerability (it can be replaced by hard coded or bypassed modifiers on old blocks when it is no longer being used)
        return GetKernelStakeModifierV03(pindexPrev, hashBlockFrom, params, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake);
    }
}

// peercoin kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.offset + txPrev.nTime + txPrev.vout.n + nTime) < bnTarget * nCoinDayWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier:
//       (v0.5) uses dynamic stake modifier around 21 days before the kernel,
//              versus static stake modifier about 9 days after the staked
//              coin (txPrev) used in v0.3
//       (v0.3) scrambles computation to make it very difficult to precompute
//              future proof-of-stake at the time of the coin's confirmation
//       (v0.2) nBits (deprecated): encodes all past block timestamps
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.offset: offset of txPrev inside block, to reduce the chance of
//                  nodes generating coinstake at the same time
//   txPrev.nTime: reduce the chance of nodes generating coinstake at the same
//                 time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
// Instead of looping outside and reinitializing variables many times, we will give a nTimeTx and also search interval so that we can do all the hashing here
bool CheckStakeKernelHash(const unsigned int& nBits, CBlockIndex* pindexPrev, const CBlockIndex* pindexFrom, const CTransactionRef& txPrev, const COutPoint& prevout, unsigned int& nTimeTx, unsigned int nHashDrift, bool fCheck, uint256& hashProofOfStake, bool fPrintProofOfStake)
{
    const Consensus::Params& params = Params().GetConsensus();
    int nHeightCurrent = pindexPrev->nHeight + 1;
    // Assign new variables to make it easier to read
    int64_t nValueIn = txPrev->vout[prevout.n].nValue;
    unsigned int nTimeBlockFrom = pindexFrom->GetBlockTime();
    int nHeightBlockFrom = pindexFrom->nHeight;
    int64_t nStakeMinAge = nHeightCurrent >= params.nMandatoryUpgradeBlock[1] ? params.nStakeMinAge[1] : params.nStakeMinAge[0];
    int nStakeMinDepth = nHeightCurrent >= params.nMandatoryUpgradeBlock[0] ? params.nStakeMinDepth[1] : params.nStakeMinDepth[0];

    if (nTimeTx < nTimeBlockFrom) // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    if (nTimeBlockFrom + nStakeMinAge > nTimeTx || nHeightCurrent - nHeightBlockFrom < nStakeMinDepth) // Min age requirement
        return error("CheckStakeKernelHash() : min age violation - height=%d - nHeightBlockFrom=%d nTimeBlockFrom=%d nStakeMinAge=%d nTimeTx=%d", nHeightCurrent, nHeightBlockFrom, nTimeBlockFrom, nStakeMinAge, nTimeTx);

    // Grab difficulty
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTargetPerCoinDay == 0 || fOverflow || bnTargetPerCoinDay > UintToArith256(params.powLimit[0]))
        return false;

    // Create data stream once instead of repeating it in the loop
    CDataStream ss(SER_GETHASH, 0);
    // Grab stake modifier
    uint64_t nStakeModifier = 0;
    int nStakeModifierHeight = 0;
    int64_t nStakeModifierTime = 0;
    //if (IsProtocolV03(nTimeTx))  // v0.3 protocol
    {
        if (!GetKernelStakeModifier(pindexPrev, pindexFrom->GetBlockHash(), nTimeTx, params, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake)) {
            LogPrintf("CheckStakeKernelHash() : failed to get kernel stake modifier\n");
            return false;
        }
        ss << nStakeModifier;
    }
    /*else // v0.2 protocol
    {
        ss << nBits;
    }*/

    // If wallet is simply checking to make sure a hash is valid
    if (fCheck) {
        hashProofOfStake = stakeHash(nTimeTx, ss, prevout.n, prevout.hash, nTimeBlockFrom);
        if (gArgs.GetBoolArg("-debug", false) || fPrintProofOfStake) {
            //if (IsProtocolV03(nTimeTx))
                LogPrintf("CheckStakeKernelHash() : using modifier 0x%016x at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
                    nStakeModifier, nStakeModifierHeight,
                    FormatISO8601DateTime(nStakeModifierTime),
                    nHeightBlockFrom,
                    FormatISO8601DateTime(nTimeBlockFrom));
            LogPrintf("CheckStakeKernelHash() : check protocol=%s modifier=0x%016x nTimeBlockFrom=%u prevoutHash=%s nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
                //IsProtocolV05(nTimeTx) ? "0.5" : (IsProtocolV03(nTimeTx) ? "0.3" : "0.2"),
                //IsProtocolV03(nTimeTx) ? nStakeModifier : (uint64_t) nBits,
                nHeightCurrent >= params.nMandatoryUpgradeBlock[1] ? "0.5" : "0.3", nStakeModifier,
                nTimeBlockFrom, prevout.hash.ToString(), nTimeBlockFrom, prevout.n, nTimeTx,
                hashProofOfStake.ToString());
        }
        // Bypass PoS checks on historic blocks created by old wallets
        return stakeTargetHit(hashProofOfStake, nValueIn, bnTargetPerCoinDay, nHeightCurrent >= params.nMandatoryUpgradeBlock[1]) || (nHeightCurrent < params.nMandatoryUpgradeBlock[0] && params.hashGenesisBlock == uint256S("0xf4bbfc518aa3622dbeb8d2818a606b82c2b8b1ac2f28553ebdb6fc04d7abaccf"));
    }

    // nHashDrift should be <= MAX_FUTURE_BLOCK_TIME otherwise we risk creating a block which will be rejected due to nTimeTx being too far in the future
    bool fSuccess = false;
    unsigned int nTryTime = 0;
    int nHeightStart = nHeightCurrent - 1;
    int iteration = nHeightCurrent >= params.nMandatoryUpgradeBlock[1] ? params.nStakeTimestampMask + 1 : 1; // 16 second time slots for 0xf masked time
    assert((nHashDrift & params.nStakeTimestampMask) == 0);
    for (int i = nHashDrift; i >= 0; i-=iteration) //iterate the hashing
    {
        // New block came in, move on
        if (::ChainActive().Height() != nHeightStart)
            break;

        // Hash this iteration - start at nHashDrift and work backwards to nTimeTx
        nTryTime = nTimeTx + i; //nTimeTx + nHashDrift - i;
        hashProofOfStake = stakeHash(nTryTime, ss, prevout.n, prevout.hash, nTimeBlockFrom);

        // If stake hash does not meet the target then continue to next iteration
        if (!stakeTargetHit(hashProofOfStake, nValueIn, bnTargetPerCoinDay, nHeightCurrent >= params.nMandatoryUpgradeBlock[1]))
            continue;

        fSuccess = true; // If we make it this far then we have successfully created a stake hash
        nTimeTx = nTryTime;

        if (gArgs.GetBoolArg("-debug", false) || fPrintProofOfStake) {
            //if (IsProtocolV03(nTimeTx))
                LogPrintf("CheckStakeKernelHash() : using modifier 0x%016x at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
                    nStakeModifier, nStakeModifierHeight,
                    FormatISO8601DateTime(nStakeModifierTime),
                    nHeightBlockFrom,
                    FormatISO8601DateTime(nTimeBlockFrom));
            LogPrintf("CheckStakeKernelHash() : pass protocol=%s modifier=0x%016x nTimeBlockFrom=%u prevoutHash=%s nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
                //IsProtocolV03(nTimeTx) ? "0.3" : "0.2",
                //IsProtocolV03(nTimeTx) ? nStakeModifier : (uint64_t) nBits,
                nHeightCurrent >= params.nMandatoryUpgradeBlock[1] ? "0.5" : "0.3", nStakeModifier,
                nTimeBlockFrom, prevout.hash.ToString(), nTimeBlockFrom, prevout.n, nTryTime,
                hashProofOfStake.ToString());
        }
        break;
    }

    return fSuccess;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CValidationState &state, CBlockIndex* pindexPrev, const CTransactionRef& tx, const unsigned int& nBits, unsigned int nTimeTx, uint256& hashProofOfStake)
{
    if (!tx->IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx->GetHash().ToString());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx->vin[0];

    // Transaction index is required to get to block header
    //if (!fTxIndex)
        //return error("CheckProofOfStake() : transaction index not available");

    // Get transaction index for the previous transaction
    const Consensus::Params& params = Params().GetConsensus();
    uint256 hashBlock;
    CTransactionRef txPrev;
    if (!GetTransaction(txin.prevout.hash, txPrev, params, hashBlock, true, nullptr))
        return error("CheckProofOfStake() : tx index not found");  // tx index not found

    // Read txPrev and header of its block
    const CBlockIndex* pindexFrom = LookupBlockIndex(hashBlock);
    if (!pindexFrom)
        return error("CheckProofOfStake() : block index not found");

    // Verify signature
    {
        int nIn = 0;
        const CTxOut& prevOut = txPrev->vout[tx->vin[nIn].prevout.n];
        TransactionSignatureChecker checker(&(*tx), nIn, prevOut.nValue, PrecomputedTransactionData(*tx));
        ScriptError serror = SCRIPT_ERR_OK;

        if (!VerifyScript(tx->vin[nIn].scriptSig, prevOut.scriptPubKey, &(tx->vin[nIn].scriptWitness), STANDARD_SCRIPT_VERIFY_FLAGS, checker, &serror))
            return state.Invalid(ValidationInvalidReason::CONSENSUS, false, REJECT_INVALID, "invalid-pos-script", strprintf("%s: VerifyScript failed on coinstake %s, %s", __func__, tx->GetHash().ToString(), ScriptErrorString(serror)));
    }

    unsigned int nInterval = 0;
    if (!CheckStakeKernelHash(nBits, pindexPrev, pindexFrom, txPrev, txin.prevout, nTimeTx, nInterval, true, hashProofOfStake, gArgs.GetBoolArg("-debug", false)))
        return error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx->GetHash().ToString(), hashProofOfStake.ToString()); // may occur during initial download or if behind on block chain sync

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    //if (IsProtocolV03(nTimeTx))  // v0.3 protocol
        return (nTimeBlock == nTimeTx);
    //else // v0.2 protocol
        //return ((nTimeTx <= nTimeBlock) && (nTimeBlock <= nTimeTx + MAX_FUTURE_BLOCK_TIME));
}

// Get stake modifier checksum
/*unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex)
{
    assert(pindex->pprev || pindex->GetBlockHash() == Params().GetConsensus().hashGenesisBlock);
    // Hash previous checksum with flags, hashProofOfStake and nStakeModifier
    CDataStream ss(SER_GETHASH, 0);
    if (pindex->pprev)
        ss << pindex->pprev->nStakeModifierChecksum;
    ss << pindex->nFlags << pindex->hashProofOfStake << pindex->nStakeModifier;
    arith_uint256 hashChecksum = UintToArith256(Hash(ss.begin(), ss.end()));
    hashChecksum >>= (256 - 32);
    return hashChecksum.GetLow64();
}*/

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum)
{
    bool fTestNet = Params().NetworkIDString() == CBaseChainParams::TESTNET;
    if (fTestNet && mapStakeModifierTestnetCheckpoints.count(nHeight))
        return nStakeModifierChecksum == mapStakeModifierTestnetCheckpoints[nHeight];

    if (!fTestNet && mapStakeModifierCheckpoints.count(nHeight))
        return nStakeModifierChecksum == mapStakeModifierCheckpoints[nHeight];

    return true;
}

bool IsSuperMajority(unsigned int minVersion, const CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck)
{
    unsigned int nFound = 0;
    for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; pstart = pstart->pprev )
    {
        if (!pstart->IsProofOfStake())
            continue;

        if (pstart->nVersion >= minVersion)
            ++nFound;

        i++;
    }
    return (nFound >= nRequired);
}

// peercoin: entropy bit for stake modifier if chosen by modifier
unsigned int GetStakeEntropyBit(const CBlock& block)
{
    unsigned int nEntropyBit = 0;
    if (block.nVersion >= Params().GetConsensus().nUpgradeBlockVersion[1]) //IsProtocolV04(block.nTime)
    {
        nEntropyBit = UintToArith256(block.GetHash()).GetLow64() & 1llu; // last bit of block hash
        if (gArgs.GetBoolArg("-printstakemodifier", false))
            LogPrintf("GetStakeEntropyBit(v0.4+): nTime=%u hashBlock=%s entropybit=%d\n", block.nTime, block.GetHash().ToString(), nEntropyBit);
    }
    else
    {
        // old protocol for entropy bit pre v0.4
        uint160 hashSig = Hash160(block.vchBlockSig);
        if (gArgs.GetBoolArg("-printstakemodifier", false))
            LogPrintf("GetStakeEntropyBit(v0.3): nTime=%u hashSig=%s", block.nTime, hashSig.ToString());
        nEntropyBit = hashSig.GetDataPtr()[4] >> 31; // take the first bit of the hash
        if (gArgs.GetBoolArg("-printstakemodifier", false))
            LogPrintf(" entropybit=%d\n", nEntropyBit);
    }
    return nEntropyBit;
}
