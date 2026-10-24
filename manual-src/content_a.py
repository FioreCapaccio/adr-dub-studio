# -*- coding: utf-8 -*-
# Contenuto del manuale, parte A: introduzione e finestra principale. Ogni testo e' (italiano, inglese).
TITLES = [
    dict(manual='Manuale utente', version='versione', contents='Indice', note='Nota', tip='Consiglio', warn='Attenzione',
         cover1='Studio di doppiaggio e ADR per macOS e Windows'),
    dict(manual='User manual', version='version', contents='Contents', note='Note', tip='Tip', warn='Warning',
         cover1='Dubbing and ADR studio for macOS and Windows'),
]

A = []
def h2(it, en): A.append(('h2', (it, en)))
def h3(it, en): A.append(('h3', (it, en)))
def h4(it, en): A.append(('h4', (it, en)))
def p(it, en): A.append(('p', (it, en)))
def ul(*items): A.append(('ul', list(items)))
def ol(*items): A.append(('ol', list(items)))
def note(it, en): A.append(('note', (it, en)))
def tip(it, en): A.append(('tip', (it, en)))
def warn(it, en): A.append(('warn', (it, en)))
def table(head, *rows): A.append(('table', head, list(rows)))
def fig(name, it, en, **spec): A.append(('fig', name, (it, en), spec))
def menu(nm_it, nm_en, img, cat): A.append(('menu', (nm_it, nm_en), img, cat))

# ======================================================================================================================
h2('Introduzione', 'Introduction')
p('**ADR Dub Studio** è un piccolo studio digitale pensato per il **doppiaggio** e la **sostituzione dei dialoghi (ADR)**: si importa un video, si importa o si scrive l\'elenco delle battute (i *cue*), si registrano più *take* di ogni battuta direttamente sul video, si scelgono i migliori, si allineano alla guida, si mixa e si consegna il materiale (mix e stem) con un rapporto di controllo. Il programma funziona su macOS (Apple Silicon e Intel) e su Windows e legge e scrive file WAV, AIFF e FLAC; i plug-in VST3 si possono usare come inserti.',
  '**ADR Dub Studio** is a compact digital studio designed for **dubbing** and **automated dialogue replacement (ADR)**: you import a video, import or write the list of lines (the *cues*), record several *takes* of every line right on the picture, pick the best ones, align them to the guide, mix, and deliver the material (mix and stems) with a check report. The program runs on macOS (Apple Silicon and Intel) and on Windows, reads and writes WAV, AIFF and FLAC files, and can host VST3 plug-ins as inserts.')
p('Questo manuale descrive ogni pulsante e ogni funzione. Le immagini sono schermate reali del programma: i **numeri arancioni** rimandano agli elenchi sotto ogni immagine. Nel testo, le scorciatoie da tastiera sono scritte con **Cmd** per macOS; su Windows si usa **Ctrl** al suo posto, e **Alt** corrisponde a **Option**.',
  'This manual describes every button and every function. The pictures are real screenshots of the program: the **orange numbers** refer to the lists below each picture. In the text, keyboard shortcuts are written with **Cmd** for macOS; on Windows use **Ctrl** instead, and **Alt** corresponds to **Option**.')

h3('Il flusso di lavoro in breve', 'The workflow at a glance')
ol(('**Video** – importa il video del film: l\'audio originale diventa la traccia *Guida* e il video appare in alto a destra, in sincronia con la timeline.',
    '**Video** – import the film: its original audio becomes the *Guide* track and the picture appears at the top right, in sync with the timeline.'),
   ('**Copione** – importa i cue da Pro Tools, da EDL, da una tabella CSV/TSV o da sottotitoli SRT, oppure creali a mano; ogni cue ha personaggio, scena, ingresso, uscita e battuta.',
    '**Script** – import the cues from Pro Tools, from an EDL, from a CSV/TSV table or from SRT subtitles, or create them by hand; each cue has character, scene, in, out and dialogue.'),
   ('**Tracce** – una traccia per ogni personaggio (i dialoghi), più M&E ed effetti; ogni ruolo ha il suo bus e una catena di EQ, compressore e limiter già pronta.',
    '**Tracks** – one track per character (the dialogue), plus M&E and effects; every role has its own bus and a ready-made chain of EQ, compressor and limiter.'),
   ('**Registrazione** – il programma conta alla rovescia con bip e sbarra scorrevole sul video, registra tra ingresso e uscita del cue e crea un nuovo take; si può ripetere quanto si vuole.',
    '**Recording** – the program counts down with beeps and a streamer over the picture, records between the cue\'s in and out points and creates a new take; you can repeat as often as you like.'),
   ('**Scelta e sincronia** – si danno le stelle ai take, si tiene il migliore e si allinea alla guida (anche frase per frase con il Time Warp).',
    '**Choice and sync** – rate the takes, keep the best one and align it to the guide (even phrase by phrase with the Time Warp).'),
   ('**Mix** – fader, pan, EQ, compressore, ducking automatico del fondo sotto i dialoghi, misura del loudness.',
    '**Mix** – faders, pan, EQ, compressor, automatic ducking of the background under the dialogue, loudness measurement.'),
   ('**Consegna** – mix e stem Dialoghi / M&E / Effetti con rapporto HTML, preset per TV e streaming, video di revisione con il mix.',
    '**Delivery** – mix and Dialogue / M&E / Effects stems with an HTML report, presets for TV and streaming, a review video with the mix.'))
