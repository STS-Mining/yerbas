// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2019 The Dash Core developers
// Copyright (c) 2020 The Memeium developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpc/server.h"

#include "base58.h"
#include "fs.h"
#include "init.h"
#include "random.h"
#include "sync.h"
#include "ui_interface.h"
#include "util.h"
#include "utilstrencodings.h"

#include <univalue.h>

#include <boost/algorithm/string/case_conv.hpp> // for to_upper()
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/bind.hpp>
#include <boost/signals2/signal.hpp>

#include <algorithm>
#include <memory> // for unique_ptr
#include <unordered_map>

static bool fRPCRunning = false;
static bool fRPCInWarmup = true;
static std::string rpcWarmupStatus("RPC server started");
static CCriticalSection cs_rpcWarmup;
/* Timer-creating functions */
static RPCTimerInterface* timerInterface = nullptr;
/* Map of name to timer. */
static std::map<std::string, std::unique_ptr<RPCTimerBase>> deadlineTimers;

static struct CRPCSignals {
    boost::signals2::signal<void()> Started;
    boost::signals2::signal<void()> Stopped;
    boost::signals2::signal<void(const CRPCCommand&)> PreCommand;
} g_rpcSignals;

void RPCServer::OnStarted(std::function<void()> slot)
{
    g_rpcSignals.Started.connect(slot);
}

void RPCServer::OnStopped(std::function<void()> slot)
{
    g_rpcSignals.Stopped.connect(slot);
}

void RPCServer::OnPreCommand(std::function<void(const CRPCCommand&)> slot)
{
    g_rpcSignals.PreCommand.connect(boost::bind(slot, _1));
}

void RPCTypeCheck(const UniValue& params,
    const std::list<UniValue::VType>& typesExpected,
    bool fAllowNull)
{
    unsigned int i = 0;
    for (UniValue::VType t : typesExpected) {
        if (params.size() <= i)
            break;

        const UniValue& v = params[i];
        if (!(fAllowNull && v.isNull())) {
            RPCTypeCheckArgument(v, t);
        }
        i++;
    }
}

void RPCTypeCheckArgument(const UniValue& value, UniValue::VType typeExpected)
{
    if (value.type() != typeExpected) {
        throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Expected type %s, got %s", uvTypeName(typeExpected), uvTypeName(value.type())));
    }
}

void RPCTypeCheckObj(const UniValue& o,
    const std::map<std::string, UniValueType>& typesExpected,
    bool fAllowNull,
    bool fStrict)
{
    for (const auto& t : typesExpected) {
        const UniValue& v = find_value(o, t.first);
        if (!fAllowNull && v.isNull())
            throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Missing %s", t.first));

        if (!(t.second.typeAny || v.type() == t.second.type || (fAllowNull && v.isNull()))) {
            std::string err = strprintf("Expected type %s for %s, got %s",
                uvTypeName(t.second.type), t.first, uvTypeName(v.type()));
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
    }

    if (fStrict) {
        for (const std::string& k : o.getKeys()) {
            if (typesExpected.count(k) == 0) {
                std::string err = strprintf("Unexpected key %s", k);
                throw JSONRPCError(RPC_TYPE_ERROR, err);
            }
        }
    }
}

CAmount AmountFromValue(const UniValue& value, bool p_isMMM)
{
    if (!value.isNum() && !value.isStr())
        throw JSONRPCError(RPC_TYPE_ERROR, "Amount is not a number or string");
    CAmount amount;
    if (!ParseFixedPoint(value.getValStr(), 8, &amount))
        throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Invalid amount (3): %s", value.getValStr()));
    if (p_isMMM && !MoneyRange(amount))
        throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Amount out of range: %s", amount));
    return amount;
}

uint256 ParseHashV(const UniValue& v, std::string strName)
{
    std::string strHex;
    if (v.isStr())
        strHex = v.get_str();
    if (!IsHex(strHex)) // Note: IsHex("") is false
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName + " must be hexadecimal string (not '" + strHex + "')");
    if (64 != strHex.length())
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s must be of length %d (not %d)", strName, 64, strHex.length()));
    uint256 result;
    result.SetHex(strHex);
    return result;
}
uint256 ParseHashO(const UniValue& o, std::string strKey)
{
    return ParseHashV(find_value(o, strKey), strKey);
}
std::vector<unsigned char> ParseHexV(const UniValue& v, std::string strName)
{
    std::string strHex;
    if (v.isStr())
        strHex = v.get_str();
    if (!IsHex(strHex))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName + " must be hexadecimal string (not '" + strHex + "')");
    return ParseHex(strHex);
}
std::vector<unsigned char> ParseHexO(const UniValue& o, std::string strKey)
{
    return ParseHexV(find_value(o, strKey), strKey);
}

