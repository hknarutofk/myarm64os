#!/bin/bash
workdir=$(pwd)
mkdir sysroot
(
	cd sysroot
	tar -xvf ${workdir}/busybox-install.tar.gz
	mkdir home mnt opt var media -p
	mkdir proc sys dev run
	(
		cd dev
		mknod console c 5 1
		mknod null c 1 3
	)
	cp ${workdir}/res/* . -rvf
	rm linuxrc
	ln -sv bin/busybox init
	ln -svf usr/lib lib
	ln -svf usr/lib64 lib64
	
)

