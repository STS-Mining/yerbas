// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2020 The Dash Core developers
// Copyright (c) 2020 The Memeium developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "amount.h"
#include "base58.h"
#include "chain.h"
#include "consensus/validation.h"
#include "core_io.h"
#include "httpserver.h"
#include "init.h"
#include "keepass.h"
#include "net.h"
#include "policy/feerate.h"
#include "policy/fees.h"
#include "privatesend/privatesend-client.h"
#include "rpc/mining.h"
#include "rpc/server.h"
#include "timedata.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validation.h"
#include "wallet/coincontrol.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"

#include "llmq/quorums_chainlocks.h"
#include "llmq/quorums_instantsend.h"

#include <stdint.h>

#include <univalue.h>

static const std::string WALLET_ENDPOINT_BASE = "/wallet/";

CWallet* GetWalletForJSONRPCRequest(const JSONRPCRequest& request)
{
    if (request.URI.substr(0, WALLET_ENDPOINT_BASE.size()) == WALLET_ENDPOINT_BASE) {
        // wallet endpoint was used
        std::string requestedWallet = urlDecode(request.URI.substr(WALLET_ENDPOINT_BASE.size()));
        for (CWalletRef pwallet : ::vpwallets) {
            if (pwallet->GetName() == requestedWallet) {
                return pwallet;
            }
        }
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Requested wallet does not exist or is not loaded");
    }
    return ::vpwallets.size() == 1 || (request.fHelp && ::vpwallets.size() > 0) ? ::vpwallets[0] : nullptr;
}

std::string HelpRequiringPassphrase(CWallet* const pwallet)
{
    return pwallet && pwallet->IsCrypted()
               ? "\nRequires wallet passphrase to be set with walletpassphrase call."
               : "";
}

bool EnsureWalletIsAvailable(CWallet* const pwallet, bool avoidException)
{
    if (pwallet) return true;
    if (avoidException) return false;
    if (::vpwallets.empty()) {
        // Note: It isn't currently possible to trigger this error because
        // wallet RPC methods aren't registered unless a wallet is loaded. But
        // this error is being kept as a precaution, because it's possible in
        // the future that wallet RPC methods might get or remain registered
        // when no wallets are loaded.
        throw JSONRPCError(
            RPC_METHOD_NOT_FOUND, "Method not found (wallet method is disabled because no wallet is loaded)");
    }
    throw JSONRPCError(RPC_WALLET_NOT_SPECIFIED,
        "Wallet file not specified (must request wallet RPC through /wallet/<filename> uri-path).");
}

void EnsureWalletIsUnlocked(CWallet* const pwallet)
{
    if (pwallet->IsLocked()) {
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
    }
}

void WalletTxToJSON(const CWalletTx& wtx, UniValue& entry)
{
    AssertLockHeld(cs_main); // for mapBlockIndex
    int confirms = wtx.GetDepthInMainChain();
    bool fLocked = llmq::quorumInstantSendManager->IsLocked(wtx.GetHash());
    bool chainlock = false;
    if (confirms > 0) {
        chainlock = llmq::chainLocksHandler->HasChainLock(mapBlockIndex[wtx.hashBlock]->nHeight, wtx.hashBlock);
    }
    entry.push_back(Pair("confirmations", confirms));
    entry.push_back(Pair("instantlock", fLocked || chainlock));
    entry.push_back(Pair("instantlock_internal", fLocked));
    entry.push_back(Pair("chainlock", chainlock));
    if (wtx.IsCoinBase())
        entry.push_back(Pair("generated", true));
    if (confirms > 0) {
        entry.push_back(Pair("blockhash", wtx.hashBlock.GetHex()));
        entry.push_back(Pair("blockindex", wtx.nIndex));
        entry.push_back(Pair("blocktime", mapBlockIndex[wtx.hashBlock]->GetBlockTime()));
    } else {
        entry.push_back(Pair("trusted", wtx.IsTrusted()));
    }
    uint256 hash = wtx.GetHash();
    entry.push_back(Pair("txid", hash.GetHex()));
    UniValue conflicts(UniValue::VARR);
    for (const uint256& conflict : wtx.GetConflicts())
        conflicts.push_back(conflict.GetHex());
    entry.push_back(Pair("walletconflicts", conflicts));
    entry.push_back(Pair("time", wtx.GetTxTime()));
    entry.push_back(Pair("timereceived", (int64_t)wtx.nTimeReceived));

    for (const std::pair<std::string, std::string>& item : wtx.mapValue)
        entry.push_back(Pair(item.first, item.second));
}

std::string AccountFromValue(const UniValue& value)
{
    std::string strAccount = value.get_str();
    if (strAccount == "*")
        throw JSONRPCError(RPC_WALLET_INVALID_ACCOUNT_NAME, "Invalid account name");
    return strAccount;
}

UniValue getnewaddress(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "getnewaddress ( \"account\" )\n"
            "\nReturns a new Memeium address for receiving payments.\n"
            "If 'account' is specified (DEPRECATED), it is added to the address book \n"
            "so payments received with the address will be credited to 'account'.\n"
            "\nArguments:\n"
            "1. \"account\"        (string, optional) DEPRECATED. The account name for the address to be linked to. If not provided, the default account \"\" is used. It can also be set to the empty string \"\" to represent the default account. The account does not need to exist, it will be created if there is no account by the given name.\n"
            "\nResult:\n"
            "\"address\"    (string) The new memeium address\n"
            "\nExamples:\n" +
            HelpExampleCli("getnewaddress", "") + HelpExampleRpc("getnewaddress", ""));

    LOCK2(cs_main, pwallet->cs_wallet);

    // Parse the account first so we don't generate a key if there's an error
    std::string strAccount;
    if (!request.params[0].isNull())
        strAccount = AccountFromValue(request.params[0]);

    if (!pwallet->IsLocked(true)) {
        pwallet->TopUpKeyPool();
    }

    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!pwallet->GetKeyFromPool(newKey, false)) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    }
    CKeyID keyID = newKey.GetID();

    pwallet->SetAddressBook(keyID, strAccount, "receive");

    return CBitcoinAddress(keyID).ToString();
}


CBitcoinAddress GetAccountAddress(CWallet* const pwallet, std::string strAccount, bool bForceNew = false)
{
    CPubKey pubKey;
    if (!pwallet->GetAccountPubkey(pubKey, strAccount, bForceNew)) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    }

    return CBitcoinAddress(pubKey.GetID());
}

UniValue getaccountaddress(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "getaccountaddress \"account\"\n"
            "\nDEPRECATED. Returns the current Memeium address for receiving payments to this account.\n"
            "\nArguments:\n"
            "1. \"account\"       (string, required) The account name for the address. It can also be set to the empty string \"\" to represent the default account. The account does not need to exist, it will be created and a new address created  if there is no account by the given name.\n"
            "\nResult:\n"
            "\"address\"          (string) The account memeium address\n"
            "\nExamples:\n" +
            HelpExampleCli("getaccountaddress", "") + HelpExampleCli("getaccountaddress", "\"\"") + HelpExampleCli("getaccountaddress", "\"myaccount\"") + HelpExampleRpc("getaccountaddress", "\"myaccount\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    // Parse the account first so we don't generate a key if there's an error
    std::string strAccount = AccountFromValue(request.params[0]);

    UniValue ret(UniValue::VSTR);

    ret = GetAccountAddress(pwallet, strAccount).ToString();
    return ret;
}


UniValue getrawchangeaddress(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
            "getrawchangeaddress\n"
            "\nReturns a new Memeium address, for receiving change.\n"
            "This is for use with raw transactions, NOT normal use.\n"
            "\nResult:\n"
            "\"address\"    (string) The address\n"
            "\nExamples:\n" +
            HelpExampleCli("getrawchangeaddress", "") + HelpExampleRpc("getrawchangeaddress", ""));

    LOCK2(cs_main, pwallet->cs_wallet);

    if (!pwallet->IsLocked(true)) {
        pwallet->TopUpKeyPool();
    }

    CReserveKey reservekey(pwallet);
    CPubKey vchPubKey;
    if (!reservekey.GetReservedKey(vchPubKey, true))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");

    reservekey.KeepKey();

    CKeyID keyID = vchPubKey.GetID();

    return CBitcoinAddress(keyID).ToString();
}


UniValue setaccount(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "setaccount \"address\" \"account\"\n"
            "\nDEPRECATED. Sets the account associated with the given address.\n"
            "\nArguments:\n"
            "1. \"address\"         (string, required) The memeium address to be associated with an account.\n"
            "2. \"account\"         (string, required) The account to assign the address to.\n"
            "\nExamples:\n" +
            HelpExampleCli("setaccount", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\" \"tabby\"") + HelpExampleRpc("setaccount", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\", \"tabby\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    CBitcoinAddress address(request.params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Memeium address");

    std::string strAccount;
    if (request.params.size() > 1)
        strAccount = AccountFromValue(request.params[1]);

    // Only add the account if the address is yours.
    if (IsMine(*pwallet, address.Get())) {
        // Detect when changing the account of an address that is the 'unused current key' of another account:
        if (pwallet->mapAddressBook.count(address.Get())) {
            std::string strOldAccount = pwallet->mapAddressBook[address.Get()].name;
            if (address == GetAccountAddress(pwallet, strOldAccount)) {
                GetAccountAddress(pwallet, strOldAccount, true);
            }
        }
        pwallet->SetAddressBook(address.Get(), strAccount, "receive");
    } else
        throw JSONRPCError(RPC_MISC_ERROR, "setaccount can only be used with own address");

    return NullUniValue;
}