tip('Aiuto > **Flusso di lavoro del doppiaggio** apre una lista di controllo con questi passi: ognuno si spunta da solo quando è fatto e ha un pulsante che lancia il comando giusto.',
    'Help > **Dubbing workflow** opens a checklist with these steps: each one ticks itself when done and has a button that launches the right command.')

fig('workflow', 'La lista di controllo del flusso di lavoro (Aiuto > Flusso di lavoro del doppiaggio).', 'The workflow checklist (Help > Dubbing workflow).', maxw=900)

# ======================================================================================================================
h2('Installazione e primo avvio', 'Installation and first launch')
h3('Installare il programma', 'Installing the program')
ul(('**macOS**: scarica il file `ADR-Dub-Studio-…-macOS-universal.zip`, decomprimilo e trascina **ADR Dub Studio** nella cartella Applicazioni. È una build universale (Apple Silicon e Intel), richiede macOS 11 o successivo. Al primo avvio, se macOS segnala uno sviluppatore non identificato, apri il programma con clic destro > Apri.',
    '**macOS**: download `ADR-Dub-Studio-…-macOS-universal.zip`, unzip it and drag **ADR Dub Studio** to the Applications folder. It is a universal build (Apple Silicon and Intel) and needs macOS 11 or later. On first launch, if macOS warns about an unidentified developer, open the app with right-click > Open.'),
   ('**Windows**: usa l\'installatore `…-Windows-x64-Setup.exe`, oppure decomprimi la versione portatile `…-Windows-x64-portable.zip` dove vuoi. Per l\'esportazione del video con il mix e per alcune funzioni video serve `ffmpeg` (accanto al programma o nel PATH).',
    '**Windows**: use the installer `…-Windows-x64-Setup.exe`, or unzip the portable version `…-Windows-x64-portable.zip` wherever you like. Exporting the video with the mix and some video features need `ffmpeg` (next to the program or in the PATH).'),
   ('**Microfono**: su macOS il sistema chiede il permesso la prima volta che si registra. Se lo si nega, si può riattivare in Impostazioni di Sistema > Privacy e sicurezza > Microfono.',
    '**Microphone**: on macOS the system asks for permission the first time you record. If you deny it, you can re-enable it in System Settings > Privacy & Security > Microphone.'))
h3('Scegliere la scheda audio', 'Choosing the audio interface')
p('Il programma usa per l\'uscita la scheda audio predefinita del sistema. Per cambiarla, o per scegliere l\'ingresso del microfono, apri **File > Impostazioni audio**.',
  'The program plays through the system\'s default audio device. To change it, or to choose the microphone input, open **File > Audio settings**.')
