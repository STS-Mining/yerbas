// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2019 The Dash Core developers
// Copyright (c) 2020 The Memeium developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Server/client environment: argument handling, config file parsing,
 * logging, thread wrappers, startup time
 */
#ifndef BITCOIN_UTIL_H
#define BITCOIN_UTIL_H

#if defined(HAVE_CONFIG_H)
#include "config/memeium-config.h"
#endif

#include "amount.h"
#include "compat.h"
#include "fs.h"
#include "sync.h"
#include "tinyformat.h"
#include "utiltime.h"

#include <atomic>
#include <exception>
#include <map>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/signals2/signal.hpp>

// Debugging macros

// Uncomment the following line to enable debugging messages
// or enable on a per file basis prior to inclusion of util.h
// #define ENABLE_MEMEIUM_DEBUG
#ifdef ENABLE_MEMEIUM_DEBUG
#define DBG(x) x
#else
#define DBG(x)
#endif

// Memeium only features

extern bool fSmartnodeMode;
extern bool fLiteMode;
extern int nWalletBackups;

// Application startup time (used for uptime calculation)
int64_t GetStartupTime();

static const bool DEFAULT_LOGTIMEMICROS = false;
static const bool DEFAULT_LOGIPS = false;
static const bool DEFAULT_LOGTIMESTAMPS = true;
static const bool DEFAULT_LOGTHREADNAMES = false;
static const int DEFAULT_POW_CACHE_SIZE = 150000;


/** Signals for translation. */
class CTranslationInterface
{
public:
    /** Translate a message to the native language of the user. */
    boost::signals2::signal<std::string(const char* psz)> Translate;
};

extern bool fPrintToConsole;
extern bool fPrintToDebugLog;

extern bool fLogTimestamps;
extern bool fLogTimeMicros;
extern bool fLogThreadNames;
extern bool fLogIPs;
extern std::atomic<bool> fReopenDebugLog;
extern CTranslationInterface translationInterface;

extern const char* const BITCOIN_CONF_FILENAME;
extern const char* const BITCOIN_PID_FILENAME;

extern std::atomic<uint64_t> logCategories;

/**
 * Translation function: Call Translate signal on UI interface, which returns a boost::optional result.
 * If no translation slot is registered, nothing is returned, and simply return the input.
 */
inline std::string _(const char* psz)
{
    boost::optional<std::string> rv = translationInterface.Translate(psz);
    return rv ? (*rv) : psz;
}

void SetupEnvironment();
bool SetupNetworking();

struct CLogCategoryActive {
    std::string category;
    bool active;
};

namespace BCLog
{
enum LogFlags : uint64_t {
    NONE = 0,
    NET = (1 << 0),
    TOR = (1 << 1),
    MEMPOOL = (1 << 2),
    HTTP = (1 << 3),
    BENCHMARK = (1 << 4),
    ZMQ = (1 << 5),
    DB = (1 << 6),
    RPC = (1 << 7),
    ESTIMATEFEE = (1 << 8),
    ADDRMAN = (1 << 9),
    SELECTCOINS = (1 << 10),
    REINDEX = (1 << 11),
    CMPCTBLOCK = (1 << 12),
    RANDOM = (1 << 13),
    PRUNE = (1 << 14),
    PROXY = (1 << 15),
    MEMPOOLREJ = (1 << 16),
    LIBEVENT = (1 << 17),
    COINDB = (1 << 18),
    QT = (1 << 19),
    LEVELDB = (1 << 20),
    REWARDS = (1 << 21),

    // Start Memeium
    CHAINLOCKS = ((uint64_t)1 << 32),
    GOBJECT = ((uint64_t)1 << 33),
    INSTANTSEND = ((uint64_t)1 << 34),
    KEEPASS = ((uint64_t)1 << 35),
    LLMQ = ((uint64_t)1 << 36),
    LLMQ_DKG = ((uint64_t)1 << 37),
    LLMQ_SIGS = ((uint64_t)1 << 38),
    MNPAYMENTS = ((uint64_t)1 << 39),
    MNSYNC = ((uint64_t)1 << 40),
    PRIVATESEND = ((uint64_t)1 << 41),
    SPORK = ((uint64_t)1 << 42),
    // End Memeium

    ALL = ~(uint64_t)0,
};
}
static inline bool LogAcceptCategory(uint64_t category)
{
    return (logCategories.load(std::memory_order_relaxed) & category) != 0;
}

/** Returns a string with the log categories. */
std::string ListLogCategories();

