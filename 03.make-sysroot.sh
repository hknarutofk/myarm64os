#!/bin/bash
set -x
workdir=$(pwd)
mkdir sysroot
rsync -avr -P res/* sysroot/
rsync -av -P src/linux-4.19.90-2112.8.0.0131.oe1.aarch64/arch/arm64/boot/Image.gz sysroot/boot/vmlinuz-4.19.90
rsync -av -P src/linux-4.19.90-2112.8.0.0131.oe1.aarch64/System.map sysroot/boot/System.map-4.19.90
rsync -av -P src/linux-4.19.90-2112.8.0.0131.oe1.aarch64/initramfs.img-4.19.90 sysroot/boot/initramfs.img-4.19.90
rsync -avr src/busybox-1.32.1/_install/* sysroot/
(
	cd sysroot
	mkdir home mnt opt var media -p
	mkdir proc sys dev run -p
	(
		cd dev
		mknod console c 5 1
		mknod null c 1 3
	)
	ln -svf bin/busybox init
	ln -svf usr/lib lib
	ln -svf usr/lib64 lib64
)

