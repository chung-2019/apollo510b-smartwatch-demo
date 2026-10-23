# Third-Party Notices

This repository contains original project code and selected files derived from
the AmbiqSuite SDK 5.2.0 example environment. It intentionally does **not**
include a complete copy of AmbiqSuite, NemaGFX, FreeRTOS, Cordio, FatFs, SEGGER
RTT, their prebuilt libraries, or their vendor documentation.

The project-level BSD-3-Clause license (`LICENSE`) applies only to material
owned by `chung-2019` and to material that is not marked with another notice.
Existing copyright and license notices in vendor-derived files take precedence.

## Ambiq Micro, Inc.

The following files originate from or are adapted from the AmbiqSuite
Development Package and carry Ambiq's notice:

- build and startup: `Makefile`, `gcc/Makefile`, `gcc/startup_gcc.c`
- resources and memory map: `src/am_resources.c`, `src/am_memory_map.h`
- application and tasks, grown from the SDK's `nemagfx_watchface` example:
  `src/nemagfx_watchface.c/.h`, `src/rtos.c/.h`, `src/render_task.c/.h`,
  `src/display_task.c/.h`, `src/radio_task.c/.h`, `src/sensor_task.c/.h`,
  `src/ble_freertos_tmp119_hts.h`
- font data shipped with the SDK's NemaGFX examples: `src/font/` (see also
  [DejaVu fonts](#dejavu-fonts))
- the TSVG weather icons of the SDK's `nemagfx_watchface` example:
  `src/generated/blowing.h`, `climate.h`, `day.h`

Their original notice is reproduced below.

```text
Copyright (c) 2026, Ambiq Micro, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived from this
software without specific prior written permission.

Third party software included in this distribution is subject to the
additional license terms as defined in the /docs/licenses directory.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

This is part of revision release_sdk5p2p0-66487dd10 of the AmbiqSuite
Development Package.
```

The `/docs/licenses` reference above points to the license directory in the
separately obtained AmbiqSuite SDK; that directory is not redistributed here.

## Arm Ltd. and Packetcraft, Inc. (Cordio)

`src/hts_main.c` and `src/app_hw_tmp119.c` are adapted from the Cordio BLE
sample applications distributed with AmbiqSuite. They remain under the Apache
License, Version 2.0, as stated in their headers; a copy of the license is in
[`licenses/Apache-2.0.txt`](licenses/Apache-2.0.txt).

```text
Copyright (c) 2011-2019 Arm Ltd. All Rights Reserved.

Copyright (c) 2019 Packetcraft, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

The modifications made in this project are described in the comments of those
two files.

## FreeRTOS

`src/FreeRTOSConfig.h` is adapted from the FreeRTOS configuration of the
AmbiqSuite examples and keeps its MIT notice. The FreeRTOS v10.5.1
implementation itself is not included; it is obtained with the SDK under the
same license.

```text
FreeRTOS V202212.00
Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## DejaVu fonts

`src/font/DejaVuSans_ttf_kern.c` (the NemaVG vector font used for all on-screen
text) and
`src/font/DejaVuSerif24pt4b.c` (a bitmap font, no longer built) are glyph data
converted from the DejaVu Sans and DejaVu Serif typefaces; both files come from
the AmbiqSuite NemaGFX examples. The DejaVu fonts license, as embedded in the
`DejaVuSerif.ttf` shipped with the NemaGFX SDK, is:

```text
Fonts are (c) Bitstream (see below). DejaVu changes are in public domain.

Bitstream Vera Fonts Copyright
------------------------------

Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is a
trademark of Bitstream, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of
the fonts accompanying this license ("Fonts") and associated documentation
files (the "Font Software"), to reproduce and distribute the Font Software,
including without limitation the rights to use, copy, merge, publish,
distribute, and/or sell copies of the Font Software, and to permit persons to
whom the Font Software is furnished to do so, subject to the following
conditions:

The above copyright and trademark notices and this permission notice shall be
included in all copies of one or more of the Font Software typefaces.

The Font Software may be modified, altered, or added to, and in particular the
designs of glyphs or characters in the Fonts may be modified and additional
glyphs or  or characters may be added to the Fonts, only if the fonts are
renamed to names not containing either the words "Bitstream" or the word
"Vera".

This License becomes null and void to the extent applicable to Fonts or Font
Software that has been modified and is distributed under the "Bitstream Vera"
names.

The Font Software may be sold as part of a larger software package but no copy
of one or more of the Font Software typefaces may be sold by itself.

THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF COPYRIGHT, PATENT,
TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL BITSTREAM OR THE GNOME FOUNDATION
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING ANY GENERAL,
SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF THE USE OR INABILITY TO
USE THE FONT SOFTWARE OR FROM OTHER DEALINGS IN THE FONT SOFTWARE.

Except as contained in this notice, the names of Gnome, the Gnome Foundation,
and Bitstream Inc., shall not be used in advertising or otherwise to promote
the sale, use or other dealings in this Font Software without prior written
authorization from the Gnome Foundation or Bitstream Inc., respectively. For
further information, contact: fonts at gnome dot org.
```

## OpenStreetMap data

The map tiles (`src/generated/map/map_tile_0*.h` and
`tools/assets_out/MAPTILE*.BIN`) are rendered from OpenStreetMap data (the
Geofabrik extract of Taiwan) by `tools/render_taipei_osm_tiles.py` and
`tools/map_tile_gen.py`. The raw data is not included.

Map data © [OpenStreetMap contributors](https://www.openstreetmap.org/copyright).
The data is available under the Open Database License (ODbL) 1.0; the tiles are
a Produced Work, and this notice is the attribution the ODbL requires.

## External build dependencies

| Dependency | Repository policy |
| --- | --- |
| AmbiqSuite SDK 5.2.0 | Required from Ambiq; the complete SDK is not included. Review and accept the license terms supplied with the SDK. |
| NemaGFX / NemaVG SDK and runtime | Required from the AmbiqSuite / Think Silicon distribution; headers, static libraries, runtime binaries, and specifications are not included. |
| FreeRTOS | Required from the AmbiqSuite distribution; the FreeRTOS implementation sources are not included. |
| Cordio BLE stack | Required from the AmbiqSuite distribution; apart from the two adapted sample files above, it is not included. |
| FatFs | Required from the AmbiqSuite distribution (`third_party/FatFs`); not included. |
| SEGGER RTT | Required from the AmbiqSuite distribution (`third_party/SEGGER`); not included. |
| CMSIS and device drivers | Required from the AmbiqSuite distribution; they are referenced by the build but are not copied into this repository. |

The NemaGFX header/documentation notice supplied with AmbiqSuite does not, by
itself, establish a right to redistribute the NemaGFX runtime library. For this
reason, this repository publishes source and build integration only. It does
not publish linked firmware images. The `.BIN` files in `tools/assets_out/` are
image data for the eMMC asset store, not firmware.

## No endorsement

Ambiq, Apollo, AmbiqSuite, NemaGFX, Think Silicon, Arm, Cordio, Packetcraft,
FreeRTOS, SEGGER, Bitstream, DejaVu, OpenStreetMap, and other product names
belong to their respective owners. Their names are used only to describe
technical compatibility, data sources and external build dependencies. The
"ambiq" watch face shows the Ambiq logo, a trademark of Ambiq Micro, Inc., only
to identify the hardware the demo runs on.

This is an independent project. It is not endorsed, sponsored, certified, or
approved by Ambiq Micro, Inc. or the other dependency providers.