int32_t ParseInt32V(const UniValue& v, const std::string& strName)
{
    std::string strNum = v.getValStr();
    int32_t num;
    if (!ParseInt32(strNum, &num))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName + " must be a 32bit integer (not '" + strNum + "')");
    return num;
}

int64_t ParseInt64V(const UniValue& v, const std::string& strName)
{
    std::string strNum = v.getValStr();
    int64_t num;
    if (!ParseInt64(strNum, &num))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName + " must be a 64bit integer (not '" + strNum + "')");
    return num;
}

double ParseDoubleV(const UniValue& v, const std::string& strName)
{
    std::string strNum = v.getValStr();
    double num;
    if (!ParseDouble(strNum, &num))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName + " must be a be number (not '" + strNum + "')");
    return num;
}

bool ParseBoolV(const UniValue& v, const std::string& strName)
{
    std::string strBool;
    if (v.isBool())
        return v.get_bool();
    else if (v.isNum())
        strBool = itostr(v.get_int());
    else if (v.isStr())
        strBool = v.get_str();

    std::transform(strBool.begin(), strBool.end(), strBool.begin(), ::tolower);

    if (strBool == "true" || strBool == "yes" || strBool == "1") {
        return true;
    } else if (strBool == "false" || strBool == "no" || strBool == "0") {
        return false;
    }
    throw JSONRPCError(RPC_INVALID_PARAMETER, strName + " must be true, false, yes, no, 1 or 0 (not '" + strBool + "')");
}

/**
 * Note: This interface may still be subject to change.
 */

std::string CRPCTable::help(const std::string& strCommand, const std::string& strSubCommand, const JSONRPCRequest& helpreq) const
{
    std::string strRet;
    std::string category;
    std::set<rpcfn_type> setDone;
    std::vector<std::pair<std::string, const CRPCCommand*>> vCommands;

    for (std::map<std::string, const CRPCCommand*>::const_iterator mi = mapCommands.begin(); mi != mapCommands.end(); ++mi)
        vCommands.push_back(make_pair(mi->second->category + mi->first, mi->second));
    sort(vCommands.begin(), vCommands.end());

    JSONRPCRequest jreq(helpreq);
    jreq.fHelp = true;
    jreq.params = UniValue();

    for (const std::pair<std::string, const CRPCCommand*>& command : vCommands) {
        const CRPCCommand* pcmd = command.second;
        std::string strMethod = pcmd->name;
        if ((strCommand != "" || pcmd->category == "hidden") && strMethod != strCommand)
            continue;
        jreq.strMethod = strMethod;
        try {
            if (!strSubCommand.empty()) {
                jreq.params.setArray();
                jreq.params.push_back(strSubCommand);
            }
            rpcfn_type pfn = pcmd->actor;
            if (setDone.insert(pfn).second)
                (*pfn)(jreq);
        } catch (const std::exception& e) {
            // Help text is returned in an exception
            std::string strHelp = std::string(e.what());
            if (strCommand == "") {
                if (strHelp.find('\n') != std::string::npos)
                    strHelp = strHelp.substr(0, strHelp.find('\n'));

                if (category != pcmd->category) {
                    if (!category.empty())
                        strRet += "\n";
                    category = pcmd->category;
                    std::string firstLetter = category.substr(0, 1);
                    boost::to_upper(firstLetter);
                    strRet += "== " + firstLetter + category.substr(1) + " ==\n";
                }
            }
            strRet += strHelp + "\n";
        }
    }
    if (strRet == "")
        strRet = strprintf("help: unknown command: %s\n", strCommand);
    strRet = strRet.substr(0, strRet.size() - 1);
    return strRet;
}

