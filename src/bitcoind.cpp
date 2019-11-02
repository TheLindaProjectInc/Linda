// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "clientversion.h"
#include "init.h"
#include "main.h"
#include "noui.h"
#include "rpcserver.h"
#include "scheduler.h"
#include "ui_interface.h"
#include "util.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/thread.hpp>

static bool fDaemon;

void WaitForShutdown(boost::thread_group* threadGroup)
{
    bool fShutdown = ShutdownRequested();
    //! Tell the main threads to shutdown.
    while (!fShutdown) {
        MilliSleep(200);
        fShutdown = ShutdownRequested();
    }
    if (threadGroup) {
        threadGroup->interrupt_all();
        threadGroup->join_all();
    }
}

//////////////////////////////////////////////////////////////////////////////
/**
 * Start
 */
bool AppInit(int argc, char* argv[])
{
    boost::thread_group threadGroup;
    CScheduler scheduler;


    bool fRet = false;
    try {
        /**
         * Parameters
         *
         * If Qt is used, parameters/bitcoin.conf are parsed in qt/bitcoin.cpp's main()
         */
        ParseParameters(argc, argv);
        //! check migrate linda datadir to metrix datadir
        checkMigrateDataDir();
        if (!boost::filesystem::is_directory(GetDataDir(false))) {
            fprintf(stderr, "Error: Specified data directory \"%s\" does not exist.\n", mapArgs["-datadir"].c_str());
            return false;
        }
        try 
        {
            ReadConfigFile(mapArgs, mapMultiArgs);
        } catch (std::exception& e) {
            fprintf(stderr, "Error reading configuration file: %s\n", e.what());
            return false;
        }
        //! Check for -testnet or -regtest parameter (Params() calls are only valid after this clause)
        if (!SelectParamsFromCommandLine()) {
            fprintf(stderr, "Error: Invalid combination of -regtest and -testnet.\n");
            return false;
        }

        if (mapArgs.count("-?") || mapArgs.count("-h") ||  mapArgs.count("-help") || mapArgs.count("-version")) {
            std::string strUsage = _("Metrix Version") + " " + _("version") + " " + FormatFullVersion() + "\n";

            if (!mapArgs.count("-version")) {
                strUsage += LicenseInfo();
            } else {
                strUsage += "\n" + _("Usage:") + "\n" +
                            "  metrixd [options]                     " + _("Start Metrix Core Daemon") + "\n";

                strUsage += "\n" + HelpMessage(HMM_BITCOIND);
            }
            fprintf(stdout, "%s", strUsage.c_str());
            return false;
        }

        //! Command-line RPC
        for (int i = 1; i < argc; i++)
            if (!IsSwitchChar(argv[i][0]) && !boost::algorithm::istarts_with(argv[i], "Metrix:"))
                fCommandLine = true;

        if (fCommandLine) {
            fprintf(stderr, "Error: There is no RPC client functionality in metrixd anymore. Use the metrix-cli utility instead.\n");
            exit(1);
        }
#ifndef WIN32
        fDaemon = GetBoolArg("-daemon", false);
        if (fDaemon) {
            fprintf(stdout, "Metrix server starting\n");

            //! Daemonize
            pid_t pid = fork();
            if (pid < 0) {
                fprintf(stderr, "Error: fork() returned %d errno %d\n", pid, errno);
                return false;
            }
            if (pid > 0) //! Parent process, pid is child process id
            {
                return true;
            }
            //! Child process falls through to rest of initialization

            pid_t sid = setsid();
            if (sid < 0)
                fprintf(stderr, "Error: setsid() returned %d errno %d\n", sid, errno);
        }
#endif
        SoftSetBoolArg("-server", true);

        fRet = AppInit2(threadGroup, scheduler);
    } catch (std::exception& e) {
        PrintExceptionContinue(&e, "AppInit()");
        throw;
    } catch (...) {
        PrintExceptionContinue(NULL, "AppInit()");
        throw;
    }

    if (!fRet) {
        threadGroup.interrupt_all();
        /**
         * threadGroup.join_all(); was left out intentionally here, because we didn't re-test all of
         * the startup-failure cases to make sure they don't result in a hang due to some
         * thread-blocking-waiting-for-another-thread-during-startup case
         */
    } else {
        WaitForShutdown(&threadGroup);
    }
    Shutdown();

    return fRet;
}

int main(int argc, char* argv[])
{
    SetupEnvironment();

    //! Connect bitcoind signal handlers
    noui_connect();

    return (AppInit(argc, argv) ? 0 : 1);
}