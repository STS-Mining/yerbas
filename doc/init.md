# Sample init scripts and service configuration for memeiumd

Sample scripts and configuration files for systemd, Upstart and OpenRC
can be found in the contrib/init folder.

    contrib/init/memeiumd.service:    systemd service unit configuration
    contrib/init/memeiumd.openrc:     OpenRC compatible SysV style init script
    contrib/init/memeiumd.openrcconf: OpenRC conf.d file
    contrib/init/memeiumd.conf:       Upstart service configuration file
    contrib/init/memeiumd.init:       CentOS compatible SysV style init script

## Service User

All three Linux startup configurations assume the existence of a "memeiumcore" user
and group. They must be created before attempting to use these scripts.
The OS X configuration assumes memeiumd will be set up for the current user.

## Configuration

At a bare minimum, memeiumd requires that the rpcpassword setting be set
when running as a daemon. If the configuration file does not exist or this
setting is not set, memeiumd will shutdown promptly after startup.

This password does not have to be remembered or typed as it is mostly used
as a fixed token that memeiumd and client programs read from the configuration
file, however it is recommended that a strong and secure password be used
as this password is security critical to securing the wallet should the
wallet be enabled.

If memeiumd is run with the "-server" flag (set by default), and no rpcpassword is set,
it will use a special cookie file for authentication. The cookie is generated with random
content when the daemon starts, and deleted when it exits. Read access to this file
controls who can access it through RPC.

By default the cookie is stored in the data directory, but it's location can be overridden
with the option '-rpccookiefile'.

This allows for running memeiumd without having to do any manual configuration.

`conf`, `pid`, and `wallet` accept relative paths which are interpreted as
relative to the data directory. `wallet` _only_ supports relative paths.

For an example configuration file that describes the configuration settings,
see `contrib/debian/examples/memeium.conf`.

## Paths

### Linux

All three configurations assume several paths that might need to be adjusted.

Binary: `/usr/bin/memeiumd`  
Configuration file: `/etc/memeiumcore/memeium.conf`  
Data directory: `/var/lib/memeiumd`  
PID file: `/var/run/memeiumd/memeiumd.pid` (OpenRC and Upstart) or `/var/lib/memeiumd/memeiumd.pid` (systemd)  
Lock file: `/var/lock/subsys/memeiumd` (CentOS)

The configuration file, PID directory (if applicable) and data directory
should all be owned by the memeiumcore user and group. It is advised for security
reasons to make the configuration file and data directory only readable by the
memeiumcore user and group. Access to memeium-cli and other memeiumd rpc clients
can then be controlled by group membership.

### Mac OS X

Binary: `/usr/local/bin/memeiumd`  
Configuration file: `~/Library/Application Support/MemeiumCore/memeium.conf`  
Data directory: `~/Library/Application Support/MemeiumCore`
Lock file: `~/Library/Application Support/MemeiumCore/.lock`

## Installing Service Configuration

### systemd

Installing this .service file consists of just copying it to
/usr/lib/systemd/system directory, followed by the command
`systemctl daemon-reload` in order to update running systemd configuration.

To test, run `systemctl start memeiumd` and to enable for system startup run
`systemctl enable memeiumd`

### OpenRC

Rename memeiumd.openrc to memeiumd and drop it in /etc/init.d. Double
check ownership and permissions and make it executable. Test it with
`/etc/init.d/memeiumd start` and configure it to run on startup with
`rc-update add memeiumd`

### Upstart (for Debian/Ubuntu based distributions)

Drop memeiumd.conf in /etc/init. Test by running `service memeiumd start`
it will automatically start on reboot.

NOTE: This script is incompatible with CentOS 5 and Amazon Linux 2014 as they
use old versions of Upstart and do not supply the start-stop-daemon utility.

### CentOS

Copy memeiumd.init to /etc/init.d/memeiumd. Test by running `service memeiumd start`.

Using this script, you can adjust the path and flags to the memeiumd program by
setting the MEMEIUMD and FLAGS environment variables in the file
/etc/sysconfig/memeiumd. You can also use the DAEMONOPTS environment variable here.

### Mac OS X

Copy org.memeium.memeiumd.plist into ~/Library/LaunchAgents. Load the launch agent by
running `launchctl load ~/Library/LaunchAgents/org.memeium.memeiumd.plist`.

This Launch Agent will cause memeiumd to start whenever the user logs in.

NOTE: This approach is intended for those wanting to run memeiumd as the current user.
You will need to modify org.memeium.memeiumd.plist if you intend to use it as a
Launch Daemon with a dedicated memeiumcore user.

## Auto-respawn

Auto respawning is currently only configured for Upstart and systemd.
Reasonable defaults have been chosen but YMMV.