/** Returns a string with the list of active log categories */
std::string ListActiveLogCategoriesString();

/** Returns a vector of the active log categories. */
std::vector<CLogCategoryActive> ListActiveLogCategories();

/** Return true if str parses as a log category and set the flags in f */
bool GetLogCategory(uint64_t* f, const std::string* str);

/** Send a string to the log output */
int LogPrintStr(const std::string& str);

/** Formats a string without throwing exceptions. Instead, it'll return an error string instead of formatted string. */
template <typename... Args>
std::string SafeStringFormat(const std::string& fmt, const Args&... args)
{
    try {
        return tinyformat::format(fmt, args...);
    } catch (std::runtime_error& fmterr) {
        std::string message = tinyformat::format("\n****TINYFORMAT ERROR****\n    err=\"%s\"\n    fmt=\"%s\"\n", fmterr.what(), fmt);
        fprintf(stderr, "%s", message.c_str());
        return message;
    }
}

/** Get format string from VA_ARGS for error reporting */
template <typename... Args>
std::string FormatStringFromLogArgs(const char* fmt, const Args&... args)
{
    return fmt;
}

static inline void MarkUsed() {}
template <typename T, typename... Args>
static inline void MarkUsed(const T& t, const Args&... args)
{
    (void)t;
    MarkUsed(args...);
}

#ifdef USE_COVERAGE
#define LogPrintf(...)         \
    do {                       \
        MarkUsed(__VA_ARGS__); \
    } while (0)
#define LogPrint(category, ...) \
    do {                        \
        MarkUsed(__VA_ARGS__);  \
    } while (0)
#else
#define LogPrintf(...)                                                                                                                   \
    do {                                                                                                                                 \
        std::string _log_msg_; /* Unlikely name to avoid shadowing variables */                                                          \
        try {                                                                                                                            \
            _log_msg_ = tfm::format(__VA_ARGS__);                                                                                        \
        } catch (tinyformat::format_error & e) {                                                                                         \
            /* Original format string will have newline so don't add one here */                                                         \
            _log_msg_ = "Error \"" + std::string(e.what()) + "\" while formatting log message: " + FormatStringFromLogArgs(__VA_ARGS__); \
        }                                                                                                                                \
        LogPrintStr(_log_msg_);                                                                                                          \
    } while (0)

#define LogPrint(category, ...)              \
    do {                                     \
        if (LogAcceptCategory((category))) { \
            LogPrintf(__VA_ARGS__);          \
        }                                    \
    } while (0)
#endif

template <typename... Args>
bool error(const char* fmt, const Args&... args)
{
    LogPrintStr("ERROR: " + SafeStringFormat(fmt, args...) + "\n");
    return false;
}

void PrintExceptionContinue(const std::exception_ptr pex, const char* pszThread);
void FileCommit(FILE* file);
bool TruncateFile(FILE* file, unsigned int length);
int RaiseFileDescriptorLimit(int nMinFD);
void AllocateFileRange(FILE* file, unsigned int offset, unsigned int length);
bool RenameOver(fs::path src, fs::path dest);
bool TryCreateDirectories(const fs::path& p);
fs::path GetDefaultDataDir();
const fs::path& GetDataDir(bool fNetSpecific = true);
fs::path GetBackupsDir();
void ClearDatadirCache();
fs::path GetConfigFile(const std::string& confPath);
#ifndef WIN32
fs::path GetPidFile();
void CreatePidFile(const fs::path& path, pid_t pid);
#endif
#ifdef WIN32
fs::path GetSpecialFolderPath(int nFolder, bool fCreate = true);
#endif
void OpenDebugLog();
void ShrinkDebugFile();
void runCommand(const std::string& strCommand);

inline bool IsSwitchChar(char c)
{
#ifdef WIN32
    return c == '-' || c == '/';
#else
    return c == '-';
#endif
}

class ArgsManager
{
protected:
    CCriticalSection cs_args;
    std::map<std::string, std::string> mapArgs;
    std::map<std::string, std::vector<std::string>> mapMultiArgs;

public:
    void ParseParameters(int argc, const char* const argv[]);
    void ReadConfigFile(const std::string& confPath);
    std::vector<std::string> GetArgs(const std::string& strArg);

    /**
     * Return true if the given argument has been manually set
     *
     * @param strArg Argument to get (e.g. "-foo")
     * @return true if the argument has been set
     */
    bool IsArgSet(const std::string& strArg);