fig('dlg_audio_settings', 'File > Impostazioni audio: uscita, ingresso, canali attivi, frequenza di campionamento e dimensione del buffer.', 'File > Audio settings: output, input, active channels, sample rate and buffer size.', maxw=900)
ul(('**Uscita / Ingresso**: i dispositivi disponibili. All\'avvio il programma non apre nessun ingresso (evita blocchi se l\'ingresso predefinito è un dispositivo virtuale); l\'ingresso si apre quando serve per registrare, o quando lo scegli qui.',
    '**Output / Input**: the available devices. At startup the program opens no input (this avoids stalls when the default input is a virtual device); the input is opened when needed for recording, or when you choose it here.'),
   ('**Canali attivi**: quali uscite usare. Con una scheda a 4 o più uscite la seconda coppia può alimentare le cuffie dell\'attore (vedi *Sala di registrazione*).',
    '**Active channels**: which outputs to use. With an interface of 4 or more outputs the second pair can feed the actor\'s headphones (see *Recording room*).'),
   ('**Frequenza di campionamento / Buffer**: un buffer più piccolo riduce la latenza ma carica di più il processore. La barra di stato in basso a destra mostra uscita, frequenza, buffer e uso della CPU.',
    '**Sample rate / Buffer**: a smaller buffer lowers latency but loads the processor more. The status bar at the bottom right shows output, rate, buffer and CPU load.'),
   ('**Test**: emette un suono di prova sull\'uscita scelta.', '**Test**: plays a test tone on the chosen output.'))
h3('Lingua e tema', 'Language and theme')
p('La lingua dell\'interfaccia (italiano o inglese) si cambia da **Aiuto > Lingua / Language** e si applica al riavvio. Il tema grafico si sceglie da **Vista > Tema**: *Scuro*, *Chiaro*, *Alto contrasto* e *Default 5* (grigio neutro con accenti turchesi). Vista > Layout regola la densità dell\'interfaccia (Compatto, Standard, Ampio).',
  'The interface language (Italian or English) is changed from **Help > Lingua / Language** and applies on restart. The graphic theme is chosen in **View > Theme**: *Dark*, *Light*, *High contrast* and *Default 5* (neutral grey with teal accents). View > Layout sets the interface density (Compact, Standard, Large).')
fig('theme_light', 'Il tema Chiaro.', 'The Light theme.', maxw=1100)
fig('theme_d5', 'Il tema Default 5.', 'The Default 5 theme.', maxw=1100)
h3('Il recupero dopo una chiusura anomala', 'Recovery after an abnormal exit')
p('Il programma salva ogni pochi minuti una copia di sicurezza della sessione. Se si chiude in modo anomalo (blocco, spegnimento, interruzione di corrente) all\'avvio successivo propone di **ripristinare** la copia di sicurezza oppure di **ignorarla**.',
  'The program saves a safety copy of the session every few minutes. If it exits abnormally (crash, shutdown, power cut) the next launch offers to **restore** the safety copy or to **ignore** it.')
fig('recovery', 'La richiesta di recupero all\'avvio.', 'The recovery request at launch.', maxw=420)

# ======================================================================================================================
h2('La finestra principale', 'The main window')
p('La finestra principale riunisce la barra del trasporto, gli strumenti di modifica, la timeline con le tracce, il lettore video e il mixer. Le parti si possono mostrare o nascondere con i pulsanti **VIDEO** e **MIXER** della barra superiore.',
  'The main window gathers the transport bar, the editing tools, the timeline with the tracks, the video viewer and the mixer. The parts can be shown or hidden with the **VIDEO** and **MIXER** buttons in the top bar.')
