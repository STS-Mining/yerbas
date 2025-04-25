#!/bin/bash
# use testnet settings,  if you need mainnet,  use ~/.memeiumcore/memeiumd.pid file instead
memeium_pid=$(<~/.memeiumcore/testnet3/memeiumd.pid)
sudo gdb -batch -ex "source debug.gdb" memeiumd ${memeium_pid}
