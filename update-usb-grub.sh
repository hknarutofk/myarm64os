#!/bin/bash
set -x

# DF716 FT2000/4 麒麟桌面系统测试通过 
echo "危险，注意检查目标USB设备，此处为/dev/sdb, ctrl+C 终止此程序"
read -p "输入 yes 继续：" param
if [ "${param}" -eq "yes" ];
do
    EFI_DIR=/media/greatwall/2ACD-883D
    sudo mkdir ${EFI_DIR}/boot -p
    sudo /usr/local/sbin/grub-install --efi-directory=${EFI_DIR} --boot-directory=${EFI_DIR}/boot /dev/sdb --removable
done
