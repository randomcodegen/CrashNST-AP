# Client dependencies

The `licenses` directory contains dependency notices.

Corresponding APCpp source and relinking materials are available in
`CrashNST-AP-Relinking-1.0.5.zip` alongside the client download in the same release.
These files are for developers and do not need to be installed to play.

The standalone AP DLL statically links APCpp, its dependencies, liblzma and the MinGW runtime.
It uses Windows' D3D11, DXGI, shader compiler, DirectInput and GDI APIs.
It reads the Zoinks bitmap font from the installed game's permanent.pak, and falls back to GDI for missing glyphs/assets.

| Component | Source | License |
| --- | --- | --- |
| APCpp | https://github.com/randomcodegen/APCpp | LGPL 2.1 |
| IXWebSocket | APCpp's `IXWebSocket` | BSD 3-clause |
| jsoncpp | APCpp's `jsoncpp` | MIT/public-domain alternative |
| zlib | APCpp's `zlib` | zlib |
| liblzma 5.8.4 | [Official MSYS2 package](https://packages.msys2.org/packages/mingw-w64-x86_64-xz) , XZ Utils | 0BSD |
| Mbed TLS 3.6.4 | APCpp's bundled source archive | Apache 2.0 |
| Everest/HACL/KreMLin | Mbed TLS `3rdparty/everest` , copyright INRIA and Microsoft Corporation | Apache 2.0 |
| p256-m | Mbed TLS `3rdparty/p256-m` , copyright The Mbed TLS Contributors | Apache 2.0 alternative |
| GCC libraries | MSYS2 MinGW runtime | GPL 3 with GCC Runtime Library Exception |
| winpthreads | MSYS2 MinGW runtime | MIT/BSD notices in COPYING |