UniValue help(const JSONRPCRequest& jsonRequest)
{
    if (jsonRequest.fHelp || jsonRequest.params.size() > 2)
        throw std::runtime_error(
            "help ( \"command\" ) (\"subCommand\")\n"
            "\nList all commands, or get help for a specified command.\n"
            "\nArguments:\n"
            "1. \"command\"     (string, optional) The command to get help on\n"
            "2. \"subCommand\"  (string, optional) The subcommand to get help on. Please not that not all subcommands support this at the moment\n"
            "\nResult:\n"
            "\"text\"     (string) The help text\n");

    std::string strCommand, strSubCommand;
    if (jsonRequest.params.size() > 0)
        strCommand = jsonRequest.params[0].get_str();
    if (jsonRequest.params.size() > 1)
        strSubCommand = jsonRequest.params[1].get_str();

    return tableRPC.help(strCommand, strSubCommand, jsonRequest);
}


UniValue stop(const JSONRPCRequest& jsonRequest)
{
    // Accept the deprecated and ignored 'detach' boolean argument
    // Also accept the hidden 'wait' integer argument (milliseconds)
    // For instance, 'stop 1000' makes the call wait 1 second before returning
    // to the client (intended for testing)
    if (jsonRequest.fHelp || jsonRequest.params.size() > 1)
        throw std::runtime_error(
            "stop\n"
            "\nStop Memeium Core server.");
    // Event loop will exit after current HTTP requests have been handled, so
    // this reply will get back to the client.
    StartShutdown();
    if (jsonRequest.params[0].isNum()) {
        MilliSleep(jsonRequest.params[0].get_int());
    }
    return "Memeium Core server stopping";
}

UniValue uptime(const JSONRPCRequest& jsonRequest)
{
    if (jsonRequest.fHelp || jsonRequest.params.size() > 1)
        throw std::runtime_error(
            "uptime\n"
            "\nReturns the total uptime of the server.\n"
            "\nResult:\n"
            "ttt        (numeric) The number of seconds that the server has been running\n"
            "\nExamples:\n" +
            HelpExampleCli("uptime", "") + HelpExampleRpc("uptime", ""));

    return GetTime() - GetStartupTime();
}

/**
 * Call Table
 */
static const CRPCCommand vRPCCommands[] =
    {
        //  category              name                      actor (function)         okSafe argNames
        //  --------------------- ------------------------  -----------------------  ------ ----------
        /* Overall control/query calls */
        {"control", "help", &help, true, {"command"}},
        {"control", "stop", &stop, true, {"wait"}},
        {"control", "uptime", &uptime, true, {}},
};

CRPCTable::CRPCTable()
{
    unsigned int vcidx;
    for (vcidx = 0; vcidx < (sizeof(vRPCCommands) / sizeof(vRPCCommands[0])); vcidx++) {
        const CRPCCommand* pcmd;

        pcmd = &vRPCCommands[vcidx];
        mapCommands[pcmd->name] = pcmd;
    }
}

const CRPCCommand* CRPCTable::operator[](const std::string& name) const
{
    std::map<std::string, const CRPCCommand*>::const_iterator it = mapCommands.find(name);
    if (it == mapCommands.end())
        return nullptr;
    return (*it).second;
}

bool CRPCTable::appendCommand(const std::string& name, const CRPCCommand* pcmd)
{
    if (IsRPCRunning())
        return false;

    // don't allow overwriting for now
    std::map<std::string, const CRPCCommand*>::const_iterator it = mapCommands.find(name);
    if (it != mapCommands.end())
        return false;

    mapCommands[name] = pcmd;
    return true;
}

bool StartRPC()
{
    LogPrint(BCLog::RPC, "Starting RPC\n");
    fRPCRunning = true;
    g_rpcSignals.Started();
    return true;
}