fig('main', 'La finestra principale con una sessione di esempio.', 'The main window with a sample session.', maxw=1500, callouts=[
    (260, 95, 't', ('Barra del trasporto: riavvolgi, play, stop, registra e display del timecode.', 'Transport bar: rewind, play, stop, record and the timecode display.')),
    (790, 95, 't', ('Opzioni: LOOP, SNAP, SEGUI, monitor d\'ingresso, zoom e adatta.', 'Options: LOOP, SNAP, FOLLOW, input monitor, zoom and fit.')),
    (1568, 95, 't', ('Pulsanti di sessione: + TRACCIA, IMPORTA, ESPORTA, VIDEO, MIXER.', 'Session buttons: + TRACK, IMPORT, EXPORT, VIDEO, MIXER.')),
    (1865, 95, 't', ('ESCI: chiude il programma (chiede se salvare).', 'QUIT: closes the program (asks whether to save).')),
    (360, 175, 't', ('Strumenti di modifica (selezione, lama, slip, stretch, mano, zoom, range, mute, gomma, comp, Time Warp, inviluppo, scrub, automazione).', 'Editing tools (select, razor, slip, stretch, hand, zoom, range, mute, eraser, comp, Time Warp, envelope, scrub, automation).')),
    (940, 175, 't', ('Pulsanti di modifica: DIVIDI, DUPLICA, ELIMINA, RIPPLE, TAGLIA IN/OUT, modalità.', 'Edit buttons: SPLIT, DUPLICATE, DELETE, RIPPLE, TRIM IN/OUT, mode.')),
    (800, 232, 'd', ('Righello con timecode, barra dell\'intervallo di esportazione e marker.', 'Ruler with timecode, export range bar and markers.')),
    (800, 310, 'd', ('Corsia del video con le miniature dei fotogrammi.', 'Video lane with frame thumbnails.')),
    (190, 430, 'd', ('Intestazioni delle tracce e dei gruppi.', 'Track and group headers.')),
    (900, 540, 'd', ('Clip sulle tracce e regioni dei cue ADR (zone colorate).', 'Clips on the tracks and ADR cue regions (coloured zones).')),
    (1640, 390, 'd', ('Lettore video con sovrimpressione ADR e controlli di offset.', 'Video viewer with ADR overlay and offset controls.')),
    (380, 950, 'd', ('Canali del mixer: uno per traccia.', 'Mixer channels: one per track.')),
    (1200, 950, 'd', ('Ritorni effetti A e B.', 'FX returns A and B.')),
    (1600, 950, 'd', ('Pannello di analisi: loudness e spettro.', 'Analysis panel: loudness and spectrum.')),
    (1918, 950, 'd', ('Canale master con limiter, MONO e DIM.', 'Master channel with limiter, MONO and DIM.')),
    (1000, 1278, 'd', ('Barra di stato: versione, uscita audio, frequenza, buffer e CPU.', 'Status bar: version, audio output, rate, buffer and CPU.')),
])

h3('La barra del trasporto', 'The transport bar')
fig('main', 'La barra superiore (a sinistra).', 'The top bar (left part).', maxw=1500, crop=(10, 50, 1262, 140), callouts=[
    (55, 95, 't', ('**Riavvolgi** (Home): porta la testina all\'inizio.', '**Rewind** (Home): moves the playhead to the start.')),
    (125, 95, 't', ('**Play / Stop** (barra spaziatrice).', '**Play / Stop** (space bar).')),
    (193, 95, 't', ('**Stop**: ferma la riproduzione; la testina resta dov\'è.', '**Stop**: stops playback; the playhead stays where it is.')),
    (263, 95, 't', ('**Registra** (R): registra sulle tracce armate; se nessuna è armata, sulla traccia selezionata.', '**Record** (R): records on the armed tracks; if none is armed, on the selected track.')),
    (475, 95, 't', ('**Timecode** della testina e secondi; il formato (23,976…60 fps) si sceglie in Impostazioni sessione.', '**Timecode** of the playhead and seconds; the format (23.976…60 fps) is set in Session settings.')),
    (690, 95, 't', ('**LOOP** (L): ripete la regione di loop (Maiusc + trascina sul righello per definirla).', '**LOOP** (L): repeats the loop region (Shift + drag on the ruler to define it).')),
    (777, 95, 't', ('**SNAP** (N): i bordi di clip e cue si agganciano a clip, testina, marker e fotogrammi.', '**SNAP** (N): edges of clips and cues snap to clips, playhead, markers and frames.')),
    (867, 95, 't', ('**SEGUI** (F): la vista segue la testina durante la riproduzione e, da fermo, quando la testina salta fuori dalla vista (per esempio con *Vai all\'inizio*).', '**FOLLOW** (F): the view follows the playhead during playback and, when stopped, whenever the playhead jumps out of view (for example with *Go to start*).')),
    (957, 95, 't', ('Indicatore del **livello d\'ingresso** del microfono.', '**Input level** meter of the microphone.')),
    (1013, 95, 't', ('**MON** (I): ascolta l\'ingresso mentre si registra (usare le cuffie per evitare il feedback).', '**MON** (I): hear the input while recording (use headphones to avoid feedback).')),
    (1130, 95, 't', ('**−** e **+**: zoom orizzontale della timeline.', '**−** and **+**: horizontal zoom of the timeline.')),
    (1231, 95, 't', ('**ADATTA**: mostra tutto il progetto nella finestra (Maiusc + Z).', '**FIT**: shows the whole project in the window (Shift + Z).')),
])
fig('main', 'La barra superiore (a destra).', 'The top bar (right part).', maxw=1500, crop=(1290, 50, 1915, 140), callouts=[
    (1368, 95, 't', ('**+ TRACCIA**: aggiunge una traccia (Maiusc + Cmd + T).', '**+ TRACK**: adds a track (Shift + Cmd + T).')),
    (1481, 95, 't', ('**IMPORTA**: importa file audio (Cmd + I).', '**IMPORT**: imports audio files (Cmd + I).')),
    (1581, 95, 't', ('**ESPORTA**: esporta il mix (Cmd + E).', '**EXPORT**: exports the mix (Cmd + E).')),
    (1672, 95, 't', ('**VIDEO**: mostra o nasconde il lettore video (Maiusc + V).', '**VIDEO**: shows or hides the video viewer (Shift + V).')),
    (1758, 95, 't', ('**MIXER**: mostra o nasconde il mixer (Maiusc + M).', '**MIXER**: shows or hides the mixer (Shift + M).')),
    (1865, 95, 't', ('**ESCI**: chiude il programma dopo aver chiesto se salvare.', '**QUIT**: closes the program after asking whether to save.')),
])
p('La riproduzione si ferma da sola un secondo dopo la fine del contenuto (clip, video o cue). Alla fine di una registrazione, o con **Trasporto > Alla fermata la testina torna dove era partita**, la testina ritorna al punto di partenza.',
  'Playback stops by itself one second after the end of the content (clips, video or cues). At the end of a recording, or with **Transport > On stop the playhead returns to where it started**, the playhead goes back to where it began.')

