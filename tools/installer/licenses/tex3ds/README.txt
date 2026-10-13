tex3ds in FTL2CIA
=================

FTL2CIA.exe carries tex3ds.exe, version 2.3.0, the Windows build from devkitPro's tools
(https://devkitpro.org). FTL2CIA runs it to encode the backgrounds and planets from your own
ftl.dat as 3DS textures (ETC1) for the CIA it makes. FTL2CIA doesn't link with it: it runs it as a
separate program.

tex3ds is Copyright (c) 2017-2021 Michael Theall (mtheall). It is free software under the GNU
General Public License, version 3 or later: see tex3ds-COPYING.txt. It comes with NO WARRANTY.

Source code
-----------
tex3ds-2.3.0.tar.gz, next to this file in the release zip, is the exact source of the version
included. It is also at https://github.com/devkitPro/tex3ds/releases/tag/v2.3.0 . If your copy came
without it, ask on the project's GitHub (https://github.com/melonexe/FTL3DS/issues) and you'll be
sent it; this offer is valid for three years from the release you got.

devkitPro's build of tex3ds.exe is statically linked with these libraries, each under its own
licence (included here):

  ImageMagick 7.0.7-31   ImageMagick-LICENSE.txt       https://github.com/ImageMagick/ImageMagick/tree/7.0.7-31
  libjpeg-turbo 1.5.3    libjpeg-turbo-LICENSE.md,     https://github.com/libjpeg-turbo/libjpeg-turbo/tree/1.5.3
                         libjpeg-turbo-README.ijg.txt
  libpng 1.6.37          libpng-LICENSE.txt            https://github.com/glennrp/libpng/tree/v1.6.37
  zlib 1.2.11            zlib-LICENSE.txt              https://github.com/madler/zlib/tree/v1.2.11
  bzip2                  bzip2-LICENSE.txt             https://sourceware.org/bzip2/

This software is based in part on the work of the Independent JPEG Group.
