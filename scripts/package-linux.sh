#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <pentimento-binary> <dist-directory>" >&2
    exit 2
fi

repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
binary=$1
dist_dir=$2

if [ ! -x "$binary" ]; then
    echo "error: executable not found: $binary" >&2
    exit 1
fi

if [ -n "${QMAKE:-}" ]; then
    qmake=$QMAKE
elif [ -n "${QT_ROOT_DIR:-}" ] && [ -x "$QT_ROOT_DIR/bin/qmake" ]; then
    qmake=$QT_ROOT_DIR/bin/qmake
elif command -v qmake6 >/dev/null 2>&1; then
    qmake=qmake6
elif command -v qmake >/dev/null 2>&1; then
    qmake=qmake
else
    echo "error: qmake not found; set QMAKE or QT_ROOT_DIR" >&2
    exit 1
fi

qt_libs=$("$qmake" -query QT_INSTALL_LIBS)
qt_plugins=$("$qmake" -query QT_INSTALL_PLUGINS)
package_name=pentimento-linux-x64
package_dir=$dist_dir/$package_name

mkdir -p "$dist_dir"
rm -rf "$package_dir"
mkdir -p "$package_dir/bin/translations" "$package_dir/lib" \
    "$package_dir/plugins/platforms" "$package_dir/plugins/imageformats" \
    "$package_dir/plugins/sqldrivers"

cp "$binary" "$package_dir/bin/pentimento"
cp "$repo_root"/translations/*.qm "$package_dir/bin/translations/"
cp "$repo_root/README.md" "$repo_root/README.zh.md" "$repo_root/LICENSE" "$package_dir/"

for plugin in libqxcb.so libqoffscreen.so libqminimal.so; do
    if [ -f "$qt_plugins/platforms/$plugin" ]; then
        cp -a "$qt_plugins/platforms/$plugin" "$package_dir/plugins/platforms/"
    fi
done
cp -a "$qt_plugins/imageformats/." "$package_dir/plugins/imageformats/"
if [ -f "$qt_plugins/sqldrivers/libqsqlite.so" ]; then
    cp -a "$qt_plugins/sqldrivers/libqsqlite.so" "$package_dir/plugins/sqldrivers/"
else
    echo "error: Qt SQLite driver not found under $qt_plugins/sqldrivers" >&2
    exit 1
fi

copy_qt_library() {
    library=$1
    [ -e "$library" ] || return 0
    cp -a "$library" "$package_dir/lib/"
    resolved=$(readlink -f "$library")
    if [ "$resolved" != "$library" ]; then
        cp -a "$resolved" "$package_dir/lib/"
    fi
}

# Transitive dependency collection. Running ldd on the binary and plugins
# alone only covers their direct NEEDED entries: the xcb platform plugin
# hands off to libQt6XcbQpa, whose own NEEDED list is the xcb/X11 helper
# stack (libxcb-cursor, libxcb-icccm, libxcb-keysyms, libxkbcommon-x11,
# ...). Skipping that layer ships a package that cannot start on a clean
# desktop (review finding 2026-10-30: Ubuntu 24.04 startup failure). Keep
# sweeping every ELF we ship until a pass copies nothing new.
#
# glibc and its companion libraries are version-locked to the target
# system's dynamic loader; shipping them inside a directory that
# LD_LIBRARY_PATH puts ahead of the system path breaks startup on
# machines with a different glibc. The compiler runtime (libstdc++,
# libgcc_s, libatomic) is forward-compatible, so it stays bundled.
pass=0
while [ "$pass" -lt 8 ]; do
    copied=0
    for file in $(find "$package_dir/bin" "$package_dir/plugins" "$package_dir/lib" \
            -type f \( -name '*.so*' -o -name pentimento \)); do
        for dependency in $(ldd "$file" 2>/dev/null | awk '/=> \// {print $3}'); do
            case "$dependency" in
                "$qt_libs"/*) ;;
                *) continue ;;
            esac
            case "$(basename "$dependency")" in
                libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|\
                libanl.so*|libutil.so*|ld-linux*) continue ;;
            esac
            if [ ! -e "$package_dir/lib/$(basename "$dependency")" ]; then
                copy_qt_library "$dependency"
                copied=1
            fi
        done
    done
    [ "$copied" -eq 1 ] || break
    pass=$((pass + 1))
done

# Qt loads libxcb-cursor for the xcb platform plugin, and some Qt builds
# reference it only via dlopen, which no ldd sweep can see. Bundle it
# explicitly when the build machine has it; warn when it does not, so the
# packager knows the target system must provide libxcb-cursor0 itself.
cursor_lib=$(ldconfig -p 2>/dev/null | awk '/libxcb-cursor\.so\.0 \(/ {print $NF; exit}')
if [ -n "$cursor_lib" ] && [ ! -e "$package_dir/lib/libxcb-cursor.so.0" ]; then
    cp -a "$cursor_lib" "$package_dir/lib/"
fi
if [ ! -e "$package_dir/lib/libxcb-cursor.so.0" ]; then
    echo "warning: libxcb-cursor.so.0 not found on build machine;" >&2
    echo "warning: target desktop systems need the libxcb-cursor0 (Ubuntu) / xcb-util-cursor (Fedora, Arch) package" >&2
fi

if ! find "$package_dir/lib" -maxdepth 1 -name 'libQt6Core.so*' -print -quit | grep -q .; then
    echo "error: Qt runtime libraries were not bundled" >&2
    exit 1
fi

cat > "$package_dir/pentimento" <<'EOF'
#!/bin/sh
set -eu
appdir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$appdir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$appdir/plugins"

# Startup self-check: resolve the NEEDED entries of every shipped ELF under
# the environment this launcher sets up. If a system library is missing
# (too-old or minimal desktop), report it with an install hint instead of
# letting the dynamic loader abort with a cryptic error.
missing=$(for file in "$appdir"/bin/pentimento "$appdir"/plugins/*/*.so; do
    ldd "$file" 2>/dev/null
done | awk '/not found/ {print $1}' | sort -u)
if [ -n "$missing" ]; then
    echo "pentimento: missing system libraries:" >&2
    for lib in $missing; do
        echo "  $lib" >&2
    done
    echo "hint (Ubuntu/Debian): sudo apt install libxcb-cursor0 libxkbcommon-x11-0" >&2
    echo "hint (Fedora):        sudo dnf install xcb-util-cursor libxkbcommon-x11" >&2
    echo "hint (Arch):          sudo pacman -S xcb-util-cursor libxkbcommon" >&2
    exit 1
fi

exec "$appdir/bin/pentimento" "$@"
EOF
chmod +x "$package_dir/pentimento"

tar -C "$dist_dir" -czf "$dist_dir/$package_name.tar.gz" "$package_name"
echo "created $dist_dir/$package_name.tar.gz"