h3('Gli strumenti e i pulsanti di modifica', 'The tools and edit buttons')
fig('main', 'Gli strumenti.', 'The tools.', maxw=1500, crop=(10, 150, 725, 200), callouts=[
    (35, 174, 't', ('**Selezione** (V)', '**Select** (V)')),
    (86, 174, 't', ('**Lama** (B)', '**Razor** (B)')),
    (136, 174, 't', ('**Slip** (Y)', '**Slip** (Y)')),
    (185, 174, 't', ('**Stretch** (E)', '**Stretch** (E)')),
    (236, 174, 't', ('**Mano** (H)', '**Hand** (H)')),
    (286, 174, 't', ('**Zoom** (Z)', '**Zoom** (Z)')),
    (336, 174, 't', ('**Range** (X)', '**Range** (X)')),
    (386, 174, 't', ('**Mute** (K)', '**Mute** (K)')),
    (435, 174, 't', ('**Gomma** (G)', '**Eraser** (G)')),
    (485, 174, 't', ('**Comp** (C)', '**Comp** (C)')),
    (535, 174, 't', ('**Time Warp** (O)', '**Time Warp** (O)')),
    (585, 174, 't', ('**Inviluppo del clip** (D)', '**Clip envelope** (D)')),
    (636, 174, 't', ('**Scrub** (J)', '**Scrub** (J)')),
    (686, 174, 't', ('**Automazione** (U)', '**Automation** (U)')),
])
fig('main', 'I pulsanti di modifica.', 'The edit buttons.', maxw=1500, crop=(725, 150, 1245, 200), callouts=[
    (780, 174, 't', ('**DIVIDI** (S)', '**SPLIT** (S)')),
    (880, 174, 't', ('**DUPLICA** (Cmd + D)', '**DUPLICATE** (Cmd + D)')),
    (983, 174, 't', ('**ELIMINA** (Canc)', '**DELETE** (Del)')),
    (1081, 174, 't', ('**RIPPLE** (Maiusc + Canc)', '**RIPPLE** (Shift + Del)')),
    (1185, 174, 't', ('**TAGLIA IN** (Q) – e, a destra, TAGLIA OUT (W) e il pulsante della modalità di modifica (NORMALE / RIPPLE / SHUFFLE).', '**TRIM IN** (Q) – and, further right, TRIM OUT (W) and the edit mode button (NORMAL / RIPPLE / SHUFFLE).')),
])
p('Passando il mouse su un pulsante compare un suggerimento che spiega cosa fa. Gli strumenti sono descritti nel dettaglio nel capitolo *Clip e montaggio*.',
  'Hovering over a button shows a tooltip that explains what it does. The tools are described in detail in the chapter *Clips and editing*.')