UniValue getaccount(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "getaccount \"address\"\n"
            "\nDEPRECATED. Returns the account associated with the given address.\n"
            "\nArguments:\n"
            "1. \"address\"         (string, required) The memeium address for account lookup.\n"
            "\nResult:\n"
            "\"accountname\"        (string) the account address\n"
            "\nExamples:\n" +
            HelpExampleCli("getaccount", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\"") + HelpExampleRpc("getaccount", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    CBitcoinAddress address(request.params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Memeium address");

    std::string strAccount;
    std::map<CTxDestination, CAddressBookData>::iterator mi = pwallet->mapAddressBook.find(address.Get());
    if (mi != pwallet->mapAddressBook.end() && !(*mi).second.name.empty()) {
        strAccount = (*mi).second.name;
    }
    return strAccount;
}


UniValue getaddressesbyaccount(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "getaddressesbyaccount \"account\"\n"
            "\nDEPRECATED. Returns the list of addresses for the given account.\n"
            "\nArguments:\n"
            "1. \"account\"        (string, required) The account name.\n"
            "\nResult:\n"
            "[                     (json array of string)\n"
            "  \"address\"         (string) a memeium address associated with the given account\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n" +
            HelpExampleCli("getaddressesbyaccount", "\"tabby\"") + HelpExampleRpc("getaddressesbyaccount", "\"tabby\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strAccount = AccountFromValue(request.params[0]);

    // Find all addresses that have the given account
    UniValue ret(UniValue::VARR);
    for (const std::pair<CBitcoinAddress, CAddressBookData>& item : pwallet->mapAddressBook) {
        const CBitcoinAddress& address = item.first;
        const std::string& strName = item.second.name;
        if (strName == strAccount)
            ret.push_back(address.ToString());
    }
    return ret;
}

static void SendMoney(CWallet* const pwallet, const CTxDestination& address, CAmount nValue, bool fSubtractFeeFromAmount, CWalletTx& wtxNew, const CCoinControl& coin_control)
{
    CAmount curBalance = pwallet->GetBalance();

    // Check amount
    if (nValue <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nValue > curBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    if (pwallet->GetBroadcastTransactions() && !g_connman) {
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");
    }

    // Parse Memeium address
    CScript scriptPubKey = GetScriptForDestination(address);

    // Create and send the transaction
    CReserveKey reservekey(pwallet);
    CAmount nFeeRequired;
    std::string strError;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    CRecipient recipient = {scriptPubKey, nValue, fSubtractFeeFromAmount};
    vecSend.push_back(recipient);
    if (!pwallet->CreateTransaction(vecSend, wtxNew, reservekey, nFeeRequired, nChangePosRet,
            strError, coin_control)) {
        if (!fSubtractFeeFromAmount && nValue + nFeeRequired > curBalance)
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s", FormatMoney(nFeeRequired));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    CValidationState state;
    if (!pwallet->CommitTransaction(wtxNew, reservekey, g_connman.get(), state)) {
        strError = strprintf("Error: The transaction was rejected! Reason given: %s", state.GetRejectReason());
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
}

UniValue sendtoaddress(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 9)
        throw std::runtime_error(
            "sendtoaddress \"address\" amount ( \"comment\" \"comment_to\" subtractfeefromamount use_is use_ps conf_target \"estimate_mode\")\n"
            "\nSend an amount to a given address.\n" +
            HelpRequiringPassphrase(pwallet) +
            "\nArguments:\n"
            "1. \"address\"            (string, required) The memeium address to send to.\n"
            "2. \"amount\"             (numeric or string, required) The amount in " +
            CURRENCY_UNIT + " to send. eg 0.1\n"
                            "3. \"comment\"            (string, optional) A comment used to store what the transaction is for. \n"
                            "                             This is not part of the transaction, just kept in your wallet.\n"
                            "4. \"comment_to\"         (string, optional) A comment to store the name of the person or organization \n"
                            "                             to which you're sending the transaction. This is not part of the \n"
                            "                             transaction, just kept in your wallet.\n"
                            "5. subtractfeefromamount  (boolean, optional, default=false) The fee will be deducted from the amount being sent.\n"
                            "                             The recipient will receive less amount of Memeium than you enter in the amount field.\n"
                            "6. \"use_is\"             (bool, optional, default=false) Deprecated and ignored\n"
                            "7. \"use_ps\"             (bool, optional, default=false) Use PrivateSend funds only\n"
                            "8. conf_target            (numeric, optional) Confirmation target (in blocks)\n"
                            "9. \"estimate_mode\"      (string, optional, default=UNSET) The fee estimate mode, must be one of:\n"
                            "       \"UNSET\"\n"
                            "       \"ECONOMICAL\"\n"
                            "       \"CONSERVATIVE\"\n"
                            "\nResult:\n"
                            "\"txid\"                  (string) The transaction id.\n"
                            "\nExamples:\n" +
            HelpExampleCli("sendtoaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\" 0.1") + HelpExampleCli("sendtoaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\" 0.1 \"donation\" \"seans outpost\"") + HelpExampleCli("sendtoaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\" 0.1 \"\" \"\" true") + HelpExampleRpc("sendtoaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\", 0.1, \"donation\", \"seans outpost\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    CBitcoinAddress address(request.params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Memeium address");

    // Amount
    CAmount nAmount = AmountFromValue(request.params[1]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");

    // Wallet comments
    CWalletTx wtx;
    if (request.params.size() > 2 && !request.params[2].isNull() && !request.params[2].get_str().empty())
        wtx.mapValue["comment"] = request.params[2].get_str();
    if (request.params.size() > 3 && !request.params[3].isNull() && !request.params[3].get_str().empty())
        wtx.mapValue["to"] = request.params[3].get_str();

    bool fSubtractFeeFromAmount = false;
    if (request.params.size() > 4 && !request.params[4].isNull()) {
        fSubtractFeeFromAmount = request.params[4].get_bool();
    }

    CCoinControl coin_control;

    if (request.params.size() > 6 && !request.params[6].isNull()) {
        coin_control.UsePrivateSend(request.params[6].get_bool());
    }

    if (request.params.size() > 7 && !request.params[7].isNull()) {
        coin_control.m_confirm_target = ParseConfirmTarget(request.params[7]);
    }

    if (request.params.size() > 8 && !request.params[8].isNull()) {
        if (!FeeModeFromString(request.params[8].get_str(), coin_control.m_fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }

    EnsureWalletIsUnlocked(pwallet);

    SendMoney(pwallet, address.Get(), nAmount, fSubtractFeeFromAmount, wtx, coin_control);

    return wtx.GetHash().GetHex();
}

// DEPRECATED
UniValue instantsendtoaddress(const JSONRPCRequest& request)
{
    if (request.fHelp) {
        throw std::runtime_error("instantsendtoaddress is deprecated and sendtoaddress should be used instead");
    }
    LogPrintf("WARNING: Used deprecated RPC method 'instantsendtoaddress'! Please use 'sendtoaddress' instead\n");
    return sendtoaddress(request);
}

UniValue listaddressgroupings(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "listaddressgroupings\n"
            "\nLists groups of addresses which have had their common ownership\n"
            "made public by common use as inputs or as the resulting change\n"
            "in past transactions\n"
            "\nResult:\n"
            "[\n"
            "  [\n"
            "    [\n"
            "      \"address\",            (string) The memeium address\n"
            "      amount,                 (numeric) The amount in " +
            CURRENCY_UNIT + "\n"
                            "      \"account\"             (string, optional) DEPRECATED. The account\n"
                            "    ]\n"
                            "    ,...\n"
                            "  ]\n"
                            "  ,...\n"
                            "]\n"
                            "\nExamples:\n" +
            HelpExampleCli("listaddressgroupings", "") + HelpExampleRpc("listaddressgroupings", ""));

    LOCK2(cs_main, pwallet->cs_wallet);

    UniValue jsonGroupings(UniValue::VARR);
    std::map<CTxDestination, CAmount> balances = pwallet->GetAddressBalances();
    for (std::set<CTxDestination> grouping : pwallet->GetAddressGroupings()) {
        UniValue jsonGrouping(UniValue::VARR);
        for (CTxDestination address : grouping) {
            UniValue addressInfo(UniValue::VARR);
            addressInfo.push_back(CBitcoinAddress(address).ToString());
            addressInfo.push_back(ValueFromAmount(balances[address]));
            {
                if (pwallet->mapAddressBook.find(CBitcoinAddress(address).Get()) != pwallet->mapAddressBook.end()) {
                    addressInfo.push_back(pwallet->mapAddressBook.find(CBitcoinAddress(address).Get())->second.name);
                }
            }
            jsonGrouping.push_back(addressInfo);
        }
        jsonGroupings.push_back(jsonGrouping);
    }
    return jsonGroupings;
}

UniValue listaddressbalances(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "listaddressbalances ( minamount )\n"
            "\nLists addresses of this wallet and their balances\n"
            "\nArguments:\n"
            "1. minamount               (numeric, optional, default=0) Minimum balance in " +
            CURRENCY_UNIT + " an address should have to be shown in the list\n"
                            "\nResult:\n"
                            "{\n"
                            "  \"address\": amount,       (string) The memeium address and the amount in " +
            CURRENCY_UNIT + "\n"
                            "  ,...\n"
                            "}\n"
                            "\nExamples:\n" +
            HelpExampleCli("listaddressbalances", "") + HelpExampleCli("listaddressbalances", "10") + HelpExampleRpc("listaddressbalances", "") + HelpExampleRpc("listaddressbalances", "10"));

    LOCK2(cs_main, pwallet->cs_wallet);

    CAmount nMinAmount = 0;
    if (request.params.size() > 0)
        nMinAmount = AmountFromValue(request.params[0]);

    if (nMinAmount < 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");

    UniValue jsonBalances(UniValue::VOBJ);
    std::map<CTxDestination, CAmount> balances = pwallet->GetAddressBalances();
    for (auto& balance : balances)
        if (balance.second >= nMinAmount)
            jsonBalances.push_back(Pair(CBitcoinAddress(balance.first).ToString(), ValueFromAmount(balance.second)));

    return jsonBalances;
}

UniValue signmessage(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            "signmessage \"address\" \"message\"\n"
            "\nSign a message with the private key of an address" +
            HelpRequiringPassphrase(pwallet) + "\n"
                                               "\nArguments:\n"
                                               "1. \"address\"         (string, required) The memeium address to use for the private key.\n"
                                               "2. \"message\"         (string, required) The message to create a signature of.\n"
                                               "\nResult:\n"
                                               "\"signature\"          (string) The signature of the message encoded in base 64\n"
                                               "\nExamples:\n"
                                               "\nUnlock the wallet for 30 seconds\n" +
            HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
            "\nCreate the signature\n" + HelpExampleCli("signmessage", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\" \"my message\"") +
            "\nVerify the signature\n" + HelpExampleCli("verifymessage", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\" \"signature\" \"my message\"") +
            "\nAs json rpc\n" + HelpExampleRpc("signmessage", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\", \"my message\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    std::string strAddress = request.params[0].get_str();
    std::string strMessage = request.params[1].get_str();

    CBitcoinAddress addr(strAddress);
    if (!addr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID;
    if (!addr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    CKey key;
    if (!pwallet->GetKey(keyID, key)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");
    }

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    std::vector<unsigned char> vchSig;
    if (!key.SignCompact(ss.GetHash(), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    return EncodeBase64(&vchSig[0], vchSig.size());
}

UniValue getreceivedbyaddress(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
            "getreceivedbyaddress \"address\" ( minconf addlocked )\n"
            "\nReturns the total amount received by the given address in transactions with at least minconf confirmations.\n"
            "\nArguments:\n"
            "1. \"address\"         (string, required) The memeium address for transactions.\n"
            "2. minconf             (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "3. addlocked           (bool, optional, default=false) Whether to include transactions locked via InstantSend.\n"
            "\nResult:\n"
            "amount   (numeric) The total amount in " +
            CURRENCY_UNIT + " received at this address.\n"
                            "\nExamples:\n"
                            "\nThe amount from transactions with at least 1 confirmation\n" +
            HelpExampleCli("getreceivedbyaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\"") +
            "\nThe amount including unconfirmed transactions, zero confirmations\n" + HelpExampleCli("getreceivedbyaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\" 0") +
            "\nThe amount with at least 6 confirmations\n" + HelpExampleCli("getreceivedbyaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\" 6") +
            "\nAs a json rpc call\n" + HelpExampleRpc("getreceivedbyaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\", 6"));

    LOCK2(cs_main, pwallet->cs_wallet);

    // Memeium address
    CBitcoinAddress address = CBitcoinAddress(request.params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Memeium address");
    CScript scriptPubKey = GetScriptForDestination(address.Get());
    if (!IsMine(*pwallet, scriptPubKey)) {
        return ValueFromAmount(0);
    }

    // Minimum confirmations
    int nMinDepth = 1;
    if (!request.params[1].isNull())
        nMinDepth = request.params[1].get_int();
    bool fAddLocked = (request.params.size() > 2 && request.params[2].get_bool());

    // Tally
    CAmount nAmount = 0;
    for (const std::pair<uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;
        if (wtx.IsCoinBase() || !CheckFinalTx(*wtx.tx))
            continue;

        for (const CTxOut& txout : wtx.tx->vout)
            if (txout.scriptPubKey == scriptPubKey)
                if ((wtx.GetDepthInMainChain() >= nMinDepth) || (fAddLocked && wtx.IsLockedByInstantSend()))
                    nAmount += txout.nValue;
    }

    return ValueFromAmount(nAmount);
}


UniValue getreceivedbyaccount(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
            "getreceivedbyaccount \"account\" ( minconf addlocked )\n"
            "\nDEPRECATED. Returns the total amount received by addresses with <account> in transactions with specified minimum number of confirmations.\n"
            "\nArguments:\n"
            "1. \"account\"      (string, required) The selected account, may be the default account using \"\".\n"
            "2. minconf        (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "3. addlocked      (bool, optional, default=false) Whether to include transactions locked via InstantSend.\n"
            "\nResult:\n"
            "amount            (numeric) The total amount in " +
            CURRENCY_UNIT + " received for this account.\n"
                            "\nExamples:\n"
                            "\nAmount received by the default account with at least 1 confirmation\n" +
            HelpExampleCli("getreceivedbyaccount", "\"\"") +
            "\nAmount received at the tabby account including unconfirmed amounts with zero confirmations\n" + HelpExampleCli("getreceivedbyaccount", "\"tabby\" 0") +
            "\nThe amount with at least 6 confirmations\n" + HelpExampleCli("getreceivedbyaccount", "\"tabby\" 6") +
            "\nAs a json rpc call\n" + HelpExampleRpc("getreceivedbyaccount", "\"tabby\", 6"));

    LOCK2(cs_main, pwallet->cs_wallet);

    // Minimum confirmations
    int nMinDepth = 1;
    if (!request.params[1].isNull())
        nMinDepth = request.params[1].get_int();
    bool fAddLocked = (request.params.size() > 2 && request.params[2].get_bool());

    // Get the set of pub keys assigned to account
    std::string strAccount = AccountFromValue(request.params[0]);
    std::set<CTxDestination> setAddress = pwallet->GetAccountAddresses(strAccount);

    // Tally
    CAmount nAmount = 0;
    for (const std::pair<uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;
        if (wtx.IsCoinBase() || !CheckFinalTx(*wtx.tx))
            continue;

        for (const CTxOut& txout : wtx.tx->vout) {
            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*pwallet, address) && setAddress.count(address)) {
                if ((wtx.GetDepthInMainChain() >= nMinDepth) || (fAddLocked && wtx.IsLockedByInstantSend()))
                    nAmount += txout.nValue;
            }
        }
    }

    return ValueFromAmount(nAmount);
}


UniValue getbalance(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            "getbalance ( \"account\" minconf addlocked include_watchonly )\n"
            "\nIf account is not specified, returns the server's total available balance.\n"
            "If account is specified (DEPRECATED), returns the balance in the account.\n"
            "Note that the account \"\" is not the same as leaving the parameter out.\n"
            "The server total may be different to the balance in the default \"\" account.\n"
            "\nArguments:\n"
            "1. \"account\"        (string, optional) DEPRECATED. The selected account, or \"*\" for entire wallet. It may be the default account using \"\".\n"
            "2. minconf          (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "3. addlocked      (bool, optional, default=false) Whether to include the value of transactions locked via InstantSend in the wallet's balance.\n"
            "4. include_watchonly (bool, optional, default=false) Also include balance in watch-only addresses (see 'importaddress')\n"
            "\nResult:\n"
            "amount              (numeric) The total amount in " +
            CURRENCY_UNIT + " received for this account.\n"
                            "\nExamples:\n"
                            "\nThe total amount in the wallet with 1 or more confirmations\n" +
            HelpExampleCli("getbalance", "") +
            "\nThe total amount in the wallet at least 6 blocks confirmed\n" + HelpExampleCli("getbalance", "\"*\" 6") +
            "\nAs a json rpc call\n" + HelpExampleRpc("getbalance", "\"*\", 6"));

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.params.size() == 0)
        return ValueFromAmount(pwallet->GetBalance());

    const std::string& account_param = request.params[0].get_str();
    const std::string* account = account_param != "*" ? &account_param : nullptr;

    int nMinDepth = 1;
    if (!request.params[1].isNull())
        nMinDepth = request.params[1].get_int();
    bool fAddLocked = (request.params.size() > 2 && request.params[2].get_bool());
    isminefilter filter = ISMINE_SPENDABLE;
    if (!request.params[3].isNull())
        if (request.params[3].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    return ValueFromAmount(pwallet->GetLegacyBalance(filter, nMinDepth, account, fAddLocked));
}

UniValue getunconfirmedbalance(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
            "getunconfirmedbalance\n"
            "Returns the server's total unconfirmed balance\n");

    LOCK2(cs_main, pwallet->cs_wallet);

    return ValueFromAmount(pwallet->GetUnconfirmedBalance());
}


UniValue movecmd(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 3 || request.params.size() > 5)
        throw std::runtime_error(
            "move \"fromaccount\" \"toaccount\" amount ( minconf \"comment\" )\n"
            "\nDEPRECATED. Move a specified amount from one account in your wallet to another.\n"
            "\nArguments:\n"
            "1. \"fromaccount\"    (string, required) The name of the account to move funds from. May be the default account using \"\".\n"
            "2. \"toaccount\"      (string, required) The name of the account to move funds to. May be the default account using \"\".\n"
            "3. amount           (numeric) Quantity of " +
            CURRENCY_UNIT + " to move between accounts.\n"
                            "4. (dummy)          (numeric, optional) Ignored. Remains for backward compatibility.\n"
                            "5. \"comment\"        (string, optional) An optional comment, stored in the wallet only.\n"
                            "\nResult:\n"
                            "true|false          (boolean) true if successful.\n"
                            "\nExamples:\n"
                            "\nMove 0.01 " +
            CURRENCY_UNIT + " from the default account to the account named tabby\n" + HelpExampleCli("move", "\"\" \"tabby\" 0.01") +
            "\nMove 0.01 " + CURRENCY_UNIT + " timotei to akiko with a comment and funds have 6 confirmations\n" + HelpExampleCli("move", "\"timotei\" \"akiko\" 0.01 6 \"happy birthday!\"") +
            "\nAs a json rpc call\n" + HelpExampleRpc("move", "\"timotei\", \"akiko\", 0.01, 6, \"happy birthday!\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strFrom = AccountFromValue(request.params[0]);
    std::string strTo = AccountFromValue(request.params[1]);
    CAmount nAmount = AmountFromValue(request.params[2]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");
    if (request.params.size() > 3)
        // unused parameter, used to be nMinDepth, keep type-checking it though
        (void)request.params[3].get_int();
    std::string strComment;
    if (request.params.size() > 4)
        strComment = request.params[4].get_str();

    if (!pwallet->AccountMove(strFrom, strTo, nAmount, strComment)) {
        throw JSONRPCError(RPC_DATABASE_ERROR, "database error");
    }

    return true;
}


UniValue sendfrom(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 3 || request.params.size() > 7)
        throw std::runtime_error(
            "sendfrom \"fromaccount\" \"toaddress\" amount ( minconf addlocked \"comment\" \"comment_to\" )\n"
            "\nDEPRECATED (use sendtoaddress). Sent an amount from an account to a memeium address." +
            HelpRequiringPassphrase(pwallet) + "\n"
                                               "\nArguments:\n"
                                               "1. \"fromaccount\"       (string, required) The name of the account to send funds from. May be the default account using \"\".\n"
                                               "                       Specifying an account does not influence coin selection, but it does associate the newly created\n"
                                               "                       transaction with the account, so the account's balance computation and transaction history can reflect\n"
                                               "                       the spend.\n"
                                               "2. \"toaddress\"         (string, required) The memeium address to send funds to.\n"
                                               "3. amount              (numeric or string, required) The amount in " +
            CURRENCY_UNIT + " (transaction fee is added on top).\n"
                            "4. minconf             (numeric, optional, default=1) Only use funds with at least this many confirmations.\n"
                            "5. addlocked         (bool, optional, default=false) Whether to include transactions locked via InstantSend.\n"
                            "6. \"comment\"           (string, optional) A comment used to store what the transaction is for. \n"
                            "                       This is not part of the transaction, just kept in your wallet.\n"
                            "7. \"comment_to\"        (string, optional) An optional comment to store the name of the person or organization \n"
                            "                       to which you're sending the transaction. This is not part of the transaction, \n"
                            "                       it is just kept in your wallet.\n"
                            "\nResult:\n"
                            "\"txid\"                 (string) The transaction id.\n"
                            "\nExamples:\n"
                            "\nSend 0.01 " +
            CURRENCY_UNIT + " from the default account to the address, must have at least 1 confirmation\n" + HelpExampleCli("sendfrom", "\"\" \"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\" 0.01") +
            "\nSend 0.01 from the tabby account to the given address, funds must have at least 6 confirmations\n" + HelpExampleCli("sendfrom", "\"tabby\" \"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\" 0.01 6 false \"donation\" \"seans outpost\"") +
            "\nAs a json rpc call\n" + HelpExampleRpc("sendfrom", "\"tabby\", \"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\", 0.01, 6, false, \"donation\", \"seans outpost\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strAccount = AccountFromValue(request.params[0]);
    CBitcoinAddress address(request.params[1].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Memeium address");
    CAmount nAmount = AmountFromValue(request.params[2]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");
    int nMinDepth = 1;
    if (request.params.size() > 3)
        nMinDepth = request.params[3].get_int();
    bool fAddLocked = (request.params.size() > 4 && request.params[4].get_bool());

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;
    if (request.params.size() > 5 && !request.params[5].isNull() && !request.params[5].get_str().empty())
        wtx.mapValue["comment"] = request.params[5].get_str();
    if (request.params.size() > 6 && !request.params[6].isNull() && !request.params[6].get_str().empty())
        wtx.mapValue["to"] = request.params[6].get_str();

    EnsureWalletIsUnlocked(pwallet);

    // Check funds
    CAmount nBalance = pwallet->GetLegacyBalance(ISMINE_SPENDABLE, nMinDepth, &strAccount, fAddLocked);
    if (nAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    CCoinControl no_coin_control; // This is a deprecated API
    SendMoney(pwallet, address.Get(), nAmount, false, wtx, no_coin_control);

    return wtx.GetHash().GetHex();
}


UniValue sendmany(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 10)
        throw std::runtime_error(
            "sendmany \"fromaccount\" {\"address\":amount,...} ( minconf addlocked \"comment\" [\"address\",...] subtractfeefrom use_is use_ps conf_target \"estimate_mode\")\n"
            "\nSend multiple times. Amounts are double-precision floating point numbers." +
            HelpRequiringPassphrase(pwallet) + "\n"
                                               "\nArguments:\n"
                                               "1. \"fromaccount\"           (string, required) DEPRECATED. The account to send the funds from. Should be \"\" for the default account\n"
                                               "2. \"amounts\"               (string, required) A json object with addresses and amounts\n"
                                               "    {\n"
                                               "      \"address\":amount     (numeric or string) The memeium address is the key, the numeric amount (can be string) in " +
            CURRENCY_UNIT + " is the value\n"
                            "      ,...\n"
                            "    }\n"
                            "3. minconf                 (numeric, optional, default=1) Only use the balance confirmed at least this many times.\n"
                            "4. addlocked               (bool, optional, default=false) Whether to include transactions locked via InstantSend.\n"
                            "5. \"comment\"               (string, optional) A comment\n"
                            "6. subtractfeefrom         (array, optional) A json array with addresses.\n"
                            "                           The fee will be equally deducted from the amount of each selected address.\n"
                            "                           Those recipients will receive less memeiums than you enter in their corresponding amount field.\n"
                            "                           If no addresses are specified here, the sender pays the fee.\n"
                            "    [\n"
                            "      \"address\"          (string) Subtract fee from this address\n"
                            "      ,...\n"
                            "    ]\n"
                            "7. \"use_is\"                (bool, optional, default=false) Deprecated and ignored\n"
                            "8. \"use_ps\"                (bool, optional, default=false) Use PrivateSend funds only\n"
                            "9. conf_target            (numeric, optional) Confirmation target (in blocks)\n"
                            "10. \"estimate_mode\"      (string, optional, default=UNSET) The fee estimate mode, must be one of:\n"
                            "       \"UNSET\"\n"
                            "       \"ECONOMICAL\"\n"
                            "       \"CONSERVATIVE\"\n"
                            "\nResult:\n"
                            "\"txid\"                   (string) The transaction id for the send. Only 1 transaction is created regardless of \n"
                            "                                    the number of addresses.\n"
                            "\nExamples:\n"
                            "\nSend two amounts to two different addresses:\n" +
            HelpExampleCli("sendmany", "\"tabby\" \"{\\\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\\\":0.01,\\\"XuQQkwA4FYkq2XERzMY2CiAZhJTEDAbtcG\\\":0.02}\"") +
            "\nSend two amounts to two different addresses setting the confirmation and comment:\n" + HelpExampleCli("sendmany", "\"tabby\" \"{\\\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\\\":0.01,\\\"XuQQkwA4FYkq2XERzMY2CiAZhJTEDAbtcG\\\":0.02}\" 6 false \"testing\"") +
            "\nAs a json rpc call\n" + HelpExampleRpc("sendmany", "\"tabby\", \"{\\\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\\\":0.01,\\\"XuQQkwA4FYkq2XERzMY2CiAZhJTEDAbtcG\\\":0.02}\", 6, false, \"testing\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    if (pwallet->GetBroadcastTransactions() && !g_connman) {
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");
    }

    std::string strAccount = AccountFromValue(request.params[0]);
    UniValue sendTo = request.params[1].get_obj();
    int nMinDepth = 1;
    if (!request.params[2].isNull())
        nMinDepth = request.params[2].get_int();
    bool fAddLocked = (request.params.size() > 3 && request.params[3].get_bool());

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;
    if (request.params.size() > 4 && !request.params[4].isNull() && !request.params[4].get_str().empty())
        wtx.mapValue["comment"] = request.params[4].get_str();

    UniValue subtractFeeFrom(UniValue::VARR);
    if (request.params.size() > 5 && !request.params[5].isNull())
        subtractFeeFrom = request.params[5].get_array();

    CCoinControl coin_control;

    if (request.params.size() > 7 && !request.params[7].isNull()) {
        coin_control.UsePrivateSend(request.params[7].get_bool());
    }

    if (request.params.size() > 8 && !request.params[8].isNull()) {
        coin_control.m_confirm_target = ParseConfirmTarget(request.params[8]);
    }

    if (request.params.size() > 9 && !request.params[9].isNull()) {
        if (!FeeModeFromString(request.params[9].get_str(), coin_control.m_fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }

    std::set<CBitcoinAddress> setAddress;
    std::vector<CRecipient> vecSend;

    CAmount totalAmount = 0;
    std::vector<std::string> keys = sendTo.getKeys();
    for (const std::string& name_ : keys) {
        CBitcoinAddress address(name_);
        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Memeium address: ") + name_);

        if (setAddress.count(address))
            throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + name_);
        setAddress.insert(address);

        CScript scriptPubKey = GetScriptForDestination(address.Get());
        CAmount nAmount = AmountFromValue(sendTo[name_]);
        if (nAmount <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");
        totalAmount += nAmount;

        bool fSubtractFeeFromAmount = false;
        for (unsigned int idx = 0; idx < subtractFeeFrom.size(); idx++) {
            const UniValue& addr = subtractFeeFrom[idx];
            if (addr.get_str() == name_)
                fSubtractFeeFromAmount = true;
        }

        CRecipient recipient = {scriptPubKey, nAmount, fSubtractFeeFromAmount};
        vecSend.push_back(recipient);
    }

    EnsureWalletIsUnlocked(pwallet);

    // Check funds
    CAmount nBalance = pwallet->GetLegacyBalance(ISMINE_SPENDABLE, nMinDepth, &strAccount, fAddLocked);
    if (totalAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    // Send
    CReserveKey keyChange(pwallet);
    CAmount nFeeRequired = 0;
    int nChangePosRet = -1;
    std::string strFailReason;

    bool fCreated = pwallet->CreateTransaction(vecSend, wtx, keyChange, nFeeRequired, nChangePosRet, strFailReason,
        coin_control);
    if (!fCreated)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strFailReason);
    CValidationState state;
    if (!pwallet->CommitTransaction(wtx, keyChange, g_connman.get(), state)) {
        strFailReason = strprintf("Transaction commit failed:: %s", state.GetRejectReason());
        throw JSONRPCError(RPC_WALLET_ERROR, strFailReason);
    }

    return wtx.GetHash().GetHex();
}

// Defined in rpc/misc.cpp
extern CScript _createmultisig_redeemScript(CWallet* const pwallet, const UniValue& params);

UniValue addmultisigaddress(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3) {
        std::string msg = "addmultisigaddress nrequired [\"key\",...] ( \"account\" )\n"
                          "\nAdd a nrequired-to-sign multisignature address to the wallet. Requires a new wallet backup.\n"
                          "Each key is a Memeium address or hex-encoded public key.\n"
                          "If 'account' is specified (DEPRECATED), assign address to that account.\n"

                          "\nArguments:\n"
                          "1. nrequired        (numeric, required) The number of required signatures out of the n keys or addresses.\n"
                          "2. \"keys\"         (string, required) A json array of memeium addresses or hex-encoded public keys\n"
                          "     [\n"
                          "       \"address\"  (string) memeium address or hex-encoded public key\n"
                          "       ...,\n"
                          "     ]\n"
                          "3. \"account\"      (string, optional) DEPRECATED. An account to assign the addresses to.\n"

                          "\nResult:\n"
                          "\"address\"         (string) A memeium address associated with the keys.\n"

                          "\nExamples:\n"
                          "\nAdd a multisig address from 2 addresses\n" +
                          HelpExampleCli("addmultisigaddress", "2 \"[\\\"Xt4qk9uKvQYAonVGSZNXqxeDmtjaEWgfrS\\\",\\\"XoSoWQkpgLpppPoyyzbUFh1fq2RBvW6UK2\\\"]\"") +
                          "\nAs json rpc call\n" + HelpExampleRpc("addmultisigaddress", "2, \"[\\\"Xt4qk9uKvQYAonVGSZNXqxeDmtjaEWgfrS\\\",\\\"XoSoWQkpgLpppPoyyzbUFh1fq2RBvW6UK2\\\"]\"");
        throw std::runtime_error(msg);
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strAccount;
    if (request.params.size() > 2)
        strAccount = AccountFromValue(request.params[2]);

    // Construct using pay-to-script-hash:
    CScript inner = _createmultisig_redeemScript(pwallet, request.params);
    CScriptID innerID(inner);
    pwallet->AddCScript(inner);

    pwallet->SetAddressBook(innerID, strAccount, "send");
    return CBitcoinAddress(innerID).ToString();
}


struct tallyitem {
    CAmount nAmount;
    int nConf;
    std::vector<uint256> txids;
    bool fIsWatchonly;
    tallyitem()
    {
        nAmount = 0;
        nConf = std::numeric_limits<int>::max();
        fIsWatchonly = false;
    }
};

UniValue ListReceived(CWallet* const pwallet, const UniValue& params, bool fByAccounts)
{
    // Minimum confirmations
    int nMinDepth = 1;
    if (!params[0].isNull())
        nMinDepth = params[0].get_int();
    bool fAddLocked = (params.size() > 1 && params[1].get_bool());

    // Whether to include empty accounts
    bool fIncludeEmpty = false;
    if (!params[2].isNull())
        fIncludeEmpty = params[2].get_bool();

    isminefilter filter = ISMINE_SPENDABLE;
    if (!params[3].isNull())
        if (params[3].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    // Tally
    std::map<CBitcoinAddress, tallyitem> mapTally;
    for (const std::pair<uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;

        if (wtx.IsCoinBase() || !CheckFinalTx(*wtx.tx))
            continue;

        int nDepth = wtx.GetDepthInMainChain();
        if ((nDepth < nMinDepth) && !(fAddLocked && wtx.IsLockedByInstantSend()))
            continue;

        for (const CTxOut& txout : wtx.tx->vout) {
            CTxDestination address;
            if (!ExtractDestination(txout.scriptPubKey, address))
                continue;

            isminefilter mine = IsMine(*pwallet, address);
            if (!(mine & filter))
                continue;

            tallyitem& item = mapTally[address];
            item.nAmount += txout.nValue;
            item.nConf = std::min(item.nConf, nDepth);
            item.txids.push_back(wtx.GetHash());
            if (mine & ISMINE_WATCH_ONLY)
                item.fIsWatchonly = true;
        }
    }

    // Reply
    UniValue ret(UniValue::VARR);
    std::map<std::string, tallyitem> mapAccountTally;
    for (const std::pair<CBitcoinAddress, CAddressBookData>& item : pwallet->mapAddressBook) {
        const CBitcoinAddress& address = item.first;
        const std::string& strAccount = item.second.name;
        std::map<CBitcoinAddress, tallyitem>::iterator it = mapTally.find(address);
        if (it == mapTally.end() && !fIncludeEmpty)
            continue;

        isminefilter mine = IsMine(*pwallet, address.Get());
        if (!(mine & filter))
            continue;

        CAmount nAmount = 0;
        int nConf = std::numeric_limits<int>::max();
        bool fIsWatchonly = false;
        if (it != mapTally.end()) {
            nAmount = (*it).second.nAmount;
            nConf = (*it).second.nConf;
            fIsWatchonly = (*it).second.fIsWatchonly;
        }

        if (fByAccounts) {
            tallyitem& _item = mapAccountTally[strAccount];
            _item.nAmount += nAmount;
            _item.nConf = std::min(_item.nConf, nConf);
            _item.fIsWatchonly = fIsWatchonly;
        } else {
            UniValue obj(UniValue::VOBJ);
            if (fIsWatchonly)
                obj.push_back(Pair("involvesWatchonly", true));
            obj.push_back(Pair("address", address.ToString()));
            obj.push_back(Pair("account", strAccount));
            obj.push_back(Pair("amount", ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            if (!fByAccounts)
                obj.push_back(Pair("label", strAccount));
            UniValue transactions(UniValue::VARR);
            if (it != mapTally.end()) {
                for (const uint256& _item : (*it).second.txids) {
                    transactions.push_back(_item.GetHex());
                }
            }
            obj.push_back(Pair("txids", transactions));
            ret.push_back(obj);
        }
    }

    if (fByAccounts) {
        for (std::map<std::string, tallyitem>::iterator it = mapAccountTally.begin(); it != mapAccountTally.end(); ++it) {
            CAmount nAmount = (*it).second.nAmount;
            int nConf = (*it).second.nConf;
            UniValue obj(UniValue::VOBJ);
            if ((*it).second.fIsWatchonly)
                obj.push_back(Pair("involvesWatchonly", true));
            obj.push_back(Pair("account", (*it).first));
            obj.push_back(Pair("amount", ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            ret.push_back(obj);
        }
    }

    return ret;
}

UniValue listreceivedbyaddress(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            "listreceivedbyaddress ( minconf addlocked include_empty include_watchonly)\n"
            "\nList incoming payments grouped by receiving address.\n"
            "\nArguments:\n"
            "1. minconf           (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. addlocked         (bool, optional, default=false) Whether to include transactions locked via InstantSend.\n"
            "3. include_empty     (bool, optional, default=false) Whether to include addresses that haven't received any payments.\n"
            "4. include_watchonly (bool, optional, default=false) Whether to include watch-only addresses (see 'importaddress').\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : true,        (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"address\" : \"receivingaddress\",    (string) The receiving address\n"
            "    \"account\" : \"accountname\",         (string) DEPRECATED. The account of the receiving address. The default account is \"\".\n"
            "    \"amount\" : x.xxx,                  (numeric) The total amount in " +
            CURRENCY_UNIT + " received by the address\n"
                            "    \"confirmations\" : n                (numeric) The number of confirmations of the most recent transaction included.\n"
                            "                                                 If 'addlocked' is true, the number of confirmations can be less than\n"
                            "                                                 configured for transactions locked via InstantSend\n"
                            "    \"label\" : \"label\",               (string) A comment for the address/transaction, if any\n"
                            "    \"txids\": [\n"
                            "       n,                                (numeric) The ids of transactions received with the address \n"
                            "       ...\n"
                            "    ]\n"
                            "  }\n"
                            "  ,...\n"
                            "]\n"

                            "\nExamples:\n" +
            HelpExampleCli("listreceivedbyaddress", "") + HelpExampleCli("listreceivedbyaddress", "6 false true") + HelpExampleRpc("listreceivedbyaddress", "6, false, true, true"));

    LOCK2(cs_main, pwallet->cs_wallet);

    return ListReceived(pwallet, request.params, false);
}

UniValue listreceivedbyaccount(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            "listreceivedbyaccount ( minconf addlocked include_empty include_watchonly)\n"
            "\nDEPRECATED. List incoming payments grouped by account.\n"
            "\nArguments:\n"
            "1. minconf           (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. addlocked         (bool, optional, default=false) Whether to include transactions locked via InstantSend.\n"
            "3. include_empty     (bool, optional, default=false) Whether to include accounts that haven't received any payments.\n"
            "4. include_watchonly (bool, optional, default=false) Whether to include watch-only addresses (see 'importaddress').\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : true,   (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"account\" : \"accountname\",    (string) The account name of the receiving account\n"
            "    \"amount\" : x.xxx,             (numeric) The total amount received by addresses with this account\n"
            "    \"confirmations\" : n           (numeric) The number of blockchain confirmations of the most recent transaction included\n"
            "    \"label\" : \"label\"             (string) A comment for the address/transaction, if any\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listreceivedbyaccount", "") + HelpExampleCli("listreceivedbyaccount", "6 false true") + HelpExampleRpc("listreceivedbyaccount", "6, false, true, true"));

    LOCK2(cs_main, pwallet->cs_wallet);

    return ListReceived(pwallet, request.params, true);
}

static void MaybePushAddress(UniValue& entry, const CTxDestination& dest)
{
    CBitcoinAddress addr;
    if (addr.Set(dest))
        entry.push_back(Pair("address", addr.ToString()));
}

/**
 * List transactions based on the given criteria.
 *
 * @param  pwallet    The wallet.
 * @param  wtx        The wallet transaction.
 * @param  strAccount The account, if any, or "*" for all.
 * @param  nMinDepth  The minimum confirmation depth.
 * @param  fLong      Whether to include the JSON version of the transaction.
 * @param  ret        The UniValue into which the result is stored.
 * @param  filter     The "is mine" filter bool.
 */
void ListTransactions(CWallet* const pwallet, const CWalletTx& wtx, const std::string& strAccount, int nMinDepth, bool fLong, UniValue& ret, UniValue& retAssets, const isminefilter& filter)
{
    CAmount nFee;
    std::string strSentAccount;
    std::list<COutputEntry> listReceived;
    std::list<COutputEntry> listSent;

    std::list<CAssetOutputEntry> listAssetsReceived;
    std::list<CAssetOutputEntry> listAssetsSent;

    wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, filter, listAssetsReceived, listAssetsSent);

    bool fAllAccounts = (strAccount == std::string("*"));
    bool involvesWatchonly = wtx.IsFromMe(ISMINE_WATCH_ONLY);

    // Sent
    if ((!listSent.empty() || nFee != 0) && (fAllAccounts || strAccount == strSentAccount)) {
        for (const COutputEntry& s : listSent) {
            UniValue entry(UniValue::VOBJ);
            if (involvesWatchonly || (::IsMine(*pwallet, s.destination) & ISMINE_WATCH_ONLY)) {
                entry.push_back(Pair("involvesWatchonly", true));
            }
            entry.push_back(Pair("account", strSentAccount));
            MaybePushAddress(entry, s.destination);
            std::map<std::string, std::string>::const_iterator it = wtx.mapValue.find("DS");
            entry.push_back(Pair("category", (it != wtx.mapValue.end() && it->second == "1") ? "privatesend" : "send"));
            entry.push_back(Pair("amount", ValueFromAmount(-s.amount)));
            if (pwallet->mapAddressBook.count(s.destination)) {
                entry.push_back(Pair("label", pwallet->mapAddressBook[s.destination].name));
            }
            entry.push_back(Pair("vout", s.vout));
            entry.push_back(Pair("fee", ValueFromAmount(-nFee)));
            if (fLong)
                WalletTxToJSON(wtx, entry);
            entry.push_back(Pair("abandoned", wtx.isAbandoned()));
            ret.push_back(entry);
        }
    }

    // Received
    if (listReceived.size() > 0 && ((wtx.GetDepthInMainChain() >= nMinDepth) || wtx.IsLockedByInstantSend())) {
        for (const COutputEntry& r : listReceived) {
            std::string account;
            if (pwallet->mapAddressBook.count(r.destination)) {
                account = pwallet->mapAddressBook[r.destination].name;
            }
            if (fAllAccounts || (account == strAccount)) {
                UniValue entry(UniValue::VOBJ);
                if (involvesWatchonly || (::IsMine(*pwallet, r.destination) & ISMINE_WATCH_ONLY)) {
                    entry.push_back(Pair("involvesWatchonly", true));
                }
                entry.push_back(Pair("account", account));
                MaybePushAddress(entry, r.destination);
                if (wtx.IsCoinBase()) {
                    if (wtx.GetDepthInMainChain() < 1)
                        entry.push_back(Pair("category", "orphan"));
                    else if (wtx.GetBlocksToMaturity() > 0)
                        entry.push_back(Pair("category", "immature"));
                    else
                        entry.push_back(Pair("category", "generate"));
                } else {
                    entry.push_back(Pair("category", "receive"));
                }
                entry.push_back(Pair("amount", ValueFromAmount(r.amount)));
                if (pwallet->mapAddressBook.count(r.destination)) {
                    entry.push_back(Pair("label", account));
                }
                entry.push_back(Pair("vout", r.vout));
                if (fLong)
                    WalletTxToJSON(wtx, entry);
                ret.push_back(entry);
            }
        }
    }

    /** MMM START */
    if (AreAssetsDeployed()) {
        if (listAssetsReceived.size() > 0 && wtx.GetDepthInMainChain() >= nMinDepth) {
            for (const CAssetOutputEntry& data : listAssetsReceived) {
                UniValue entry(UniValue::VOBJ);

                if (involvesWatchonly || (::IsMine(*pwallet, data.destination) & ISMINE_WATCH_ONLY)) {
                    entry.push_back(Pair("involvesWatchonly", true));
                }

                entry.push_back(Pair("asset_type", GetTxnOutputType(data.type)));
                entry.push_back(Pair("asset_name", data.assetName));
                entry.push_back(Pair("amount", ValueFromAmount(data.nAmount)));
                entry.push_back(Pair("message", EncodeAssetData(data.message)));
                if (!data.message.empty() && data.expireTime > 0)
                    entry.push_back(Pair("message_expires", DateTimeStrFormat("%Y-%m-%d %H:%M:%S", data.expireTime)));
                entry.push_back(Pair("destination", EncodeDestination(data.destination)));
                entry.push_back(Pair("vout", data.vout));
                entry.push_back(Pair("category", "receive"));
                if (fLong)
                    WalletTxToJSON(wtx, entry);
                entry.push_back(Pair("abandoned", wtx.isAbandoned()));
                retAssets.push_back(entry);
            }
        }

        if ((!listAssetsSent.empty() || nFee != 0) && (fAllAccounts || strAccount == strSentAccount)) {
            for (const CAssetOutputEntry& data : listAssetsSent) {
                UniValue entry(UniValue::VOBJ);

                if (involvesWatchonly || (::IsMine(*pwallet, data.destination) & ISMINE_WATCH_ONLY)) {
                    entry.push_back(Pair("involvesWatchonly", true));
                }

                entry.push_back(Pair("asset_type", GetTxnOutputType(data.type)));
                entry.push_back(Pair("asset_name", data.assetName));
                entry.push_back(Pair("amount", ValueFromAmount(data.nAmount)));
                entry.push_back(Pair("message", EncodeAssetData(data.message)));
                if (!data.message.empty() && data.expireTime > 0)
                    entry.push_back(Pair("message_expires", DateTimeStrFormat("%Y-%m-%d %H:%M:%S", data.expireTime)));
                entry.push_back(Pair("destination", EncodeDestination(data.destination)));
                entry.push_back(Pair("vout", data.vout));
                entry.push_back(Pair("category", "send"));
                if (fLong)
                    WalletTxToJSON(wtx, entry);
                entry.push_back(Pair("abandoned", wtx.isAbandoned()));
                retAssets.push_back(entry);
            }
        }
    }
    /** MMM END */
}

void ListTransactions(CWallet* const pwallet, const CWalletTx& wtx, const std::string& strAccount, int nMinDepth, bool fLong, UniValue& ret, const isminefilter& filter)
{
    UniValue assetDetails(UniValue::VARR);

    ListTransactions(pwallet, wtx, strAccount, nMinDepth, fLong, ret, assetDetails, filter);
}

void AcentryToJSON(const CAccountingEntry& acentry, const std::string& strAccount, UniValue& ret)
{
    bool fAllAccounts = (strAccount == std::string("*"));

    if (fAllAccounts || acentry.strAccount == strAccount) {
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("account", acentry.strAccount));
        entry.push_back(Pair("category", "move"));
        entry.push_back(Pair("time", acentry.nTime));
        entry.push_back(Pair("amount", ValueFromAmount(acentry.nCreditDebit)));
        entry.push_back(Pair("otheraccount", acentry.strOtherAccount));
        entry.push_back(Pair("comment", acentry.strComment));
        ret.push_back(entry);
    }
}

UniValue listtransactions(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            "listtransactions ( \"account\" count skip include_watchonly)\n"
            "\nReturns up to 'count' most recent transactions skipping the first 'from' transactions for account 'account'.\n"
            "\nArguments:\n"
            "1. \"account\"        (string, optional) DEPRECATED. The account name. Should be \"*\".\n"
            "2. count            (numeric, optional, default=10) The number of transactions to return\n"
            "3. skip           (numeric, optional, default=0) The number of transactions to skip\n"
            "4. include_watchonly (bool, optional, default=false) Include transactions to watch-only addresses (see 'importaddress')\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"account\":\"accountname\",  (string) DEPRECATED. The account name associated with the transaction. \n"
            "                                                It will be \"\" for the default account.\n"
            "    \"address\":\"address\",    (string) The memeium address of the transaction. Not present for \n"
            "                                                move transactions (category = move).\n"
            "    \"category\":\"send|receive|move\", (string) The transaction category. 'move' is a local (off blockchain)\n"
            "                                                transaction between accounts, and not associated with an address,\n"
            "                                                transaction id or block. 'send' and 'receive' transactions are \n"
            "                                                associated with an address, transaction id and block details\n"
            "    \"amount\": x.xxx,          (numeric) The amount in " +
            CURRENCY_UNIT + ". This is negative for the 'send' category, and for the\n"
                            "                                         'move' category for moves outbound. It is positive for the 'receive' category,\n"
                            "                                         and for the 'move' category for inbound funds.\n"
                            "    \"label\": \"label\",       (string) A comment for the address/transaction, if any\n"
                            "    \"vout\": n,                (numeric) the vout value\n"
                            "    \"fee\": x.xxx,             (numeric) The amount of the fee in " +
            CURRENCY_UNIT + ". This is negative and only available for the \n"
                            "                                         'send' category of transactions.\n"
                            "    \"confirmations\": n,       (numeric) The number of blockchain confirmations for the transaction. Available for 'send' and \n"
                            "                                         'receive' category of transactions. Negative confirmations indicate the\n"
                            "                                         transation conflicts with the block chain\n"
                            "    \"instantlock\" : true|false, (bool) Current transaction lock state. Available for 'send' and 'receive' category of transactions.\n"
                            "    \"instantlock_internal\" : true|false, (bool) Current internal transaction lock state. Available for 'send' and 'receive' category of transactions.\n"
                            "    \"chainlock\" : true|false, (bool) The state of the corresponding block chainlock\n"
                            "    \"trusted\": xxx,           (bool) Whether we consider the outputs of this unconfirmed transaction safe to spend.\n"
                            "    \"blockhash\": \"hashvalue\", (string) The block hash containing the transaction. Available for 'send' and 'receive'\n"
                            "                                          category of transactions.\n"
                            "    \"blockindex\": n,          (numeric) The index of the transaction in the block that includes it. Available for 'send' and 'receive'\n"
                            "                                          category of transactions.\n"
                            "    \"blocktime\": xxx,         (numeric) The block time in seconds since epoch (1 Jan 1970 GMT).\n"
                            "    \"txid\": \"transactionid\",  (string) The transaction id. Available for 'send' and 'receive' category of transactions.\n"
                            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (midnight Jan 1 1970 GMT).\n"
                            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (midnight Jan 1 1970 GMT). Available \n"
                            "                                          for 'send' and 'receive' category of transactions.\n"
                            "    \"comment\": \"...\",         (string) If a comment is associated with the transaction.\n"
                            "    \"otheraccount\": \"accountname\",  (string) DEPRECATED. For the 'move' category of transactions, the account the funds came \n"
                            "                                          from (for receiving funds, positive amounts), or went to (for sending funds,\n"
                            "                                          negative amounts).\n"
                            "    \"abandoned\": xxx          (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
                            "                                         'send' category of transactions.\n"
                            "  }\n"
                            "]\n"

                            "\nExamples:\n"
                            "\nList the most recent 10 transactions in the systems\n" +
            HelpExampleCli("listtransactions", "") +
            "\nList transactions 100 to 120\n" + HelpExampleCli("listtransactions", "\"*\" 20 100") +
            "\nAs a json rpc call\n" + HelpExampleRpc("listtransactions", "\"*\", 20, 100"));

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strAccount = "*";
    if (!request.params[0].isNull())
        strAccount = request.params[0].get_str();
    int nCount = 10;
    if (!request.params[1].isNull())
        nCount = request.params[1].get_int();
    int nFrom = 0;
    if (!request.params[2].isNull())
        nFrom = request.params[2].get_int();
    isminefilter filter = ISMINE_SPENDABLE;
    if (!request.params[3].isNull())
        if (request.params[3].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    if (nCount < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count");
    if (nFrom < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative from");

    UniValue ret(UniValue::VARR);

    const CWallet::TxItems& txOrdered = pwallet->wtxOrdered;

    // iterate backwards until we have nCount items to return:
    for (CWallet::TxItems::const_reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it) {
        CWalletTx* const pwtx = (*it).second.first;
        if (pwtx != 0)
            ListTransactions(pwallet, *pwtx, strAccount, 0, true, ret, filter);
        CAccountingEntry* const pacentry = (*it).second.second;
        if (pacentry != 0)
            AcentryToJSON(*pacentry, strAccount, ret);

        if ((int)ret.size() >= (nCount + nFrom)) break;
    }
    // ret is newest to oldest

    if (nFrom > (int)ret.size())
        nFrom = ret.size();
    if ((nFrom + nCount) > (int)ret.size())
        nCount = ret.size() - nFrom;

    std::vector<UniValue> arrTmp = ret.getValues();

    std::vector<UniValue>::iterator first = arrTmp.begin();
    std::advance(first, nFrom);
    std::vector<UniValue>::iterator last = arrTmp.begin();
    std::advance(last, nFrom + nCount);

    if (last != arrTmp.end()) arrTmp.erase(last, arrTmp.end());
    if (first != arrTmp.begin()) arrTmp.erase(arrTmp.begin(), first);

    std::reverse(arrTmp.begin(), arrTmp.end()); // Return oldest to newest

    ret.clear();
    ret.setArray();
    ret.push_backV(arrTmp);

    return ret;
}

UniValue listaccounts(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 3)
        throw std::runtime_error(
            "listaccounts ( minconf addlocked include_watchonly)\n"
            "\nDEPRECATED. Returns Object that has account names as keys, account balances as values.\n"
            "\nArguments:\n"
            "1. minconf             (numeric, optional, default=1) Only include transactions with at least this many confirmations\n"
            "2. addlocked           (bool, optional, default=false) Whether to include transactions locked via InstantSend.\n"
            "3. include_watchonly   (bool, optional, default=false) Include balances in watch-only addresses (see 'importaddress')\n"
            "\nResult:\n"
            "{                    (json object where keys are account names, and values are numeric balances\n"
            "  \"account\": x.xxx,  (numeric) The property name is the account name, and the value is the total balance for the account.\n"
            "  ...\n"
            "}\n"
            "\nExamples:\n"
            "\nList account balances where there at least 1 confirmation\n" +
            HelpExampleCli("listaccounts", "") +
            "\nList account balances including zero confirmation transactions\n" + HelpExampleCli("listaccounts", "0") +
            "\nList account balances for 6 or more confirmations\n" + HelpExampleCli("listaccounts", "6") +
            "\nAs json rpc call\n" + HelpExampleRpc("listaccounts", "6"));

    LOCK2(cs_main, pwallet->cs_wallet);

    int nMinDepth = 1;
    if (request.params.size() > 0)
        nMinDepth = request.params[0].get_int();
    bool fAddLocked = (request.params.size() > 1 && request.params[1].get_bool());
    isminefilter includeWatchonly = ISMINE_SPENDABLE;
    if (request.params.size() > 2)
        if (request.params[2].get_bool())
            includeWatchonly = includeWatchonly | ISMINE_WATCH_ONLY;

    std::map<std::string, CAmount> mapAccountBalances;
    for (const std::pair<CTxDestination, CAddressBookData>& entry : pwallet->mapAddressBook) {
        if (IsMine(*pwallet, entry.first) & includeWatchonly) { // This address belongs to me
            mapAccountBalances[entry.second.name] = 0;
        }
    }

    for (const std::pair<uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;
        CAmount nFee;
        std::string strSentAccount;
        std::list<COutputEntry> listReceived;
        std::list<COutputEntry> listSent;
        int nDepth = wtx.GetDepthInMainChain();
        if (wtx.GetBlocksToMaturity() > 0 || nDepth < 0)
            continue;
        wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, includeWatchonly);
        mapAccountBalances[strSentAccount] -= nFee;
        for (const COutputEntry& s : listSent)
            mapAccountBalances[strSentAccount] -= s.amount;
        if ((nDepth >= nMinDepth) || (fAddLocked && wtx.IsLockedByInstantSend())) {
            for (const COutputEntry& r : listReceived)
                if (pwallet->mapAddressBook.count(r.destination)) {
                    mapAccountBalances[pwallet->mapAddressBook[r.destination].name] += r.amount;
                } else
                    mapAccountBalances[""] += r.amount;
        }
    }

    const std::list<CAccountingEntry>& acentries = pwallet->laccentries;
    for (const CAccountingEntry& entry : acentries)
        mapAccountBalances[entry.strAccount] += entry.nCreditDebit;

    UniValue ret(UniValue::VOBJ);
    for (const std::pair<std::string, CAmount>& accountBalance : mapAccountBalances) {
        ret.push_back(Pair(accountBalance.first, ValueFromAmount(accountBalance.second)));
    }
    return ret;
}

UniValue listsinceblock(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            "listsinceblock ( \"blockhash\" target_confirmations include_watchonly include_removed )\n"
            "\nGet all transactions in blocks since block [blockhash], or all transactions if omitted.\n"
            "If \"blockhash\" is no longer a part of the main chain, transactions from the fork point onward are included.\n"
            "Additionally, if include_removed is set, transactions affecting the wallet which were removed are returned in the \"removed\" array.\n"
            "\nArguments:\n"
            "1. \"blockhash\"            (string, optional) The block hash to list transactions since\n"
            "2. target_confirmations:    (numeric, optional, default=1) Return the nth block hash from the main chain. e.g. 1 would mean the best block hash. Note: this is not used as a filter, but only affects [lastblock] in the return value\n"
            "3. include_watchonly:       (bool, optional, default=false) Include transactions to watch-only addresses (see 'importaddress')\n"
            "4. include_removed:         (bool, optional, default=true) Show transactions that were removed due to a reorg in the \"removed\" array\n"
            "                                                           (not guaranteed to work on pruned nodes)\n"
            "\nResult:\n"
            "{\n"
            "  \"transactions\": [\n"
            "    \"account\":\"accountname\",  (string) DEPRECATED. The account name associated with the transaction. Will be \"\" for the default account.\n"
            "    \"address\":\"address\",    (string) The memeium address of the transaction. Not present for move transactions (category = move).\n"
            "    \"category\":\"send|receive\",  (string) The transaction category. 'send' has negative amounts, 'receive' has positive amounts.\n"
            "    \"amount\": x.xxx,          (numeric) The amount in " +
            CURRENCY_UNIT + ". This is negative for the 'send' category, and for the 'move' category for moves \n"
                            "                                          outbound. It is positive for the 'receive' category, and for the 'move' category for inbound funds.\n"
                            "    \"vout\" : n,               (numeric) the vout value\n"
                            "    \"fee\": x.xxx,             (numeric) The amount of the fee in " +
            CURRENCY_UNIT + ". This is negative and only available for the 'send' category of transactions.\n"
                            "    \"confirmations\" : n,      (numeric) The number of blockchain confirmations for the transaction. Available for 'send' and 'receive' category of transactions.\n"
                            "                                          When it's < 0, it means the transaction conflicted that many blocks ago.\n"
                            "    \"instantlock\" : true|false, (bool) Current transaction lock state. Available for 'send' and 'receive' category of transactions.\n"
                            "    \"instantlock_internal\" : true|false, (bool) Current internal transaction lock state. Available for 'send' and 'receive' category of transactions.\n"
                            "    \"chainlock\" : true|false, (bool) The state of the corresponding block chainlock\n"
                            "    \"blockhash\": \"hashvalue\", (string) The block hash containing the transaction. Available for 'send' and 'receive' category of transactions.\n"
                            "    \"blockindex\": n,          (numeric) The index of the transaction in the block that includes it. Available for 'send' and 'receive' category of transactions.\n"
                            "    \"blocktime\": xxx,         (numeric) The block time in seconds since epoch (1 Jan 1970 GMT).\n"
                            "    \"txid\": \"transactionid\",  (string) The transaction id. Available for 'send' and 'receive' category of transactions.\n"
                            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (Jan 1 1970 GMT).\n"
                            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (Jan 1 1970 GMT). Available for 'send' and 'receive' category of transactions.\n"
                            "    \"abandoned\": xxx,         (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the 'send' category of transactions.\n"
                            "    \"comment\": \"...\",         (string) If a comment is associated with the transaction.\n"
                            "    \"label\" : \"label\"         (string) A comment for the address/transaction, if any\n"
                            "    \"to\": \"...\",              (string) If a comment to is associated with the transaction.\n"
                            "  ],\n"
                            "  \"removed\": [\n"
                            "    <structure is the same as \"transactions\" above, only present if include_removed=true>\n"
                            "    Note: transactions that were readded in the active chain will appear as-is in this array, and may thus have a positive confirmation count.\n"
                            "  ],\n"
                            "  \"lastblock\": \"lastblockhash\"  (string) The hash of the block (target_confirmations-1) from the best block on the main chain. This is typically used to feed back into listsinceblock the next time you call it. So you would generally use a target_confirmations of say 6, so you will be continually re-notified of transactions until they've reached 6 confirmations plus any new ones\n"
                            "}\n"
                            "\nExamples:\n" +
            HelpExampleCli("listsinceblock", "") + HelpExampleCli("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\" 6") + HelpExampleRpc("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\", 6"));

    LOCK2(cs_main, pwallet->cs_wallet);

    const CBlockIndex* pindex = nullptr;    // Block index of the specified block or the common ancestor, if the block provided was in a deactivated chain.
    const CBlockIndex* paltindex = nullptr; // Block index of the specified block, even if it's in a deactivated chain.
    int target_confirms = 1;
    isminefilter filter = ISMINE_SPENDABLE;

    if (!request.params[0].isNull() && !request.params[0].get_str().empty()) {
        uint256 blockId;

        blockId.SetHex(request.params[0].get_str());
        BlockMap::iterator it = mapBlockIndex.find(blockId);
        if (it == mapBlockIndex.end()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
        paltindex = pindex = it->second;
        if (chainActive[pindex->nHeight] != pindex) {
            // the block being asked for is a part of a deactivated chain;
            // we don't want to depend on its perceived height in the block
            // chain, we want to instead use the last common ancestor
            pindex = chainActive.FindFork(pindex);
        }
    }

    if (!request.params[1].isNull()) {
        target_confirms = request.params[1].get_int();

        if (target_confirms < 1) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
        }
    }

    if (!request.params[2].isNull() && request.params[2].get_bool()) {
        filter = filter | ISMINE_WATCH_ONLY;
    }

    bool include_removed = (request.params[3].isNull() || request.params[3].get_bool());

    int depth = pindex ? (1 + chainActive.Height() - pindex->nHeight) : -1;

    UniValue transactions(UniValue::VARR);
    UniValue assetTransactions(UniValue::VARR);

    for (const std::pair<uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        CWalletTx tx = pairWtx.second;

        if (depth == -1 || tx.GetDepthInMainChain() < depth) {
            ListTransactions(pwallet, tx, "*", 0, true, transactions, assetTransactions, filter);
        }
    }

    // when a reorg'd block is requested, we also list any relevant transactions
    // in the blocks of the chain that was detached
    UniValue removed(UniValue::VARR);
    UniValue assetRemoved(UniValue::VARR);
    while (include_removed && paltindex && paltindex != pindex) {
        CBlock block;
        if (!ReadBlockFromDisk(block, paltindex, Params().GetConsensus())) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Can't read block from disk");
        }
        for (const CTransactionRef& tx : block.vtx) {
            if (pwallet->mapWallet.count(tx->GetHash()) > 0) {
                // We want all transactions regardless of confirmation count to appear here,
                // even negative confirmation ones, hence the big negative.
                ListTransactions(pwallet, pwallet->mapWallet[tx->GetHash()], "*", -100000000, true, removed, assetRemoved, filter);
            }
        }
        paltindex = paltindex->pprev;
    }

    CBlockIndex* pblockLast = chainActive[chainActive.Height() + 1 - target_confirms];
    uint256 lastblock = pblockLast ? pblockLast->GetBlockHash() : uint256();

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("transactions", transactions));
    if (include_removed) ret.push_back(Pair("removed", removed));
    ret.push_back(Pair("lastblock", lastblock.GetHex()));

    return ret;
}

UniValue gettransaction(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "gettransaction \"txid\" ( include_watchonly )\n"
            "\nGet detailed information about in-wallet transaction <txid>\n"
            "\nArguments:\n"
            "1. \"txid\"                  (string, required) The transaction id\n"
            "2. \"include_watchonly\"     (bool, optional, default=false) Whether to include watch-only addresses in balance calculation and details[]\n"
            "\nResult:\n"
            "{\n"
            "  \"amount\" : x.xxx,        (numeric) The transaction amount in " +
            CURRENCY_UNIT + "\n"
                            "  \"fee\": x.xxx,            (numeric) The amount of the fee in " +
            CURRENCY_UNIT + ". This is negative and only available for the \n"
                            "                              'send' category of transactions.\n"
                            "  \"instantlock\" : true|false, (bool) Current transaction lock state\n"
                            "  \"instantlock_internal\" : true|false, (bool) Current internal transaction lock state\n"
                            "  \"chainlock\" : true|false, (bool) The state of the corresponding block chainlock\n"
                            "  \"confirmations\" : n,     (numeric) The number of blockchain confirmations\n"
                            "  \"blockhash\" : \"hash\",    (string) The block hash\n"
                            "  \"blockindex\" : xx,       (numeric) The index of the transaction in the block that includes it\n"
                            "  \"blocktime\" : ttt,       (numeric) The time in seconds since epoch (1 Jan 1970 GMT)\n"
                            "  \"txid\" : \"transactionid\",   (string) The transaction id.\n"
                            "  \"time\" : ttt,            (numeric) The transaction time in seconds since epoch (1 Jan 1970 GMT)\n"
                            "  \"timereceived\" : ttt,    (numeric) The time received in seconds since epoch (1 Jan 1970 GMT)\n"
                            "  \"details\" : [\n"
                            "    {\n"
                            "      \"account\" : \"accountname\",      (string) DEPRECATED. The account name involved in the transaction, can be \"\" for the default account.\n"
                            "      \"address\" : \"address\",          (string) The memeium address involved in the transaction\n"
                            "      \"category\" : \"send|receive\",    (string) The category, either 'send' or 'receive'\n"
                            "      \"amount\" : x.xxx,               (numeric) The amount in " +
            CURRENCY_UNIT + "\n"
                            "      \"label\" : \"label\",              (string) A comment for the address/transaction, if any\n"
                            "      \"vout\" : n,                     (numeric) the vout value\n"
                            "      \"fee\": x.xxx,                     (numeric) The amount of the fee in " +
            CURRENCY_UNIT + ". This is negative and only available for the \n"
                            "                                           'send' category of transactions.\n"
                            "      \"abandoned\": xxx                  (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
                            "                                           'send' category of transactions.\n"
                            "    }\n"
                            "    ,...\n"
                            "  ],\n"
                            "  \"hex\" : \"data\"                      (string) Raw data for transaction\n"
                            "}\n"

                            "\nExamples:\n" +
            HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"") + HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\" true") + HelpExampleRpc("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    uint256 hash;
    hash.SetHex(request.params[0].get_str());

    isminefilter filter = ISMINE_SPENDABLE;
    if (!request.params[1].isNull())
        if (request.params[1].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    UniValue entry(UniValue::VOBJ);
    if (!pwallet->mapWallet.count(hash)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    }
    const CWalletTx& wtx = pwallet->mapWallet[hash];

    CAmount nCredit = wtx.GetCredit(filter);
    CAmount nDebit = wtx.GetDebit(filter);
    CAmount nNet = nCredit - nDebit;
    CAmount nFee = (wtx.IsFromMe(filter) ? wtx.tx->GetValueOut() - nDebit : 0);

    entry.push_back(Pair("amount", ValueFromAmount(nNet - nFee)));
    if (wtx.IsFromMe(filter))
        entry.push_back(Pair("fee", ValueFromAmount(nFee)));

    WalletTxToJSON(wtx, entry);

    UniValue details(UniValue::VARR);
    UniValue assetDetails(UniValue::VARR);
    ListTransactions(pwallet, wtx, "*", 0, false, details, assetDetails, filter);
    entry.push_back(Pair("details", details));
    entry.push_back(Pair("asset_details", assetDetails));

    std::string strHex = EncodeHexTx(static_cast<CTransaction>(wtx));
    entry.push_back(Pair("hex", strHex));

    return entry;
}

UniValue abandontransaction(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "abandontransaction \"txid\"\n"
            "\nMark in-wallet transaction <txid> as abandoned\n"
            "This will mark this transaction and all its in-wallet descendants as abandoned which will allow\n"
            "for their inputs to be respent.  It can be used to replace \"stuck\" or evicted transactions.\n"
            "It only works on transactions which are not included in a block and are not currently in the mempool.\n"
            "It has no effect on transactions which are already conflicted or abandoned.\n"
            "\nArguments:\n"
            "1. \"txid\"    (string, required) The transaction id\n"
            "\nResult:\n"
            "\nExamples:\n" +
            HelpExampleCli("abandontransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"") + HelpExampleRpc("abandontransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    uint256 hash;
    hash.SetHex(request.params[0].get_str());

    if (!pwallet->mapWallet.count(hash)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    }
    if (!pwallet->AbandonTransaction(hash)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not eligible for abandonment");
    }

    return NullUniValue;
}


UniValue backupwallet(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "backupwallet \"destination\"\n"
            "\nSafely copies current wallet file to destination, which can be a directory or a path with filename.\n"
            "\nArguments:\n"
            "1. \"destination\"   (string) The destination directory or file\n"
            "\nExamples:\n" +
            HelpExampleCli("backupwallet", "\"backup.dat\"") + HelpExampleRpc("backupwallet", "\"backup.dat\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strDest = request.params[0].get_str();
    if (!pwallet->BackupWallet(strDest)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet backup failed!");
    }

    return NullUniValue;
}


UniValue keypoolrefill(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "keypoolrefill ( newsize )\n"
            "\nFills the keypool." +
            HelpRequiringPassphrase(pwallet) + "\n"
                                               "\nArguments:\n"
                                               "1. newsize     (numeric, optional, default=" +
            itostr(DEFAULT_KEYPOOL_SIZE) + ") The new keypool size\n"
                                           "\nExamples:\n" +
            HelpExampleCli("keypoolrefill", "") + HelpExampleRpc("keypoolrefill", ""));

    LOCK2(cs_main, pwallet->cs_wallet);

    // 0 is interpreted by TopUpKeyPool() as the default keypool size given by -keypool
    unsigned int kpSize = 0;
    if (!request.params[0].isNull()) {
        if (request.params[0].get_int() < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected valid size.");
        kpSize = (unsigned int)request.params[0].get_int();
    }

    EnsureWalletIsUnlocked(pwallet);
    pwallet->TopUpKeyPool(kpSize);

    if (pwallet->GetKeyPoolSize() < (pwallet->IsHDEnabled() ? kpSize * 2 : kpSize)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error refreshing keypool.");
    }

    return NullUniValue;
}


static void LockWallet(CWallet* pWallet)
{
    LOCK(pWallet->cs_wallet);
    pWallet->nRelockTime = 0;
    pWallet->Lock();
}

UniValue walletpassphrase(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3) {
        throw std::runtime_error(
            "walletpassphrase \"passphrase\" timeout ( mixingonly )\n"
            "\nStores the wallet decryption key in memory for 'timeout' seconds.\n"
            "This is needed prior to performing transactions related to private keys such as sending memeiums\n"
            "\nArguments:\n"
            "1. \"passphrase\"        (string, required) The wallet passphrase\n"
            "2. timeout             (numeric, required) The time to keep the decryption key in seconds.\n"
            "3. mixingonly          (boolean, optional, default=false) If is true sending functions are disabled.\n"
            "\nNote:\n"
            "Issuing the walletpassphrase command while the wallet is already unlocked will set a new unlock\n"
            "time that overrides the old one.\n"
            "\nExamples:\n"
            "\nUnlock the wallet for 60 seconds\n" +
            HelpExampleCli("walletpassphrase", "\"my pass phrase\" 60") +
            "\nUnlock the wallet for 60 seconds but allow PrivateSend mixing only\n" + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 60 true") +
            "\nLock the wallet again (before 60 seconds)\n" + HelpExampleCli("walletlock", "") +
            "\nAs json rpc call\n" + HelpExampleRpc("walletpassphrase", "\"my pass phrase\", 60"));
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrase was called.");
    }

    // Note that the walletpassphrase is stored in request.params[0] which is not mlock()ed
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    strWalletPass = request.params[0].get_str().c_str();

    int64_t nSleepTime = request.params[1].get_int64();

    bool fForMixingOnly = false;
    if (request.params.size() >= 3)
        fForMixingOnly = request.params[2].get_bool();

    if (fForMixingOnly && !pwallet->IsLocked(true) && pwallet->IsLocked())
        throw JSONRPCError(RPC_WALLET_ALREADY_UNLOCKED, "Error: Wallet is already unlocked for mixing only.");

    if (!pwallet->IsLocked())
        throw JSONRPCError(RPC_WALLET_ALREADY_UNLOCKED, "Error: Wallet is already fully unlocked.");

    if (!pwallet->Unlock(strWalletPass, fForMixingOnly))
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");

    pwallet->TopUpKeyPool();

    pwallet->nRelockTime = GetTime() + nSleepTime;
    RPCRunLater(strprintf("lockwallet(%s)", pwallet->GetName()), boost::bind(LockWallet, pwallet), nSleepTime);

    return NullUniValue;
}


UniValue walletpassphrasechange(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 2) {
        throw std::runtime_error(
            "walletpassphrasechange \"oldpassphrase\" \"newpassphrase\"\n"
            "\nChanges the wallet passphrase from 'oldpassphrase' to 'newpassphrase'.\n"
            "\nArguments:\n"
            "1. \"oldpassphrase\"      (string) The current passphrase\n"
            "2. \"newpassphrase\"      (string) The new passphrase\n"
            "\nExamples:\n" +
            HelpExampleCli("walletpassphrasechange", "\"old one\" \"new one\"") + HelpExampleRpc("walletpassphrasechange", "\"old one\", \"new one\""));
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrasechange was called.");
    }

    // TODO: get rid of these .c_str() calls by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strOldWalletPass;
    strOldWalletPass.reserve(100);
    strOldWalletPass = request.params[0].get_str().c_str();

    SecureString strNewWalletPass;
    strNewWalletPass.reserve(100);
    strNewWalletPass = request.params[1].get_str().c_str();

    if (strOldWalletPass.length() < 1 || strNewWalletPass.length() < 1)
        throw std::runtime_error(
            "walletpassphrasechange <oldpassphrase> <newpassphrase>\n"
            "Changes the wallet passphrase from <oldpassphrase> to <newpassphrase>.");

    if (!pwallet->ChangeWalletPassphrase(strOldWalletPass, strNewWalletPass)) {
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
    }

    return NullUniValue;
}


UniValue walletlock(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 0) {
        throw std::runtime_error(
            "walletlock\n"
            "\nRemoves the wallet encryption key from memory, locking the wallet.\n"
            "After calling this method, you will need to call walletpassphrase again\n"
            "before being able to call any methods which require the wallet to be unlocked.\n"
            "\nExamples:\n"
            "\nSet the passphrase for 2 minutes to perform a transaction\n" +
            HelpExampleCli("walletpassphrase", "\"my pass phrase\" 120") +
            "\nPerform a send (requires passphrase set)\n" + HelpExampleCli("sendtoaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwG\" 1.0") +
            "\nClear the passphrase since we are done before 2 minutes is up\n" + HelpExampleCli("walletlock", "") +
            "\nAs json rpc call\n" + HelpExampleRpc("walletlock", ""));
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletlock was called.");
    }

    pwallet->Lock();
    pwallet->nRelockTime = 0;

    return NullUniValue;
}


UniValue encryptwallet(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "encryptwallet \"passphrase\"\n"
            "\nEncrypts the wallet with 'passphrase'. This is for first time encryption.\n"
            "After this, any calls that interact with private keys such as sending or signing \n"
            "will require the passphrase to be set prior the making these calls.\n"
            "Use the walletpassphrase call for this, and then walletlock call.\n"
            "If the wallet is already encrypted, use the walletpassphrasechange call.\n"
            "Note that this will shutdown the server.\n"
            "\nArguments:\n"
            "1. \"passphrase\"    (string) The pass phrase to encrypt the wallet with. It must be at least 1 character, but should be long.\n"
            "\nExamples:\n"
            "\nEncrypt your wallet\n" +
            HelpExampleCli("encryptwallet", "\"my pass phrase\"") +
            "\nNow set the passphrase to use the wallet, such as for signing or sending memeium\n" + HelpExampleCli("walletpassphrase", "\"my pass phrase\"") +
            "\nNow we can do something like sign\n" + HelpExampleCli("signmessage", "\"address\" \"test message\"") +
            "\nNow lock the wallet again by removing the passphrase\n" + HelpExampleCli("walletlock", "") +
            "\nAs a json rpc call\n" + HelpExampleRpc("encryptwallet", "\"my pass phrase\""));
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an encrypted wallet, but encryptwallet was called.");
    }

    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    strWalletPass = request.params[0].get_str().c_str();

    if (strWalletPass.length() < 1)
        throw std::runtime_error(
            "encryptwallet <passphrase>\n"
            "Encrypts the wallet with <passphrase>.");

    if (!pwallet->EncryptWallet(strWalletPass)) {
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: Failed to encrypt the wallet.");
    }

    // BDB seems to have a bad habit of writing old data into
    // slack space in .dat files; that is bad if the old data is
    // unencrypted private keys. So:
    StartShutdown();
    return "Wallet encrypted; Memeium Core server stopping, restart to run with encrypted wallet. The keypool has been flushed and a new HD seed was generated (if you are using HD). You need to make a new backup.";
}

UniValue lockunspent(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "lockunspent unlock ([{\"txid\":\"txid\",\"vout\":n},...])\n"
            "\nUpdates list of temporarily unspendable outputs.\n"
            "Temporarily lock (unlock=false) or unlock (unlock=true) specified transaction outputs.\n"
            "If no transaction outputs are specified when unlocking then all current locked transaction outputs are unlocked.\n"
            "A locked transaction output will not be chosen by automatic coin selection, when spending memeiums.\n"
            "Locks are stored in memory only. Nodes start with zero locked outputs, and the locked output list\n"
            "is always cleared (by virtue of process exit) when a node stops or fails.\n"
            "Also see the listunspent call\n"
            "\nArguments:\n"
            "1. unlock            (boolean, required) Whether to unlock (true) or lock (false) the specified transactions\n"
            "2. \"transactions\"  (string, optional) A json array of objects. Each object the txid (string) vout (numeric)\n"
            "     [           (json array of json objects)\n"
            "       {\n"
            "         \"txid\":\"id\",    (string) The transaction id\n"
            "         \"vout\": n         (numeric) The output number\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"

            "\nResult:\n"
            "true|false    (boolean) Whether the command was successful or not\n"

            "\nExamples:\n"
            "\nList the unspent transactions\n" +
            HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n" + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n" + HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n" + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a json rpc call\n" + HelpExampleRpc("lockunspent", "false, \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.params.size() == 1)
        RPCTypeCheck(request.params, {UniValue::VBOOL});
    else
        RPCTypeCheck(request.params, {UniValue::VBOOL, UniValue::VARR});

    bool fUnlock = request.params[0].get_bool();

    if (request.params.size() == 1) {
        if (fUnlock)
            pwallet->UnlockAllCoins();
        return true;
    }

    UniValue outputs = request.params[1].get_array();
    for (unsigned int idx = 0; idx < outputs.size(); idx++) {
        const UniValue& output = outputs[idx];
        if (!output.isObject())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected object");
        const UniValue& o = output.get_obj();

        RPCTypeCheckObj(o,
            {
                {"txid", UniValueType(UniValue::VSTR)},
                {"vout", UniValueType(UniValue::VNUM)},
            });

        std::string txid = find_value(o, "txid").get_str();
        if (!IsHex(txid))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected hex txid");

        int nOutput = find_value(o, "vout").get_int();
        if (nOutput < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");

        COutPoint outpt(uint256S(txid), nOutput);

        if (fUnlock)
            pwallet->UnlockCoin(outpt);
        else
            pwallet->LockCoin(outpt);
    }

    return true;
}

UniValue listlockunspent(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
            "listlockunspent\n"
            "\nReturns list of temporarily unspendable outputs.\n"
            "See the lockunspent call to lock and unlock transactions for spending.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"txid\" : \"transactionid\",     (string) The transaction id locked\n"
            "    \"vout\" : n                      (numeric) The vout value\n"
            "  }\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n"
            "\nList the unspent transactions\n" +
            HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n" + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n" + HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n" + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a json rpc call\n" + HelpExampleRpc("listlockunspent", ""));

    LOCK2(cs_main, pwallet->cs_wallet);

    std::vector<COutPoint> vOutpts;
    pwallet->ListLockedCoins(vOutpts);

    UniValue ret(UniValue::VARR);

    for (COutPoint& outpt : vOutpts) {
        UniValue o(UniValue::VOBJ);

        o.push_back(Pair("txid", outpt.hash.GetHex()));
        o.push_back(Pair("vout", (int)outpt.n));
        ret.push_back(o);
    }

    return ret;
}

UniValue settxfee(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 1)
        throw std::runtime_error(
            "settxfee amount\n"
            "\nSet the transaction fee per kB. Overwrites the paytxfee parameter.\n"
            "\nArguments:\n"
            "1. amount         (numeric or string, required) The transaction fee in " +
            CURRENCY_UNIT + "/kB\n"
                            "\nResult:\n"
                            "true|false        (boolean) Returns true if successful\n"
                            "\nExamples:\n" +
            HelpExampleCli("settxfee", "0.00001") + HelpExampleRpc("settxfee", "0.00001"));

    LOCK2(cs_main, pwallet->cs_wallet);

    // Amount
    CAmount nAmount = AmountFromValue(request.params[0]);

    payTxFee = CFeeRate(nAmount, 1000);
    return true;
}

UniValue setprivatesendrounds(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "setprivatesendrounds rounds\n"
            "\nSet the number of rounds for PrivateSend mixing.\n"
            "\nArguments:\n"
            "1. rounds         (numeric, required) The default number of rounds is " +
            std::to_string(DEFAULT_PRIVATESEND_ROUNDS) +
            " Cannot be more than " + std::to_string(MAX_PRIVATESEND_ROUNDS) + " nor less than " + std::to_string(MIN_PRIVATESEND_ROUNDS) +
            "\nExamples:\n" + HelpExampleCli("setprivatesendrounds", "4") + HelpExampleRpc("setprivatesendrounds", "16"));

    int nRounds = request.params[0].get_int();

    if (nRounds > MAX_PRIVATESEND_ROUNDS || nRounds < MIN_PRIVATESEND_ROUNDS)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid number of rounds");

    privateSendClient.nPrivateSendRounds = nRounds;

    return NullUniValue;
}

UniValue setprivatesendamount(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "setprivatesendamount amount\n"
            "\nSet the goal amount in " +
            CURRENCY_UNIT + " for PrivateSend mixing.\n"
                            "\nArguments:\n"
                            "1. amount         (numeric, required) The default amount is " +
            std::to_string(DEFAULT_PRIVATESEND_AMOUNT) +
            " Cannot be more than " + std::to_string(MAX_PRIVATESEND_AMOUNT) + " nor less than " + std::to_string(MIN_PRIVATESEND_AMOUNT) +
            "\nExamples:\n" + HelpExampleCli("setprivatesendamount", "500") + HelpExampleRpc("setprivatesendamount", "208"));

    int nAmount = request.params[0].get_int();

    if (nAmount > MAX_PRIVATESEND_AMOUNT || nAmount < MIN_PRIVATESEND_AMOUNT)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount of " + CURRENCY_UNIT + " as mixing goal amount");

    privateSendClient.nPrivateSendAmount = nAmount;

    return NullUniValue;
}

UniValue getwalletinfo(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getwalletinfo\n"
            "Returns an object containing various wallet state info.\n"
            "\nResult:\n"
            "{\n"
            "  \"walletname\": xxxxx,             (string) the wallet name\n"
            "  \"walletversion\": xxxxx,     (numeric) the wallet version\n"
            "  \"balance\": xxxxxxx,         (numeric) the total confirmed balance of the wallet in " +
            CURRENCY_UNIT + "\n"
                            "  \"privatesend_balance\": xxxxxx, (numeric) the PrivateSend balance in " +
            CURRENCY_UNIT + "\n"
                            "  \"unconfirmed_balance\": xxx, (numeric) the total unconfirmed balance of the wallet in " +
            CURRENCY_UNIT + "\n"
                            "  \"immature_balance\": xxxxxx, (numeric) the total immature balance of the wallet in " +
            CURRENCY_UNIT + "\n"
                            "  \"txcount\": xxxxxxx,         (numeric) the total number of transactions in the wallet\n"
                            "  \"keypoololdest\": xxxxxx,    (numeric) the timestamp (seconds since Unix epoch) of the oldest pre-generated key in the key pool\n"
                            "  \"keypoolsize\": xxxx,        (numeric) how many new keys are pre-generated (only counts external keys)\n"
                            "  \"keypoolsize_hd_internal\": xxxx, (numeric) how many new keys are pre-generated for internal use (used for change outputs, only appears if the wallet is using this feature, otherwise external keys are used)\n"
                            "  \"keys_left\": xxxx,          (numeric) how many new keys are left since last automatic backup\n"
                            "  \"unlocked_until\": ttt,      (numeric) the timestamp in seconds since epoch (midnight Jan 1 1970 GMT) that the wallet is unlocked for transfers, or 0 if the wallet is locked\n"
                            "  \"paytxfee\": x.xxxx,         (numeric) the transaction fee configuration, set in " +
            CURRENCY_UNIT + "/kB\n"
                            "  \"hdchainid\": \"<hash>\",      (string) the ID of the HD chain\n"
                            "  \"hdaccountcount\": xxx,      (numeric) how many accounts of the HD chain are in this wallet\n"
                            "    [\n"
                            "      {\n"
                            "      \"hdaccountindex\": xxx,         (numeric) the index of the account\n"
                            "      \"hdexternalkeyindex\": xxxx,    (numeric) current external childkey index\n"
                            "      \"hdinternalkeyindex\": xxxx,    (numeric) current internal childkey index\n"
                            "      }\n"
                            "      ,...\n"
                            "    ]\n"
                            "}\n"
                            "\nExamples:\n" +
            HelpExampleCli("getwalletinfo", "") + HelpExampleRpc("getwalletinfo", ""));

    LOCK2(cs_main, pwallet->cs_wallet);

    CHDChain hdChainCurrent;
    bool fHDEnabled = pwallet->GetHDChain(hdChainCurrent);
    UniValue obj(UniValue::VOBJ);

    obj.push_back(Pair("walletname", pwallet->GetName()));
    obj.push_back(Pair("walletversion", pwallet->GetVersion()));
    obj.push_back(Pair("balance", ValueFromAmount(pwallet->GetBalance())));
    obj.push_back(Pair("privatesend_balance", ValueFromAmount(pwallet->GetAnonymizedBalance())));
    obj.push_back(Pair("unconfirmed_balance", ValueFromAmount(pwallet->GetUnconfirmedBalance())));
    obj.push_back(Pair("immature_balance", ValueFromAmount(pwallet->GetImmatureBalance())));
    obj.push_back(Pair("txcount", (int)pwallet->mapWallet.size()));
    obj.push_back(Pair("keypoololdest", pwallet->GetOldestKeyPoolTime()));
    obj.push_back(Pair("keypoolsize", (int64_t)pwallet->KeypoolCountExternalKeys()));
    if (fHDEnabled) {
        obj.push_back(Pair("keypoolsize_hd_internal", (int64_t)(pwallet->KeypoolCountInternalKeys())));
    }
    obj.push_back(Pair("keys_left", pwallet->nKeysLeftSinceAutoBackup));
    if (pwallet->IsCrypted())
        obj.push_back(Pair("unlocked_until", pwallet->nRelockTime));
    obj.push_back(Pair("paytxfee", ValueFromAmount(payTxFee.GetFeePerK())));
    if (fHDEnabled) {
        obj.push_back(Pair("hdchainid", hdChainCurrent.GetID().GetHex()));
        obj.push_back(Pair("hdaccountcount", (int64_t)hdChainCurrent.CountAccounts()));
        UniValue accounts(UniValue::VARR);
        for (size_t i = 0; i < hdChainCurrent.CountAccounts(); ++i) {
            CHDAccount acc;
            UniValue account(UniValue::VOBJ);
            account.push_back(Pair("hdaccountindex", (int64_t)i));
            if (hdChainCurrent.GetAccount(i, acc)) {
                account.push_back(Pair("hdexternalkeyindex", (int64_t)acc.nExternalChainCounter));
                account.push_back(Pair("hdinternalkeyindex", (int64_t)acc.nInternalChainCounter));
            } else {
                account.push_back(Pair("error", strprintf("account %d is missing", i)));
            }
            accounts.push_back(account);
        }
        obj.push_back(Pair("hdaccounts", accounts));
    }
    return obj;
}

UniValue listwallets(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "listwallets\n"
            "Returns a list of currently loaded wallets.\n"
            "For full information on the wallet, use \"getwalletinfo\"\n"
            "\nResult:\n"
            "[                         (json array of strings)\n"
            "  \"walletname\"            (string) the wallet name\n"
            "   ...\n"
            "]\n"
            "\nExamples:\n" +
            HelpExampleCli("listwallets", "") + HelpExampleRpc("listwallets", ""));

    UniValue obj(UniValue::VARR);

    for (CWalletRef pwallet : vpwallets) {
        if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
            return NullUniValue;
        }

        LOCK(pwallet->cs_wallet);

        obj.push_back(pwallet->GetName());
    }

    return obj;
}

UniValue keepass(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    std::string strCommand;

    if (request.params.size() >= 1)
        strCommand = request.params[0].get_str();

    if (request.fHelp ||
        (strCommand != "genkey" && strCommand != "init" && strCommand != "setpassphrase"))
        throw std::runtime_error(
            "keepass <genkey|init|setpassphrase>\n");

    if (strCommand == "genkey") {
        SecureString sResult;
        // Generate RSA key
        SecureString sKey = CKeePassIntegrator::generateKeePassKey();
        sResult = "Generated Key: ";
        sResult += sKey;
        return sResult.c_str();
    } else if (strCommand == "init") {
        // Generate base64 encoded 256 bit RSA key and associate with KeePassHttp
        SecureString sResult;
        SecureString sKey;
        std::string strId;
        keePassInt.rpcAssociate(strId, sKey);
        sResult = "Association successful. Id: ";
        sResult += strId.c_str();
        sResult += " - Key: ";
        sResult += sKey.c_str();
        return sResult.c_str();
    } else if (strCommand == "setpassphrase") {
        if (request.params.size() != 2) {
            return "setlogin: invalid number of parameters. Requires a passphrase";
        }

        SecureString sPassphrase = SecureString(request.params[1].get_str().c_str());

        keePassInt.updatePassphrase(sPassphrase);

        return "setlogin: Updated credentials.";
    }

    return "Invalid command";
}

UniValue resendwallettransactions(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "resendwallettransactions\n"
            "Immediately re-broadcast unconfirmed wallet transactions to all peers.\n"
            "Intended only for testing; the wallet code periodically re-broadcasts\n"
            "automatically.\n"
            "Returns an RPC error if -walletbroadcast is set to false.\n"
            "Returns array of transaction ids that were re-broadcast.\n");

    if (!g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    LOCK2(cs_main, pwallet->cs_wallet);

    if (!pwallet->GetBroadcastTransactions()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet transaction broadcasting is disabled with -walletbroadcast");
    }

    std::vector<uint256> txids = pwallet->ResendWalletTransactionsBefore(GetTime(), g_connman.get());
    UniValue result(UniValue::VARR);
    for (const uint256& txid : txids) {
        result.push_back(txid.ToString());
    }
    return result;
}

UniValue listunspent(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 5)
        throw std::runtime_error(
            "listunspent ( minconf maxconf  [\"addresses\",...] [include_unsafe] [query_options])\n"
            "\nReturns array of unspent transaction outputs\n"
            "with between minconf and maxconf (inclusive) confirmations.\n"
            "Optionally filter to only include txouts paid to specified addresses.\n"
            "\nArguments:\n"
            "1. minconf          (numeric, optional, default=1) The minimum confirmations to filter\n"
            "2. maxconf          (numeric, optional, default=9999999) The maximum confirmations to filter\n"
            "3. \"addresses\"      (string) A json array of memeium addresses to filter\n"
            "    [\n"
            "      \"address\"     (string) memeium address\n"
            "      ,...\n"
            "    ]\n"
            "4. include_unsafe (bool, optional, default=true) Include outputs that are not safe to spend\n"
            "                  See description of \"safe\" attribute below.\n"
            "5. query_options    (json, optional) JSON with query options\n"
            "    {\n"
            "      \"minimumAmount\"    (numeric or string, default=0) Minimum value of each UTXO in " +
            CURRENCY_UNIT + "\n"
                            "      \"maximumAmount\"    (numeric or string, default=unlimited) Maximum value of each UTXO in " +
            CURRENCY_UNIT + "\n"
                            "      \"maximumCount\"     (numeric or string, default=unlimited) Maximum number of UTXOs\n"
                            "      \"minimumSumAmount\" (numeric or string, default=unlimited) Minimum sum value of all UTXOs in " +
            CURRENCY_UNIT + "\n"
                            "    }\n"
                            "\nResult\n"
                            "[                   (array of json object)\n"
                            "  {\n"
                            "    \"txid\" : \"txid\",          (string) the transaction id \n"
                            "    \"vout\" : n,               (numeric) the vout value\n"
                            "    \"address\" : \"address\",    (string) the memeium address\n"
                            "    \"account\" : \"account\",    (string) DEPRECATED. The associated account, or \"\" for the default account\n"
                            "    \"scriptPubKey\" : \"key\",   (string) the script key\n"
                            "    \"amount\" : x.xxx,         (numeric) the transaction output amount in " +
            CURRENCY_UNIT + "\n"
                            "    \"confirmations\" : n,      (numeric) The number of confirmations\n"
                            "    \"redeemScript\" : n        (string) The redeemScript if scriptPubKey is P2SH\n"
                            "    \"spendable\" : xxx,        (bool) Whether we have the private keys to spend this output\n"
                            "    \"solvable\" : xxx,         (bool) Whether we know how to spend this output, ignoring the lack of keys\n"
                            "    \"safe\" : xxx              (bool) Whether this output is considered safe to spend. Unconfirmed transactions\n"
                            "                              from outside keys and unconfirmed replacement transactions are considered unsafe\n"
                            "                              and are not eligible for spending by fundrawtransaction and sendtoaddress.\n"
                            "    \"ps_rounds\" : n           (numeric) The number of PS rounds\n"
                            "  }\n"
                            "  ,...\n"
                            "]\n"

                            "\nExamples:\n" +
            HelpExampleCli("listunspent", "") + HelpExampleCli("listunspent", "6 9999999 \"[\\\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\\\",\\\"XuQQkwA4FYkq2XERzMY2CiAZhJTEDAbtcg\\\"]\"") + HelpExampleRpc("listunspent", "6, 9999999 \"[\\\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\\\",\\\"XuQQkwA4FYkq2XERzMY2CiAZhJTEDAbtcg\\\"]\"") + HelpExampleCli("listunspent", "6 9999999 '[]' true '{ \"minimumAmount\": 0.005 }'") + HelpExampleRpc("listunspent", "6, 9999999, [] , true, { \"minimumAmount\": 0.005 } "));

    int nMinDepth = 1;
    if (request.params.size() > 0 && !request.params[0].isNull()) {
        RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
        nMinDepth = request.params[0].get_int();
    }

    int nMaxDepth = 9999999;
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        RPCTypeCheckArgument(request.params[1], UniValue::VNUM);
        nMaxDepth = request.params[1].get_int();
    }

    std::set<CBitcoinAddress> setAddress;
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        RPCTypeCheckArgument(request.params[2], UniValue::VARR);
        UniValue inputs = request.params[2].get_array();
        for (unsigned int idx = 0; idx < inputs.size(); idx++) {
            const UniValue& input = inputs[idx];
            CBitcoinAddress address(input.get_str());
            if (!address.IsValid())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Memeium address: ") + input.get_str());
            if (setAddress.count(address))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + input.get_str());
            setAddress.insert(address);
        }
    }

    bool include_unsafe = true;
    if (request.params.size() > 3 && !request.params[3].isNull()) {
        RPCTypeCheckArgument(request.params[3], UniValue::VBOOL);
        include_unsafe = request.params[3].get_bool();
    }

    CAmount nMinimumAmount = 0;
    CAmount nMaximumAmount = MAX_MONEY;
    CAmount nMinimumSumAmount = MAX_MONEY;
    uint64_t nMaximumCount = 0;

    if (!request.params[4].isNull()) {
        const UniValue& options = request.params[4].get_obj();

        if (options.exists("minimumAmount"))
            nMinimumAmount = AmountFromValue(options["minimumAmount"]);

        if (options.exists("maximumAmount"))
            nMaximumAmount = AmountFromValue(options["maximumAmount"]);

        if (options.exists("minimumSumAmount"))
            nMinimumSumAmount = AmountFromValue(options["minimumSumAmount"]);

        if (options.exists("maximumCount"))
            nMaximumCount = options["maximumCount"].get_int64();
    }

    UniValue results(UniValue::VARR);
    std::vector<COutput> vecOutputs;
    assert(pwallet != nullptr);
    LOCK2(cs_main, pwallet->cs_wallet);

    pwallet->AvailableCoins(vecOutputs, !include_unsafe, nullptr, nMinimumAmount, nMaximumAmount, nMinimumSumAmount, nMaximumCount, nMinDepth, nMaxDepth);
    for (const COutput& out : vecOutputs) {
        CTxDestination address;
        const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
        bool fValidAddress = ExtractDestination(scriptPubKey, address);

        if (setAddress.size() && (!fValidAddress || !setAddress.count(address)))
            continue;

        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("txid", out.tx->GetHash().GetHex()));
        entry.push_back(Pair("vout", out.i));

        if (fValidAddress) {
            entry.push_back(Pair("address", CBitcoinAddress(address).ToString()));

            if (pwallet->mapAddressBook.count(address)) {
                entry.push_back(Pair("account", pwallet->mapAddressBook[address].name));
            }

            if (scriptPubKey.IsPayToScriptHash()) {
                const CScriptID& hash = boost::get<CScriptID>(address);
                CScript redeemScript;
                if (pwallet->GetCScript(hash, redeemScript)) {
                    entry.push_back(Pair("redeemScript", HexStr(redeemScript.begin(), redeemScript.end())));
                }
            }
        }

        entry.push_back(Pair("scriptPubKey", HexStr(scriptPubKey.begin(), scriptPubKey.end())));
        entry.push_back(Pair("amount", ValueFromAmount(out.tx->tx->vout[out.i].nValue)));
        entry.push_back(Pair("confirmations", out.nDepth));
        entry.push_back(Pair("spendable", out.fSpendable));
        entry.push_back(Pair("solvable", out.fSolvable));
        entry.push_back(Pair("safe", out.fSafe));
        entry.push_back(Pair("ps_rounds", pwallet->GetCappedOutpointPrivateSendRounds(COutPoint(out.tx->GetHash(), out.i))));
        results.push_back(entry);
    }

    return results;
}

