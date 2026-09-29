# Third-Party Notices — SteBeam

SteBeam ships Qt 6.11.2 runtime libraries (Qt6Core/Gui/Widgets/Network/Svg and plugins),
a MinGW runtime (libgcc_s_seh-1, libstdc++-6, libwinpthread-1), D3Dcompiler_47.dll and Mesa
llvmpipe (opengl32sw.dll) alongside its binaries — whether you build them yourself per
[BUILDING.md](../BUILDING.md) or download them from us. The Qt libraries are licensed under the
GNU LGPL-3.0 (license text: [LGPL-3.0.txt](LGPL-3.0.txt)); the complete Qt 6.11.2 source code is
available at https://download.qt.io/archive/qt/6.11/ . The Qt DLLs sit as separate files in the
installation folder and may be replaced with your own builds; nothing restricts your right to
modify the Qt libraries for your own use, and reverse engineering for debugging such
modifications is permitted.
The following third-party components are attributed per the license requirements of the components actually shipped.
Full license texts are in this folder.

| Module group | Component | License text file |
|---|---|---|
| Qt Core | Apache Tika MimeType Definitions, version c29724782854bb5a319f4a1940d0828a58ace3f9 | [qtcore-attribution-tika-mimetypes.md](third-party/qtcore-attribution-tika-mimetypes.md) |
| Qt Core | BLAKE2 (reference implementation), version ed1974ea83433eba7b2d95c5dcd9ac33cb847913 | [qtcore-attribution-blake2.md](third-party/qtcore-attribution-blake2.md) |
| Qt Core | Data Compression Library (zlib), version 1.3.2 | [qtcore-attribution-zlib.md](third-party/qtcore-attribution-zlib.md) |
| Qt Core | Easing Equations by Robert Penner | [qtcore-attribution-easing.md](third-party/qtcore-attribution-easing.md) |
| Qt Core | Efficient Binary-Decimal and Decimal-Binary Conversion Routines for IEEE Doubles, version 3.4.0 | [qtcore-attribution-doubleconversion.md](third-party/qtcore-attribution-doubleconversion.md) |
| Qt Core | MD4 | [qtcore-attribution-md4.md](third-party/qtcore-attribution-md4.md) |
| Qt Core | MD5 | [qtcore-attribution-md5.md](third-party/qtcore-attribution-md5.md) |
| Qt Core | PCRE2 - Stack-less Just-In-Time Compiler, version 10.47 | [qtcore-attribution-pcre2-sljit.md](third-party/qtcore-attribution-pcre2-sljit.md) |
| Qt Core | PCRE2, version 10.47 | [qtcore-attribution-pcre2.md](third-party/qtcore-attribution-pcre2.md) |
| Qt Core | QEventDispatcher on macOS | [qtcore-attribution-qeventdispatcher-cf.md](third-party/qtcore-attribution-qeventdispatcher-cf.md) |
| Qt Core | Secure Hash Algorithm SHA-1 | [qtcore-attribution-sha1.md](third-party/qtcore-attribution-sha1.md) |
| Qt Core | Secure Hash Algorithm SHA-3 - Keccak, version 3.2 | [qtcore-attribution-sha3-keccak.md](third-party/qtcore-attribution-sha3-keccak.md) |
| Qt Core | Secure Hash Algorithm SHA-3 - brg_endian, version 1.0.0 | [qtcore-attribution-sha3-endian.md](third-party/qtcore-attribution-sha3-endian.md) |
| Qt Core | Secure Hash Algorithms SHA-384 and SHA-512 | [qtcore-attribution-rfc6234.md](third-party/qtcore-attribution-rfc6234.md) |
| Qt Core | SipHash Algorithm | [qtcore-attribution-siphash.md](third-party/qtcore-attribution-siphash.md) |
| Qt Core | TinyCBOR, version 7.0 | [qtcore-attribution-tinycbor.md](third-party/qtcore-attribution-tinycbor.md) |
| Qt Core | Unicode Character Database (UCD), version 36 | [qtcore-attribution-unicode-character-database.md](third-party/qtcore-attribution-unicode-character-database.md) |
| Qt Core | Unicode Common Locale Data Repository (CLDR), version v48.2 | [qtcore-attribution-unicode-cldr.md](third-party/qtcore-attribution-unicode-cldr.md) |
| Qt Core | forkfd | [qtcore-attribution-forkfd.md](third-party/qtcore-attribution-forkfd.md) |
| Qt Core | tl::expected, version 41d3e1f48d682992a2230b2a715bca38b848b269 | [qtcore-attribution-tlexpected.md](third-party/qtcore-attribution-tlexpected.md) |
| Qt GUI | Adobe Glyph List For New Fonts, version 1.7 | [qtgui-attribution-aglfn.md](third-party/qtgui-attribution-aglfn.md) |
| Qt GUI | Anti-aliasing rasterizer from FreeType 2 | [qtgui-attribution-grayraster.md](third-party/qtgui-attribution-grayraster.md) |
| Qt GUI | Cocoa Platform Plugin | [qtgui-attribution-cocoa-platform-plugin.md](third-party/qtgui-attribution-cocoa-platform-plugin.md) |
| Qt GUI | D3D12 Memory Allocator, version f128d39b7a95b4235bd228d231646278dc6c24b2 | [qtgui-attribution-d3d12memoryallocator.md](third-party/qtgui-attribution-d3d12memoryallocator.md) |
| Qt GUI | DejaVu Fonts, version 2.37 | [qtgui-attribution-dejayvu.md](third-party/qtgui-attribution-dejayvu.md) |
| Qt GUI | Emoji Segmenter, version 0.4.0 | [qtgui-attribution-emoji-segmenter.md](third-party/qtgui-attribution-emoji-segmenter.md) |
| Qt GUI | Freetype 2 - Bitmap Distribution Format (BDF) support | [qtgui-attribution-freetype-bdf.md](third-party/qtgui-attribution-freetype-bdf.md) |
| Qt GUI | Freetype 2 - Portable Compiled Format (PCF) support | [qtgui-attribution-freetype-pcf.md](third-party/qtgui-attribution-freetype-pcf.md) |
| Qt GUI | Freetype 2 - zlib | [qtgui-attribution-freetype-zlib.md](third-party/qtgui-attribution-freetype-zlib.md) |
| Qt GUI | Freetype 2, version 2.14.3 | [qtgui-attribution-freetype.md](third-party/qtgui-attribution-freetype.md) |
| Qt GUI | HarfBuzz-NG, version 14.3.0 | [qtgui-attribution-harfbuzz-ng.md](third-party/qtgui-attribution-harfbuzz-ng.md) |
| Qt GUI | LibJPEG-turbo, version 3.2.0 | [qtgui-attribution-libjpeg.md](third-party/qtgui-attribution-libjpeg.md) |
| Qt GUI | LibPNG, version 1.6.58 | [qtgui-attribution-libpng.md](third-party/qtgui-attribution-libpng.md) |
| Qt GUI | MD4C, version 0.5.3 | [qtgui-attribution-md4c.md](third-party/qtgui-attribution-md4c.md) |
| Qt GUI | Mipmap generator for D3D12, version 0aa79bad78992da0b6a8279ddb9002c1753cb849 | [qtgui-attribution-rhi-miniengine-d3d12-mipmap.md](third-party/qtgui-attribution-rhi-miniengine-d3d12-mipmap.md) |
| Qt GUI | Native Style for Android | [qtgui-attribution-android-native-style.md](third-party/qtgui-attribution-android-native-style.md) |
| Qt GUI | OpenGL ES 2 Headers, version Revision 27673 | [qtgui-attribution-opengl-es2-headers.md](third-party/qtgui-attribution-opengl-es2-headers.md) |
| Qt GUI | OpenGL Headers, version Revision 27684 | [qtgui-attribution-opengl-headers.md](third-party/qtgui-attribution-opengl-headers.md) |
| Qt GUI | Pixman, version 0.17.12 | [qtgui-attribution-pixman.md](third-party/qtgui-attribution-pixman.md) |
| Qt GUI | Smooth Scaling Algorithm | [qtgui-attribution-smooth-scaling-algorithm.md](third-party/qtgui-attribution-smooth-scaling-algorithm.md) |
| Qt GUI | Vulkan API Registry, version 1.4.308 | [qtgui-attribution-vulkan-xml-spec.md](third-party/qtgui-attribution-vulkan-xml-spec.md) |
| Qt GUI | Vulkan Memory Allocator, version 3.2.1 | [qtgui-attribution-vulkanmemoryallocator.md](third-party/qtgui-attribution-vulkanmemoryallocator.md) |
| Qt GUI | WebGradients | [qtgui-attribution-webgradients.md](third-party/qtgui-attribution-webgradients.md) |
| Qt GUI | Wintab API | [qtgui-attribution-wintab.md](third-party/qtgui-attribution-wintab.md) |
| Qt GUI | X Server helper | [qtgui-attribution-xserverhelper.md](third-party/qtgui-attribution-xserverhelper.md) |
| Qt GUI | XCB-XInput | [qtgui-attribution-xcb-xinput.md](third-party/qtgui-attribution-xcb-xinput.md) |
| Qt GUI | sRGB color profile icc file | [qtgui-attribution-icc-srgb-color-profile.md](third-party/qtgui-attribution-icc-srgb-color-profile.md) |
| Qt Network | The Public Suffix List, version 2026-05-14_08-35-31_UTC | [qtnetwork-attribution-psl-data.md](third-party/qtnetwork-attribution-psl-data.md) |
| Qt Network | libpsl - C library to handle the Public Suffix List, version 664f3dc85259ec65e30248a61fa1c45b7b0e4c3f | [qtnetwork-attribution-libpsl.md](third-party/qtnetwork-attribution-libpsl.md) |
| Qt SVG | XSVG | [qtsvg-attribution-xsvg.md](third-party/qtsvg-attribution-xsvg.md) |
| Additional Information | LLVM Attribution | [qt-attribution-llvm.md](third-party/qt-attribution-llvm.md) |
| Additional Information | Mesa llvmpipe | [qt-attribution-llvmpipe.md](third-party/qt-attribution-llvmpipe.md) |
| MinGW runtime | libgcc_s_seh-1.dll and libstdc++-6.dll (GCC runtime libraries, GPL-3.0 with GCC Runtime Library Exception — the Exception permits programs linked against them) | [mingw/GPL-3.0.txt](mingw/GPL-3.0.txt) + [mingw/GCC-Runtime-Library-Exception.txt](mingw/GCC-Runtime-Library-Exception.txt) |
| MinGW runtime | libwinpthread-1.dll (winpthreads, MIT license; parts derived from Lockless Inc. POSIX threads for Windows, BSD-3) | [mingw/winpthreads-COPYING.txt](mingw/winpthreads-COPYING.txt) |
| MinGW runtime | MinGW-w64 runtime binary-redistribution notices | [mingw/MinGW-w64-runtime-notices.txt](mingw/MinGW-w64-runtime-notices.txt) |
| Windows | D3Dcompiler_47.dll (Microsoft; redistribution together with applications is permitted by its license) | Microsoft license: https://learn.microsoft.com/windows/win32/direct3dhlsl/dx-graphics-hlsl-dcompiler-licenses |
| Installer tool | Installers are created with NSIS (Nullsoft Scriptable Install System) 3.x — NSIS itself is zlib/libpng licensed; its LZMA compression module (used here) is Common Public License 1.0 with an explicit linking exception permitting non-CPL linked code; NSIS source code is available from the contributors | NSIS licenses: https://nsis.sourceforge.io/Docs/AppendixI.html · source: https://github.com/nsis-dev/nsis |
