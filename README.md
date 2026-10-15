# ADR Dub Studio

DAW per doppiaggio e ADR (JUCE / C++): timeline e mixer, registrazione, video, cue ADR stile Pro Tools,
EQ parametrico, limiter, automazione ed esportazioni.

- **macOS** (universale, Apple Silicon e Intel) e **Windows** (x64, senza video per ora): vedi le [Release](../../releases).
- Build automatica con GitHub Actions (`.github/workflows/build.yml`); per pubblicare una versione basta pushare un tag `v*`.
- Compilazione locale: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`. JUCE 8.0.9 viene scaricato da CMake.
- Verifica senza interfaccia: `"ADR Dub Studio" --selftest a.wav b.wav c.wav [video.mp4] [plugin.vst3]` deve stampare `TUTTO OK`.

## Licenza

Copyright (C) 2026 Fioravante Capaccio.

Questo programma e' software libero: puoi ridistribuirlo e modificarlo secondo i termini della
GNU Affero General Public License v3 (vedi [LICENSE](LICENSE)). E' costruito con [JUCE](https://juce.com),
usato sotto licenza AGPLv3.
