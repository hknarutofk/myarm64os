#!/bin/bash
set -x
cp res/boot/config-4.19.90-2112.8.0.0131.oe1.aarch64 src/linux-4.19.90-2112.8.0.0131.oe1.aarch64/.config -fv
(
	cd src/linux-4.19.90-2112.8.0.0131.oe1.aarch64/
#	make clean
	make -j64
	make modules_install -j64
	mkinitrd -v initramfs-4.19.90.img 4.19.90 --force
)
