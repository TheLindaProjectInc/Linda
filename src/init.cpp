// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "init.h"
#include "activemasternode.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "db.h"
#include "keepass.h"
#include "key.h"
#include "main.h"
#include "masternodeconfig.h"
#include "net.h"
#include "rpcserver.h"
#include "spork.h"
#include "txdb.h"
#include "ui_interface.h"
#include "util.h"
#include "utilmoneystr.h"

#ifdef ENABLE_WALLET
#include "wallet.h"
#include "walletdb.h"
#endif

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/thread.hpp>

#include <openssl/crypto.h>

#include <stdio.h>

#ifndef WIN32
#include <signal.h>
#endif

#include "compat/sanity.h"

using namespace boost;
using namespace std;

#ifdef ENABLE_WALLET
CWallet* pwalletMain = NULL;
#endif

#ifdef WIN32
// Win32 LevelDB doesn't use filedescriptors, and the ones used for
// accessing block files, don't count towards to fd_set size limit
// anyway.
#define MIN_CORE_FILEDESCRIPTORS 0
#else
#define MIN_CORE_FILEDESCRIPTORS 150
#endif
bool fConfChange;
bool fMinimizeCoinAge;
unsigned int nNodeLifespan;
unsigned int nDerivationMethodIndex;
unsigned int nMinerSleep;
bool fUseFastIndex;
bool fOnlyTor = false;

// Used to pass flags to the Bind() function
enum BindFlags {
    BF_NONE = 0,
    BF_EXPLICIT = (1U << 0),
    BF_REPORT_ERROR = (1U << 1),
    BF_WHITELIST = (1U << 2),
};

static const char* FEE_ESTIMATES_FILENAME = "fee_estimates.dat";
CClientUIInterface uiInterface;

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
// Note that if running -daemon the parent process returns from AppInit2
// before adding any threads to the threadGroup, so .join_all() returns
// immediately and the parent exits from main().
//
// Shutdown for Qt is very similar, only it uses a QTimer to detect
// fRequestShutdown getting set, and then does the normal Qt
// shutdown thing.
//

volatile bool fRequestShutdown = false;

void StartShutdown()
{
    fRequestShutdown = true;
}
bool ShutdownRequested()
{
    return fRequestShutdown;
}

static CCoinsViewDB* pcoinsdbview;

void Shutdown()
{
    LogPrintf("%s: In progress...\n", __func__);
    static CCriticalSection cs_Shutdown;
    TRY_LOCK(cs_Shutdown, lockShutdown);
    if (!lockShutdown)
        return;

    RenameThread("Linda-shutoff");
    mempool.AddTransactionsUpdated(1);
    StopRPCThreads();
#ifdef ENABLE_WALLET
    if (pwalletMain)
        bitdb.Flush(false);
#endif
    StopNode();
    UnregisterNodeSignals(GetNodeSignals());

    {
        boost::filesystem::path est_path = GetDataDir() / FEE_ESTIMATES_FILENAME;
        CAutoFile est_fileout(fopen(est_path.string().c_str(), "wb"), SER_DISK, CLIENT_VERSION);
        if (!est_fileout.IsNull())
            mempool.WriteFeeEstimates(est_fileout);
        else
            LogPrintf("%s: Failed to write fee estimates to %s\n", __func__, est_path.string());
    }

    {
        LOCK(cs_main);
#ifdef ENABLE_WALLET
        if (pwalletMain)
            pwalletMain->SetBestChain(chainActive.GetLocator());
#endif
        if (pblocktree)
            pblocktree->Flush();
        if (pcoinsTip)
            pcoinsTip->Flush();
        delete pcoinsTip;
        pcoinsTip = NULL;
        delete pcoinsdbview;
        pcoinsdbview = NULL;
        delete pblocktree;
        pblocktree = NULL;
    }
#ifdef ENABLE_WALLET
    if (pwalletMain)
        bitdb.Flush(true);
#endif
#ifndef WIN32
    boost::filesystem::remove(GetPidFile());
#endif
    UnregisterAllValidationInterfaces();
#ifdef ENABLE_WALLET
    if (pwalletMain) {
        delete pwalletMain;
        pwalletMain = NULL;
    }
#endif
    LogPrintf("%s: done\n", __func__);
}

//
// Signal handlers are very limited in what they are allowed to do, so:
//
#ifndef WIN32
static void HandleSIGTERM(int)
{
    fRequestShutdown = true;
}

static void HandleSIGHUP(int)
{
    fReopenDebugLog = true;
}
#else
static BOOL WINAPI consoleCtrlHandler(DWORD dwCtrlType)
{
    fRequestShutdown = true;
    Sleep(INFINITE);
    return true;
}

#endif
#ifndef WIN32
static void registerSignalHandler(int signal, void (*handler)(int))
{
    struct sigaction sa;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(signal, &sa, NULL);
}
#endif

bool static InitError(const std::string& str)
{
    uiInterface.ThreadSafeMessageBox(str, "", CClientUIInterface::MSG_ERROR);
    return false;
}

bool static InitWarning(const std::string& str)
{
    uiInterface.ThreadSafeMessageBox(str, "", CClientUIInterface::MSG_WARNING);
    return true;
}

