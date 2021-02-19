#include <gtest/gtest.h>
#include "tx_creation_utils.h"
#include <gtest/libzendoo_test_files.h>
#include <sc/sidechain.h>
#include <boost/filesystem.hpp>
#include <txdb.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <txmempool.h>
#include <undo.h>
#include <main.h>

class CBlockUndo_OldVersion
{
    public:
        std::vector<CTxUndo> vtxundo;
        uint256 old_tree_root;

        ADD_SERIALIZE_METHODS;

        template <typename Stream, typename Operation>
        inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
            READWRITE(vtxundo);
            READWRITE(old_tree_root);
        }   
};

class CInMemorySidechainDb final: public CCoinsView {
public:
    CInMemorySidechainDb()  = default;
    virtual ~CInMemorySidechainDb() = default;

    bool HaveSidechain(const uint256& scId) const override { return inMemoryMap.count(scId); }
    bool GetSidechain(const uint256& scId, CSidechain& info) const override {
        if(!inMemoryMap.count(scId))
            return false;
        info = inMemoryMap[scId];
        return true;
    }

    virtual void GetScIds(std::set<uint256>& scIdsList) const override {
        for (auto& entry : inMemoryMap)
            scIdsList.insert(entry.first);
        return;
    }

    bool BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock,
                    const uint256 &hashAnchor, CAnchorsMap &mapAnchors,
                    CNullifiersMap &mapNullifiers, CSidechainsMap& sidechainMap, CSidechainEventsMap& mapSidechainEvents) override
    {
        for (auto& entry : sidechainMap)
            switch (entry.second.flag) {
                case CSidechainsCacheEntry::Flags::FRESH:
                case CSidechainsCacheEntry::Flags::DIRTY:
                    inMemoryMap[entry.first] = entry.second.sidechain;
                    break;
                case CSidechainsCacheEntry::Flags::ERASED:
                    inMemoryMap.erase(entry.first);
                    break;
                case CSidechainsCacheEntry::Flags::DEFAULT:
                    break;
                default:
                    return false;
            }
        sidechainMap.clear();
        return true;
    }

private:
    mutable boost::unordered_map<uint256, CSidechain, ObjectHasher> inMemoryMap;
};

class CNakedCCoinsViewCache : public CCoinsViewCache
{
public:
    CNakedCCoinsViewCache(CCoinsView* pWrappedView): CCoinsViewCache(pWrappedView)
    {
        uint256 dummyAnchor = uint256S("59d2cde5e65c1414c32ba54f0fe4bdb3d67618125286e6a191317917c812c6d7"); //anchor for empty block!?
        this->hashAnchor = dummyAnchor;

        CAnchorsCacheEntry dummyAnchorsEntry;
        dummyAnchorsEntry.entered = true;
        dummyAnchorsEntry.flags = CAnchorsCacheEntry::DIRTY;
        this->cacheAnchors[dummyAnchor] = dummyAnchorsEntry;

    };
    CSidechainsMap& getSidechainMap() {return this->cacheSidechains; };
};

class SidechainsTestSuite: public ::testing::Test {

public:
    SidechainsTestSuite():
          fakeChainStateDb(nullptr)
        , sidechainsView(nullptr)
        , dummyScVerifier(libzendoomc::CScProofVerifier::Disabled()) {};

    ~SidechainsTestSuite() = default;

    void SetUp() override {
        SelectParams(CBaseChainParams::REGTEST);

        fakeChainStateDb   = new CInMemorySidechainDb();
        sidechainsView     = new CNakedCCoinsViewCache(fakeChainStateDb);
    };

    void TearDown() override {
        delete sidechainsView;
        sidechainsView = nullptr;

        delete fakeChainStateDb;
        fakeChainStateDb = nullptr;

        UnloadBlockIndex();
    };

protected:
    CInMemorySidechainDb  *fakeChainStateDb;
    CNakedCCoinsViewCache *sidechainsView;

    //Helpers
    libzendoomc::CScProofVerifier dummyScVerifier;
    CBlockUndo createBlockUndoWith(const uint256 & scId, int height, CAmount amount, uint256 lastCertHash = uint256());
    void storeSidechainWithCurrentHeight(const uint256& scId, const CSidechain& sidechain, int chainActiveHeight);
};

///////////////////////////////////////////////////////////////////////////////
/////////////////////////// checkTxSemanticValidity ///////////////////////////
///////////////////////////////////////////////////////////////////////////////
TEST_F(SidechainsTestSuite, TransparentCcNullTxsAreSemanticallyValid) {
    CTransaction aTransaction = txCreationUtils::createTransparentTx(/*ccIsNull = */true);
    CValidationState txState;

    //test
    bool res = Sidechain::checkTxSemanticValidity(aTransaction, txState);

    //checks
    EXPECT_TRUE(res);
    EXPECT_TRUE(txState.IsValid());
}

TEST_F(SidechainsTestSuite, TransparentNonCcNullTxsAreNotSemanticallyValid) {
    CTransaction aTransaction = txCreationUtils::createTransparentTx(/*ccIsNull = */false);
    CValidationState txState;

    //test
    bool res = Sidechain::checkTxSemanticValidity(aTransaction, txState);

    //checks
    EXPECT_FALSE(res);
    EXPECT_FALSE(txState.IsValid());
    EXPECT_TRUE(txState.GetRejectCode() == REJECT_INVALID)
        <<"wrong reject code. Value returned: "<<txState.GetRejectCode();
}

TEST_F(SidechainsTestSuite, SproutCcNullTxsAreCurrentlySupported) {
    CTransaction aTransaction = txCreationUtils::createSproutTx(/*ccIsNull = */true);
    CValidationState txState;

    //test
    bool res = Sidechain::checkTxSemanticValidity(aTransaction, txState);

    //checks
    EXPECT_TRUE(res);
    EXPECT_TRUE(txState.IsValid());
}

TEST_F(SidechainsTestSuite, SproutNonCcNullTxsAreCurrentlySupported) {
    CTransaction aTransaction = txCreationUtils::createSproutTx(/*ccIsNull = */false);
    CValidationState txState;

    //test
    bool res = Sidechain::checkTxSemanticValidity(aTransaction, txState);

    //checks
    EXPECT_FALSE(res);
    EXPECT_FALSE(txState.IsValid());
    EXPECT_TRUE(txState.GetRejectCode() == REJECT_INVALID)
        <<"wrong reject code. Value returned: "<<txState.GetRejectCode();
}

TEST_F(SidechainsTestSuite, SidechainCreationsWithoutForwardTransferAreNotSemanticallyValid) {
    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(0));
    CValidationState txState;

    //test
    bool res = Sidechain::checkTxSemanticValidity(aTransaction, txState);

    //checks
    EXPECT_FALSE(res);
    EXPECT_FALSE(txState.IsValid());
    EXPECT_TRUE(txState.GetRejectCode() == REJECT_INVALID)
        <<"wrong reject code. Value returned: "<<txState.GetRejectCode();
}

TEST_F(SidechainsTestSuite, SidechainCreationsWithPositiveForwardTransferAreSemanticallyValid) {
    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(1000));
    CValidationState txState;

    //test
    bool res = Sidechain::checkTxSemanticValidity(aTransaction, txState);

    //checks
    EXPECT_TRUE(res);
    EXPECT_TRUE(txState.IsValid());
}

TEST_F(SidechainsTestSuite, SidechainCreationsWithTooLargePositiveForwardTransferAreNotSemanticallyValid) {
    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(MAX_MONEY +1));
    CValidationState txState;

    //test
    bool res = Sidechain::checkTxSemanticValidity(aTransaction, txState);

    //checks
    EXPECT_FALSE(res);
    EXPECT_FALSE(txState.IsValid());
    EXPECT_TRUE(txState.GetRejectCode() == REJECT_INVALID)
        <<"wrong reject code. Value returned: "<<txState.GetRejectCode();
}

