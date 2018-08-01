cur_path=`dirname $0`
source ${cur_path}/../envsetup.sh

export ARCH=arm64
make mrproper
make mvebu_v8_lsp_defconfig
make -j20
unset ARCH