UniValue fundrawtransaction(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "fundrawtransaction \"hexstring\" ( options )\n"
            "\nAdd inputs to a transaction until it has enough in value to meet its out value.\n"
            "This will not modify existing inputs, and will add at most one change output to the outputs.\n"
            "No existing outputs will be modified unless \"subtractFeeFromOutputs\" is specified.\n"
            "Note that inputs which were signed may need to be resigned after completion since in/outputs have been added.\n"
            "The inputs added will not be signed, use signrawtransaction for that.\n"
            "Note that all existing inputs must have their previous output transaction be in the wallet.\n"
            "Note that all inputs selected must be of standard form and P2SH scripts must be\n"
            "in the wallet using importaddress or addmultisigaddress (to calculate fees).\n"
            "You can see whether this is the case by checking the \"solvable\" field in the listunspent output.\n"
            "Only pay-to-pubkey, multisig, and P2SH versions thereof are currently supported for watch-only\n"
            "\nArguments:\n"
            "1. \"hexstring\"           (string, required) The hex string of the raw transaction\n"
            "2. options                 (object, optional)\n"
            "   {\n"
            "     \"changeAddress\"          (string, optional, default pool address) The memeium address to receive the change\n"
            "     \"changePosition\"         (numeric, optional, default random) The index of the change output\n"
            "     \"includeWatching\"        (boolean, optional, default false) Also select inputs which are watch only\n"
            "     \"lockUnspents\"           (boolean, optional, default false) Lock selected unspent outputs\n"
            "     \"feeRate\"                (numeric, optional, default not set: makes wallet determine the fee) Set a specific feerate (" +
            CURRENCY_UNIT + " per KB)\n"
                            "     \"subtractFeeFromOutputs\" (array, optional) A json array of integers.\n"
                            "                              The fee will be equally deducted from the amount of each specified output.\n"
                            "                              The outputs are specified by their zero-based index, before any change output is added.\n"
                            "                              Those recipients will receive less memeium than you enter in their corresponding amount field.\n"
                            "                              If no outputs are specified here, the sender pays the fee.\n"
                            "                                  [vout_index,...]\n"
                            "     \"conf_target\"            (numeric, optional) Confirmation target (in blocks)\n"
                            "     \"estimate_mode\"          (string, optional, default=UNSET) The fee estimate mode, must be one of:\n"
                            "         \"UNSET\"\n"
                            "         \"ECONOMICAL\"\n"
                            "         \"CONSERVATIVE\"\n"
                            "   }\n"
                            "                         for backward compatibility: passing in a true instead of an object will result in {\"includeWatching\":true}\n"
                            "\nResult:\n"
                            "{\n"
                            "  \"hex\":       \"value\", (string)  The resulting raw transaction (hex-encoded string)\n"
                            "  \"fee\":       n,         (numeric) Fee in " +
            CURRENCY_UNIT + " the resulting transaction pays\n"
                            "  \"changepos\": n          (numeric) The position of the added change output, or -1\n"
                            "}\n"
                            "\nExamples:\n"
                            "\nCreate a transaction with no inputs\n" +
            HelpExampleCli("createrawtransaction", "\"[]\" \"{\\\"myaddress\\\":0.01}\"") +
            "\nAdd sufficient unsigned inputs to meet the output value\n" + HelpExampleCli("fundrawtransaction", "\"rawtransactionhex\"") +
            "\nSign the transaction\n" + HelpExampleCli("signrawtransaction", "\"fundedtransactionhex\"") +
            "\nSend the transaction\n" + HelpExampleCli("sendrawtransaction", "\"signedtransactionhex\""));

    RPCTypeCheck(request.params, {UniValue::VSTR});

    CCoinControl coinControl;
    int changePosition = -1;
    bool lockUnspents = false;
    UniValue subtractFeeFromOutputs;
    std::set<int> setSubtractFeeFromOutputs;

    if (!request.params[1].isNull()) {
        if (request.params[1].type() == UniValue::VBOOL) {
            // backward compatibility bool only fallback
            coinControl.fAllowWatchOnly = request.params[1].get_bool();
        } else {
            RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VOBJ});

            UniValue options = request.params[1];

            RPCTypeCheckObj(options,
                {
                    {"changeAddress", UniValueType(UniValue::VSTR)},
                    {"changePosition", UniValueType(UniValue::VNUM)},
                    {"includeWatching", UniValueType(UniValue::VBOOL)},
                    {"lockUnspents", UniValueType(UniValue::VBOOL)},
                    {"reserveChangeKey", UniValueType(UniValue::VBOOL)}, // DEPRECATED (and ignored), should be removed in 0.16 or so.
                    {"feeRate", UniValueType()},                         // will be checked below
                    {"subtractFeeFromOutputs", UniValueType(UniValue::VARR)},
                    {"conf_target", UniValueType(UniValue::VNUM)},
                    {"estimate_mode", UniValueType(UniValue::VSTR)},
                },
                true, true);

            if (options.exists("changeAddress")) {
                CBitcoinAddress address(options["changeAddress"].get_str());

                if (!address.IsValid())
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "changeAddress must be a valid memeium address");

                coinControl.destChange = address.Get();
            }

            if (options.exists("changePosition"))
                changePosition = options["changePosition"].get_int();

            if (options.exists("includeWatching"))
                coinControl.fAllowWatchOnly = options["includeWatching"].get_bool();

            if (options.exists("lockUnspents"))
                lockUnspents = options["lockUnspents"].get_bool();

            if (options.exists("feeRate")) {
                coinControl.m_feerate = CFeeRate(AmountFromValue(options["feeRate"]));
                coinControl.fOverrideFeeRate = true;
            }

            if (options.exists("subtractFeeFromOutputs"))
                subtractFeeFromOutputs = options["subtractFeeFromOutputs"].get_array();
            if (options.exists("conf_target")) {
                if (options.exists("feeRate")) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both conf_target and feeRate");
                }
                coinControl.m_confirm_target = ParseConfirmTarget(options["conf_target"]);
            }
            if (options.exists("estimate_mode")) {
                if (options.exists("feeRate")) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both estimate_mode and feeRate");
                }
                if (!FeeModeFromString(options["estimate_mode"].get_str(), coinControl.m_fee_mode)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
                }
            }
        }
    }

    // parse hex string from parameter
    CMutableTransaction tx;
    if (!DecodeHexTx(tx, request.params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");

    if (tx.vout.size() == 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "TX must have at least one output");

    if (changePosition != -1 && (changePosition < 0 || (unsigned int)changePosition > tx.vout.size()))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "changePosition out of bounds");

    for (unsigned int idx = 0; idx < subtractFeeFromOutputs.size(); idx++) {
        int pos = subtractFeeFromOutputs[idx].get_int();
        if (setSubtractFeeFromOutputs.count(pos))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, duplicated position: %d", pos));
        if (pos < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, negative position: %d", pos));
        if (pos >= int(tx.vout.size()))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, position too large: %d", pos));
        setSubtractFeeFromOutputs.insert(pos);
    }

    CAmount nFeeOut;
    std::string strFailReason;

    if (!pwallet->FundTransaction(tx, nFeeOut, changePosition, strFailReason, lockUnspents, setSubtractFeeFromOutputs, coinControl)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strFailReason);
    }

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hex", EncodeHexTx(tx)));
    result.push_back(Pair("changepos", changePosition));
    result.push_back(Pair("fee", ValueFromAmount(nFeeOut)));

    return result;
}

