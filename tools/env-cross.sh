# Source in Git Bash before cross-compiling for the Quest (aarch64 Linux): portable cmake, ninja
# and clang, plus a python3 shim because FEX's generators call "python3" by name.
tools_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# The folder of the Python named in tools/python.local (a full path), if there is one.
python_dir=$(cat "$tools_dir/python.local" 2>/dev/null)
python_dir=${python_dir:+$(cygpath -u "$(dirname "$python_dir")"):}
export PATH="$tools_dir/shims:$tools_dir/cmake/bin:$tools_dir/ninja:$tools_dir/llvm/bin:$python_dir$PATH"
unset INCLUDE LIB
export MSYS2_ARG_CONV_EXCL='*'
export MSYS_NO_PATHCONV=1
