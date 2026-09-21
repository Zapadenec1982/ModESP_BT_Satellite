libfreeaptx, as carried in this satellite
=========================================

The code in `main/` is the libfreeaptx library proper: `freeaptx.c`, `freeaptx.h`,
the upstream `README` and the licence text in `COPYING`. As `main/README` says,
libfreeaptx "is based on version 0.2.0 of libopenaptx with the intent of continuing
under a free license", and is "derived from ffmpeg 4.0 project and licensed under
LGPLv2.1+". The copy here came by way of the fork the codec patches were taken from
(`docs/BASE.md`); no upstream URL is recorded in the files themselves.

Licence: **LGPL-2.1-or-later**, as stated in the header of every source file
("This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License ... either version 2.1 of the
License, or (at your option) any later version"). The full text is `main/COPYING`;
`LICENSE` next to this file is a copy of it.

Until 2026-09-21 this directory carried an ESP-IDF application-template `README.md`
and `LICENSE` claiming "Public Domain (or CC0)". Those two files belonged to the
template of the fork this code was taken from, not to libfreeaptx, and were replaced.

The library is compiled into `libbt.a` together with the Bluedroid stack
(`patches/0001`, CMake). `tools/relink_kit.ps1` collects everything needed to relink
the firmware against a modified build of it; `THIRD_PARTY_NOTICES.md` at the
repository root lists the other third-party code in the image.
