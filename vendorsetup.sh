# Kernel must be built with clang-r510928, newer clang builds a kernel that hangs at boot
CUSCOI_KERNEL_CLANG=prebuilts/clang/host/linux-x86/clang-r510928
if [ ! -x "$CUSCOI_KERNEL_CLANG/bin/clang" ]; then
    echo "sm7435-common: fetching clang-r510928"
    mkdir -p "$CUSCOI_KERNEL_CLANG"
    curl -fL --retry 3 -o "$CUSCOI_KERNEL_CLANG.tar.gz" \
        "https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/tags/android-15.0.0_r1/clang-r510928.tar.gz" &&
        tar xzf "$CUSCOI_KERNEL_CLANG.tar.gz" -C "$CUSCOI_KERNEL_CLANG" &&
        rm -f "$CUSCOI_KERNEL_CLANG.tar.gz"
fi
unset CUSCOI_KERNEL_CLANG
