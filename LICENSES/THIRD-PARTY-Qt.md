# Third-party software: Qt

This package redistributes unmodified runtime libraries of **Qt 6**, the
cross-platform application framework, under the **GNU Lesser General Public
License version 3 (LGPL-3.0-only)**. The full license text ships with this
package as `LICENSES/LGPL-3.0-only.txt`.

- Copyright: Copyright (C) The Qt Company Ltd. and other contributors.
- Bundled Qt libraries: `libQt6Core`, `libQt6DBus`, `libQt6Gui`,
  `libQt6Sql`, `libQt6Svg`, `libQt6Widgets`, `libQt6XcbQpa`.
- Bundled Qt plugins: platform plugins (xcb, offscreen, minimal), image
  format plugins (gif, icns, ico, jpeg, mng, svg, tga, tiff, wbmp, webp)
  and the SQLite SQL driver plugin.
- Qt source code: <https://www.qt.io/download/open-source> and
  <https://download.qt.io/official_releases/qt/>; distribution source
  packages provide the same sources.
- Using your own Qt build: this application links Qt dynamically, so the
  files under `lib/` (`libQt6*.so*`) and `plugins/` can be replaced with a
  different unmodified Qt 6 build of your choosing and the application
  keeps working.
- Qt itself carries third-party components (zlib, libpng, libjpeg-turbo,
  FreeType, HarfBuzz, SQLite and others). The authoritative copyright
  notices and license texts are published with each Qt release at
  <https://doc.qt.io/qt-6/thirdpartylicenses.html>, and in each module's
  source tree as `qt_attribution.json`.

System libraries redistributed in `lib/` alongside Qt (the X11/xcb stack,
glib, ICU, FreeType, fontconfig, image codecs, and others) keep their own
licenses; verbatim texts collected from the build host's package metadata
ship in `LICENSES/third-party-system.txt`.