bool static Bind(const CService& addr, unsigned int flags)
{
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

// Core-specific options shared between daemon and RPC client
std::string HelpMessage(HelpMessageMode mode)
{
    // When adding new options to the categories, please keep and ensure alphabetical ordering.
    string strUsage = _("Options:") + "\n";
    strUsage += "  -?                     " + _("This help message") + "\n";
    strUsage += "  -alertnotify=<cmd>     " + _("Execute command when a relevant alert is received or we see a really long fork (%s in cmd is replaced by message)") + "\n";
    strUsage += "  -blocknotify=<cmd>     " + _("Execute command when the best block changes (%s in cmd is replaced by block hash)") + "\n";
    strUsage += "  -checkblocks=<n>       " + _("How many blocks to check at startup (default: 500, 0 = all)") + "\n";
    strUsage += "  -checklevel=<n>        " + _("How thorough the block verification is (0-6, default: 1)") + "\n";
    strUsage += "  -conf=<file>           " + _("Specify configuration file (default: Linda.conf)") + "\n";

    if (mode == HMM_BITCOIND) {
#if !defined(WIN32)
        strUsage += "  -daemon                " + _("Run in the background as a daemon and accept commands") + "\n";
#endif
    }

    strUsage += "  -datadir=<dir>         " + _("Specify data directory") + "\n";
    strUsage += "  -loadblock=<file>      " + _("Imports blocks from external blk000?.dat file") + "\n";
    strUsage += "  -par=<n>               " + strprintf(_("Set the number of script verification threads (%u to %d, 0 = auto, <0 = leave that many cores free, default: %d)"), -(int)boost::thread::hardware_concurrency(), MAX_SCRIPTCHECK_THREADS, DEFAULT_SCRIPTCHECK_THREADS) + "\n";
#ifndef WIN32
    strUsage += "  -pid=<file>            " + _("Specify pid file (default: Lindad.pid)") + "\n";
#endif
    strUsage += "  -reindex               " + _("Rebuild blockchain index from current blk000??.dat files") + "\n";
#if !defined(WIN32)
    strUsage += "  -sysperms              " + _("Create new files with system default permissions, instead of umask 077 (only effective with disabled wallet functionality)") + "\n";
#endif

    strUsage += "\n" + _("Connection options:") + "\n";
    strUsage += "  -addnode=<ip>          " + _("Add a node to connect to and attempt to keep the connection open") + "\n";
    strUsage += "  -banscore=<n>          " + _("Threshold for disconnecting misbehaving peers (default: 100)") + "\n";
    strUsage += "  -bantime=<n>           " + _("Number of seconds to keep misbehaving peers from reconnecting (default: 86400)") + "\n";
    strUsage += "  -bind=<addr>           " + _("Bind to given address and always listen on it. Use [host]:port notation for IPv6") + "\n";
    strUsage += "  -connect=<ip>          " + _("Connect only to the specified node(s)") + "\n";
    strUsage += "  -discover              " + _("Discover own IP address (default: 1 when listening and no -externalip)") + "\n";
    strUsage += "  -dns                   " + _("Allow DNS lookups for -addnode, -seednode and -connect") + "\n";
    strUsage += "  -dnsseed               " + _("Query for peer addresses via DNS lookup, if low on addresses (default: 1 unless -connect)") + "\n";
    strUsage += "  -externalip=<ip>       " + _("Specify your own public address") + "\n";
    strUsage += "  -forcednsseed          " + _("Always query for peer addresses via DNS lookup (default: 0)") + "\n";
    strUsage += "  -listen                " + _("Accept connections from outside (default: 1 if no -proxy or -connect)") + "\n";
    strUsage += "  -maxconnections=<n>    " + _("Maintain at most <n> connections to peers (default: 256)") + "\n";
    strUsage += "  -maxreceivebuffer=<n>  " + _("Maximum per-connection receive buffer, <n>*1000 bytes (default: 5000)") + "\n";
    strUsage += "  -maxsendbuffer=<n>     " + _("Maximum per-connection send buffer, <n>*1000 bytes (default: 1000)") + "\n";
    strUsage += "  -onlynet=<net>         " + _("Only connect to nodes in network <net> (ipv4, ipv6 or onion)") + "\n";
    strUsage += "  -permitbaremultisig    " + _("Relay non-P2SH multisig (default: 1)") + "\n";
    strUsage += "  -port=<port>           " + _("Listen for connections on <port> (default: 15714 or testnet: 25714)") + "\n";
    strUsage += "  -proxy=<ip:port>       " + _("Connect through SOCKS5 proxy") + "\n";
    strUsage += "  -seednode=<ip>         " + _("Connect to a node to retrieve peer addresses, and disconnect") + "\n";
    strUsage += "  -onion=<ip:port>       " + _("Use proxy to reach tor hidden services (default: same as -proxy)") + "\n";
    strUsage += "  -timeout=<n>           " + strprintf(_("Specify connection timeout in milliseconds (minimum: 1, default: %d)"), DEFAULT_CONNECT_TIMEOUT) + "\n";
    strUsage += "  -whitebind=<addr>      " + _("Bind to given address and whitelist peers connecting to it. Use [host]:port notation for IPv6") + "\n";
    strUsage += "  -whitelist=<netmask>   " + _("Whitelist peers connecting from the given netmask or ip. Can be specified multiple times.") + "\n";
#ifdef USE_UPNP
#if USE_UPNP
    strUsage += "  -upnp                  " + _("Use UPnP to map the listening port (default: 1 when listening)") + "\n";
#else
    strUsage += "  -upnp                  " + _("Use UPnP to map the listening port (default: 0)") + "\n";
#endif
#endif

#ifdef ENABLE_WALLET
    strUsage += "\n" + _("Wallet options:") + "\n";
    strUsage += "  -disablewallet         " + _("Do not load the wallet and disable wallet RPC calls") + "\n";
    if (GetBoolArg("-help-debug", false))
        strUsage += "  -mintxfee=<amt>        " + strprintf(_("Fees (in BTC/Kb) smaller than this are considered zero fee for transaction creation (default: %s)"), FormatMoney(CWallet::minTxFee.GetFeePerK())) + "\n";
    strUsage += "  -keypool=<n>           " + _("Set key pool size to <n> (default: 100)") + "\n";
    strUsage += "  -paytxfee=<amt>        " + _("Fee per KB to add to transactions you send") + "\n";
    strUsage += "  -txconfirmtarget=<n>   " + _("If paytxfee is not set, include enough fee so transactions are confirmed on average within n blocks (default: 1)") + "\n";
    strUsage += "  -rescan                " + _("Rescan the block chain for missing wallet transactions") + "\n";
    strUsage += "  -respendnotify=<cmd>   " + _("Execute command when a network tx respends wallet tx input (%s=respend TxID, %t=wallet TxID)") + "\n";
    strUsage += "  -salvagewallet         " + _("Attempt to recover private keys from a corrupt wallet.dat") + "\n";
    strUsage += "  -spendzeroconfchange   " + _("Spend unconfirmed change when sending transactions (default: 1)") + "\n";
    strUsage += "  -txconfirmtarget=<n>   " + _("If paytxfee is not set, include enough fee so transactions are confirmed on average within n blocks (default: 1)") + "\n";
    strUsage += "  -upgradewallet         " + _("Upgrade wallet to latest format") + "\n";
    strUsage += "  -wallet=<file>          " + _("Specify wallet file (within data directory)") + "\n";
    strUsage += "  -walletnotify=<cmd>    " + _("Execute command when a wallet transaction changes (%s in cmd is replaced by TxID)") + "\n";
    strUsage += "  -zapwallettxes=<mode>  " + _("Delete all wallet transactions and only recover those parts of the blockchain through -rescan on startup") + "\n";
    strUsage += "                         " + _("(default: 1, 1 = keep tx meta data e.g. account owner and payment request information, 2 = drop tx meta data)") + "\n";
#endif

    strUsage += "\n" + _("Debugging/Testing options:") + "\n";
    if (GetBoolArg("-help-debug", false)) {
        strUsage += "  -checkpoints           " + _("Only accept block chain matching built-in checkpoints (default: 1)") + "\n";
        strUsage += "  -confchange            " + _("Require a confirmations for change (default: 0)") + "\n";
        strUsage += "  -minimizecoinage       " + _("Minimize weight consumption (experimental) (default: 0)") + "\n";
        strUsage += "  -regtest               " + _("Enter regression test mode, which uses a special chain in which blocks can be ");
        strUsage += _("solved instantly. This is intended for regression testing tools and app development.") + "\n";
        strUsage += "  -stopafterblockimport  " + _("Stop running after importing blocks from disk (default: 0)") + "\n";
    }
    strUsage += "  -debug=<category>      " + _("Output debugging information (default: 0, supplying <category> is optional)") + "\n";
    strUsage += _("If <category> is not supplied, output all debugging information.") + "\n";
    strUsage += _("<category> can be:");
    strUsage += " addrman, alert, bench, db, lock, rand, rpc, selectcoins, mempool, net,";
    strUsage += " coinage, coinstake, creation, stakemodifier";
    if (mode == HMM_BITCOIN_QT) {
        strUsage += ", qt.\n";
    } else {
        strUsage += ".\n";
    }
    strUsage += "  -help-debug            " + _("Show all debugging options (usage: --help -help-debug)") + "\n";
    strUsage += "  -logips                " + _("Include IP addresses in debug output (default: 0)") + "\n";
    strUsage += "  -logtimestamps         " + _("Prepend debug output with timestamp") + "\n";
    strUsage += "  -shrinkdebugfile       " + _("Shrink debug.log file on client startup (default: 1 when no -debug)") + "\n";
    strUsage += "  -minrelaytxfee=<amt>   " + strprintf(_("Fees (in BTC/Kb) smaller than this are considered zero fee for relaying (default: %s)"), FormatMoney(::minRelayTxFee.GetFeePerK())) + "\n";
    strUsage += "  -testnet               " + _("Use the test network") + "\n";

    strUsage += "\n" + _("Node relay options:") + "\n";
    strUsage += "  -datacarrier           " + _("Relay and mine data carrier transactions (default: 1)") + "\n";

    strUsage += "\n" + _("Block creation options:") + "\n";
    strUsage += "  -blockminsize=<n>      " + _("Set minimum block size in bytes (default: 0)") + "\n";
    strUsage += "  -blockmaxsize=<n>      " + _("Set maximum block size in bytes (default: 250000)") + "\n";
    strUsage += "  -blockprioritysize=<n> " + _("Set maximum size of high-priority/low-fee transactions in bytes (default: 27000)") + "\n";
    strUsage += "  -mininput=<amt>        " + _("When creating transactions, ignore inputs with value less than this (default: 0.01)") + "\n";
    strUsage += "\n" + _("RPC server options:") + "\n";
#if !defined(WIN32)
    strUsage += "  -daemon                " + _("Run in the background as a daemon and accept commands") + "\n";
#endif
    strUsage += "  -server                " + _("Accept command line and JSON-RPC commands") + "\n";
    strUsage += "  -rpcbind=<addr>        " + _("Bind to given address to listen for JSON-RPC connections. Use [host]:port notation for IPv6. This option can be specified multiple times (default: bind to all interfaces)") + "\n";
    strUsage += "  -rpcuser=<user>        " + _("Username for JSON-RPC connections") + "\n";
    strUsage += "  -rpcpassword=<pw>      " + _("Password for JSON-RPC connections") + "\n";
    strUsage += "  -rpcport=<port>        " + _("Listen for JSON-RPC connections on <port> (default: 33821)") + "\n";
    strUsage += "  -rpcallowip=<ip>       " + _("Allow JSON-RPC connections from specified source. Valid for <ip> are a single IP (e.g. 1.2.3.4), a network/netmask (e.g. 1.2.3.4/255.255.255.0) or a network/CIDR (e.g. 1.2.3.4/24). This option can be specified multiple times") + "\n";
    strUsage += "  -rpcconnect=<ip>       " + _("Send commands to node running on <ip> (default: 127.0.0.1)") + "\n";
    strUsage += "  -rpcthreads=<n>        " + _("Set the number of threads to service RPC calls (default: 4)") + "\n";
    strUsage += "  -rpcwait               " + _("Wait for RPC server to start") + "\n";

    strUsage += "\n" + _("Block sync/database options:") + "\n";
    strUsage += "  -dbcache=<n>           " + strprintf(_("Set database cache size in megabytes (%d to %d, default: %d)"), nMinDbCache, nMaxDbCache, nDefaultDbCache) + "\n";
    strUsage += "  -maxorphanblocks=<n>   " + strprintf(_("Keep at most <n> unconnectable blocks in memory (default: %u)"), DEFAULT_MAX_ORPHAN_BLOCKS) + "\n";
    strUsage += "  -maxorphantx=<n>       " + strprintf(_("Keep at most <n> unconnectable transactions in memory (default: %u)"), DEFAULT_MAX_ORPHAN_TRANSACTIONS) + "\n";
    strUsage += "  -stopafterblockimport  " + _("Stop running after importing blocks from disk (default: 0)") + "\n";
    strUsage += "  -synctimeout=<n>       " + _("Specify block download timeout in seconds (default: 60)") + "\n";
    strUsage += "  -synctime              " + _("Sync time with other nodes. Disable if time on your system is precise e.g. syncing with NTP (default: 1)") + "\n";

    strUsage += "\n" + _("SSL options: (see the Bitcoin Wiki for SSL setup instructions)") + "\n";
    strUsage += "  -rpcssl                                  " + _("Use OpenSSL (https) for JSON-RPC connections") + "\n";
    strUsage += "  -rpcsslcertificatechainfile=<file.cert>  " + _("Server certificate file (default: server.cert)") + "\n";
    strUsage += "  -rpcsslprivatekeyfile=<file.pem>         " + _("Server private key (default: server.pem)") + "\n";
    strUsage += "  -rpcsslciphers=<ciphers>                 " + _("Acceptable ciphers (default: TLSv1.2+HIGH:TLSv1+HIGH:!SSLv2:!aNULL:!eNULL:!3DES:@STRENGTH)") + "\n";

    strUsage += "  -litemode=<n>          " + _("Disable all Masternode and Darksend related functionality (0-1, default: 0)") + "\n";

    strUsage += "\n" + _("Masternode options:") + "\n";
    strUsage += "  -masternode=<n>            " + _("Enable the client to act as a masternode (0-1, default: 0)") + "\n";
    strUsage += "  -mnconf=<file>             " + _("Specify masternode configuration file (default: masternode.conf)") + "\n";
    strUsage += "  -masternodeprivkey=<n>     " + _("Set the masternode private key") + "\n";
    strUsage += "  -masternodeaddr=<n>        " + _("Set external address:port to get to this masternode (example: address:port)") + "\n";
    strUsage += "  -masternodeminprotocol=<n> " + _("Ignore masternodes less than version (example: 70007; default : 0)") + "\n";

    strUsage += "\n" + _("Darksend options:") + "\n";
    strUsage += "  -enabledarksend=<n>          " + _("Enable use of automated darksend for funds stored in this wallet (0-1, default: 0)") + "\n";
    strUsage += "  -darksendrounds=<n>          " + _("Use N separate masternodes to anonymize funds  (2-8, default: 2)") + "\n";
    strUsage += "  -anonymizeLindaamount=<n> " + _("Keep N Linda anonymized (default: 0)") + "\n";
    strUsage += "  -liquidityprovider=<n>       " + _("Provide liquidity to Darksend by infrequently mixing coins on a continual basis (0-100, default: 0, 1=very frequent, high fees, 100=very infrequent, low fees)") + "\n";

    strUsage += "\n" + _("InstantX options:") + "\n";
    strUsage += "  -enableinstantx=<n>    " + _("Enable instantx, show confirmations for locked transactions (bool, default: true)") + "\n";
    strUsage += "  -instantxdepth=<n>     " + _("Show N confirmations for a successfully locked transaction (0-9999, default: 1)") + "\n";

    return strUsage;
}

std::string LicenseInfo()
{
    return FormatParagraph(strprintf(_("Copyright (C) 2017-%i The Linda Project Developers"), COPYRIGHT_YEAR)) + "\n" +
           "\n" +
           FormatParagraph(_("This is experimental software.")) + "\n" +
           "\n" +
           FormatParagraph(_("Distributed under the MIT/X11 software license, see the accompanying file COPYING or <http://www.opensource.org/licenses/mit-license.php>.")) + "\n" +
           "\n" +
           FormatParagraph(_("This product includes software developed by the OpenSSL Project for use in the OpenSSL Toolkit <https://www.openssl.org/> and cryptographic software written by Eric Young and UPnP software written by Thomas Bernard.")) +
           "\n";
}

/** Sanity checks
 *  Ensure that Bitcoin is running in a usable environment with all
 *  necessary library support.
 */
bool InitSanityCheck(void)
{
    if (!ECC_InitSanityCheck()) {
        InitError("OpenSSL appears to lack support for elliptic curve cryptography. For more "
                  "information, visit https://en.bitcoin.it/wiki/OpenSSL_and_EC_Libraries");
        return false;
    }

    if (!glibc_sanity_test() || !glibcxx_sanity_test())
        return false;

    return true;
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

void ThreadImport(std::vector<boost::filesystem::path> vImportFiles)
{
    RenameThread("Linda-loadblk");

    // -reindex
    if (fReindex) {
        CImportingNow imp;
        int nFile = 0;
        while (true) {
            CDiskBlockPos pos(nFile, 0);
            if (!boost::filesystem::exists(GetBlockPosFilename(pos, "blk")))
                break; // No block files left to reindex
            FILE* file = OpenBlockFile(pos, true);
            if (!file)
                break; // This error is logged in OpenBlockFile
            LogPrintf("Reindexing block file blk%05u.dat...\n", (unsigned int)nFile);
            LoadExternalBlockFile(file, &pos);
            nFile++;
        }
        pblocktree->WriteReindexing(false);
        fReindex = false;
        LogPrintf("Reindexing finished\n");
        // To avoid ending up in a situation without genesis block, re-try initializing (no-op if reindexing worked):
        InitBlockIndex();
    }
    // hardcoded $DATADIR/bootstrap.dat
    filesystem::path pathBootstrap = GetDataDir() / "bootstrap.dat";
    if (filesystem::exists(pathBootstrap)) {
        FILE* file = fopen(pathBootstrap.string().c_str(), "rb");
        if (file) {
            CImportingNow imp;
            filesystem::path pathBootstrapOld = GetDataDir() / "bootstrap.dat.old";
            LogPrintf("Importing bootstrap.dat...\n");
            LoadExternalBlockFile(file);
            RenameOver(pathBootstrap, pathBootstrapOld);
        } else {
            LogPrintf("Warning: Could not open bootstrap file %s\n", pathBootstrap.string());
        }
    }
    // -loadblock=
    BOOST_FOREACH (boost::filesystem::path& path, vImportFiles) {
        FILE* file = fopen(path.string().c_str(), "rb");
        if (file) {
            CImportingNow imp;
            LogPrintf("Importing blocks file %s...\n", path.string());
            LoadExternalBlockFile(file);
        } else {
            LogPrintf("Warning: Could not open blocks file %s\n", path.string());
        }
    }

    if (GetBoolArg("-stopafterblockimport", false)) {
        LogPrintf("Stopping after block import\n");
        StartShutdown();
    }
}

/** Initialize Lindacoin.
 *  @pre Parameters should be parsed and config file should be read.
 */
bool AppInit2(boost::thread_group& threadGroup)
{
    // ********************************************************* Step 1: setup
#ifdef _MSC_VER
    // Turn off Microsoft heap dump noise
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, CreateFileA("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0));
#endif
#if _MSC_VER >= 1400
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
    if (setProcDEPPol != NULL)
        setProcDEPPol(PROCESS_DEP_ENABLE);

    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsadata);
    if (ret != NO_ERROR || LOBYTE(wsadata.wVersion) != 2 || HIBYTE(wsadata.wVersion) != 2) {
        return InitError(strprintf("Error: Winsock library failed to start (WSAStartup returned error %d)", ret));
    }
#endif
#ifndef WIN32

    if (GetBoolArg("-sysperms", false)) {
#ifdef ENABLE_WALLET
        if (!GetBoolArg("-disablewallet", false))
            return InitError("Error: -sysperms is not allowed in combination with enabled wallet functionality");
#endif
    } else {
        umask(077);
    }

    // Clean shutdown on SIGTERM
    registerSignalHandler(SIGTERM, HandleSIGTERM);
    registerSignalHandler(SIGINT, HandleSIGTERM);

    // Reopen debug.log on SIGHUP
    registerSignalHandler(SIGHUP, HandleSIGHUP);
    // Ignore SIGPIPE, otherwise it will bring the daemon down if the client closes unexpectedly
    signal(SIGPIPE, SIG_IGN);
#else
    SetConsoleCtrlHandler(consoleCtrlHandler, true);
#endif

    // ********************************************************* Step 2: parameter interactions

    nNodeLifespan = GetArg("-addrlifespan", 7);
    fUseFastIndex = GetBoolArg("-fastindex", true);
    nMinerSleep = GetArg("-minersleep", 500);

    nDerivationMethodIndex = 0;

    if (mapArgs.count("-bind") || mapArgs.count("-whitebind")) {
        // when specifying an explicit binding address, you want to listen on it
        // even when -connect or -proxy is specified
        if (SoftSetBoolArg("-listen", true))
            LogPrintf("AppInit2 : parameter interaction: -bind or -whitebind set -> setting -listen=1\n");
    }
    // Process Masternode config
    std::string err;
    masternodeConfig.read(err);
    if (err.empty())
        LogPrintf("Error: while parsing masternode.conf Error: %s \n", err);

    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0) {
        // when only connecting to trusted nodes, do not seed via DNS, or listen by default
        if (SoftSetBoolArg("-dnsseed", false))
            LogPrintf("AppInit2 : parameter interaction: -connect set -> setting -dnsseed=0\n");
        if (SoftSetBoolArg("-listen", false))
            LogPrintf("AppInit2 : parameter interaction: -connect set -> setting -listen=0\n");
    }

    if (mapArgs.count("-proxy")) {
        // to protect privacy, do not listen by default if a default proxy server is specified
        if (SoftSetBoolArg("-listen", false))
            LogPrintf("AppInit2 : parameter interaction: -proxy set -> setting -listen=0\n");
        // to protect privacy, do not discover addresses by default
        if (SoftSetBoolArg("-discover", false))
            LogPrintf("AppInit2 : parameter interaction: -proxy set -> setting -discover=0\n");
    }

    if (!GetBoolArg("-listen", true)) {
        // do not map ports or try to retrieve public IP when not listening (pointless)
        if (SoftSetBoolArg("-upnp", false))
            LogPrintf("AppInit2 : parameter interaction: -listen=0 -> setting -upnp=0\n");
        if (SoftSetBoolArg("-discover", false))
            LogPrintf("AppInit2 : parameter interaction: -listen=0 -> setting -discover=0\n");
    }

    if (mapArgs.count("-externalip")) {
        // if an explicit public IP is specified, do not try to find others
        if (SoftSetBoolArg("-discover", false))
            LogPrintf("AppInit2 : parameter interaction: -externalip set -> setting -discover=0\n");
    }

    if (GetBoolArg("-salvagewallet", false)) {
        // Rewrite just private keys: rescan to find transactions
        if (SoftSetBoolArg("-rescan", true))
            LogPrintf("AppInit2 : parameter interaction: -salvagewallet=1 -> setting -rescan=1\n");
    }

    // -zapwallettx implies a rescan
    if (GetBoolArg("-zapwallettxes", false)) {
        if (SoftSetBoolArg("-rescan", true))
            LogPrintf("AppInit2 : parameter interaction: -zapwallettxes=1 -> setting -rescan=1\n");
    }

    // Make sure enough file descriptors are available
    int nBind = std::max((int)mapArgs.count("-bind") + (int)mapArgs.count("-whitebind"), 1);
    nMaxConnections = GetArg("-maxconnections", 256);
    nMaxConnections = max(min(nMaxConnections, (int)(FD_SETSIZE - nBind - MIN_CORE_FILEDESCRIPTORS)), 0);
    int nFD = RaiseFileDescriptorLimit(nMaxConnections + MIN_CORE_FILEDESCRIPTORS);
    if (nFD < MIN_CORE_FILEDESCRIPTORS)
        return InitError(_("Not enough file descriptors available."));
    if (nFD - MIN_CORE_FILEDESCRIPTORS < nMaxConnections)
        nMaxConnections = nFD - MIN_CORE_FILEDESCRIPTORS;
    // ********************************************************* Step 3: parameter-to-internal-flags


    fDebug = !mapMultiArgs["-debug"].empty();
    // Special-case: if -debug=0/-nodebug is set, turn off debugging messages
    const vector<string>& categories = mapMultiArgs["-debug"];
    if (GetBoolArg("-nodebug", false) || find(categories.begin(), categories.end(), string("0")) != categories.end())
        fDebug = false;

    mempool.setSanityCheck(GetBoolArg("-checkmempool", Params().DefaultCheckMemPool()));
    Checkpoints::fEnabled = GetBoolArg("-checkpoints", true);
    // -par=0 means autodetect, but nScriptCheckThreads==0 means no concurrency
    nScriptCheckThreads = GetArg("-par", DEFAULT_SCRIPTCHECK_THREADS);
    if (nScriptCheckThreads <= 0)
        nScriptCheckThreads += boost::thread::hardware_concurrency();
    if (nScriptCheckThreads <= 1)
        nScriptCheckThreads = 0;
    else if (nScriptCheckThreads > MAX_SCRIPTCHECK_THREADS)
        nScriptCheckThreads = MAX_SCRIPTCHECK_THREADS;

    // Check for -debugnet
    if (GetBoolArg("-debugnet", false))
        InitWarning(_("Warning: Unsupported argument -debugnet ignored, use -debug=net."));
    // Check for -tor - as this is a privacy risk to continue, exit here
    if (GetBoolArg("-tor", false))
        return InitError(_("Error: Unsupported argument -tor found, use -onion."));
    if (GetBoolArg("-benchmark", false))
        InitWarning(_("Warning: Unsupported argument -benchmark ignored, use -debug=bench."));
    // Check for -socks - as this is a privacy risk to continue, exit here
    if (mapArgs.count("-socks"))
        return InitError(_("Error: Unsupported argument -socks found. Setting SOCKS version isn't possible anymore, only SOCKS5 proxies are supported."));

    fServer = GetBoolArg("-server", false);
    fPrintToConsole = GetBoolArg("-printtoconsole", false);
    fLogTimestamps = GetBoolArg("-logtimestamps", true);
    fLogIPs = GetBoolArg("-logips", false);
#ifdef ENABLE_WALLET
    bool fDisableWallet = GetBoolArg("-disablewallet", false);
#endif

    nConnectTimeout = GetArg("-timeout", DEFAULT_CONNECT_TIMEOUT);
    if (nConnectTimeout <= 0)
        nConnectTimeout = DEFAULT_CONNECT_TIMEOUT;

    // Fee-per-kilobyte amount considered the same as "free"
    // If you are mining, be careful setting this:
    // if you set it to zero then
    // a transaction spammer can cheaply fill blocks using
    // 1-satoshi-fee transactions. It should be set above the real
    // cost to you of processing a transaction.
    if (mapArgs.count("-minrelaytxfee")) {
        CAmount n = 0;
        if (ParseMoney(mapArgs["-minrelaytxfee"], n) && n > 0)
            ::minRelayTxFee = CFeeRate(n);
        else
            return InitError(strprintf(_("Invalid amount for -minrelaytxfee=<amount>: '%s'"), mapArgs["-minrelaytxfee"]));
    }

#ifdef ENABLE_WALLET
    if (mapArgs.count("-mintxfee")) {
        CAmount n = 0;
        if (ParseMoney(mapArgs["-mintxfee"], n) && n > 0)
            CWallet::minTxFee = CFeeRate(n);
        else
            return InitError(strprintf(_("Invalid amount for -mintxfee=<amount>: '%s'"), mapArgs["-mintxfee"]));
    }
    if (mapArgs.count("-paytxfee")) {
        CAmount nFeePerK = 0;
        if (!ParseMoney(mapArgs["-paytxfee"], nFeePerK))
            return InitError(strprintf(_("Invalid amount for -paytxfee=<amount>: '%s'"), mapArgs["-paytxfee"]));
        if (nFeePerK > nHighTransactionFeeWarning)
            InitWarning(_("Warning: -paytxfee is set very high! This is the transaction fee you will pay if you send a transaction."));
        payTxFee = CFeeRate(nFeePerK, 10000);
        if (payTxFee < ::minRelayTxFee) {
            return InitError(strprintf(_("Invalid amount for -paytxfee=<amount>: '%s' (must be at least %s)"),
                                       mapArgs["-paytxfee"], ::minRelayTxFee.ToString()));
        }
    }
    nTxConfirmTarget = GetArg("-txconfirmtarget", 1);
    bSpendZeroConfChange = GetArg("-spendzeroconfchange", true);

    std::string strWalletFile = GetArg("-wallet", "wallet.dat");
#endif // ENABLE_WALLET

    fIsBareMultisigStd = GetArg("-permitbaremultisig", true) != 0;

    fConfChange = GetBoolArg("-confchange", false);
    fMinimizeCoinAge = GetBoolArg("-minimizecoinage", false);

#ifdef ENABLE_WALLET
    if (mapArgs.count("-mininput")) {
        if (!ParseMoney(mapArgs["-mininput"], nMinimumInputValue))
            return InitError(strprintf(_("Invalid amount for -mininput=<amount>: '%s'"), mapArgs["-mininput"]));
    }
#endif

    // ********************************************************* Step 4: application initialization: dir lock, daemonize, pidfile, debug log

    // Sanity check
    if (!InitSanityCheck())
        return InitError(_("Initialization sanity check failed. Linda is shutting down."));

    std::string strDataDir = GetDataDir().string();
#ifdef ENABLE_WALLET
    // Wallet file must be a plain filename without a directory
    if (strWalletFile != boost::filesystem::basename(strWalletFile) + boost::filesystem::extension(strWalletFile))
        return InitError(strprintf(_("Wallet %s resides outside data directory %s"), strWalletFile, strDataDir));
#endif
    // Make sure only a single Lindacoin process is using the data directory.
    boost::filesystem::path pathLockFile = GetDataDir() / ".lock";
    FILE* file = fopen(pathLockFile.string().c_str(), "a"); // empty lock file; created if it doesn't exist.
    if (file)
        fclose(file);
    static boost::interprocess::file_lock lock(pathLockFile.string().c_str());
    if (!lock.try_lock())
        return InitError(strprintf(_("Cannot obtain a lock on data directory %s. Linda Core is probably already running."), strDataDir));
#ifndef WIN32
    CreatePidFile(GetPidFile(), getpid());
#endif
    if (GetBoolArg("-shrinkdebugfile", !fDebug))
        ShrinkDebugFile();
    LogPrintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    LogPrintf("Linda version %s (%s)\n", FormatFullVersion(), CLIENT_DATE);
    LogPrintf("Using OpenSSL version %s\n", SSLeay_version(SSLEAY_VERSION));
#ifdef ENABLE_WALLET
    LogPrintf("Using BerkeleyDB version %s\n", DbEnv::version(0, 0, 0));
#endif
    if (!fLogTimestamps)
        LogPrintf("Startup time: %s\n", DateTimeStrFormat("%x %H:%M:%S", GetTime()));
    LogPrintf("Default data directory %s\n", GetDefaultDataDir().string());
    LogPrintf("Used data directory %s\n", strDataDir);
    LogPrintf("Using config file %s\n", GetConfigFile().string());
    std::ostringstream strErrors;

    if (mapArgs.count("-masternodepaymentskey")) // masternode payments priv key
    {
        if (!masternodePayments.SetPrivKey(GetArg("-masternodepaymentskey", "")))
            return InitError(_("Unable to sign masternode payment winner, wrong key?"));
        if (!sporkManager.SetPrivKey(GetArg("-masternodepaymentskey", "")))
            return InitError(_("Unable to sign spork message, wrong key?"));
    }

    //ignore masternodes below protocol version
    CMasterNode::minProtoVersion = GetArg("-masternodeminprotocol", MIN_MN_PROTO_VERSION);


    if (nScriptCheckThreads) {
        LogPrintf("Using %u threads for script verification\n", nScriptCheckThreads);
        for (int i = 0; i < nScriptCheckThreads - 1; i++)
            threadGroup.create_thread(&ThreadScriptCheck);
    }

    /* Start the RPC server already.  It will be started in "warmup" mode
     * and not really process calls already (but it will signify connections
     * that the server is there and will be ready later).  Warmup mode will
     * be disabled when initialisation is finished.
     */
    if (fServer) {
        uiInterface.InitMessage.connect(SetRPCWarmupStatus);
        StartRPCThreads();
    }

    int64_t nStart;

    // ********************************************************* Step 5: verify wallet database integrity
#ifdef ENABLE_WALLET
    if (!fDisableWallet) {
        LogPrintf("Using wallet %s\n", strWalletFile);
        uiInterface.InitMessage(_("Verifying wallet..."));

        if (!bitdb.Open(GetDataDir())) {
            // try moving the database env out of the way
            boost::filesystem::path pathDatabase = GetDataDir() / "database";
            boost::filesystem::path pathDatabaseBak = GetDataDir() / strprintf("database.%d.bak", GetTime());
            try {
                boost::filesystem::rename(pathDatabase, pathDatabaseBak);
                LogPrintf("Moved old %s to %s. Retrying.\n", pathDatabase.string(), pathDatabaseBak.string());
            } catch (boost::filesystem::filesystem_error& error) {
                // failure is ok (well, not really, but it's not worse than what we started with)
            }

            // try again
            if (!bitdb.Open(GetDataDir())) {
                // if it still fails, it probably means we can't even create the database env
                string msg = strprintf(_("Error initializing wallet database environment %s!"), strDataDir);
                return InitError(msg);
            }
        }

        if (GetBoolArg("-salvagewallet", false)) {
            // Recover readable keypairs:
            if (!CWalletDB::Recover(bitdb, strWalletFile, true))
                return false;
        }

        if (filesystem::exists(GetDataDir() / strWalletFile)) {
            CDBEnv::VerifyResult r = bitdb.Verify(strWalletFile, CWalletDB::Recover);
            if (r == CDBEnv::RECOVER_OK) {
                string msg = strprintf(_("Warning: wallet.dat corrupt, data salvaged!"
                                         " Original wallet.dat saved as wallet.{timestamp}.bak in %s; if"
                                         " your balance or transactions are incorrect you should"
                                         " restore from a backup."),
                                       strDataDir);
                InitWarning(msg);
            }
            if (r == CDBEnv::RECOVER_FAIL)
                return InitError(_("wallet.dat corrupt, salvage failed"));
        }

        // Initialize KeePass Integration
        keePassInt.init();
    }  // (!fDisableWallet)
#endif // ENABLE_WALLET
    // ********************************************************* Step 6: network initialization

    RegisterNodeSignals(GetNodeSignals());

    if (mapArgs.count("-onlynet")) {
        std::set<enum Network> nets;
        BOOST_FOREACH (std::string snet, mapMultiArgs["-onlynet"]) {
            enum Network net = ParseNetwork(snet);
            if (net == NET_TOR)
                fOnlyTor = true;

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
    if (mapArgs.count("-whitelist")) {
        BOOST_FOREACH (const std::string& net, mapMultiArgs["-whitelist"]) {
            CSubNet subnet(net);
            if (!subnet.IsValid())
                return InitError(strprintf(_("Invalid netmask specified in -whitelist: '%s'"), net));
            CNode::AddWhitelistedRange(subnet);
        }
    }

    CService addrProxy;
    bool fProxy = false;
    if (mapArgs.count("-proxy")) {
        addrProxy = CService(mapArgs["-proxy"], 9050);
        if (!addrProxy.IsValid())
            return InitError(strprintf(_("Invalid -proxy address: '%s'"), mapArgs["-proxy"]));

        if (!IsLimited(NET_IPV4))
            SetProxy(NET_IPV4, addrProxy);
        if (!IsLimited(NET_IPV6))
            SetProxy(NET_IPV6, addrProxy);
        SetNameProxy(addrProxy);
        fProxy = true;
    }

    // -onion can override normal proxy, -noonion disables tor entirely
    if (!(mapArgs.count("-onion") && mapArgs["-onion"] == "0") &&
        (fProxy || mapArgs.count("-onion"))) {
        CService addrOnion;
        if (!mapArgs.count("-onion"))
            addrOnion = addrProxy;
        else
            addrOnion = CService(mapArgs["-onion"], 9050);
        if (!addrOnion.IsValid())
            return InitError(strprintf(_("Invalid -onion address: '%s'"), mapArgs["-onion"]));
        SetProxy(NET_TOR, addrOnion);
        SetReachable(NET_TOR);
    }

    // see Step 2: parameter interactions for more information about these
    fListen = GetBoolArg("-listen", true);
    fDiscover = GetBoolArg("-discover", true);
    fNameLookup = GetBoolArg("-dns", true);

    bool fBound = false;
    if (fListen) {
        if (mapArgs.count("-bind") || mapArgs.count("-whitebind")) {
            BOOST_FOREACH (std::string strBind, mapMultiArgs["-bind"]) {
                CService addrBind;
                if (!Lookup(strBind.c_str(), addrBind, GetListenPort(), false))
                    return InitError(strprintf(_("Cannot resolve -bind address: '%s'"), strBind));
                fBound |= Bind(addrBind, (BF_EXPLICIT | BF_REPORT_ERROR));
            }
            BOOST_FOREACH (std::string strBind, mapMultiArgs["-whitebind"]) {
                CService addrBind;
                if (!Lookup(strBind.c_str(), addrBind, 0, false))
                    return InitError(strprintf(_("Cannot resolve -whitebind address: '%s'"), strBind));
                if (addrBind.GetPort() == 0)
                    return InitError(strprintf(_("Need to specify a port with -whitebind: '%s'"), strBind));
                fBound |= Bind(addrBind, (BF_EXPLICIT | BF_REPORT_ERROR | BF_WHITELIST));
            }
        } else {
            struct in_addr inaddr_any;
            inaddr_any.s_addr = INADDR_ANY;
            if (!IsLimited(NET_IPV6))
                fBound |= Bind(CService(in6addr_any, GetListenPort()), BF_NONE);
            if (!IsLimited(NET_IPV4))
                fBound |= Bind(CService(inaddr_any, GetListenPort()), !fBound ? BF_REPORT_ERROR : BF_NONE);
        }
        if (!fBound)
            return InitError(_("Failed to listen on any port. Use -listen=0 if you want this."));
    }

    if (mapArgs.count("-externalip")) {
        BOOST_FOREACH (string strAddr, mapMultiArgs["-externalip"]) {
            CService addrLocal(strAddr, GetListenPort(), fNameLookup);
            if (!addrLocal.IsValid())
                return InitError(strprintf(_("Cannot resolve -externalip address: '%s'"), strAddr));
            AddLocal(CService(strAddr, GetListenPort(), fNameLookup), LOCAL_MANUAL);
        }
    }

#ifdef ENABLE_WALLET
    if (mapArgs.count("-reservebalance")) // ppcoin: reserve balance amount
    {
        if (!ParseMoney(mapArgs["-reservebalance"], nReserveBalance)) {
            InitError(_("Invalid amount for -reservebalance=<amount>"));
            return false;
        }
    }
#endif

    BOOST_FOREACH (string strDest, mapMultiArgs["-seednode"])
        AddOneShot(strDest);

    // ********************************************************* Step 7: load blockchain

    fReindex = GetBoolArg("-reindex", false);

    // Upgrading to BTC 0.8; hard-link the old blknnnn.dat files into /blocks/
    filesystem::path blocksDir = GetDataDir() / "blocks";
    if (!filesystem::exists(blocksDir)) {
        filesystem::create_directories(blocksDir);
        bool linked = false;
        for (unsigned int i = 1; i < 10000; i++) {
            filesystem::path source = GetDataDir() / strprintf("blk%04u.dat", i);
            if (!filesystem::exists(source))
                break;
            filesystem::path dest = blocksDir / strprintf("blk%05u.dat", i - 1);
            try {
                filesystem::create_hard_link(source, dest);
                LogPrintf("Hardlinked %s -> %s\n", source.string().c_str(), dest.string().c_str());
                linked = true;
            } catch (filesystem::filesystem_error& e) {
                // Note: hardlink creation failing is not a disaster, it just means
                // blocks will get re-downloaded from peers.
                LogPrintf("Error hardlinking blk%04u.dat : %s\n", i, e.what());
                break;
            }
        }
        if (linked) {
            fReindex = true;
        }
    }

    // cache size calculations
    size_t nTotalCache = (GetArg("-dbcache", nDefaultDbCache) << 20);
    if (nTotalCache < (nMinDbCache << 20))
        nTotalCache = (nMinDbCache << 20); // total cache cannot be less than nMinDbCache
    else if (nTotalCache > (nMaxDbCache << 20))
        nTotalCache = (nMaxDbCache << 20); // total cache cannot be greater than nMaxDbCache
    size_t nBlockTreeDBCache = nTotalCache / 8;
    if (nBlockTreeDBCache > (1 << 21) && false)
        nBlockTreeDBCache = (1 << 21); // block tree db cache shouldn't be larger than 2 MiB
    nTotalCache -= nBlockTreeDBCache;
    size_t nCoinDBCache = nTotalCache / 2; // use half of the remaining cache for coindb cache
    nTotalCache -= nCoinDBCache;
    nCoinCacheSize = nTotalCache / 300; // coins in memory require around 300 bytes

    bool fLoaded = false;
    while (!fLoaded) {
        bool fReset = fReindex;
        std::string strLoadError;

        uiInterface.InitMessage(_("Loading block index..."));

        nStart = GetTimeMillis();
        do {
            try {
                UnloadBlockIndex();
                delete pcoinsTip;
                delete pcoinsdbview;
                delete pblocktree;

                pblocktree = new CBlockTreeDB(nBlockTreeDBCache, false, fReindex);
                pcoinsdbview = new CCoinsViewDB(nCoinDBCache, false, fReindex);
                pcoinsTip = new CCoinsViewCache(pcoinsdbview);

                if (fReindex)
                    pblocktree->WriteReindexing(true);

                if (!LoadBlockIndex()) {
                    strLoadError = _("Error loading block database");
                    break;
                }

                // If the loaded chain has a wrong genesis, bail out immediately
                // (we're likely using a testnet datadir, or the other way around).
                if (!mapBlockIndex.empty() && mapBlockIndex.count(Params().HashGenesisBlock()) == 0)
                    return InitError(_("Incorrect or no genesis block found. Wrong datadir for network?"));

                // Initialize the block index (no-op if non-empty database was already loaded)
                if (!InitBlockIndex()) {
                    strLoadError = _("Error initializing block database");
                    break;
                }

                uiInterface.InitMessage(_("Verifying block database..."));
                if (!CVerifyDB().VerifyDB(pcoinsdbview, GetArg("-checklevel", 3), GetArg("-checkblocks", 288))) {
                    strLoadError = _("Corrupted block database detected");
                    break;
                }
            } catch (std::exception& e) {
                if (fDebug)
                    LogPrintf("%s\n", e.what());
                strLoadError = _("Error opening block database");
                break;
            }

            fLoaded = true;
        } while (false);

        if (!fLoaded) {
            // first suggest a reindex
            if (!fReset) {
                bool fRet = uiInterface.ThreadSafeMessageBox(
                    strLoadError + ".\n\n" + _("Do you want to rebuild the block database now?"),
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


    // as LoadBlockIndex can take several minutes, it's possible the user
    // requested to kill bitcoin-qt during the last operation. If so, exit.
    // As the program has not fully started yet, Shutdown() is possibly overkill.
    if (fRequestShutdown) {
        LogPrintf("Shutdown requested. Exiting.\n");
        return false;
    }
    LogPrintf(" block index %15dms\n", GetTimeMillis() - nStart);

    if (GetBoolArg("-printblockindex", false) || GetBoolArg("-printblocktree", false)) {
        PrintBlockTree();
        return false;
    }

    if (mapArgs.count("-printblock")) {
        string strMatch = mapArgs["-printblock"];
        int nFound = 0;
        for (BlockMap::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi) {
            uint256 hash = (*mi).first;
            if (boost::algorithm::starts_with(hash.ToString(), strMatch)) {
                CBlockIndex* pindex = (*mi).second;
                CBlock block;
                ReadBlockFromDisk(block, pindex);
                block.BuildMerkleTree();
                LogPrintf("%s\n", block.ToString());
                nFound++;
            }
        }
        if (nFound == 0)
            LogPrintf("No blocks matching %s were found\n", strMatch);
        return false;
    }

    boost::filesystem::path est_path = GetDataDir() / FEE_ESTIMATES_FILENAME;
    CAutoFile est_filein(fopen(est_path.string().c_str(), "rb"), SER_DISK, CLIENT_VERSION);
    // Allowed to fail as this file IS missing on first startup.
    if (!est_filein.IsNull())
        mempool.ReadFeeEstimates(est_filein);

        // ********************************************************* Step 8: load wallet
#ifdef ENABLE_WALLET
    if (fDisableWallet) {
        pwalletMain = NULL;
        LogPrintf("Wallet disabled!\n");
    } else {
        // needed to restore wallet transaction meta data after -zapwallettxes
        std::vector<CWalletTx> vWtx;

        if (GetBoolArg("-zapwallettxes", false)) {
            uiInterface.InitMessage(_("Zapping all transactions from wallet..."));

            pwalletMain = new CWallet(strWalletFile);
            DBErrors nZapWalletRet = pwalletMain->ZapWalletTx(vWtx);
            if (nZapWalletRet != DB_LOAD_OK) {
                uiInterface.InitMessage(_("Error loading wallet.dat: Wallet corrupted"));
                return false;
            }

            delete pwalletMain;
            pwalletMain = NULL;
        }

        uiInterface.InitMessage(_("Loading wallet..."));

        nStart = GetTimeMillis();
        bool fFirstRun = true;
        pwalletMain = new CWallet(strWalletFile);
        DBErrors nLoadWalletRet = pwalletMain->LoadWallet(fFirstRun);
        if (nLoadWalletRet != DB_LOAD_OK) {
            if (nLoadWalletRet == DB_CORRUPT)
                strErrors << _("Error loading wallet.dat: Wallet corrupted") << "\n";
            else if (nLoadWalletRet == DB_NONCRITICAL_ERROR) {
                string msg(_("Warning: error reading wallet.dat! All keys read correctly, but transaction data"
                             " or address book entries might be missing or incorrect."));
                InitWarning(msg);
            } else if (nLoadWalletRet == DB_TOO_NEW)
                strErrors << _("Error loading wallet.dat: Wallet requires newer version of Linda Core") << "\n";
            else if (nLoadWalletRet == DB_NEED_REWRITE) {
                strErrors << _("Wallet needed to be rewritten: restart Linda Core to complete") << "\n";
                LogPrintf("%s", strErrors.str());
                return InitError(strErrors.str());
            } else
                strErrors << _("Error loading wallet.dat") << "\n";
        }

        if (GetBoolArg("-upgradewallet", fFirstRun)) {
            int nMaxVersion = GetArg("-upgradewallet", 0);
            if (nMaxVersion == 0) // the -upgradewallet without argument case
            {
                LogPrintf("Performing wallet upgrade to %i\n", FEATURE_LATEST);
                nMaxVersion = CLIENT_VERSION;
                pwalletMain->SetMinVersion(FEATURE_LATEST); // permanently upgrade the wallet immediately
            } else
                LogPrintf("Allowing wallet upgrade up to %i\n", nMaxVersion);
            if (nMaxVersion < pwalletMain->GetVersion())
                strErrors << _("Cannot downgrade wallet") << "\n";
            pwalletMain->SetMaxVersion(nMaxVersion);
        }

        if (fFirstRun) {
            // Create new keyUser and set as default key
            RandAddSeedPerfmon();

            CPubKey newDefaultKey;
            if (pwalletMain->GetKeyFromPool(newDefaultKey)) {
                pwalletMain->SetDefaultKey(newDefaultKey);
                if (!pwalletMain->SetAddressBook(pwalletMain->vchDefaultKey.GetID(), "", "receive"))
                    strErrors << _("Cannot write default address") << "\n";
            }
            pwalletMain->SetBestChain(chainActive.GetLocator());
        }

        LogPrintf("%s", strErrors.str());
        LogPrintf(" wallet      %15dms\n", GetTimeMillis() - nStart);

        RegisterValidationInterface(pwalletMain);

        CBlockIndex* pindexRescan = chainActive.Tip();
        if (GetBoolArg("-rescan", false))
            pindexRescan = chainActive.Genesis();
        else {
            CWalletDB walletdb(strWalletFile);
            CBlockLocator locator;
            if (walletdb.ReadBestBlock(locator))
                pindexRescan = chainActive.FindFork(locator);
            else
                pindexRescan = chainActive.Genesis();
        }
        if (chainActive.Tip() != pindexRescan && chainActive.Tip() && pindexRescan && chainActive.Height() > pindexRescan->nHeight) {
            uiInterface.InitMessage(_("Rescanning..."));
            LogPrintf("Rescanning last %i blocks (from block %i)...\n", chainActive.Height() - pindexRescan->nHeight, pindexRescan->nHeight);
            nStart = GetTimeMillis();
            pwalletMain->ScanForWalletTransactions(pindexRescan, true);
            LogPrintf(" rescan      %15dms\n", GetTimeMillis() - nStart);

            pwalletMain->SetBestChain(chainActive.GetLocator());

            nWalletDBUpdated++;

            // Restore wallet transaction metadata after -zapwallettxes=1
            if (GetBoolArg("-zapwallettxes", false) && GetArg("-zapwallettxes", "1") != "2") {
                CWalletDB walletdb(strWalletFile);

                BOOST_FOREACH (const CWalletTx& wtxOld, vWtx) {
                    uint256 hash = wtxOld.GetHash();
                    std::map<uint256, CWalletTx>::iterator mi = pwalletMain->mapWallet.find(hash);
                    if (mi != pwalletMain->mapWallet.end()) {
                        const CWalletTx* copyFrom = &wtxOld;
                        CWalletTx* copyTo = &mi->second;
                        copyTo->mapValue = copyFrom->mapValue;
                        copyTo->vOrderForm = copyFrom->vOrderForm;
                        copyTo->nTimeReceived = copyFrom->nTimeReceived;
                        copyTo->nTimeSmart = copyFrom->nTimeSmart;
                        copyTo->fFromMe = copyFrom->fFromMe;
                        copyTo->strFromAccount = copyFrom->strFromAccount;
                        copyTo->nOrderPos = copyFrom->nOrderPos;
                        copyTo->WriteToDisk();
                    }
                }
            }
        }
    }  // (!fDisableWallet)
#else  // ENABLE_WALLET
    LogPrintf("No wallet compiled in!\n");
#endif // !ENABLE_WALLET
    // ********************************************************* Step 9: import blocks

    // scan for better chains in the block chain database, that are not yet connected in the active best chain
    uiInterface.InitMessage(_("Importing blocks from block database..."));
    CValidationState state;
    if (!ActivateBestChain(state))
        strErrors << "Failed to connect best block";

    std::vector<boost::filesystem::path> vImportFiles;
    if (mapArgs.count("-loadblock")) {
        BOOST_FOREACH (string strFile, mapMultiArgs["-loadblock"])
            vImportFiles.push_back(strFile);
    }
    threadGroup.create_thread(boost::bind(&ThreadImport, vImportFiles));

    // ********************************************************* Step 10: load peers

    uiInterface.InitMessage(_("Loading addresses..."));

    nStart = GetTimeMillis();

    {
        CAddrDB adb;
        if (!adb.Read(addrman))
            LogPrintf("Invalid or missing peers.dat; recreating\n");
    }

    LogPrintf("Loaded %i addresses from peers.dat  %dms\n",
              addrman.size(), GetTimeMillis() - nStart);

    // ********************************************************* Step 11: start node

    if (!CheckDiskSpace())
        return false;

    if (!strErrors.str().empty())
        return InitError(strErrors.str());

    fMasterNode = GetBoolArg("-masternode", false);
    if (fMasterNode) {
        LogPrintf("IS DARKSEND MASTER NODE\n");
        strMasterNodeAddr = GetArg("-masternodeaddr", "");

        LogPrintf(" addr %s\n", strMasterNodeAddr.c_str());

        if (!strMasterNodeAddr.empty()) {
            CService addrTest = CService(strMasterNodeAddr);
            if (!addrTest.IsValid()) {
                return InitError("Invalid -masternodeaddr address: " + strMasterNodeAddr);
            }
        }

        strMasterNodePrivKey = GetArg("-masternodeprivkey", "");
        if (!strMasterNodePrivKey.empty()) {
            std::string errorMessage;

            CKey key;
            CPubKey pubkey;

            if (!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, key, pubkey)) {
                return InitError(_("Invalid masternodeprivkey. Please see documenation."));
            }

            activeMasternode.pubKeyMasternode = pubkey;

        } else {
            return InitError(_("You must specify a masternodeprivkey in the configuration. Please see documentation for help."));
        }
    }
    if (GetBoolArg("-mnconflock", true) && pwalletMain) {
        LOCK(pwalletMain->cs_wallet);
        LogPrintf("Locking Masternodes:\n");
        uint256 mnTxHash;
        unsigned int outputIndex;
        BOOST_FOREACH (CMasternodeConfig::CMasternodeEntry mne, masternodeConfig.getEntries()) {
            mnTxHash.SetHex(mne.getTxHash());
            outputIndex = boost::lexical_cast<unsigned int>(mne.getOutputIndex());
            CWalletTx tx;
            if (!pwalletMain->GetTransaction(mnTxHash, tx)) {
                LogPrintf("  %s %s - WAS NOT FOUND IN WALLET, was not locked\n", mne.getTxHash(), mne.getOutputIndex());
                continue;
            }
            // don't lock spent
            if (pwalletMain->IsSpent(mnTxHash, outputIndex)) {
                LogPrintf("  %s %s - IS NOT SPENDABLE, was not locked\n", mne.getTxHash(), mne.getOutputIndex());
                continue;
            }
            COutPoint outpoint = COutPoint(mnTxHash, outputIndex);
            pwalletMain->LockCoin(outpoint);
            LogPrintf("  %s %s - locked successfully\n", mne.getTxHash(), mne.getOutputIndex());
        }
    }

    fEnableDarksend = GetBoolArg("-enabledarksend", false);

    nDarksendRounds = GetArg("-darksendrounds", 2);
    if (nDarksendRounds > 16)
        nDarksendRounds = 16;
    if (nDarksendRounds < 1)
        nDarksendRounds = 1;

    nLiquidityProvider = GetArg("-liquidityprovider", 0); //0-100
    if (nLiquidityProvider != 0) {
        darkSendPool.SetMinBlockSpacing(std::min(nLiquidityProvider, 100) * 15);
        fEnableDarksend = true;
        nDarksendRounds = 99999;
    }

    nAnonymizeLindaAmount = GetArg("-anonymizeLindaamount", 0);
    if (nAnonymizeLindaAmount > 999999)
        nAnonymizeLindaAmount = 999999;
    if (nAnonymizeLindaAmount < 2)
        nAnonymizeLindaAmount = 2;

    bool fEnableInstantX = GetBoolArg("-enableinstantx", true);
    if (fEnableInstantX) {
        nInstantXDepth = GetArg("-instantxdepth", 5);
        if (nInstantXDepth > 60)
            nInstantXDepth = 60;
        if (nInstantXDepth < 0)
            nAnonymizeLindaAmount = 0;
    } else {
        nInstantXDepth = 0;
    }

    //lite mode disables all Masternode and Darksend related functionality
    fLiteMode = GetBoolArg("-litemode", false);
    if (fMasterNode && fLiteMode) {
        return InitError("You can not start a masternode in litemode");
    }

    LogPrintf("fLiteMode %d\n", fLiteMode);
    LogPrintf("nInstantXDepth %d\n", nInstantXDepth);
    LogPrintf("Darksend rounds %d\n", nDarksendRounds);
    LogPrintf("Anonymize Linda Amount %d\n", nAnonymizeLindaAmount);

    /* Denominations
       A note about convertability. Within Darksend pools, each denomination
       is convertable to another.
       For example:
       1Linda+1000 == (.1Linda+100)*10
       10Linda+10000 == (1Linda+1000)*10
    */
    darkSendDenominations.push_back((100000 * COIN) + 100000000);
    darkSendDenominations.push_back((10000 * COIN) + 10000000);
    darkSendDenominations.push_back((1000 * COIN) + 1000000);
    darkSendDenominations.push_back((100 * COIN) + 100000);
    darkSendDenominations.push_back((10 * COIN) + 10000);
    darkSendDenominations.push_back((1 * COIN) + 1000);
    darkSendDenominations.push_back((.1 * COIN) + 100);
    /* Disabled till we need them
    darkSendDenominations.push_back( (.01      * COIN)+10 );
    darkSendDenominations.push_back( (.001     * COIN)+1 );
    */

    darkSendPool.InitCollateralAddress();

    threadGroup.create_thread(boost::bind(&ThreadCheckDarkSendPool));


    RandAddSeedPerfmon();

    //// debug print
    LogPrintf("mapBlockIndex.size() = %u\n", mapBlockIndex.size());
    LogPrintf("nBestHeight = %d\n", chainActive.Height());
#ifdef ENABLE_WALLET
    LogPrintf("setKeyPool.size() = %u\n", pwalletMain ? pwalletMain->setKeyPool.size() : 0);
    LogPrintf("mapWallet.size() = %u\n", pwalletMain ? pwalletMain->mapWallet.size() : 0);
    LogPrintf("mapAddressBook.size() = %u\n", pwalletMain ? pwalletMain->mapAddressBook.size() : 0);
#endif

    StartNode(threadGroup);

#ifdef ENABLE_WALLET
    // Mine proof-of-stake blocks in the background
    if (!GetBoolArg("-staking", true))
        LogPrintf("Staking disabled\n");
    else if (pwalletMain)
        threadGroup.create_thread(boost::bind(&ThreadStakeMiner, pwalletMain, true));
#endif


#ifdef ENABLE_WALLET
    if (pwalletMain) {
        uiInterface.InitMessage(_("Reaccepting wallet transactions..."));
        // Add wallet transactions that aren't already in a block to mapTransactions
        pwalletMain->ReacceptWalletTransactions();

        // Run a thread to flush wallet periodically
        threadGroup.create_thread(boost::bind(&ThreadFlushWalletDB, boost::ref(pwalletMain->strWalletFile)));
    }
#endif

    // ********************************************************* Step 12: finished
    SetRPCWarmupFinished();
    uiInterface.InitMessage(_("Done loading"));

    return !fRequestShutdown;
}