h3('La timeline', 'The timeline')
fig('main', 'La timeline: righello, video, intestazioni delle tracce e clip.', 'The timeline: ruler, video, track headers and clips.', maxw=1500, crop=(0, 200, 1270, 655), callouts=[
    (270, 233, 'd', ('Angolo in alto a sinistra: **M** toglie tutti i mute, **S** toglie tutti i solo, **V** gestisce la visibilità delle tracce. Il clic destro scelge i controlli mostrati nelle intestazioni.', 'Top-left corner: **M** clears all mutes, **S** clears all solos, **V** manages track visibility. Right-click chooses which controls the headers show.')),
    (700, 211, 'd', ('Barra dell\'**intervallo di esportazione**: trascina per sceglierlo, doppio clic per toglierlo, clic destro per le opzioni.', '**Export range** bar: drag to choose it, double-click to remove it, right-click for options.')),
    (800, 240, 'd', ('**Righello**: clic per spostare la testina, Maiusc + trascina per la regione di loop, doppio clic per toglierla; Alt + trascina per lo scrub.', '**Ruler**: click to move the playhead, Shift + drag for the loop region, double-click to clear it; Alt + drag to scrub.')),
    (480, 251, 'd', ('**Marker** (qui «Scena 12»): clic per andarci, doppio clic per rinominare, trascina per spostare, clic destro per il menu.', '**Markers** (here "Scene 12"): click to jump, double-click to rename, drag to move, right-click for the menu.')),
    (800, 310, 'd', ('**Corsia del video**: trascinala per cambiare l\'offset del video rispetto all\'audio.', '**Video lane**: drag it to change the video offset against the audio.')),
    (60, 382, 'd', ('Numero (colore della traccia) e **nome**: doppio clic per rinominare, trascina la maniglia a sinistra per spostare la traccia.', 'Number (track colour) and **name**: double-click to rename, drag the handle on the left to reorder the track.')),
    (190, 380, 'd', ('**L** blocca, **R** arma per la registrazione, **M** mute, **S** solo.', '**L** locks, **R** arms for recording, **M** mutes, **S** solos.')),
    (170, 411, 'd', ('Indicatore di livello della traccia.', 'Track level meter.')),
    (80, 439, 'd', ('**Gruppo di tracce** comprimibile (qui «Dialoghi») con M e S per tutte le sue tracce.', 'Collapsible **track group** (here "Dialogue") with M and S for all its tracks.')),
    (620, 475, 'd', ('**Clip** (qui un take ADR: «T2» è il secondo take; i take non attivi sono in grigio).', '**Clip** (here an ADR take: "T2" is the second take; inactive takes are greyed out).')),
    (620, 560, 'd', ('**Regione del cue ADR**: la zona colorata sul fondo, con le maniglie ai bordi, mostra ingresso e uscita del cue.', '**ADR cue region**: the coloured zone in the background, with handles on its edges, shows the cue\'s in and out points.')),
])
ul(('**Muovere la vista**: rotella o trascinamento a due dita per scorrere; **Cmd + rotella** (o pizzico sul trackpad) per lo zoom orizzontale; **Alt + rotella** per l\'altezza delle tracce; strumento *Mano* per trascinare la vista.',
    '**Moving the view**: wheel or two-finger drag to scroll; **Cmd + wheel** (or trackpad pinch) to zoom horizontally; **Alt + wheel** for track height; the *Hand* tool to drag the view.'),
   ('**Altezza delle tracce**: trascina il bordo inferiore di una traccia (Maiusc: tutte); doppio clic sul bordo per l\'altezza predefinita; **Vista > Adatta altezza tracce** le fa entrare tutte nella finestra.',
    '**Track height**: drag the bottom edge of a track (Shift: all); double-click the edge for the default height; **View > Fit track height** makes all of them fit the window.'),
   ('**Selezione**: clic su un clip; Maiusc + clic aggiunge; trascina su uno spazio vuoto per il rettangolo di selezione; Cmd + A seleziona tutto.',
    '**Selection**: click a clip; Shift + click adds; drag over an empty space for a selection rectangle; Cmd + A selects everything.'),
   ('**Menu contestuali**: clic destro su un clip, su una traccia, sul righello o su un marker.', '**Context menus**: right-click a clip, a track, the ruler or a marker.'))

