// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <banman.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <crypto/sha256.h>
#include <init.h>
#include <miner.h>
#include <net.h>
#include <net_processing.h>
#include <noui.h>
#include <pow.h>
#include <rpc/blockchain.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <script/sigcache.h>
#include <streams.h>
#include <txdb.h>
#include <util/memory.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>
#include <util/translation.h>
#include <util/url.h>
#include <util/vector.h>
#include <validation.h>
#include <validationinterface.h>

#include <functional>

const std::function<std::string(const char*)> G_TRANSLATION_FUN = nullptr;
UrlDecodeFn* const URL_DECODE = nullptr;

FastRandomContext g_insecure_rand_ctx;
/** Random context to get unique temp data dirs. Separate from g_insecure_rand_ctx, which can be seeded from a const env var */
static FastRandomContext g_insecure_rand_ctx_temp_path;

/** Return the unsigned from the environment var if available, otherwise 0 */
static uint256 GetUintFromEnv(const std::string& env_name)
{
    const char* num = std::getenv(env_name.c_str());
    if (!num) return {};
    return uint256S(num);
}

void Seed(FastRandomContext& ctx)
{
    // Should be enough to get the seed once for the process
    static uint256 seed{};
    static const std::string RANDOM_CTX_SEED{"RANDOM_CTX_SEED"};
    if (seed.IsNull()) seed = GetUintFromEnv(RANDOM_CTX_SEED);
    if (seed.IsNull()) seed = GetRandHash();
    LogPrintf("%s: Setting random seed for current tests to %s=%s\n", __func__, RANDOM_CTX_SEED, seed.GetHex());
    ctx = FastRandomContext(seed);
}

std::ostream& operator<<(std::ostream& os, const uint256& num)
{
    os << num.ToString();
    return os;
}

BasicTestingSetup::BasicTestingSetup(const std::string& chainName, const std::vector<const char*>& extra_args)
    : m_path_root{fs::temp_directory_path() / "test_common_" PACKAGE_NAME / g_insecure_rand_ctx_temp_path.rand256().ToString()}
{
    const std::vector<const char*> arguments = Cat(
        {
            "dummy",
            "-printtoconsole=0",
            "-logtimemicros",
            "-debug",
            "-debugexclude=libevent",
            "-debugexclude=leveldb",
        },
        extra_args);
    fs::create_directories(m_path_root);
    gArgs.ForceSetArg("-datadir", m_path_root.string());
    ClearDatadirCache();
    {
        SetupServerArgs(m_node);
        std::string error;
        const bool success{m_node.args->ParseParameters(arguments.size(), arguments.data(), error)};
        assert(success);
        assert(error.empty());
    }
    SelectParams(chainName);
    SeedInsecureRand();
    if (G_TEST_LOG_FUN) LogInstance().PushBackCallback(G_TEST_LOG_FUN);
    InitLogging();
    AppInitParameterInteraction();
    LogInstance().StartLogging();
    SHA256AutoDetect();
    ECC_Start();
    SetupEnvironment();
    SetupNetworking();
    InitSignatureCache();
    InitScriptExecutionCache();
    fCheckBlockIndex = true;
    static bool noui_connected = false;
    if (!noui_connected) {
        noui_connect();
        noui_connected = true;
    }
}

BasicTestingSetup::~BasicTestingSetup()
{
    LogInstance().DisconnectTestLogger();
    fs::remove_all(m_path_root);
    gArgs.ClearArgs();
    ECC_Stop();
}

TestingSetup::TestingSetup(const std::string& chainName, const std::vector<const char*>& extra_args)
    : BasicTestingSetup(chainName, extra_args)
{
    const CChainParams& chainparams = Params();
    // Ideally we'd move all the RPC tests to the functional testing framework
    // instead of unit tests, but for now we need these here.
    g_rpc_node = &m_node;
    RegisterAllCoreRPCCommands(tableRPC);

    m_node.scheduler = MakeUnique<CScheduler>();

    // We have to run a scheduler thread to prevent ActivateBestChain
    // from blocking due to queue overrun.
    threadGroup.create_thread([&]{ m_node.scheduler->serviceQueue(); });
    GetMainSignals().RegisterBackgroundSignalScheduler(*g_rpc_node->scheduler);

    pblocktree.reset(new CBlockTreeDB(1 << 20, true));

    g_chainman.InitializeChainstate();
    ::ChainstateActive().InitCoinsDB(
        /* cache_size_bytes */ 1 << 23, /* in_memory */ true, /* should_wipe */ false);
    assert(!::ChainstateActive().CanFlushToDisk());
    ::ChainstateActive().InitCoinsCache();
    assert(::ChainstateActive().CanFlushToDisk());
    if (!LoadGenesisBlock(chainparams)) {
        throw std::runtime_error("LoadGenesisBlock failed.");
    }

    BlockValidationState state;
    if (!ActivateBestChain(state, chainparams)) {
        throw std::runtime_error(strprintf("ActivateBestChain failed. (%s)", state.ToString()));
    }

    // Start script-checking threads. Set g_parallel_script_checks to true so they are used.
    constexpr int script_check_threads = 2;
    for (int i = 0; i < script_check_threads; ++i) {
        threadGroup.create_thread([i]() { return ThreadScriptCheck(i); });
    }
    g_parallel_script_checks = true;

    m_node.mempool = &::mempool;
    m_node.mempool->setSanityCheck(1.0);
    m_node.banman = MakeUnique<BanMan>(GetDataDir() / "banlist.dat", nullptr, DEFAULT_MISBEHAVING_BANTIME);
    m_node.connman = MakeUnique<CConnman>(0x1337, 0x1337); // Deterministic randomness for tests.
    m_node.peer_logic = MakeUnique<PeerLogicValidation>(m_node.connman.get(), m_node.banman.get(), *m_node.scheduler, *m_node.mempool);
    {
        CConnman::Options options;
        options.m_msgproc = m_node.peer_logic.get();
        m_node.connman->Init(options);
    }
}