TEST_F(SidechainsTestSuite, SidechainCreationsWithZeroForwardTransferAreNotSemanticallyValid) {
    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(0));
    CValidationState txState;

    //test
    bool res = Sidechain::checkTxSemanticValidity(aTransaction, txState);

    //checks
    EXPECT_FALSE(res);
    EXPECT_FALSE(txState.IsValid());
    EXPECT_TRUE(txState.GetRejectCode() == REJECT_INVALID)
        <<"wrong reject code. Value returned: "<<txState.GetRejectCode();
}

TEST_F(SidechainsTestSuite, SidechainCreationsWithNegativeForwardTransferNotAreSemanticallyValid) {
    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(-1));
    CValidationState txState;

    //test
    bool res = Sidechain::checkTxSemanticValidity(aTransaction, txState);

    //checks
    EXPECT_FALSE(res);
    EXPECT_FALSE(txState.IsValid());
    EXPECT_TRUE(txState.GetRejectCode() == REJECT_INVALID)
        <<"wrong reject code. Value returned: "<<txState.GetRejectCode();
}

TEST_F(SidechainsTestSuite, FwdTransferCumulatedAmountDoesNotOverFlow) {
    CAmount initialFwdTrasfer(1);
    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(initialFwdTrasfer);
    txCreationUtils::addNewScCreationToTx(aTransaction, MAX_MONEY);
    CValidationState txState;

    //test
    bool res = Sidechain::checkTxSemanticValidity(aTransaction, txState);

    //checks
    EXPECT_FALSE(res);
    EXPECT_FALSE(txState.IsValid());
    EXPECT_TRUE(txState.GetRejectCode() == REJECT_INVALID)
        <<"wrong reject code. Value returned: "<<txState.GetRejectCode();
}

///////////////////////////////////////////////////////////////////////////////
//////////////////////////// checkCcOutputAmounts /////////////////////////////
///////////////////////////////////////////////////////////////////////////////
TEST(SidechainsAmounts, NegativeScFeesAreRejected)
{
    CBwtRequestOut bwtReqOut;
    bwtReqOut.scFee = CAmount(-10);

    CMutableTransaction mutTx;
    mutTx.add(bwtReqOut);

    CValidationState dummyState;
    EXPECT_FALSE(CTransaction(mutTx).CheckAmounts(dummyState));
}

TEST(SidechainsAmounts, ExcessiveScFeesAreRejected)
{
    CBwtRequestOut bwtReqOut;
    bwtReqOut.scFee = MAX_MONEY +1;

    CMutableTransaction mutTx;
    mutTx.add(bwtReqOut);

    CValidationState dummyState;
    EXPECT_FALSE(CTransaction(mutTx).CheckAmounts(dummyState));
}

TEST(SidechainsAmounts, CumulativeExcessiveScFeesAreRejected)
{
    CBwtRequestOut bwtReqOut;
    bwtReqOut.scFee = MAX_MONEY/2 + 1;

    CMutableTransaction mutTx;
    mutTx.add(bwtReqOut);
    mutTx.add(bwtReqOut);

    CValidationState dummyState;
    EXPECT_FALSE(CTransaction(mutTx).CheckAmounts(dummyState));
}

TEST(SidechainsAmounts, ScFeesLargerThanInputAreRejected)
{
    CBwtRequestOut bwtReqOut;
    bwtReqOut.scFee = CAmount(10);

    CMutableTransaction mutTx;
    mutTx.add(bwtReqOut);

    CAmount totalVinAmount = bwtReqOut.scFee / 2;
    ASSERT_TRUE(totalVinAmount < bwtReqOut.scFee);

    CValidationState dummyState;
    EXPECT_FALSE(CTransaction(mutTx).CheckFeeAmount(totalVinAmount, dummyState));
}
///////////////////////////////////////////////////////////////////////////////
/////////////////////////// IsScTxApplicableToState ///////////////////////////
///////////////////////////////////////////////////////////////////////////////

TEST_F(SidechainsTestSuite, ScCreationIsApplicableToStateIfScDoesntNotExistYet) {
    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(1953));
    uint256 scId = aTransaction.GetScIdFromScCcOut(0);
    ASSERT_FALSE(sidechainsView->HaveSidechain(scId));

    //test
    bool res = sidechainsView->IsScTxApplicableToState(aTransaction, dummyScVerifier);

    //checks
    EXPECT_TRUE(res);
}

//TEST_F(SidechainsTestSuite, ScCreationIsNotApplicableToStateIfScIsAlreadyUnconfirmed) {
//    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(1953));
//
//    // setup sidechain initial state
//    CSidechain initialScState;
//    uint256 scId = aTransaction.GetScIdFromScCcOut(0);
//    initialScState.currentState = static_cast<uint8_t>(CSidechain::State::UNCONFIRMED);
//    txCreationUtils::storeSidechain(sidechainsView->getSidechainMap(), scId, initialScState);
//    ASSERT_TRUE(sidechainsView->GetSidechainState(scId) == CSidechain::State::UNCONFIRMED);
//
//    //test
//    bool res = sidechainsView->IsScTxApplicableToState(aTransaction, dummyScVerifier);
//
//    //checks
//    EXPECT_FALSE(res);
//}

TEST_F(SidechainsTestSuite, ScCreationIsNotApplicableToStateIfScIsAlreadyAlive) {
    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(1953));

    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = aTransaction.GetScIdFromScCcOut(0);
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    int heightWhereAlive = initialScState.GetScheduledCeasingHeight() -1;

    storeSidechainWithCurrentHeight(scId, initialScState, heightWhereAlive);
    ASSERT_TRUE(sidechainsView->GetSidechainState(scId) == CSidechain::State::ALIVE);

    //test
    bool res = sidechainsView->IsScTxApplicableToState(aTransaction, dummyScVerifier);

    //checks
    EXPECT_FALSE(res);
}

TEST_F(SidechainsTestSuite, ScCreationIsNotApplicableToStateIfScIsAlreadyCeased) {
    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(1953));

    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = aTransaction.GetScIdFromScCcOut(0);
    initialScState.creationBlockHeight = 200;
    initialScState.creationData.withdrawalEpochLength = 10;
    int heightWhereCeased = initialScState.GetScheduledCeasingHeight();

    storeSidechainWithCurrentHeight(scId, initialScState, heightWhereCeased);
    ASSERT_TRUE(sidechainsView->GetSidechainState(scId) == CSidechain::State::CEASED);

    //test
    bool res = sidechainsView->IsScTxApplicableToState(aTransaction, dummyScVerifier);

    //checks
    EXPECT_FALSE(res);
}

TEST_F(SidechainsTestSuite, ForwardTransferToUnknownSCsIsApplicableToState) {
    // setup sidechain initial state
    uint256 scId = uint256S("aaaa");
    ASSERT_FALSE(sidechainsView->HaveSidechain(scId));

    CTransaction aTransaction = txCreationUtils::createFwdTransferTxWith(scId, CAmount(5));

    //test
    bool res = sidechainsView->IsScTxApplicableToState(aTransaction, dummyScVerifier);

    //checks
    EXPECT_FALSE(res);
}

//TEST_F(SidechainsTestSuite, ForwardTransferToUnconfirmedSCsIsApplicableToState) {
//    // setup sidechain initial state
//    CSidechain initialScState;
//    uint256 scId = uint256S("aaaa");
//    initialScState.currentState = static_cast<uint8_t>(CSidechain::State::UNCONFIRMED);
//    txCreationUtils::storeSidechain(sidechainsView->getSidechainMap(), scId, initialScState);
//    ASSERT_TRUE(sidechainsView->GetSidechainState(scId) == CSidechain::State::UNCONFIRMED);
//
//    CTransaction aTransaction = txCreationUtils::createFwdTransferTxWith(scId, CAmount(5));
//
//    //test
//    bool res = sidechainsView->IsScTxApplicableToState(aTransaction, dummyScVerifier);
//
//    //checks
//    EXPECT_TRUE(res);
//}

