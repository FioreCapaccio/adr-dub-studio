#!/usr/bin/env python3
"""Seconda passata sulle immagini del manuale: finestre (ADR, copione, pool, direttore, EQ, Sample Editor, Info Line, impostazioni, controllo)
rifatte dal film, e la legenda della finestra ADR aggiornata (UNISCI, APPROVA TUTTI, colonna Appr.).
uso: tools/manual_images2.py CARTELLA   (con out2_it/ e out2_en/ dentro)"""
import os, re, sys, zipfile, shutil
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
base = sys.argv[1]
names = ['adr_selected', 'editor_wave', 'editor_spectro', 'script', 'pool', 'director', 'eq_editor', 'infoline', 'dlg_check', 'dlg_session_settings', 'workflow', 'recovery']
NEW = {
 'it': {4: "<b>UNISCI</b>: unisce i cue selezionati (almeno due: Cmd/Ctrl o Maiusc + clic sulle righe) in uno solo, dal primo ingresso all'ultima uscita, con le battute una dopo l'altra. Si annulla con un solo Annulla.",
        11: "<b>APPROVA TUTTI</b>: approva tutti i cue mostrati (segue i filtri) che hanno almeno un take. Si annulla con un solo Annulla.",
        13: "Colonna <b>Appr.</b>: la casella approva il cue (il suo take attivo); serve almeno un take. Per togliere l'approvazione si toglie la spunta."},
 'en': {4: "<b>MERGE</b>: merges the selected cues (at least two: Cmd/Ctrl or Shift + click on the rows) into one, from the first in-point to the last out-point, with the lines joined in order. A single Undo brings them back.",
        11: "<b>APPROVE ALL</b>: approves all the shown cues (it follows the filters) that have at least one take. A single Undo reverts it.",
        13: "<b>Appr.</b> column: the checkbox approves the cue (its active take); at least one take is needed. Untick it to remove the approval."}}
# vecchio numero -> nuovo numero (i nuovi 4, 11, 13 sono inseriti)
MAP = {1: 1, 2: 2, 3: 3, 4: 5, 5: 6, 6: 7, 7: 8, 8: 9, 9: 10, 10: 12, 11: 14, 12: 15, 13: 16}
MAP.update({o: o + 3 for o in range(14, 24)})
for L in ('it', 'en'):
    zp = os.path.join(ROOT, 'Resources', 'manual', f'manual_{L}.zip')
    z = zipfile.ZipFile(zp)
    files = {n: z.read(n) for n in z.namelist()}
    html = files['index.html'].decode('utf-8')
    i = html.index('media/adr_selected.jpg'); a = html.index('<ol class="legend">', i); b = html.index('</ol>', a)
    items = re.findall(r'<li>(.*?)</li>', html[a:b], re.S)
    if len(items) == 26: print(L, 'legenda gia aggiornata'); lis = None
    else:
        assert len(items) == 23, len(items)
        out = {}
        for it in items:
            m = re.match(r'<span class="n">(\d+)</span>(.*)', it, re.S); out[MAP[int(m.group(1))]] = m.group(2)
        for n, t in NEW[L].items(): out[n] = f'<span>{t}</span>'
        assert sorted(out) == list(range(1, 27))
        lis = ''.join(f'<li><span class="n">{n}</span>{out[n]}</li>' for n in range(1, 27))
        html = html[:a] + '<ol class="legend">' + lis + html[b:]
    files['index.html'] = html.encode('utf-8')
    for n in names:
        files[f'media/{n}.jpg'] = open(os.path.join(base, f'out2_{L}', n + '.jpg'), 'rb').read()
    tmp = zp + '.tmp'
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_STORED) as o:
        for n, d in files.items(): o.writestr(n, d)
    shutil.move(tmp, zp)
    print(L, 'ok')
