# ADR Dub Studio

DAW per doppiaggio e ADR (JUCE / C++): timeline e mixer, registrazione, video, cue ADR stile Pro Tools,
EQ parametrico, limiter, automazione ed esportazioni. Effetti integrati (filtro HP/LP, de-esser, gate, riverbero, delay,
saturazione, chorus, compressore multibanda), supporto ai plug-in VST3 come insert, temi (anche importati dai temi di Reaper: ne usa i colori) e layout. Flusso di doppiaggio: ruoli di traccia (Dialoghi, Guida, M&E, Effetti) con bus dedicati, modelli di sessione, una traccia per personaggio dall'importazione del copione, schema dei nomi dei take e foglio cue stampabile. Consegna: pacchetto MIX + stem Dialoghi/M&E/Effetti con rapporto, esclusione della Guida, preset TV e streaming, video di revisione con burn-in (macOS). Sincronia: allineamento automatico del take alla guida, ducking automatico del fondo sotto i dialoghi, rilevamento dei tagli del video. Sala di registrazione: lettore copione per l'attore e cuffie separate (seconda coppia di uscite) con mix, bip e talkback del direttore. Sessioni affidabili: i media mancanti restano come clip offline e si ricollegano, versioni numerate con nota, controllo della sessione e guida al flusso di lavoro. ReConform: riporta audio, cue e marker su un nuovo montaggio a partire dagli EDL vecchio e nuovo. Sincronia esterna MTC e scorciatoie da tastiera personalizzabili. Importazione delle take registrate insieme ai cue (file tipo Cue012_Marco_Take2_BEST.wav, posizione dal BWF). Importazione dei cue da Pro Tools, EDL (anche Nuendo), CSV/TSV (Nuendo, Fairlight, Resolve, report ADR) e sottotitoli SRT/WebVTT. Selezione di piu' tracce (Cmd/Ctrl+clic, Maiusc+clic sull'intestazione). Gruppi di tracce comprimibili (per esempio Dialoghi con le tracce dei personaggi), sessioni .adb (le .arostudio si aprono ancora), timecode che prende gli fps del video. Pannello ADR filtrabile per personaggio; nella timeline i cue si ridimensionano trascinando i bordi e si spostano dalla fascia in alto
- Interfaccia in italiano e inglese (Aiuto > Lingua / Language, si applica al riavvio).

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