#if ENABLE_MINER
UniValue generate(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2) {
        throw std::runtime_error(
            "generate nblocks ( maxtries )\n"
            "\nMine up to nblocks blocks immediately (before the RPC call returns) to an address in the wallet.\n"
            "\nArguments:\n"
            "1. nblocks      (numeric, required) How many blocks are generated immediately.\n"
            "2. maxtries     (numeric, optional) How many iterations to try (default = 1000000).\n"
            "\nResult:\n"
            "[ blockhashes ]     (array) hashes of blocks generated\n"
            "\nExamples:\n"
            "\nGenerate 11 blocks\n" +
            HelpExampleCli("generate", "11"));
    }

    int num_generate = request.params[0].get_int();
    uint64_t max_tries = 1000000;
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        max_tries = request.params[1].get_int();
    }

    std::shared_ptr<CReserveScript> coinbase_script;
    pwallet->GetScriptForMining(coinbase_script);

    // If the keypool is exhausted, no script is returned at all.  Catch this.
    if (!coinbase_script) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    }

    // throw an error if no script was provided
    if (coinbase_script->reserveScript.empty()) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No coinbase script available");
    }

    return generateBlocks(coinbase_script, num_generate, max_tries, true);
}
#endif // ENABLE_MINING

extern UniValue abortrescan(const JSONRPCRequest& request); // in rpcdump.cpp
extern UniValue dumpprivkey(const JSONRPCRequest& request); // in rpcdump.cpp
extern UniValue importprivkey(const JSONRPCRequest& request);
extern UniValue importaddress(const JSONRPCRequest& request);
extern UniValue importpubkey(const JSONRPCRequest& request);
extern UniValue dumpwallet(const JSONRPCRequest& request);
extern UniValue importwallet(const JSONRPCRequest& request);
extern UniValue importprunedfunds(const JSONRPCRequest& request);
extern UniValue removeprunedfunds(const JSONRPCRequest& request);
extern UniValue importmulti(const JSONRPCRequest& request);