TEST_F(SidechainsTestSuite, ForwardTransferToAliveSCsIsApplicableToState) {
    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    int heightWhereAlive = initialScState.GetScheduledCeasingHeight() -1;

    storeSidechainWithCurrentHeight(scId, initialScState, heightWhereAlive);
    ASSERT_TRUE(sidechainsView->GetSidechainState(scId) == CSidechain::State::ALIVE);

    CTransaction aTransaction = txCreationUtils::createFwdTransferTxWith(scId, CAmount(5));

    //test
    bool res = sidechainsView->IsScTxApplicableToState(aTransaction, dummyScVerifier);

    //checks
    EXPECT_TRUE(res);
}

TEST_F(SidechainsTestSuite, ForwardTransferToCeasedSCsIsNotApplicableToState) {
    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    int heightWhereCeased = initialScState.GetScheduledCeasingHeight();

    storeSidechainWithCurrentHeight(scId, initialScState, heightWhereCeased);
    ASSERT_TRUE(sidechainsView->GetSidechainState(scId) == CSidechain::State::CEASED);

    CTransaction aTransaction = txCreationUtils::createFwdTransferTxWith(scId, CAmount(5));

    //test
    bool res = sidechainsView->IsScTxApplicableToState(aTransaction, dummyScVerifier);

    //checks
    EXPECT_FALSE(res);
}

TEST_F(SidechainsTestSuite, McBwtRequestToAliveSidechainWithKeyIsApplicableToState) {
    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    initialScState.creationData.wMbtrVk = libzendoomc::ScVk(ParseHex(SAMPLE_VK));
    int heightWhereAlive = initialScState.GetScheduledCeasingHeight()-1;

    storeSidechainWithCurrentHeight(scId, initialScState, heightWhereAlive);
    ASSERT_TRUE(sidechainsView->GetSidechainState(scId) == CSidechain::State::ALIVE);

    // create mc Bwt request
    CBwtRequestOut mcBwtReq;
    mcBwtReq.scId = scId;
    mcBwtReq.scProof = libzendoomc::ScProof(ParseHex(SAMPLE_PROOF));
    CMutableTransaction mutTx;
    mutTx.nVersion = SC_TX_VERSION;
    mutTx.vmbtr_out.push_back(mcBwtReq);

    //test
    bool res = sidechainsView->IsScTxApplicableToState(CTransaction(mutTx), dummyScVerifier);

    //checks
    EXPECT_TRUE(res);
}

//TEST_F(SidechainsTestSuite, McBwtRequestToUnconfirmedSidechainWithKeyIsApplicableToState) {
//    //setup blockchain
//    int viewHeight {1963};
//    chainSettingUtils::ExtendChainActiveToHeight(viewHeight);
//    sidechainsView->SetBestBlock(*(chainActive.Tip()->phashBlock));
//
//    // setup sidechain initial state
//    CSidechain initialScState;
//    uint256 scId = uint256S("aaaa");
//    initialScState.currentState = static_cast<uint8_t>(CSidechain::State::UNCONFIRMED);
//    initialScState.creationData.wMbtrVk = libzendoomc::ScVk(ParseHex(SAMPLE_VK));
//    txCreationUtils::storeSidechain(sidechainsView->getSidechainMap(), scId, initialScState);
//    ASSERT_TRUE(sidechainsView->GetSidechainState(scId) == CSidechain::State::UNCONFIRMED);
//
//    // create mc Bwt request
//    CBwtRequestOut mcBwtReq;
//    mcBwtReq.scId = scId;
//    mcBwtReq.scProof = libzendoomc::ScProof(ParseHex(SAMPLE_PROOF));
//    CMutableTransaction mutTx;
//    mutTx.nVersion = SC_TX_VERSION;
//    mutTx.vmbtr_out.push_back(mcBwtReq);
//
//    //test
//    bool res = sidechainsView->IsScTxApplicableToState(CTransaction(mutTx), dummyScVerifier);
//
//    //checks
//    EXPECT_TRUE(res);
//
//    //cleanup blockchain
//    chainActive.SetTip(nullptr);
//    mapBlockIndex.clear();
//}
//
TEST_F(SidechainsTestSuite, McBwtRequestToUnknownSidechainIsNotApplicableToState) {
    uint256 scId = uint256S("aaa");
    ASSERT_FALSE(sidechainsView->HaveSidechain(scId));

    CBwtRequestOut mcBwtReq;
    mcBwtReq.scId = scId;
    CMutableTransaction mutTx;
    mutTx.nVersion = SC_TX_VERSION;
    mutTx.vmbtr_out.push_back(mcBwtReq);

    //test
    bool res = sidechainsView->IsScTxApplicableToState(CTransaction(mutTx), dummyScVerifier);

    //checks
    EXPECT_FALSE(res);
}

TEST_F(SidechainsTestSuite, McBwtRequestToAliveSidechainWithoutKeyIsNotApplicableToState) {
    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    ASSERT_FALSE(initialScState.creationData.wMbtrVk.is_initialized());
    int heightWhereAlive = initialScState.GetScheduledCeasingHeight()-1;

    storeSidechainWithCurrentHeight(scId, initialScState, heightWhereAlive);
    ASSERT_TRUE(sidechainsView->GetSidechainState(scId) == CSidechain::State::ALIVE);

    CSidechain storedSc;
    ASSERT_TRUE(sidechainsView->GetSidechain(scId, storedSc));
    ASSERT_TRUE(!storedSc.creationData.wMbtrVk.is_initialized());

    // create mc Bwt request
    CBwtRequestOut mcBwtReq;
    mcBwtReq.scId = scId;
    mcBwtReq.scProof = libzendoomc::ScProof(ParseHex(SAMPLE_PROOF));
    CMutableTransaction mutTx;
    mutTx.nVersion = SC_TX_VERSION;
    mutTx.vmbtr_out.push_back(mcBwtReq);

    //test
    bool res = sidechainsView->IsScTxApplicableToState(CTransaction(mutTx), dummyScVerifier);

    //checks
    EXPECT_FALSE(res);
}

//TEST_F(SidechainsTestSuite, McBwtRequestToUnconfirmedSidechainWithoutKeyIsNotApplicableToState) {
//    // setup sidechain initial state
//    CSidechain initialScState;
//    uint256 scId = uint256S("aaaa");
//    initialScState.currentState = static_cast<uint8_t>(CSidechain::State::UNCONFIRMED);
//    txCreationUtils::storeSidechain(sidechainsView->getSidechainMap(), scId, initialScState);
//
//    CSidechain storedSc;
//    ASSERT_TRUE(sidechainsView->GetSidechain(scId, storedSc));
//    ASSERT_TRUE(!storedSc.creationData.wMbtrVk.is_initialized());
//
//    // create mc Bwt request
//    CBwtRequestOut mcBwtReq;
//    mcBwtReq.scId = scId;
//    mcBwtReq.scProof = libzendoomc::ScProof(ParseHex(SAMPLE_PROOF));
//    CMutableTransaction mutTx;
//    mutTx.nVersion = SC_TX_VERSION;
//    mutTx.vmbtr_out.push_back(mcBwtReq);
//
//    //test
//    bool res = sidechainsView->IsScTxApplicableToState(CTransaction(mutTx), dummyScVerifier);
//
//    //checks
//    EXPECT_FALSE(res);
//}

