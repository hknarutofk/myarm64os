#!/bin/bash
# DF716 FT2000/4 麒麟桌面系统测试通过
(
	cd src/grub-2.06/
	./configure --prefix=/usr/local
	make -j4
	sudo make install
)
