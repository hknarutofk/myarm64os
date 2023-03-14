#!/bin/bash
set -x
rsync -avr -P res/boot/config-busybox-1.32.1 src/busybox-1.32.1/.config
(
	cd src/busybox-1.32.1/
	make -j64
	make install
)
