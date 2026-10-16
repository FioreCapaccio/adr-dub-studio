# ADR Dub Studio

DAW per doppiaggio e ADR (JUCE / C++): timeline e mixer, registrazione, video, cue ADR stile Pro Tools,
EQ parametrico, limiter, automazione ed esportazioni. Effetti integrati (filtro HP/LP, de-esser, gate, riverbero, delay,
saturazione, chorus, compressore multibanda), supporto ai plug-in VST3 come insert, temi (anche importati dai temi di Reaper: ne usa i colori) e layout. Flusso di doppiaggio: ruoli di traccia (Dialoghi, Guida, M&E, Effetti) con bus dedicati, modelli di sessione, una traccia per personaggio dall'importazione del copione, schema dei nomi dei take e foglio cue stampabile. Consegna: pacchetto MIX + stem Dialoghi/M&E/Effetti con rapporto, esclusione della Guida, preset TV e streaming, video di revisione con burn-in (macOS).

- **macOS** (universale, Apple Silicon e Intel) e **Windows** (x64, con il video: usa ffmpeg, incluso nello zip): vedi le [Release](../../releases).
- Build automatica con GitHub Actions (`.github/workflows/build.yml`); per pubblicare una versione basta pushare un tag `v*`.
- Compilazione locale: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`. JUCE 8.0.9 viene scaricato da CMake.
- Verifica senza interfaccia: `"ADR Dub Studio" --selftest a.wav b.wav c.wav [video.mp4] [plugin.vst3]` deve stampare `TUTTO OK`.

## Licenza

Copyright (C) 2026 Fioravante Capaccio.

Questo programma e' software libero: puoi ridistribuirlo e modificarlo secondo i termini della
GNU Affero General Public License v3 (vedi [LICENSE](LICENSE)). E' costruito con [JUCE](https://juce.com),
usato sotto licenza AGPLv3.

## Video

Su macOS il video usa AVFoundation. Su Windows usa **ffmpeg/ffprobe** (build LGPL di [BtbN](https://github.com/BtbN/FFmpeg-Builds), inclusi nello zip, oppure nel PATH): lettura, miniature, esportazione con il mix e anteprima.
