#!/usr/bin/env bash
# =============================================================================
#  构建 7zpack.dll
#
#  依赖：
#    x86_64-w64-mingw32-g++ / -gcc   （w64devkit 或 MSYS2 均可）
#    curl、tar、python3
#
#  用法：
#    ./build.sh                                    # 下载 master 分支源码并构建
#    SEVENZIP_VER=v26.02-v1.5.7-R2 ./build.sh      # 指定 tag（分支名也可以）
#    SRCDIR=/path/to/7-Zip-zstd ./build.sh         # 用已解压的源码，不联网
#    CXX=/d/w64devkit/bin/x86_64-w64-mingw32-g++.exe ./build.sh
#    OUT=./7zPack.dll ./build.sh                   # 指定输出
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SEVENZIP_VER="${SEVENZIP_VER:-master}"
BUILD="${BUILD:-$HERE/build}"
OUT="${OUT:-$HERE/../7zPack.dll}"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
CC="${CC:-x86_64-w64-mingw32-gcc}"

for t in "$CXX" "$CC" curl tar python3; do
  command -v "$t" >/dev/null 2>&1 || { echo "缺少依赖：$t"; exit 1; }
done

# ---------------------------------------------------------------- 取源码
SRC="$BUILD/7-Zip-zstd-$SEVENZIP_VER"
[ -n "${SRCDIR:-}" ] && SRC="$SRCDIR"      # 指定了现成目录就直接用
if [ ! -d "$SRC" ]; then
  mkdir -p "$BUILD"
  echo "== 下载 7-Zip-zstd ($SEVENZIP_VER)"
  URL="https://github.com/mcmilk/7-Zip-zstd/archive/refs"
  # 先按分支取，失败再按 tag 取（版本号是 tag，不是分支）
  curl -fsSL --retry 2 -o "$BUILD/src.tar.gz" "$URL/heads/$SEVENZIP_VER.tar.gz" \
    || curl -fsSL --retry 2 -o "$BUILD/src.tar.gz" "$URL/tags/$SEVENZIP_VER.tar.gz" \
    || { echo "下载失败：没有分支或 tag 名为 $SEVENZIP_VER"; exit 1; }
  tar -xzf "$BUILD/src.tar.gz" -C "$BUILD"
fi
[ -d "$SRC" ] || { echo "源码目录不存在：$SRC"; exit 1; }

C7Z="$SRC/CPP/7zip/UI/Client7z"
OBJ="$BUILD/obj"
mkdir -p "$OBJ"

# ---------------------------------------------------------------- 打补丁
# 把 7-Zip 的进度回调接到本项目，并让「取消」能中断操作。
# 改动等价于 client7z.patch，这里直接用文本替换，免得依赖 patch 命令。
echo "== 应用补丁"
python3 - "$C7Z/Client7z.cpp" <<'PYEOF'
import io, sys

path = sys.argv[1]
s = io.open(path, encoding="utf-8", newline="").read()

OLD = (
    "Z7_COM7F_IMF({c}::SetTotal(UInt64 /* size */))\n"
    "{{\n  return S_OK;\n}}\n\n"
    "Z7_COM7F_IMF({c}::SetCompleted(const UInt64 * /* completeValue */))\n"
    "{{\n  return S_OK;\n}}"
)
NEW = (
    "Z7_COM7F_IMF({c}::SetTotal(UInt64 size))\n"
    "{{\n  Ev_OnTotal(size);\n  return S_OK;\n}}\n\n"
    "Z7_COM7F_IMF({c}::SetCompleted(const UInt64 * completeValue))\n"
    "{{\n"
    "  if (completeValue)\n"
    "  {{\n"
    "    Ev_OnProgress(*completeValue);\n"
    "    if (Ev_IsCancelled())\n"
    "      return E_ABORT;\n"
    "  }}\n"
    "  return S_OK;\n"
    "}}"
)

patched = 0
for cls in ("CArchiveExtractCallback", "CArchiveUpdateCallback"):
    old, new = OLD.format(c=cls), NEW.format(c=cls)
    if new in s:
        continue
    if old not in s:
        sys.exit("补丁锚点未找到（上游源码可能已改动）：" + cls)
    s = s.replace(old, new, 1)
    patched += 1

if patched:
    io.open(path, "w", encoding="utf-8", newline="").write(s)
    print("   已应用 %d 处改动" % patched)
else:
    print("   已是最新，跳过")
PYEOF

# ---------------------------------------------------------------- 拼接
# 只取 Client7z.cpp 中 main() 之前的部分（类定义 + 回调实现），
# 再把本项目的 7zapi.cpp 接在后面，形成一个编译单元。
echo "== 拼接编译单元"
CUT=$(awk '/Main function/{print NR-1; exit}' "$C7Z/Client7z.cpp")
{
  echo '#define DLL_EXPORT extern "C" __declspec(dllexport)'
  echo 'void Ev_OnTotal(unsigned long long t);'
  echo 'void Ev_OnProgress(unsigned long long done);'
  echo 'bool Ev_IsCancelled();'
  sed -n "1,${CUT}p" "$C7Z/Client7z.cpp"
  cat "$HERE/7zapi.cpp"
} > "$C7Z/7zpack.cpp"

# ---------------------------------------------------------------- 编译
FLAGS="-O2 -DNDEBUG -Wall -fno-strict-aliasing"

echo "== 编译 7-Zip 依赖"
"$CC" $FLAGS -c "$SRC/C/Alloc.c" -o "$OBJ/Alloc.o"
for f in \
  Common/IntToString.cpp Common/MyString.cpp Common/MyVector.cpp \
  Common/NewHandler.cpp Common/StringConvert.cpp Common/StringToInt.cpp \
  Common/UTFConvert.cpp Common/Wildcard.cpp Common/MyWindows.cpp \
  Windows/DLL.cpp Windows/FileDir.cpp Windows/FileFind.cpp Windows/FileIO.cpp \
  Windows/FileName.cpp Windows/PropVariant.cpp Windows/PropVariantConv.cpp \
  Windows/TimeUtils.cpp 7zip/Common/FileStreams.cpp
do
  "$CXX" $FLAGS -c "$SRC/CPP/$f" -o "$OBJ/$(basename "${f%.cpp}").o"
done

echo "== 编译 7zpack"
"$CXX" $FLAGS -c "$C7Z/7zpack.cpp" -o "$OBJ/7zpack.o"

echo "== 链接"
"$CXX" -shared -static -static-libgcc -static-libstdc++ \
  -o "$OUT" "$OBJ"/*.o \
  -lole32 -loleaut32 -luuid -luser32 -ladvapi32 -lgdi32 -lshell32

echo "== 完成：$OUT"
ls -l "$OUT"
