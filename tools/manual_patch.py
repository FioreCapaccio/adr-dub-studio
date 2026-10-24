#!/usr/bin/env python3
"""Aggiorna il manuale (IT/EN) partendo dagli zip gia' pronti in Resources/manual/ (fonte di verita'),
applica le modifiche di testo, aggiorna la versione, rifa' gli zip e i PDF (Chrome headless) in docs/.

uso: tools/manual_patch.py VERSIONE [modifiche.json]
modifiche.json: {"it": [["vecchio html", "nuovo html"], ...], "en": [...]}  (ogni 'vecchio' deve comparire una sola volta)
"""
import base64, json, mimetypes, os, re, shutil, subprocess, sys, tempfile, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'


def main():
    version = sys.argv[1]
    edits = json.load(open(sys.argv[2], encoding='utf-8')) if len(sys.argv) > 2 else {}
    for lang in ('it', 'en'):
        zp = os.path.join(ROOT, 'Resources', 'manual', f'manual_{lang}.zip')
        work = tempfile.mkdtemp(prefix='manual_')
        with zipfile.ZipFile(zp) as z:
            z.extractall(work)
        logo = os.path.join(work, 'media', 'logo.png')   # copertina: icona attuale dell'app
        if os.path.exists(logo):
            subprocess.run(['sips', '-Z', '256', os.path.join(ROOT, 'Resources', 'icon.png'), '--out', logo], capture_output=True)
        idx = os.path.join(work, 'index.html')
        html = open(idx, encoding='utf-8').read()
        for old, new in edits.get(lang, []):
            n = html.count(old)
            if n != 1:
                sys.exit(f'{lang}: il testo da sostituire compare {n} volte: {old[:80]!r}')
            html = html.replace(old, new)
        # versione nella barra e nella copertina ("Manuale utente 2.32.0", "versione 2.32.0", "User manual ...", "version ...")
        html, k = re.subn(r'((?:[Vv]ersione?|Manuale utente|User manual) )\d+\.\d+\.\d+', r'\g<1>' + version, html)
        open(idx, 'w', encoding='utf-8').write(html)
        # zip per l'app (immagini esterne)
        tmpzip = zp + '.tmp'
        with zipfile.ZipFile(tmpzip, 'w', zipfile.ZIP_STORED) as z:
            for root, _, files in os.walk(work):
                for f in sorted(files):
                    full = os.path.join(root, f)
                    z.write(full, os.path.relpath(full, work))
        os.replace(tmpzip, zp)
        # PDF: stesso HTML con le immagini incorporate
        def inline(m):
            path = os.path.join(work, m.group(2))
            if not os.path.exists(path):
                return m.group(0)
            mime = mimetypes.guess_type(path)[0] or 'image/png'
            return m.group(1) + 'data:%s;base64,%s' % (mime, base64.b64encode(open(path, 'rb').read()).decode()) + m.group(3)
        pdfsrc = re.sub(r'(src=")(media/[^"]+)(")', inline, html)
        src = os.path.join(work, 'pdfsrc.html')
        open(src, 'w', encoding='utf-8').write(pdfsrc)
        pdf = os.path.join(ROOT, 'docs', f'ADR-Dub-Studio-Manual-{lang.upper()}.pdf')
        subprocess.run([CHROME, '--headless=new', '--disable-gpu', '--no-pdf-header-footer', '--print-to-pdf=' + pdf, 'file://' + src],
                       capture_output=True, timeout=300)
        shutil.rmtree(work, ignore_errors=True)
        print(f'{lang}: versione aggiornata in {k} punti, zip {os.path.getsize(zp) // 1024} KB, pdf {os.path.getsize(pdf) // 1024} KB')


if __name__ == '__main__':
    main()