void InterruptRPC()
{
    LogPrint(BCLog::RPC, "Interrupting RPC\n");
    // Interrupt e.g. running longpolls
    fRPCRunning = false;
}

void StopRPC()
{
    LogPrint(BCLog::RPC, "Stopping RPC\n");
    deadlineTimers.clear();
    DeleteAuthCookie();
    g_rpcSignals.Stopped();
}

bool IsRPCRunning()
{
    return fRPCRunning;
}

void SetRPCWarmupStatus(const std::string& newStatus)
{
    LOCK(cs_rpcWarmup);
    rpcWarmupStatus = newStatus;
}

void SetRPCWarmupFinished()
{
    LOCK(cs_rpcWarmup);
    assert(fRPCInWarmup);
    fRPCInWarmup = false;
}

bool RPCIsInWarmup(std::string* outStatus)
{
    LOCK(cs_rpcWarmup);
    if (outStatus)
        *outStatus = rpcWarmupStatus;
    return fRPCInWarmup;
}

void JSONRPCRequest::parse(const UniValue& valRequest)
{
    // Parse request
    if (!valRequest.isObject())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Invalid Request object");
    const UniValue& request = valRequest.get_obj();

    // Parse id now so errors from here on will have the id
    id = find_value(request, "id");

    // Parse method
    UniValue valMethod = find_value(request, "method");
    if (valMethod.isNull())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Missing method");
    if (!valMethod.isStr())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Method must be a string");
    strMethod = valMethod.get_str();
    if (strMethod != "getblocktemplate") {
        LogPrint(BCLog::RPC, "ThreadRPCServer method=%s\n", SanitizeString(strMethod));
    }

    // Parse params
    UniValue valParams = find_value(request, "params");
    if (valParams.isArray() || valParams.isObject())
        params = valParams;
    else if (valParams.isNull())
        params = UniValue(UniValue::VARR);
    else
        throw JSONRPCError(RPC_INVALID_REQUEST, "Params must be an array or object");
}

