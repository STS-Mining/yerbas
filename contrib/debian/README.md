# Debian

This directory contains files used to package memeiumd/memeium-qt
for Debian-based Linux systems. If you compile memeiumd/memeium-qt yourself, there are some useful files here.

## memeium: URI support

memeium-qt.desktop (Gnome / Open Desktop)
To install:

    sudo desktop-file-install memeium-qt.desktop
    sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your memeium-qt binary to `/usr/bin`
and the `../../share/pixmaps/memeium128.png` to `/usr/share/pixmaps`

memeium-qt.protocol (KDE)