h3('Il mixer', 'The mixer')
p('Il mixer mostra un canale per ogni traccia, i due ritorni effetti, il pannello di analisi e il master. In alto ogni canale riporta il nome e, sotto, il **ruolo** della traccia (Guida, Dialoghi, M&E, Effetti).',
  'The mixer shows one channel for every track, the two FX returns, the analysis panel and the master. At the top each channel shows the name and, below it, the track **role** (Guide, Dialogue, M&E, Effects).')
fig('main', 'Un canale del mixer (traccia Guida).', 'A mixer channel (Guide track).', maxw=950, crop=(5, 675, 305, 1245), callouts=[
    (80, 697, 'l', ('Nome e **ruolo** della traccia.', 'Track name and **role**.')),
    (80, 745, 'l', ('**Pan**: manopola da sinistra (L) a destra (R); doppio clic riporta al centro (C).', '**Pan** knob from left (L) to right (R); double-click returns to the centre (C).')),
    (79, 818, 'l', ('**M** (mute), **S** (solo) e **Ø**: inverte la polarità (fase).', '**M** (mute), **S** (solo) and **Ø**: inverts the polarity (phase).')),
    (79, 858, 'l', ('**SEND A** e **SEND B**: mandate ai ritorni effetti, dopo il fader; doppio clic le spegne.', '**SEND A** and **SEND B**: sends to the FX returns, post-fader; double-click turns them off.')),
    (38, 918, 'l', ('**EQ**: apre l\'editor di EQ e compressore; è acceso (blu) se la traccia ne usa uno.', '**EQ**: opens the EQ and compressor editor; it is lit (blue) when the track uses one.')),
    (106, 918, 'r', ('**Uscita** della traccia: MASTER oppure un bus di gruppo (Dialoghi, M&E, Effetti, Guida).', 'Track **output**: MASTER or a group bus (Dialogue, M&E, Effects, Guide).')),
    (62, 948, 'l', ('**LIM**: limiter di traccia dopo il fader (la freccia accanto sceglie il tetto).', '**LIM**: track limiter after the fader (the arrow next to it sets the ceiling).')),
    (80, 1020, 'l', ('**Inserti** (4 per canale): clic su «+ Inserisci» per aggiungere un effetto integrato o un plug-in VST3; clic destro per sposta, bypass, apri, rimuovi.', '**Inserts** (4 per channel): click "+ Insert" to add a built-in effect or a VST3 plug-in; right-click to move, bypass, open, remove.')),
    (55, 1115, 'l', ('**Fader** del volume (in dB; doppio clic = 0 dB) e indicatore di picco con scala.', 'Volume **fader** (in dB; double-click = 0 dB) and peak meter with scale.')),
    (80, 1222, 'l', ('Casella del valore del fader: si può scrivere un valore.', 'Fader value box: you can type a value.')),
])
fig('main', 'Ritorni effetti, pannello di analisi (loudness) e master.', 'FX returns, analysis panel (loudness) and master.', maxw=1500, crop=(1050, 675, 1995, 1250), callouts=[
    (1127, 700, 't', ('**FX A / FX B**: ritorni effetti con quattro inserti, fader e mute (M); ricevono le mandate delle tracce.', '**FX A / FX B**: effect returns with four inserts, fader and mute (M); they receive the tracks\' sends.')),
    (1590, 699, 't', ('Schede **LOUDNESS / SPETTRO** del pannello di analisi.', '**LOUDNESS / SPECTRUM** tabs of the analysis panel.')),
    (1600, 752, 't', ('**Standard di loudness** (EBU R128, ATSC A/85, ARIB, Netflix, podcast, streaming, audiolibri): imposta obiettivo e tetto.', '**Loudness standard** (EBU R128, ATSC A/85, ARIB, Netflix, podcast, streaming, audiobooks): sets target and ceiling.')),
    (1600, 830, 'd', ('Loudness **integrato** (LUFS) con l\'obiettivo del programma scelto.', '**Integrated** loudness (LUFS) with the chosen programme target.')),
    (1600, 940, 'd', ('Loudness **momentaneo** e, sotto, short-term, loudness range e true peak.', '**Momentary** loudness and, below, short-term, loudness range and true peak.')),
    (1486, 1224, 'b', ('**AZZERA**: azzera le misure.', '**RESET**: resets the measurements.')),
    (1712, 1224, 'b', ('**SOLO PARLATO**: conta solo i blocchi in cui c\'è parlato (misura «dialogue-gated»).', '**SPEECH ONLY**: counts only the blocks that contain speech ("dialogue-gated" measurement).')),
    (1918, 740, 'r', ('**LIMITER** del master (attacco anticipato 3 ms, rilascio 80 ms): nessun picco supera il tetto.', 'Master **LIMITER** (3 ms look-ahead, 80 ms release): no peak exceeds the ceiling.')),
    (1918, 773, 'r', ('**MONO** (controllo di compatibilità) e **DIM** (-20 dB): solo ascolto, non influiscono su esportazione e misure.', '**MONO** (compatibility check) and **DIM** (-20 dB): listening only, they do not affect export and measurements.')),
    (1918, 818, 'r', ('Manopola del **tetto del limiter** (doppio clic: -1 dB).', '**Limiter ceiling** knob (double-click: -1 dB).')),
    (1880, 1030, 'r', ('Fader e indicatore del master.', 'Master fader and meter.')),
])

