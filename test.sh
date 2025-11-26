#!/bin/bash
g++ -O0 -g3 -Werror -pthread -static -Iproxy_user/include -std=c++23 proxy_user/main.cpp ../googletest/build/lib/libgtest.a -o RAMFS/bin/pu &&
pushd RAMFS/ &&
chmod 600 root/private_data &&
rm -rf dev/ &&
mkdir dev/ &&
sudo mknod dev/proxy c 242 0 &&
find . | sudo cpio -oHnewc | gzip > ../initramfs.gz &&
popd &&
cp test_config .config &&
make -j$(nproc) &&
qemu-system-x86_64 -kernel arch/x86/boot/bzImage -initrd initramfs.gz -nographic -append "console=ttyS0" -m 512 --enable-kvm -cpu host -s