static UniValue JSONRPCExecOne(JSONRPCRequest jreq, const UniValue& req)
{
    UniValue rpc_result(UniValue::VOBJ);

    try {
        jreq.parse(req);

        UniValue result = tableRPC.execute(jreq);
        rpc_result = JSONRPCReplyObj(result, NullUniValue, jreq.id);
    } catch (const UniValue& objError) {
        rpc_result = JSONRPCReplyObj(NullUniValue, objError, jreq.id);
    } catch (const std::exception& e) {
        rpc_result = JSONRPCReplyObj(NullUniValue,
            JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
    }

    return rpc_result;
}

std::string JSONRPCExecBatch(const JSONRPCRequest& jreq, const UniValue& vReq)
{
    UniValue ret(UniValue::VARR);
    for (unsigned int reqIdx = 0; reqIdx < vReq.size(); reqIdx++)
        ret.push_back(JSONRPCExecOne(jreq, vReq[reqIdx]));

    return ret.write() + "\n";
}

/**
 * Process named arguments into a vector of positional arguments, based on the
 * passed-in specification for the RPC call's arguments.
 */
static inline JSONRPCRequest transformNamedArguments(const JSONRPCRequest& in, const std::vector<std::string>& argNames)
{
    JSONRPCRequest out = in;
    out.params = UniValue(UniValue::VARR);
    // Build a map of parameters, and remove ones that have been processed, so that we can throw a focused error if
    // there is an unknown one.
    const std::vector<std::string>& keys = in.params.getKeys();
    const std::vector<UniValue>& values = in.params.getValues();
    std::unordered_map<std::string, const UniValue*> argsIn;
    for (size_t i = 0; i < keys.size(); ++i) {
        argsIn[keys[i]] = &values[i];
    }
    // Process expected parameters.
    int hole = 0;
    for (const std::string& argNamePattern : argNames) {
        std::vector<std::string> vargNames;
        boost::algorithm::split(vargNames, argNamePattern, boost::algorithm::is_any_of("|"));
        auto fr = argsIn.end();
        for (const std::string& argName : vargNames) {
            fr = argsIn.find(argName);
            if (fr != argsIn.end()) {
                break;
            }
        }
        if (fr != argsIn.end()) {
            for (int i = 0; i < hole; ++i) {
                // Fill hole between specified parameters with JSON nulls,
                // but not at the end (for backwards compatibility with calls
                // that act based on number of specified parameters).
                out.params.push_back(UniValue());
            }
            hole = 0;
            out.params.push_back(*fr->second);
            argsIn.erase(fr);
        } else {
            hole += 1;
        }
    }
    // If there are still arguments in the argsIn map, this is an error.
    if (!argsIn.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown named parameter " + argsIn.begin()->first);
    }
    // Return request with named arguments transformed to positional arguments
    return out;
}

UniValue CRPCTable::execute(const JSONRPCRequest& request) const
{
    // Return immediately if in warmup
    {
        LOCK(cs_rpcWarmup);
        if (fRPCInWarmup)
            throw JSONRPCError(RPC_IN_WARMUP, rpcWarmupStatus);
    }

    // Find method
    const CRPCCommand* pcmd = tableRPC[request.strMethod];
    if (!pcmd)
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found");

    g_rpcSignals.PreCommand(*pcmd);

    try {
        // Execute, convert arguments to array if necessary
        if (request.params.isObject()) {
            return pcmd->actor(transformNamedArguments(request, pcmd->argNames));
        } else {
            return pcmd->actor(request);
        }
    } catch (const std::exception& e) {
        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }
}

std::vector<std::string> CRPCTable::listCommands() const
{
    std::vector<std::string> commandList;
    typedef std::map<std::string, const CRPCCommand*> commandMap;

    std::transform(mapCommands.begin(), mapCommands.end(),
        std::back_inserter(commandList),
        boost::bind(&commandMap::value_type::first, _1));
    return commandList;
}

std::string HelpExampleCli(const std::string& methodname, const std::string& args)
{
    return "> memeium-cli " + methodname + " " + args + "\n";
}

std::string HelpExampleRpc(const std::string& methodname, const std::string& args)
{
    return "> curl --user myusername --data-binary '{\"jsonrpc\": \"1.0\", \"id\":\"curltest\", "
           "\"method\": \"" +
           methodname + "\", \"params\": [" + args + "] }' -H 'content-type: text/plain;'"
                                                     " http://127.0.0.1:" +
           strprintf("%d", gArgs.GetArg("-rpcport", BaseParams().RPCPort())) + "/\n";
}

void RPCSetTimerInterfaceIfUnset(RPCTimerInterface* iface)
{
    if (!timerInterface)
        timerInterface = iface;
}

void RPCSetTimerInterface(RPCTimerInterface* iface)
{
    timerInterface = iface;
}

void RPCUnsetTimerInterface(RPCTimerInterface* iface)
{
    if (timerInterface == iface)
        timerInterface = nullptr;
}

void RPCRunLater(const std::string& name, std::function<void(void)> func, int64_t nSeconds)
{
    if (!timerInterface)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No timer handler registered for RPC");
    deadlineTimers.erase(name);
    LogPrint(BCLog::RPC, "queue run of timer %s in %i seconds (using %s)\n", name, nSeconds, timerInterface->Name());
    deadlineTimers.emplace(name, std::unique_ptr<RPCTimerBase>(timerInterface->NewTimer(func, nSeconds * 1000)));
}

CRPCTable tableRPC;

void CheckIPFSTxidMessage(const std::string& message, int64_t expireTime)
{
    size_t msglen = message.length();
    if (msglen != 46) {
        throw JSONRPCError(RPC_INVALID_PARAMS, std::string("Invalid IPFS hash (must be 46 characters)"));
    }

    if (message.substr(0, 2) != "Qm") {
        throw JSONRPCError(RPC_INVALID_PARAMS, std::string("Invalid IPFS/Txid hash"));
    }

    if (expireTime < 0)
        throw JSONRPCError(RPC_INVALID_PARAMS, std::string("Expire time must be a positive number"));
}