h3('Il lettore video', 'The video viewer')
fig('main', 'Il pannello del video.', 'The video panel.', maxw=1300, crop=(1280, 150, 1995, 660), callouts=[
    (1330, 171, 't', ('Nome e formato del video.', 'Video name and format.')),
    (1764, 170, 't', ('**STACCA**: apre il video in una finestra separata (anche su un altro schermo); di nuovo per riattaccarlo.', '**DETACH**: opens the video in a separate window (even on another screen); again to re-attach.')),
    (1910, 170, 't', ('**TUTTO SCHERMO**: il video a schermo intero (Esc per uscire).', '**FULL SCREEN**: the video full screen (Esc to exit).')),
    (1640, 390, 'd', ('**Immagine**, con il timecode del fotogramma e, durante i cue ADR, la sovrimpressione.', '**Picture**, with the frame timecode and, during ADR cues, the overlay.')),
    (1400, 623, 'b', ('Timecode della testina.', 'Playhead timecode.')),
    (1662, 632, 'b', ('**−F / +F**: sposta il video di un fotogramma; **− / +**: di 1 ms (tieni premuto per ripetere).', '**−F / +F**: shift the video by one frame; **− / +**: by 1 ms (hold to repeat).')),
    (1785, 632, 'b', ('**Offset del video** in millisecondi: positivo = video in ritardo rispetto all\'audio. Doppio clic per scrivere un valore.', '**Video offset** in milliseconds: positive = video late against the audio. Double-click to type a value.')),
    (1960, 632, 'b', ('**0**: azzera l\'offset.', '**0**: resets the offset.')),
])
p('Il video viene letto con il motore del sistema (macOS) o con ffmpeg (Windows). Il suono del video viene estratto e messo sulla traccia **Guida**; il video si può anche togliere con **File > Rimuovi il video**.',
  'The video is read with the system engine (macOS) or with ffmpeg (Windows). The video\'s sound is extracted and put on the **Guide** track; the video can also be removed with **File > Remove video**.')
fig('main_cue', 'Con un cue ADR selezionato, sul video compaiono la barra scorrevole e il testo della battuta.', 'With an ADR cue selected, the streamer bar and the line of dialogue appear over the picture.', maxw=1500)
note('Quando si registra un cue, il video si mostra da solo se era nascosto. La sovrimpressione (numero del cue, personaggio, scena, timecode, battuta) si sceglie in *Impostazioni sessione*.',
     'When a cue is recorded the video shows itself if it was hidden. The overlay (cue number, character, scene, timecode, line) is chosen in *Session settings*.')

h3('I menu', 'The menus')
p('Tutte le funzioni sono raggiungibili dai menu in alto. Qui sotto, ogni menu con le sue scorciatoie; le funzioni sono spiegate nei capitoli successivi.',
  'All functions can be reached from the menus at the top. Below, each menu with its shortcuts; the functions are explained in the following chapters.')
menu('File', 'File', 'file', 'File')
menu('Modifica', 'Edit', 'edit', 'Edit')
menu('Traccia', 'Track', 'track', 'Track')
menu('Strumenti', 'Tools', 'tools', 'Tools')
menu('Marker', 'Marker', 'marker', 'Marker')
menu('ADR', 'ADR', 'adr', 'ADR')
menu('Trasporto', 'Transport', 'transport', 'Transport')
menu('Vista', 'View', 'view', 'View')
menu('Aiuto', 'Help', 'help', 'Help')
