#!/bin/bash
# XSL: download and build the Lua interpreter and BusyBox as static
# riscv64 Linux programs, the normal way, with Ubuntu's Linux cross
# compiler. nothing in their source is changed. the results go to
# linux/lua and linux/busybox, and make puts them on the xv6 disk.
#
#   cd xv6-riscv && bash linux/ports.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=${WORK:-$HOME/xsl-ports}
mkdir -p "$work" && cd "$work"

# Lua 5.4: "make posix" is the plain POSIX build (no dlopen)
[ -f lua-5.4.7.tar.gz ] || curl -sSLO https://www.lua.org/ftp/lua-5.4.7.tar.gz
[ -d lua-5.4.7 ] || tar xzf lua-5.4.7.tar.gz
make -C lua-5.4.7/src clean >/dev/null
make -C lua-5.4.7/src posix CC="riscv64-linux-gnu-gcc -std=gnu99" \
  AR="riscv64-linux-gnu-ar rcu" RANLIB=riscv64-linux-gnu-ranlib MYLDFLAGS=-static
cp lua-5.4.7/src/lua "$here/lua"

# BusyBox 1.36.1: its default config (defconfig) with these changes:
#   CONFIG_STATIC=y                   no dynamic loader on xv6
#   CONFIG_TC off                     does not build with kernel headers >= 6.8
#   CONFIG_FEATURE_EDITING off        xv6's console has no raw mode, read lines
#   CONFIG_ASH_JOB_CONTROL off        needs /dev/tty and process groups
#   CONFIG_FEATURE_SH_STANDALONE=y    the shell runs applets itself ...
#   CONFIG_FEATURE_PREFER_APPLETS=y
#   CONFIG_BUSYBOX_EXEC_PATH=/busybox ... re-executing /busybox, since
#                                     xv6 has no /proc/self/exe
[ -f busybox-1.36.1.tar.bz2 ] || curl -sSLO https://busybox.net/downloads/busybox-1.36.1.tar.bz2
[ -d busybox-1.36.1 ] || tar xjf busybox-1.36.1.tar.bz2
cd busybox-1.36.1
make CROSS_COMPILE=riscv64-linux-gnu- defconfig > /dev/null
on()  { sed -i "s/^# $1 is not set/$1=y/; s/^$1=.*/$1=y/" .config; }
off() { sed -i "s/^$1=y/# $1 is not set/" .config; }
on  CONFIG_STATIC
off CONFIG_TC
off CONFIG_FEATURE_EDITING
off CONFIG_ASH_JOB_CONTROL
on  CONFIG_FEATURE_SH_STANDALONE
on  CONFIG_FEATURE_PREFER_APPLETS
sed -i 's|^CONFIG_BUSYBOX_EXEC_PATH=.*|CONFIG_BUSYBOX_EXEC_PATH="/busybox"|' .config
yes "" | make CROSS_COMPILE=riscv64-linux-gnu- oldconfig > /dev/null
make CROSS_COMPILE=riscv64-linux-gnu- -j"$(nproc)"
cp busybox "$here/busybox"
cd ..

ls -l "$here/lua" "$here/busybox"