TestingSetup::~TestingSetup()
{
    if (m_node.scheduler) m_node.scheduler->stop();
    threadGroup.interrupt_all();
    threadGroup.join_all();
    GetMainSignals().FlushBackgroundCallbacks();
    GetMainSignals().UnregisterBackgroundSignalScheduler();
    g_rpc_node = nullptr;
    m_node.connman.reset();
    m_node.banman.reset();
    m_node.args = nullptr;
    m_node.mempool = nullptr;
    m_node.scheduler.reset();
    UnloadBlockIndex();
    g_chainman.Reset();
    pblocktree.reset();
}

TestChain100Setup::TestChain100Setup()
{
    // CreateAndProcessBlock() does not support building SegWit blocks, so don't activate in these tests.
    // TODO: fix the code to support SegWit blocks.
    gArgs.ForceSetArg("-segwitheight", "432");
    // Need to recreate chainparams
    SelectParams(CBaseChainParams::REGTEST);

    // Generate a 100-block chain:
    coinbaseKey.MakeNewKey(true);
    CScript scriptPubKey = CScript() <<  ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    for (int i = 0; i < COINBASE_MATURITY; i++)
    {
        std::vector<CMutableTransaction> noTxns;
        CBlock b = CreateAndProcessBlock(noTxns, scriptPubKey);
        m_coinbase_txns.push_back(b.vtx[0]);
    }
}

// Create a new block with just given transactions, coinbase paying to
// scriptPubKey, and try to add it to the current chain.
CBlock TestChain100Setup::CreateAndProcessBlock(const std::vector<CMutableTransaction>& txns, const CScript& scriptPubKey)
{
    const CChainParams& chainparams = Params();
    std::unique_ptr<CBlockTemplate> pblocktemplate = BlockAssembler(*m_node.mempool, chainparams).CreateNewBlock(scriptPubKey);
    CBlock& block = pblocktemplate->block;

    // Replace mempool-selected txns with just coinbase plus passed-in txns:
    block.vtx.resize(1);
    for (const CMutableTransaction& tx : txns)
        block.vtx.push_back(MakeTransactionRef(tx));
    // IncrementExtraNonce creates a valid coinbase and merkleRoot
    {
        LOCK(cs_main);
        unsigned int extraNonce = 0;
        IncrementExtraNonce(&block, ::ChainActive().Tip(), extraNonce);
    }

    while (!CheckProofOfWork(block.GetHash(), block.nBits, chainparams.GetConsensus())) ++block.nNonce;

    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    ProcessNewBlock(chainparams, shared_pblock, true, nullptr);

    CBlock result = block;
    return result;
}

TestChain100Setup::~TestChain100Setup()
{
    gArgs.ForceSetArg("-segwitheight", "0");
}


CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CMutableTransaction &tx) {
    return FromTx(MakeTransactionRef(tx));
}

CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CTransactionRef& tx)
{
    return CTxMemPoolEntry(tx, nFee, nTime, nHeight,
                           spendsCoinbase, sigOpCost, lp);
}

/**
 * @returns a real block (000000000000eb0d0499247d81f9368107ab176acd0fddee7e442996bbde97f3)
 *      with 9 txs.
 */
CBlock getBlockeb0d0()
{
    CBlock block;
    CDataStream stream(ParseHex("00000020af876e6efcd7c6a3d8768ce32cce1db4a6045123ea73ea2d47ac0000000000002abd5d5b435c03a757ae5c8990baaaa1030f07c2b8013b793c82aa9c56b335d855c5b55c0b18011b6610b78f01010000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff1f022a680456c5b55c085ffffff2c41400000d2f6e6f64655374726174756d2f00000000030000000000000000266a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf980010b27010000001976a914ac2eb3e500b3670964ef81c2acbe276f8083ecfd88ac80f0fa02000000001976a914a09b7a0ea8e3f56bb71f1af38406a12ffc048fe988ac0120000000000000000000000000000000000000000000000000000000000000000000000000"), SER_NETWORK, PROTOCOL_VERSION);
    stream >> block;
    return block;
}
