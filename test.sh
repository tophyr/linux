#!/bin/bash
g++ -O0 -g3 -Werror -pthread -static -Iproxy_user/include -std=c++23 proxy_user/main.cpp -Lproxy_user -lgtest -o RAMFS/bin/pu &&
pushd RAMFS/ &&
find . | sudo cpio -oHnewc | gzip > ../initramfs.gz &&
popd &&
make -j96 &&
qemu-system-x86_64 -kernel arch/x86/boot/bzImage -initrd initramfs.gz -nographic -append "console=ttyS0" -m 512 --enable-kvm -cpu host -s
