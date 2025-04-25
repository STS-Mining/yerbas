# Shared Libraries

## memeiumconsensus

The purpose of this library is to make the verification functionality that is critical to Memeium's consensus available to other applications, e.g. to language bindings.

### API

The interface is defined in the C header `memeiumconsensus.h` located in `src/script/memeiumconsensus.h`.

#### Version

`memeiumconsensus_version` returns an `unsigned int` with the API version _(currently at an experimental `0`)_.

#### Script Validation

`memeiumconsensus_verify_script` returns an `int` with the status of the verification. It will be `1` if the input script correctly spends the previous output `scriptPubKey`.

##### Parameters

- `const unsigned char *scriptPubKey` - The previous output script that encumbers spending.
- `unsigned int scriptPubKeyLen` - The number of bytes for the `scriptPubKey`.
- `const unsigned char *txTo` - The transaction with the input that is spending the previous output.
- `unsigned int txToLen` - The number of bytes for the `txTo`.
- `unsigned int nIn` - The index of the input in `txTo` that spends the `scriptPubKey`.
- `unsigned int flags` - The script validation flags _(see below)_.
- `memeiumconsensus_error* err` - Will have the error/success code for the operation _(see below)_.

##### Script Flags

- `memeiumconsensus_SCRIPT_FLAGS_VERIFY_NONE`
- `memeiumconsensus_SCRIPT_FLAGS_VERIFY_P2SH` - Evaluate P2SH ([BIP16](https://github.com/bitcoin/bips/blob/master/bip-0016.mediawiki)) subscripts
- `memeiumconsensus_SCRIPT_FLAGS_VERIFY_DERSIG` - Enforce strict DER ([BIP66](https://github.com/bitcoin/bips/blob/master/bip-0066.mediawiki)) compliance
- `memeiumconsensus_SCRIPT_FLAGS_VERIFY_NULLDUMMY` - Enforce NULLDUMMY ([BIP147](https://github.com/bitcoin/bips/blob/master/bip-0147.mediawiki))
- `memeiumconsensus_SCRIPT_FLAGS_VERIFY_CHECKLOCKTIMEVERIFY` - Enable CHECKLOCKTIMEVERIFY ([BIP65](https://github.com/bitcoin/bips/blob/master/bip-0065.mediawiki))
- `memeiumconsensus_SCRIPT_FLAGS_VERIFY_CHECKSEQUENCEVERIFY` - Enable CHECKSEQUENCEVERIFY ([BIP112](https://github.com/bitcoin/bips/blob/master/bip-0112.mediawiki))

##### Errors

- `memeiumconsensus_ERR_OK` - No errors with input parameters _(see the return value of `memeiumconsensus_verify_script` for the verification status)_
- `memeiumconsensus_ERR_TX_INDEX` - An invalid index for `txTo`
- `memeiumconsensus_ERR_TX_SIZE_MISMATCH` - `txToLen` did not match with the size of `txTo`
- `memeiumconsensus_ERR_DESERIALIZE` - An error deserializing `txTo`

### Example Implementations

- [NBitcoin](https://github.com/NicolasDorier/NBitcoin/blob/master/NBitcoin/Script.cs#L814) (.NET Bindings)
- [node-libbitcoinconsensus](https://github.com/bitpay/node-libbitcoinconsensus) (Node.js Bindings)
- [java-libbitcoinconsensus](https://github.com/dexX7/java-libbitcoinconsensus) (Java Bindings)
- [bitcoinconsensus-php](https://github.com/Bit-Wasp/bitcoinconsensus-php) (PHP Bindings)
