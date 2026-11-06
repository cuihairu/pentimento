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

collect_qt_dependencies() {
    file=$1
    ldd "$file" 2>/dev/null | awk '/=> \// {print $3}' | while IFS= read -r dependency; do
        case "$dependency" in
            "$qt_libs"/*) copy_qt_library "$dependency" ;;
        esac
    done
}

collect_qt_dependencies "$package_dir/bin/pentimento"
find "$package_dir/plugins" -type f -name '*.so' -exec sh -c \
    'for file do ldd "$file" 2>/dev/null; done' sh {} + |
    awk '/=> \// {print $3}' | sort -u | while IFS= read -r dependency; do
        case "$dependency" in
            "$qt_libs"/*) copy_qt_library "$dependency" ;;
        esac
    done

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
exec "$appdir/bin/pentimento" "$@"
EOF
chmod +x "$package_dir/pentimento"

tar -C "$dist_dir" -czf "$dist_dir/$package_name.tar.gz" "$package_name"
echo "created $dist_dir/$package_name.tar.gz"