TEST_F(SidechainsTestSuite, McBwtRequestToCeasedSidechainIsNotApplicableToState) {
    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    initialScState.creationData.wMbtrVk = libzendoomc::ScVk(ParseHex(SAMPLE_VK));
    int heightWhereCeased = initialScState.GetScheduledCeasingHeight();

    storeSidechainWithCurrentHeight(scId, initialScState, heightWhereCeased);
    ASSERT_TRUE(sidechainsView->GetSidechainState(scId) == CSidechain::State::CEASED);

    // create mc Bwt request
    CBwtRequestOut mcBwtReq;
    mcBwtReq.scId = scId;
    CMutableTransaction mutTx;
    mutTx.nVersion = SC_TX_VERSION;
    mutTx.vmbtr_out.push_back(mcBwtReq);

    //test
    bool res = sidechainsView->IsScTxApplicableToState(CTransaction(mutTx), dummyScVerifier);

    //checks
    EXPECT_FALSE(res);
}
/////////////////////////////////////////////////////////////////////////////////
///////////////////////////////// RevertTxOutputs ///////////////////////////////
/////////////////////////////////////////////////////////////////////////////////
//TEST_F(SidechainsTestSuite, RevertingScCreationTxRemovesTheSc) {
//    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(10));
//    const uint256& scId = aTransaction.GetScIdFromScCcOut(0);
//    int scCreationHeight = 1;
//    CBlock aBlock;
//    sidechainsView->UpdateSidechain(aTransaction, aBlock, scCreationHeight);
//
//    //test
//    bool res = sidechainsView->RevertTxOutputs(aTransaction, scCreationHeight);
//
//    //checks
//    EXPECT_TRUE(res);
//    EXPECT_FALSE(sidechainsView->HaveSidechain(scId));
//}
//
//TEST_F(SidechainsTestSuite, RevertingFwdTransferRemovesCoinsFromImmatureBalance) {
//    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(10));
//    const uint256& scId = aTransaction.GetScIdFromScCcOut(0);
//    int scCreationHeight = 1;
//    CBlock aBlock;
//    sidechainsView->UpdateSidechain(aTransaction, aBlock, scCreationHeight);
//
//    int fwdTxHeight = 5;
//    aTransaction = txCreationUtils::createFwdTransferTxWith(scId, CAmount(7));
//    sidechainsView->UpdateSidechain(aTransaction, aBlock, fwdTxHeight);
//
//    //test
//    bool res = sidechainsView->RevertTxOutputs(aTransaction, fwdTxHeight);
//
//    //checks
//    EXPECT_TRUE(res);
//    CSidechain viewInfos;
//    ASSERT_TRUE(sidechainsView->GetSidechain(scId, viewInfos));
//    EXPECT_TRUE(viewInfos.mImmatureAmounts.count(fwdTxHeight + Params().ScCoinsMaturity()) == 0)
//        <<"resulting immature amount is "<< viewInfos.mImmatureAmounts.count(fwdTxHeight + Params().ScCoinsMaturity());
//}
//
//TEST_F(SidechainsTestSuite, ScCreationTxCannotBeRevertedIfScIsNotPreviouslyCreated) {
//    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(15));
//
//    //test
//    bool res = sidechainsView->RevertTxOutputs(aTransaction, /*height*/int(1789));
//
//    //checks
//    EXPECT_FALSE(res);
//}
//
//TEST_F(SidechainsTestSuite, FwdTransferTxToUnexistingScCannotBeReverted) {
//    CTransaction aTransaction = txCreationUtils::createFwdTransferTxWith(uint256S("a1b2"), CAmount(999));
//
//    //test
//    bool res = sidechainsView->RevertTxOutputs(aTransaction, /*height*/int(1789));
//
//    //checks
//    EXPECT_FALSE(res);
//}
//
//TEST_F(SidechainsTestSuite, RevertingAFwdTransferOnTheWrongHeightHasNoEffect) {
//    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(10));
//    const uint256& scId = aTransaction.GetScIdFromScCcOut(0);
//    int scCreationHeight = 1;
//    CBlock aBlock;
//    sidechainsView->UpdateSidechain(aTransaction, aBlock, scCreationHeight);
//
//    int fwdTxHeight = 5;
//    CAmount fwdAmount = 7;
//    aTransaction = txCreationUtils::createFwdTransferTxWith(scId, fwdAmount);
//    sidechainsView->UpdateSidechain(aTransaction, aBlock, fwdTxHeight);
//
//    //test
//    int faultyHeight = fwdTxHeight -1;
//    bool res = sidechainsView->RevertTxOutputs(aTransaction, faultyHeight);
//
//    //checks
//    EXPECT_FALSE(res);
//    CSidechain viewInfos;
//    ASSERT_TRUE(sidechainsView->GetSidechain(scId, viewInfos));
//    EXPECT_TRUE(viewInfos.mImmatureAmounts.at(fwdTxHeight + Params().ScCoinsMaturity()) == fwdAmount)
//        <<"Immature amount is "<<viewInfos.mImmatureAmounts.at(fwdTxHeight + Params().ScCoinsMaturity())
//        <<"instead of "<<fwdAmount;
//}
//
//TEST_F(SidechainsTestSuite, RestoreSidechainRestoresLastCertHash) {
//    //Create sidechain and mature it to generate first block undo
//    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(34));
//    const uint256& scId = aTransaction.GetScIdFromScCcOut(0);
//    int scCreationHeight = 71;
//    CBlock dummyBlock;
//    sidechainsView->UpdateSidechain(aTransaction, dummyBlock, scCreationHeight);
//    CSidechain sidechainAtCreation;
//    ASSERT_TRUE(sidechainsView->GetSidechain(scId, sidechainAtCreation));
//
//    CBlockUndo dummyBlockUndo;
//    for(const CTxScCreationOut& scCreationOut: aTransaction.GetVscCcOut())
//        ASSERT_TRUE(sidechainsView->ScheduleSidechainEvent(scCreationOut, scCreationHeight));
//
//    std::vector<CScCertificateStatusUpdateInfo> dummy;
//    ASSERT_TRUE(sidechainsView->HandleSidechainEvents(scCreationHeight + Params().ScCoinsMaturity(), dummyBlockUndo, &dummy));
//
//
//    //Update sc with cert and create the associate blockUndo
//    int certEpoch = 0;
//    CScCertificate cert = txCreationUtils::createCertificate(scId, certEpoch, dummyBlock.GetHash(),
//        /*changeTotalAmount*/CAmount(4),/*numChangeOut*/2, /*bwtAmount*/CAmount(2), /*numBwt*/2);
//    CBlockUndo blockUndo;
//    sidechainsView->UpdateSidechain(cert, blockUndo);
//    CSidechain sidechainPostCert;
//    ASSERT_TRUE(sidechainsView->GetSidechain(scId, sidechainPostCert));
//    EXPECT_TRUE(sidechainPostCert.lastTopQualityCertReferencedEpoch == certEpoch);
//    EXPECT_TRUE(sidechainPostCert.lastTopQualityCertHash == cert.GetHash());
//
//    //test
//    bool res = sidechainsView->RestoreSidechain(cert, blockUndo.scUndoDatabyScId.at(scId));
//
//    //checks
//    EXPECT_TRUE(res);
//    CSidechain sidechainPostCertUndo;
//    ASSERT_TRUE(sidechainsView->GetSidechain(scId, sidechainPostCertUndo));
//    EXPECT_TRUE(sidechainPostCertUndo.lastTopQualityCertHash == sidechainAtCreation.lastTopQualityCertHash);
//    EXPECT_TRUE(sidechainPostCertUndo.lastTopQualityCertReferencedEpoch == sidechainAtCreation.lastTopQualityCertReferencedEpoch);
//}
/////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////// UpdateSidechain ////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////
//
//TEST_F(SidechainsTestSuite, NewSCsAreRegistered) {
//    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(1));
//
//    const uint256& scId = aTransaction.GetScIdFromScCcOut(0);
//    CBlock aBlock;
//
//    //test
//    bool res = sidechainsView->UpdateSidechain(aTransaction, aBlock, /*height*/int(1789));
//
//    //check
//    EXPECT_TRUE(res);
//    EXPECT_TRUE(sidechainsView->HaveSidechain(scId));
//}
//
//TEST_F(SidechainsTestSuite, ForwardTransfersToNonExistentSCsAreRejected) {
//    uint256 nonExistentId = uint256S("1492");
//    CTransaction aTransaction = txCreationUtils::createFwdTransferTxWith(nonExistentId, CAmount(10));
//    CBlock aBlock;
//
//    //test
//    bool res = sidechainsView->UpdateSidechain(aTransaction, aBlock, /*height*/int(1789));
//
//    //check
//    EXPECT_FALSE(res);
//    EXPECT_FALSE(sidechainsView->HaveSidechain(nonExistentId));
//}
//
//TEST_F(SidechainsTestSuite, ForwardTransfersToExistentSCsAreRegistered) {
//    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(5));
//    const uint256& scId = aTransaction.GetScIdFromScCcOut(0);
//    CBlock aBlock;
//    sidechainsView->UpdateSidechain(aTransaction, aBlock, /*height*/int(1789));
//
//    aTransaction = txCreationUtils::createFwdTransferTxWith(scId, CAmount(15));
//
//    //test
//    bool res = sidechainsView->UpdateSidechain(aTransaction, aBlock, /*height*/int(1789));
//
//    //check
//    EXPECT_TRUE(res);
//}
//
//TEST_F(SidechainsTestSuite, CertificateUpdatesTopCommittedCertHash) {
//    //Create Sc
//    int scCreationHeight = 1987;
//    CTransaction scCreationTx = txCreationUtils::createNewSidechainTxWith(CAmount(5));
//    const uint256& scId = scCreationTx.GetScIdFromScCcOut(0);
//    CBlock dummyBlock;
//    ASSERT_TRUE(sidechainsView->UpdateSidechain(scCreationTx, dummyBlock, scCreationHeight));
//
//    CSidechain sidechain;
//    EXPECT_TRUE(sidechainsView->GetSidechain(scId,sidechain));
//    EXPECT_TRUE(sidechain.lastTopQualityCertHash.IsNull());
//
//    //Fully mature initial Sc balance
//    for(const CTxScCreationOut& scCreationOut: scCreationTx.GetVscCcOut())
//        ASSERT_TRUE(sidechainsView->ScheduleSidechainEvent(scCreationOut, scCreationHeight));
//    int coinMaturityHeight = scCreationHeight + Params().ScCoinsMaturity();
//    CBlockUndo dummyBlockUndo;
//    std::vector<CScCertificateStatusUpdateInfo> dummy;
//    ASSERT_TRUE(sidechainsView->HandleSidechainEvents(coinMaturityHeight, dummyBlockUndo, &dummy));
//
//    CBlockUndo blockUndo;
//    CScCertificate aCertificate = txCreationUtils::createCertificate(scId, /*epochNum*/0, dummyBlock.GetHash(),
//        /*changeTotalAmount*/CAmount(4),/*numChangeOut*/2, /*bwtAmount*/CAmount(2), /*numBwt*/2);
//    EXPECT_TRUE(sidechainsView->UpdateSidechain(aCertificate, blockUndo));
//
//    //check
//    ASSERT_TRUE(sidechainsView->GetSidechain(scId,sidechain));
//    EXPECT_TRUE(sidechain.lastTopQualityCertHash == aCertificate.GetHash());
//    EXPECT_TRUE(blockUndo.scUndoDatabyScId.at(scId).prevTopCommittedCertReferencedEpoch == -1);
//    EXPECT_TRUE(blockUndo.scUndoDatabyScId.at(scId).prevTopCommittedCertHash.IsNull());
//}
//
/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////// BatchWrite ///////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////
//TEST_F(SidechainsTestSuite, FRESHSidechainsGetWrittenInBackingCache) {
//    CCoinsMap mapCoins;
//    const uint256 hashBlock;
//    const uint256 hashAnchor;
//    CAnchorsMap mapAnchors;
//    CNullifiersMap mapNullifiers;
//    CSidechainEventsMap mapCeasingScs;
//
//
//    uint256 scId = uint256S("aaaa");
//    CSidechainsMap mapToWrite;
//    CSidechainsCacheEntry entry;
//    entry.sidechain = CSidechain();
//    entry.flag   = CSidechainsCacheEntry::Flags::FRESH;
//
//    mapToWrite[scId] = entry;
//
//    //write new sidechain when backing view doesn't know about it
//    bool res = sidechainsView->BatchWrite(mapCoins, hashBlock, hashAnchor, mapAnchors, mapNullifiers, mapToWrite, mapCeasingScs);
//
//    //checks
//    EXPECT_TRUE(res);
//    EXPECT_TRUE(sidechainsView->HaveSidechain(scId));
//}
//
//TEST_F(SidechainsTestSuite, FRESHSidechainsCanBeWrittenOnlyIfUnknownToBackingCache) {
//    CCoinsMap mapCoins;
//    const uint256 hashBlock;
//    const uint256 hashAnchor;
//    CAnchorsMap mapAnchors;
//    CNullifiersMap mapNullifiers;
//    CSidechainEventsMap mapCeasingScs;
//
//
//    //Prefill backing cache with sidechain
//    CTransaction scTx = txCreationUtils::createNewSidechainTxWith(CAmount(10));
//    const uint256& scId = scTx.GetScIdFromScCcOut(0);
//    sidechainsView->UpdateSidechain(scTx, CBlock(), /*nHeight*/ 1000);
//
//    //attempt to write new sidechain when backing view already knows about it
//    CSidechainsMap mapToWrite;
//    CSidechainsCacheEntry entry;
//    entry.sidechain = CSidechain();
//    entry.flag   = CSidechainsCacheEntry::Flags::FRESH;
//
//    mapToWrite[scId] = entry;
//
//    ASSERT_DEATH(sidechainsView->BatchWrite(mapCoins, hashBlock, hashAnchor, mapAnchors, mapNullifiers, mapToWrite,mapCeasingScs),"");
//}
//
//TEST_F(SidechainsTestSuite, DIRTYSidechainsAreStoredInBackingCache) {
//    CCoinsMap mapCoins;
//    const uint256 hashBlock;
//    const uint256 hashAnchor;
//    CAnchorsMap mapAnchors;
//    CNullifiersMap mapNullifiers;
//    CSidechainEventsMap mapCeasingScs;
//
//
//    uint256 scId = uint256S("aaaa");
//    CSidechainsMap mapToWrite;
//    CSidechainsCacheEntry entry;
//    entry.sidechain = CSidechain();
//    entry.flag   = CSidechainsCacheEntry::Flags::FRESH;
//
//    mapToWrite[scId] = entry;
//
//    //write dirty sidechain when backing view doesn't know about it
//    bool res = sidechainsView->BatchWrite(mapCoins, hashBlock, hashAnchor, mapAnchors, mapNullifiers, mapToWrite, mapCeasingScs);
//
//    //checks
//    EXPECT_TRUE(res);
//    EXPECT_TRUE(sidechainsView->HaveSidechain(scId));
//}
//
//TEST_F(SidechainsTestSuite, DIRTYSidechainsUpdatesDirtyOnesInBackingCache) {
//    CCoinsMap mapCoins;
//    const uint256 hashBlock;
//    const uint256 hashAnchor;
//    CAnchorsMap mapAnchors;
//    CNullifiersMap mapNullifiers;
//    CSidechainEventsMap mapCeasingScs;
//
//
//    CTransaction scTx = txCreationUtils::createNewSidechainTxWith(CAmount(10));
//    const uint256& scId = scTx.GetScIdFromScCcOut(0);
//    sidechainsView->UpdateSidechain(scTx, CBlock(), /*nHeight*/ 1000);
//
//    CSidechainsMap mapToWrite;
//    CSidechainsCacheEntry entry;
//    CSidechain updatedSidechain;
//    updatedSidechain.balance = CAmount(12);
//    entry.sidechain = updatedSidechain;
//    entry.flag   = CSidechainsCacheEntry::Flags::DIRTY;
//
//    mapToWrite[scId] = entry;
//
//    //write dirty sidechain when backing view already knows about it
//    bool res = sidechainsView->BatchWrite(mapCoins, hashBlock, hashAnchor, mapAnchors, mapNullifiers, mapToWrite, mapCeasingScs);
//
//    //checks
//    EXPECT_TRUE(res);
//    CSidechain cachedSc;
//    EXPECT_TRUE(sidechainsView->GetSidechain(scId, cachedSc));
//    EXPECT_TRUE(cachedSc.balance == CAmount(12) );
//}
//
//TEST_F(SidechainsTestSuite, DIRTYSidechainsOverwriteErasedOnesInBackingCache) {
//    CCoinsMap mapCoins;
//    const uint256 hashBlock;
//    const uint256 hashAnchor;
//    CAnchorsMap mapAnchors;
//    CNullifiersMap mapNullifiers;
//    CSidechainEventsMap mapCeasingScs;
//
//
//    //Create sidechain...
//    CTransaction scTx = txCreationUtils::createNewSidechainTxWith(CAmount(10));
//    const uint256& scId = scTx.GetScIdFromScCcOut(0);
//    sidechainsView->UpdateSidechain(scTx, CBlock(), /*nHeight*/ 1000);
//
//    //...then revert it to have it erased
//    sidechainsView->RevertTxOutputs(scTx, /*nHeight*/1000);
//    ASSERT_FALSE(sidechainsView->HaveSidechain(scId));
//
//    CSidechainsMap mapToWrite;
//    CSidechainsCacheEntry entry;
//    CSidechain updatedSidechain;
//    updatedSidechain.balance = CAmount(12);
//    entry.sidechain = updatedSidechain;
//    entry.flag   = CSidechainsCacheEntry::Flags::DIRTY;
//
//    mapToWrite[scId] = entry;
//
//    //write dirty sidechain when backing view have it erased
//    bool res = sidechainsView->BatchWrite(mapCoins, hashBlock, hashAnchor, mapAnchors, mapNullifiers, mapToWrite, mapCeasingScs);
//
//    //checks
//    EXPECT_TRUE(res);
//    CSidechain cachedSc;
//    EXPECT_TRUE(sidechainsView->GetSidechain(scId, cachedSc));
//    EXPECT_TRUE(cachedSc.balance == CAmount(12) );
//}
//
//TEST_F(SidechainsTestSuite, ERASEDSidechainsSetExistingOnesInBackingCacheasErased) {
//    CCoinsMap mapCoins;
//    const uint256 hashBlock;
//    const uint256 hashAnchor;
//    CAnchorsMap mapAnchors;
//    CNullifiersMap mapNullifiers;
//    CSidechainEventsMap mapCeasingScs;
//
//    CTransaction scTx = txCreationUtils::createNewSidechainTxWith(CAmount(10));
//    const uint256& scId = scTx.GetScIdFromScCcOut(0);
//    sidechainsView->UpdateSidechain(scTx, CBlock(), /*nHeight*/ 1000);
//
//    CSidechainsMap mapToWrite;
//    CSidechainsCacheEntry entry;
//    CSidechain updatedSidechain;
//    updatedSidechain.balance = CAmount(12);
//    entry.sidechain = updatedSidechain;
//    entry.flag   = CSidechainsCacheEntry::Flags::ERASED;
//
//    mapToWrite[scId] = entry;
//
//    //write dirty sidechain when backing view have it erased
//    bool res = sidechainsView->BatchWrite(mapCoins, hashBlock, hashAnchor, mapAnchors, mapNullifiers, mapToWrite, mapCeasingScs);
//
//    //checks
//    EXPECT_TRUE(res);
//    EXPECT_FALSE(sidechainsView->HaveSidechain(scId));
//}
//
//TEST_F(SidechainsTestSuite, DEFAULTSidechainsCanBeWrittenInBackingCacheasOnlyIfUnchanged) {
//    CCoinsMap mapCoins;
//    const uint256 hashBlock;
//    const uint256 hashAnchor;
//    CAnchorsMap mapAnchors;
//    CNullifiersMap mapNullifiers;
//    CSidechainEventsMap mapCeasingScs;
//
//    CTransaction scTx = txCreationUtils::createNewSidechainTxWith(CAmount(10));
//    const uint256& scId = scTx.GetScIdFromScCcOut(0);
//    sidechainsView->UpdateSidechain(scTx, CBlock(), /*nHeight*/ 1000);
//
//    CSidechainsMap mapToWrite;
//    CSidechainsCacheEntry entry;
//    CSidechain updatedSidechain;
//    updatedSidechain.balance = CAmount(12);
//    entry.sidechain = updatedSidechain;
//    entry.flag   = CSidechainsCacheEntry::Flags::DEFAULT;
//
//    mapToWrite[scId] = entry;
//
//    //write dirty sidechain when backing view have it erased
//    ASSERT_DEATH(sidechainsView->BatchWrite(mapCoins, hashBlock, hashAnchor, mapAnchors, mapNullifiers, mapToWrite, mapCeasingScs),"");
//}
//
/////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////// Flush /////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////
//TEST_F(SidechainsTestSuite, FlushPersistsNewSidechains) {
//    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(1000));
//    const uint256& scId = aTransaction.GetScIdFromScCcOut(0);
//    CBlock aBlock;
//    sidechainsView->UpdateSidechain(aTransaction, aBlock, /*height*/int(1789));
//
//    //test
//    bool res = sidechainsView->Flush();
//
//    //checks
//    EXPECT_TRUE(res);
//    EXPECT_TRUE(fakeChainStateDb->HaveSidechain(scId));
//}
//
//TEST_F(SidechainsTestSuite, FlushPersistsForwardTransfers) {
//    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(1));
//    const uint256& scId = aTransaction.GetScIdFromScCcOut(0);
//    int scCreationHeight = 1;
//    CBlock aBlock;
//    sidechainsView->UpdateSidechain(aTransaction, aBlock, scCreationHeight);
//    sidechainsView->Flush();
//
//    CAmount fwdTxAmount = 1000;
//    int fwdTxHeght = scCreationHeight + 10;
//    int fwdTxMaturityHeight = fwdTxHeght + Params().ScCoinsMaturity();
//    aTransaction = txCreationUtils::createFwdTransferTxWith(scId, CAmount(1000));
//    sidechainsView->UpdateSidechain(aTransaction, aBlock, fwdTxHeght);
//
//    //test
//    bool res = sidechainsView->Flush();
//
//    //checks
//    EXPECT_TRUE(res);
//
//    CSidechain persistedInfo;
//    ASSERT_TRUE(fakeChainStateDb->GetSidechain(scId, persistedInfo));
//    ASSERT_TRUE(persistedInfo.mImmatureAmounts.at(fwdTxMaturityHeight) == fwdTxAmount)
//        <<"Following flush, persisted fwd amount should equal the one in view";
//}
//
//TEST_F(SidechainsTestSuite, FlushPersistsScErasureToo) {
//    CTransaction aTransaction = txCreationUtils::createNewSidechainTxWith(CAmount(10));
//    const uint256& scId = aTransaction.GetScIdFromScCcOut(0);
//    CBlock aBlock;
//    sidechainsView->UpdateSidechain(aTransaction, aBlock, /*height*/int(1789));
//    sidechainsView->Flush();
//
//    sidechainsView->RevertTxOutputs(aTransaction, /*height*/int(1789));
//
//    //test
//    bool res = sidechainsView->Flush();
//
//    //checks
//    EXPECT_TRUE(res);
//    EXPECT_FALSE(fakeChainStateDb->HaveSidechain(scId));
//}
//
//TEST_F(SidechainsTestSuite, FlushPersistsNewScsOnTopOfErasedOnes) {
//    CBlock aBlock;
//
//    //Create new sidechain and flush it
//    CTransaction scCreationTx = txCreationUtils::createNewSidechainTxWith(CAmount(10));
//    const uint256& scId = scCreationTx.GetScIdFromScCcOut(0);
//    sidechainsView->UpdateSidechain(scCreationTx, aBlock, /*height*/int(1789));
//    sidechainsView->Flush();
//    ASSERT_TRUE(fakeChainStateDb->HaveSidechain(scId));
//
//    //Remove it and flush again
//    sidechainsView->RevertTxOutputs(scCreationTx, /*height*/int(1789));
//    sidechainsView->Flush();
//    ASSERT_FALSE(fakeChainStateDb->HaveSidechain(scId));
//
//    //re-use sc with same scId as erased one
//    CTransaction scReCreationTx = scCreationTx;
//    sidechainsView->UpdateSidechain(scReCreationTx, aBlock, /*height*/int(1815));
//    bool res = sidechainsView->Flush();
//
//    //checks
//    EXPECT_TRUE(res);
//    EXPECT_TRUE(fakeChainStateDb->HaveSidechain(scId));
//}
/////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////// GetScIds //////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////
//TEST_F(SidechainsTestSuite, GetScIdsReturnsNonErasedSidechains) {
//    CBlock aBlock;
//
//    int sc1CreationHeight(11);
//    CTransaction scTx1 = txCreationUtils::createNewSidechainTxWith(CAmount(1));
//    uint256 scId1 = scTx1.GetScIdFromScCcOut(0);
//    ASSERT_TRUE(sidechainsView->UpdateSidechain(scTx1, aBlock, sc1CreationHeight));
//    ASSERT_TRUE(sidechainsView->Flush());
//
//    CTransaction fwdTx = txCreationUtils::createFwdTransferTxWith(scId1, CAmount(3));
//    int fwdTxHeight(22);
//    sidechainsView->UpdateSidechain(fwdTx, aBlock, fwdTxHeight);
//
//    int sc2CreationHeight(33);
//    CTransaction scTx2 = txCreationUtils::createNewSidechainTxWith(CAmount(2));
//    uint256 scId2 = scTx2.GetScIdFromScCcOut(0);
//    ASSERT_TRUE(sidechainsView->UpdateSidechain(scTx2, aBlock,sc2CreationHeight));
//    ASSERT_TRUE(sidechainsView->Flush());
//
//    ASSERT_TRUE(sidechainsView->RevertTxOutputs(scTx2, sc2CreationHeight));
//
//    //test
//    std::set<uint256> knownScIdsSet;
//    sidechainsView->GetScIds(knownScIdsSet);
//
//    //check
//    EXPECT_TRUE(knownScIdsSet.size() == 1)<<"Instead knowScIdSet size is "<<knownScIdsSet.size();
//    EXPECT_TRUE(knownScIdsSet.count(scId1) == 1)<<"Actual count is "<<knownScIdsSet.count(scId1);
//    EXPECT_TRUE(knownScIdsSet.count(scId2) == 0)<<"Actual count is "<<knownScIdsSet.count(scId2);
//}
//
//TEST_F(SidechainsTestSuite, GetScIdsOnChainstateDbSelectOnlySidechains) {
//
//    //init a tmp chainstateDb
//    boost::filesystem::path pathTemp(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path());
//    const unsigned int      chainStateDbSize(2 * 1024 * 1024);
//    boost::filesystem::create_directories(pathTemp);
//    mapArgs["-datadir"] = pathTemp.string();
//
//    CCoinsViewDB chainStateDb(chainStateDbSize,/*fWipe*/true);
//    sidechainsView->SetBackend(chainStateDb);
//
//    //prepare a sidechain
//    CBlock aBlock;
//    int sc1CreationHeight(11);
//    CTransaction scTx = txCreationUtils::createNewSidechainTxWith(CAmount(1));
//    const uint256& scId = scTx.GetScIdFromScCcOut(0);
//    ASSERT_TRUE(sidechainsView->UpdateSidechain(scTx, aBlock, sc1CreationHeight));
//
//    //prepare a coin
//    CCoinsCacheEntry aCoin;
//    aCoin.flags = CCoinsCacheEntry::FRESH | CCoinsCacheEntry::DIRTY;
//    aCoin.coins.fCoinBase = false;
//    aCoin.coins.nVersion = TRANSPARENT_TX_VERSION;
//    aCoin.coins.vout.resize(1);
//    aCoin.coins.vout[0].nValue = CAmount(10);
//
//    CCoinsMap mapCoins;
//    mapCoins[uint256S("aaaa")] = aCoin;
//    CAnchorsMap    emptyAnchorsMap;
//    CNullifiersMap emptyNullifiersMap;
//    CSidechainsMap emptySidechainsMap;
//    CSidechainEventsMap mapCeasingScs;
//
//    sidechainsView->BatchWrite(mapCoins, uint256(), uint256(), emptyAnchorsMap, emptyNullifiersMap, emptySidechainsMap, mapCeasingScs);
//
//    //flush both the coin and the sidechain to the tmp chainstatedb
//    ASSERT_TRUE(sidechainsView->Flush());
//
//    //test
//    std::set<uint256> knownScIdsSet;
//    sidechainsView->GetScIds(knownScIdsSet);
//
//    //check
//    EXPECT_TRUE(knownScIdsSet.size() == 1)<<"Instead knowScIdSet size is "<<knownScIdsSet.size();
//    EXPECT_TRUE(knownScIdsSet.count(scId) == 1)<<"Actual count is "<<knownScIdsSet.count(scId);
//
//    ClearDatadirCache();
//    boost::system::error_code ec;
//    boost::filesystem::remove_all(pathTemp.string(), ec);
//}
/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////// GetSidechain /////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////
//TEST_F(SidechainsTestSuite, GetSidechainForFwdTransfersInMempool) {
//    CTxMemPool aMempool(CFeeRate(1));
//
//    //Confirm a Sidechain
//    CAmount creationAmount = 10;
//    CTransaction scTx = txCreationUtils::createNewSidechainTxWith(creationAmount);
//    const uint256& scId = scTx.GetScIdFromScCcOut(0);
//    int scCreationHeight(11);
//    CBlock aBlock;
//    ASSERT_TRUE(sidechainsView->UpdateSidechain(scTx, aBlock, scCreationHeight));
//    ASSERT_TRUE(sidechainsView->Flush());
//
//    //Fully mature initial Sc balance
//    CBlockUndo anEmptyBlockUndo;
//    for(const CTxScCreationOut& scCreationOut: scTx.GetVscCcOut())
//        ASSERT_TRUE(sidechainsView->ScheduleSidechainEvent(scCreationOut, scCreationHeight));
//    int coinMaturityHeight = scCreationHeight + Params().ScCoinsMaturity();
//    CBlockUndo dummyBlockUndo;
//    std::vector<CScCertificateStatusUpdateInfo> dummy;
//    ASSERT_TRUE(sidechainsView->HandleSidechainEvents(coinMaturityHeight, dummyBlockUndo, &dummy));
//
//    //a fwd is accepted in mempool
//    CAmount fwdAmount = 20;
//    CTransaction fwdTx = txCreationUtils::createFwdTransferTxWith(scId, fwdAmount);
//    CTxMemPoolEntry fwdPoolEntry(fwdTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
//    aMempool.addUnchecked(fwdPoolEntry.GetTx().GetHash(), fwdPoolEntry);
//
//    //a bwt cert is accepted in mempool too
//    CAmount certAmount = 4;
//    CMutableScCertificate cert;
//    cert.scId = scId;
//    cert.quality = 33;
//    CScript scriptPubKey = CScript() << OP_DUP << OP_HASH160 << ToByteVector(uint160()) << OP_EQUALVERIFY << OP_CHECKSIG;
//    cert.addBwt(CTxOut(certAmount, scriptPubKey));
//
//    CCertificateMemPoolEntry bwtPoolEntry(cert, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
//    aMempool.addUnchecked(bwtPoolEntry.GetCertificate().GetHash(), bwtPoolEntry);
//
//    //test
//    CCoinsViewMemPool viewMemPool(sidechainsView, aMempool);
//    CSidechain retrievedInfo;
//    viewMemPool.GetSidechain(scId, retrievedInfo);
//
//    //check
//    EXPECT_TRUE(retrievedInfo.creationBlockHeight == scCreationHeight);
//    EXPECT_TRUE(retrievedInfo.balance == creationAmount);             //certs in mempool do not affect balance
//    EXPECT_TRUE(retrievedInfo.lastTopQualityCertReferencedEpoch == -1); //certs in mempool do not affect topCommittedCertReferencedEpoch
//}
//
//TEST_F(SidechainsTestSuite, GetSidechainForScCreationInMempool) {
//    CTxMemPool aMempool(CFeeRate(1));
//
//    //Confirm a Sidechain
//    CAmount creationAmount = 10;
//    CTransaction scTx = txCreationUtils::createNewSidechainTxWith(creationAmount);
//    txCreationUtils::addNewScCreationToTx(scTx, creationAmount);
//    txCreationUtils::addNewScCreationToTx(scTx, creationAmount);
//    const uint256& scId = scTx.GetScIdFromScCcOut(2);
//    CTxMemPoolEntry scPoolEntry(scTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
//    aMempool.addUnchecked(scTx.GetHash(), scPoolEntry);
//
//    //a fwd is accepted in mempool
//    CAmount fwdAmount = 20;
//    CTransaction fwdTx = txCreationUtils::createFwdTransferTxWith(scId, fwdAmount);
//    CTxMemPoolEntry fwdPoolEntry(fwdTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
//    aMempool.addUnchecked(fwdPoolEntry.GetTx().GetHash(), fwdPoolEntry);
//
//    //test
//    CCoinsViewMemPool viewMemPool(sidechainsView, aMempool);
//    CSidechain retrievedInfo;
//    viewMemPool.GetSidechain(scId, retrievedInfo);
//
//    //check
//    EXPECT_TRUE(retrievedInfo.creationBlockHeight == -1);
//    EXPECT_TRUE(retrievedInfo.balance == 0);
//    EXPECT_TRUE(retrievedInfo.lastTopQualityCertReferencedEpoch == -1);
//    EXPECT_TRUE(retrievedInfo.mImmatureAmounts.size() == 0);
//}
//
/////////////////////////////////////////////////////////////////////////////////
////////////////////////////////// UndoBlock versioning /////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////
//TEST_F(SidechainsTestSuite, CSidechainBlockUndoVersioning) {
//    boost::filesystem::path pathTemp(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path());
//    boost::filesystem::create_directories(pathTemp);
//    static const std::string autofileName = "test_block_undo_versioning.txt";
//    CAutoFile fileout(fopen((pathTemp.string() + autofileName).c_str(), "wb+") , SER_DISK, CLIENT_VERSION);
//    EXPECT_TRUE(fileout.Get() != NULL);
//
//    // write an old version undo block to the file
//    //----------------------------------------------
//    CBlockUndo_OldVersion buov;
//    buov.vtxundo.reserve(1);
//    buov.vtxundo.push_back(CTxUndo());
//
//    fileout << buov;;
//
//    uint256 h_buov;
//    {
//        CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
//        hasher << buov;
//        h_buov = hasher.GetHash();
//    }
//    fileout << h_buov;
//
//    fseek(fileout.Get(), 0, SEEK_END);
//    unsigned long len = (unsigned long)ftell(fileout.Get());
//
//    unsigned long buov_sz = buov.GetSerializeSize(SER_DISK, CLIENT_VERSION);
//    EXPECT_TRUE(len == buov_sz + sizeof(uint256));
//
//    // write a new version undo block to the same file
//    //-----------------------------------------------
//    CBlockUndo buon;
//    buon.vtxundo.reserve(1);
//    buon.vtxundo.push_back(CTxUndo());
//
//    fileout << buon;;
//
//    uint256 h_buon;
//    {
//        CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
//        hasher << buon;
//        h_buon = hasher.GetHash();
//    }
//    fileout << h_buon;
//
//    fseek(fileout.Get(), 0, SEEK_END);
//    unsigned long len2 = (unsigned long)ftell(fileout.Get());
//
//    unsigned long buon_sz = buon.GetSerializeSize(SER_DISK, CLIENT_VERSION);
//    EXPECT_TRUE(len2 == len + buon_sz + sizeof(uint256));
//
//    EXPECT_TRUE(buov_sz != buon_sz);
//
//    fileout.fclose();
//
//    // read both blocks and tell their version
//    //-----------------------------------------------
//    CAutoFile filein(fopen((pathTemp.string() + autofileName).c_str(), "rb+") , SER_DISK, CLIENT_VERSION);
//    EXPECT_TRUE(filein.Get() != NULL);
//
//    bool good_read = true;
//    CBlockUndo b1, b2;
//    uint256 h1, h2;
//    try {
//        filein >> b1;
//        filein >> h1;
//        filein >> b2;
//        filein >> h2;
//    }
//    catch (const std::exception& e) {
//        good_read = false;
//    }
//
//    EXPECT_TRUE(good_read == true);
//
//    EXPECT_TRUE(b1.IncludesSidechainAttributes() == false);
//    EXPECT_TRUE(h1 == h_buov);
//
//    EXPECT_TRUE(b2.IncludesSidechainAttributes() == true);
//    EXPECT_TRUE(h2 == h_buon);
//
//    filein.fclose();
//    boost::system::error_code ec;
//    boost::filesystem::remove_all(pathTemp.string(), ec);
//}

///////////////////////////////////////////////////////////////////////////////
////////////////////////// Test Fixture definitions ///////////////////////////
///////////////////////////////////////////////////////////////////////////////
CBlockUndo SidechainsTestSuite::createBlockUndoWith(const uint256 & scId, int height, CAmount amount, uint256 lastCertHash)
{
    CBlockUndo retVal;
    CAmount AmountPerHeight = amount;
    CSidechainUndoData data;
    data.appliedMaturedAmount = AmountPerHeight;
    retVal.scUndoDatabyScId[scId] = data;

    return retVal;
}

void SidechainsTestSuite::storeSidechainWithCurrentHeight(const uint256& scId, const CSidechain& sidechain, int chainActiveHeight)
{
    chainSettingUtils::ExtendChainActiveToHeight(chainActiveHeight);
    sidechainsView->SetBestBlock(chainActive.Tip()->GetBlockHash());
    txCreationUtils::storeSidechain(sidechainsView->getSidechainMap(), scId, sidechain);
}
