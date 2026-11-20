#!/usr/bin/env python3
"""Sostituisce le immagini del capitolo 3 (e temi, tool) negli zip del manuale con quelle rifatte dal film (cartella out_it / out_en).
uso: tools/manual_images.py CARTELLA_IMMAGINI_BASE   (con out_it/ e out_en/ dentro, piu' tool{1,2,3}_{it,en}.jpg)"""
import os, sys, zipfile, shutil
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
base = sys.argv[1]
order = ['main_topleft', 'main_topright', 'main_tools', 'main_edit', 'main_timeline', 'main_mixer', 'main_fx', 'main_video']
for L in ('it', 'en'):
    zp = os.path.join(ROOT, 'Resources', 'manual', f'manual_{L}.zip')
    z = zipfile.ZipFile(zp)
    files = {n: z.read(n) for n in z.namelist()}
    html = files['index.html'].decode('utf-8')
    parts = html.split('src="media/main.jpg"')
    if len(parts) != 10: sys.exit(f'{L}: attese 9 figure con main.jpg, trovate {len(parts)-1}')
    new = parts[0] + 'src="media/main.jpg"'
    for name, rest in zip(order, parts[2:]):
        new += parts[1] if False else ''
    # ricostruzione: la prima resta main.jpg, le altre prendono i nuovi nomi in ordine
    new = parts[0] + 'src="media/main.jpg"' + parts[1]
    for name, rest in zip(order, parts[2:]):
        new += f'src="media/{name}.jpg"' + rest
    if L == 'it':
        new = new.replace('La barra superiore (a sinistra).', 'La barra superiore (a sinistra) e, sotto, i tasti del trasporto.')
        new = new.replace('(qui «Scena 12»)', '(qui «Scena 2»)')
    else:
        new = new.replace('The top bar (left part).', 'The top bar (left part) and, below, the transport buttons.')
        new = new.replace('(here "Scene 12")', '(here "Scene 2")')
    files['index.html'] = new.encode('utf-8')
    out = os.path.join(base, f'out_{L}')
    for n in order + ['main', 'main_cue', 'theme_light', 'theme_d5']:
        files[f'media/{n}.jpg'] = open(os.path.join(out, n + '.jpg'), 'rb').read()
    for k, n in ((1, 'tool_range'), (2, 'tool_warp'), (3, 'tool_envelope')):
        files[f'media/{n}.jpg'] = open(os.path.join(base, f'tool{k}_{L}.jpg'), 'rb').read()
    tmp = zp + '.tmp'
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_STORED) as o:
        for n, d in files.items(): o.writestr(n, d)
    shutil.move(tmp, zp)
    print(L, 'ok', len(files), 'file')