extern UniValue dumphdinfo(const JSONRPCRequest& request);
extern UniValue importelectrumwallet(const JSONRPCRequest& request);

static const CRPCCommand commands[] =
    {
        //  category              name                        actor (function)           okSafeMode
        //  --------------------- ------------------------    -----------------------    ----------
        {"rawtransactions", "fundrawtransaction", &fundrawtransaction, false, {"hexstring", "options"}},
        {"hidden", "resendwallettransactions", &resendwallettransactions, true, {}},
        {"wallet", "abandontransaction", &abandontransaction, false, {"txid"}},
        {"wallet", "abortrescan", &abortrescan, false, {}},
        {"wallet", "addmultisigaddress", &addmultisigaddress, true, {"nrequired", "keys", "account"}},
        {"wallet", "backupwallet", &backupwallet, true, {"destination"}},
        {"wallet", "dumpprivkey", &dumpprivkey, true, {"address"}},
        {"wallet", "dumpwallet", &dumpwallet, true, {"filename"}},
        {"wallet", "encryptwallet", &encryptwallet, true, {"passphrase"}},
        {"wallet", "getaccountaddress", &getaccountaddress, true, {"account"}},
        {"wallet", "getaccount", &getaccount, true, {"address"}},
        {"wallet", "getaddressesbyaccount", &getaddressesbyaccount, true, {"account"}},
        {"wallet", "getbalance", &getbalance, false, {"account", "minconf", "addlocked", "include_watchonly"}},
        {"wallet", "getnewaddress", &getnewaddress, true, {"account"}},
        {"wallet", "getrawchangeaddress", &getrawchangeaddress, true, {}},
        {"wallet", "getreceivedbyaccount", &getreceivedbyaccount, false, {"account", "minconf", "addlocked"}},
        {"wallet", "getreceivedbyaddress", &getreceivedbyaddress, false, {"address", "minconf", "addlocked"}},
        {"wallet", "gettransaction", &gettransaction, false, {"txid", "include_watchonly"}},
        {"wallet", "getunconfirmedbalance", &getunconfirmedbalance, false, {}},
        {"wallet", "getwalletinfo", &getwalletinfo, false, {}},
        {"wallet", "importmulti", &importmulti, true, {"requests", "options"}},
        {"wallet", "importprivkey", &importprivkey, true, {"privkey", "label", "rescan"}},
        {"wallet", "importwallet", &importwallet, true, {"filename"}},
        {"wallet", "importaddress", &importaddress, true, {"address", "label", "rescan", "p2sh"}},
        {"wallet", "importprunedfunds", &importprunedfunds, true, {"rawtransaction", "txoutproof"}},
        {"wallet", "importpubkey", &importpubkey, true, {"pubkey", "label", "rescan"}},
        {"wallet", "keypoolrefill", &keypoolrefill, true, {"newsize"}},
        {"wallet", "listaccounts", &listaccounts, false, {"minconf", "addlocked", "include_watchonly"}},
        {"wallet", "listaddressgroupings", &listaddressgroupings, false, {}},
        {"wallet", "listaddressbalances", &listaddressbalances, false, {"minamount"}},
        {"wallet", "listlockunspent", &listlockunspent, false, {}},
        {"wallet", "listreceivedbyaccount", &listreceivedbyaccount, false, {"minconf", "addlocked", "include_empty", "include_watchonly"}},
        {"wallet", "listreceivedbyaddress", &listreceivedbyaddress, false, {"minconf", "addlocked", "include_empty", "include_watchonly"}},
        {"wallet", "listsinceblock", &listsinceblock, false, {"blockhash", "target_confirmations", "include_watchonly", "include_removed"}},
        {"wallet", "listtransactions", &listtransactions, false, {"account", "count", "skip", "include_watchonly"}},
        {"wallet", "listunspent", &listunspent, false, {"minconf", "maxconf", "addresses", "include_unsafe", "query_options"}},
        {"wallet", "listwallets", &listwallets, true, {}},
        {"wallet", "lockunspent", &lockunspent, true, {"unlock", "transactions"}},
        {"wallet", "move", &movecmd, false, {"fromaccount", "toaccount", "amount", "minconf", "comment"}},
        {"wallet", "sendfrom", &sendfrom, false, {"fromaccount", "toaddress", "amount", "minconf", "addlocked", "comment", "comment_to"}},
        {"wallet", "sendmany", &sendmany, false, {"fromaccount", "amounts", "minconf", "addlocked", "comment", "subtractfeefrom", "use_ps", "conf_target", "estimate_mode"}},
        {"wallet", "sendtoaddress", &sendtoaddress, false, {"address", "amount", "comment", "comment_to", "subtractfeefromamount", "use_ps", "conf_target", "estimate_mode"}},
        {"wallet", "setaccount", &setaccount, true, {"address", "account"}},
        {"wallet", "settxfee", &settxfee, true, {"amount"}},
        {"wallet", "setprivatesendrounds", &setprivatesendrounds, true, {"rounds"}},
        {"wallet", "setprivatesendamount", &setprivatesendamount, true, {"amount"}},
        {"wallet", "signmessage", &signmessage, true, {"address", "message"}},
        {"wallet", "walletlock", &walletlock, true, {}},
        {"wallet", "walletpassphrasechange", &walletpassphrasechange, true, {"oldpassphrase", "newpassphrase"}},
        {"wallet", "walletpassphrase", &walletpassphrase, true, {"passphrase", "timeout", "mixingonly"}},
        {"wallet", "removeprunedfunds", &removeprunedfunds, true, {"txid"}},

#if ENABLE_MINER
        {"generating", "generate", &generate, true, {"nblocks", "maxtries"}},
#endif // ENABLE_MINER
        {"wallet", "keepass", &keepass, true, {}},
        {"hidden", "instantsendtoaddress", &instantsendtoaddress, false, {"address", "amount", "comment", "comment_to", "subtractfeefromamount"}},
        {"wallet", "dumphdinfo", &dumphdinfo, true, {}},
        {"wallet", "importelectrumwallet", &importelectrumwallet, true, {"filename", "index"}},
};

void RegisterWalletRPCCommands(CRPCTable& t)
{
    if (gArgs.GetBoolArg("-disablewallet", false))
        return;

    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
