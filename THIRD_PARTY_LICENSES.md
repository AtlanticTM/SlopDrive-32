# Third-Party Licenses

SlopDrive-32 itself is licensed under **CERN-OHL-S v2** — see
[`LICENSE`](LICENSE) and [`NOTICE`](NOTICE) for the full text and the reason
(it incorporates a strongly-reciprocal CERN-OHL-S v2 dependency, so the whole
project must convey under the same license).

It depends on the following third-party libraries (declared in
[`platformio.ini`](platformio.ini)) plus the Espressif Arduino core. Each is the
property of its respective author(s) and is distributed under its own license,
summarized below. This file is provided for attribution; the canonical license
text always lives with each upstream project.

| Library | Author | License | Project |
|---------|--------|---------|---------|
| NimBLE-Arduino | h2zero | Apache-2.0 | https://github.com/h2zero/NimBLE-Arduino |
| INA228 | Rob Tillaart | MIT | https://github.com/RobTillaart/INA228 |
| FastAccelStepper | Jochen Kiemes (gin66) | MIT | https://github.com/gin66/FastAccelStepper |
| Adafruit NeoPixel | Adafruit Industries | LGPL-3.0 | https://github.com/adafruit/Adafruit_NeoPixel |
| ArduinoJson | Benoît Blanchon | MIT | https://github.com/bblanchon/ArduinoJson |
| Arduino-ESP32 core | Espressif Systems | LGPL-2.1 (+ Apache-2.0 components) | https://github.com/espressif/arduino-esp32 |
| OSSM StrokeEngine / pattern classes | theelims (vendored from KinkyMakers/OSSM-hardware) | MIT | https://github.com/KinkyMakers/OSSM-hardware |

> **Note on LGPL libraries** (Adafruit NeoPixel, the Espressif Arduino core):
> the LGPL permits use and linking in projects under other licenses, provided
> the LGPL components themselves remain under the LGPL and their license/source
> is made available. These libraries are not modified in this repository — they
> are pulled unmodified by PlatformIO from their upstream sources, where their
> full license text and source are available.

---

## Full license texts

### MIT (FastAccelStepper, ArduinoJson, StrokeEngine, INA228)

```
MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Individual MIT-licensed libraries retain their own copyright notices:

- **TMCStepper** — Copyright (c) Teemu Mäntykallio
- **FastAccelStepper** — Copyright (c) Jochen Kiemes
- **ArduinoJson** — Copyright (c) Benoît Blanchon
- **StrokeEngine** — Copyright (C) 2021-2022 theelims <elims@gmx.net>. Vendored from KinkyMakers/OSSM-hardware, Software/lib/StrokeEngine/src/. The pattern code (pattern.h, PatternMath.h) carries: "This software may be modified and distributed under the terms of the MIT license."

### LGPL-3.0 (Adafruit NeoPixel)

Adafruit NeoPixel is licensed under the GNU Lesser General Public License,
version 3. Full text: https://www.gnu.org/licenses/lgpl-3.0.txt
Source and license: https://github.com/adafruit/Adafruit_NeoPixel

### LGPL-2.1 (Espressif Arduino core)

The Espressif Arduino-ESP32 core is licensed under the GNU Lesser General
Public License, version 2.1 (it additionally includes components under
Apache-2.0 and other compatible licenses).
Full LGPL-2.1 text: https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt

- Arduino-ESP32: https://github.com/espressif/arduino-esp32

---

## Historical (removed from the build)

- **TMCStepper** (Teemu Mäntykallio, MIT) — the legacy TMC2160 belt-drive
  stepper build; driver deleted with the transport-era cleanup.

These libraries were dependencies at one point but are no longer declared in
`platformio.ini`. Attribution is kept here for provenance; they carry no
obligations against the current build.

- **arduinoWebSockets** — Markus Sattler (Links2004), LGPL-2.1 —
  https://github.com/Links2004/arduinoWebSockets. Removed from the build in
  M5c (see `CLAUDE.md` §8) when the `:81` WebUI telemetry plane and the
  `:55555` Intiface/TCode WebSocket server were retired in favor of SlopSync.
  Full LGPL-2.1 text: https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt

---

If any attribution here is inaccurate, please open an issue — the upstream
repositories linked above are the authoritative source for each library's
current license.