    /**
     * Return string argument or default value
     *
     * @param strArg Argument to get (e.g. "-foo")
     * @param strDefault (e.g. "1")
     * @return command-line argument or default value
     */
    std::string GetArg(const std::string& strArg, const std::string& strDefault);

    /**
     * Return integer argument or default value
     *
     * @param strArg Argument to get (e.g. "-foo")
     * @param nDefault (e.g. 1)
     * @return command-line argument (0 if invalid number) or default value
     */
    int64_t GetArg(const std::string& strArg, int64_t nDefault);

    /**
     * Return boolean argument or default value
     *
     * @param strArg Argument to get (e.g. "-foo")
     * @param fDefault (true or false)
     * @return command-line argument or default value
     */
    bool GetBoolArg(const std::string& strArg, bool fDefault);

    /**
     * Set an argument if it doesn't already have a value
     *
     * @param strArg Argument to set (e.g. "-foo")
     * @param strValue Value (e.g. "1")
     * @return true if argument gets set, false if it already had a value
     */
    bool SoftSetArg(const std::string& strArg, const std::string& strValue);

    /**
     * Set a boolean argument if it doesn't already have a value
     *
     * @param strArg Argument to set (e.g. "-foo")
     * @param fValue Value (e.g. false)
     * @return true if argument gets set, false if it already had a value
     */
    bool SoftSetBoolArg(const std::string& strArg, bool fValue);

    // Forces an arg setting. Called by SoftSetArg() if the arg hasn't already
    // been set. Also called directly in testing.
    void ForceSetArg(const std::string& strArg, const std::string& strValue);
    void ForceSetMultiArgs(const std::string& strArg, const std::vector<std::string>& values);
    void ForceRemoveArg(const std::string& strArg);
};

extern ArgsManager gArgs;

/**
 * Format a string to be used as group of options in help messages
 *
 * @param message Group name (e.g. "RPC server options:")
 * @return the formatted string
 */
std::string HelpMessageGroup(const std::string& message);

/**
 * Format a string to be used as option description in help messages
 *
 * @param option Option message (e.g. "-rpcuser=<user>")
 * @param message Option description (e.g. "Username for JSON-RPC connections")
 * @return the formatted string
 */
std::string HelpMessageOpt(const std::string& option, const std::string& message);

/**
 * Return the number of physical cores available on the current system.
 * @note This does not count virtual cores, such as those provided by HyperThreading
 * when boost is newer than 1.56.
 */
int GetNumCores();

void RenameThread(const char* name);
std::string GetThreadName();

namespace ctpl
{
class thread_pool;
}
void RenameThreadPool(ctpl::thread_pool& tp, const char* baseName);

/**
 * .. and a wrapper that just calls func once
 */
template <typename Callable>
void TraceThread(const char* name, Callable func)
{
    std::string s = strprintf("memeium-%s", name);
    RenameThread(s.c_str());
    try {
        LogPrintf("%s thread start\n", name);
        func();
        LogPrintf("%s thread exit\n", name);
    } catch (const boost::thread_interrupted&) {
        LogPrintf("%s thread interrupt\n", name);
        throw;
    } catch (...) {
        PrintExceptionContinue(std::current_exception(), name);
        throw;
    }
}

std::string CopyrightHolders(const std::string& strPrefix, unsigned int nStartYear, unsigned int nEndYear);

/**
 * @brief Converts version strings to 4-byte unsigned integer
 * @param strVersion version in "x.x.x" format (decimal digits only)
 * @return 4-byte unsigned integer, most significant byte is always 0
 * Throws std::bad_cast if format doesn\t match.
 */
uint32_t StringVersionToInt(const std::string& strVersion);


/**
 * @brief Converts version as 4-byte unsigned integer to string
 * @param nVersion 4-byte unsigned integer, most significant byte is always 0
 * @return version string in "x.x.x" format (last 3 bytes as version parts)
 * Throws std::bad_cast if format doesn\t match.
 */
std::string IntVersionToString(uint32_t nVersion);


/**
 * @brief Copy of the IntVersionToString, that returns "Invalid version" string
 * instead of throwing std::bad_cast
 * @param nVersion 4-byte unsigned integer, most significant byte is always 0
 * @return version string in "x.x.x" format (last 3 bytes as version parts)
 * or "Invalid version" if can't cast the given value
 */
std::string SafeIntVersionToString(uint32_t nVersion);

void SetThreadPriority(int nPriority);

#endif // BITCOIN_UTIL_H
