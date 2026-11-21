#!/usr/bin/env python3
"""Rifa' la figura media/main_topleft.jpg del manuale (barra alta + barra del trasporto con Pausa) da due
istantanee della finestra (ADR_DEMO_SNAP, scala 2): uso  manual_transport.py main_it.png main_en.png
Poi: tools/manual_patch.py VER manual-src/edits-VER.json (rinumera la legenda 3.1)."""
import io, os, sys, zipfile
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
K = 1500 / 2128.0          # scala della figura rispetto all'istantanea (pixel pieni)
FONT = '/System/Library/Fonts/Helvetica.ttc'

def circle(d, x, y, n, r=13):
    d.ellipse((x - r, y - r, x + r, y + r), fill=(244, 124, 32), outline=(255, 255, 255), width=2)
    f = ImageFont.truetype(FONT, int(r * 1.25), index=1); t = str(n); b = d.textbbox((0, 0), t, font=f)
    d.text((x - (b[2] + b[0]) / 2, y - (b[3] + b[1]) / 2 - 1), t, fill=(255, 255, 255), font=f)

def figure(png):
    im = Image.open(png).convert('RGB')
    out = Image.new('RGB', (1500, 204), im.getpixel((1700, 1100)))
    out.paste(im.crop((0, 0, 2128, 170)).resize((1500, 120), Image.LANCZOS), (0, 0))
    out.paste(im.crop((993, 1175, 2071, 1290)).resize((760, 81), Image.LANCZOS), (0, 123))
    d = ImageDraw.Draw(out)
    sx = lambda x: x * 1.85 * K                  # x del pannello (punti) -> figura, barra alta
    st = lambda x: (x * 1.85 - 993) * K          # idem, barra del trasporto
    for n, x in zip((1, 2, 3, 4, 5), (673, 715, 757, 797, 839)): circle(d, st(x), 188, n)
    circle(d, sx(999), 100, 6)
    for n, x in zip((7, 8, 9, 10, 11, 12, 13), (52, 119, 189, 259, 303, 473, 531)): circle(d, sx(x), 76, n)
    return out

def main():
    for lang, png in zip(('it', 'en'), sys.argv[1:3]):
        buf = io.BytesIO(); figure(png).save(buf, 'JPEG', quality=88)
        zp = os.path.join(ROOT, 'Resources', 'manual', f'manual_{lang}.zip')
        with zipfile.ZipFile(zp) as z: items = [(i, z.read(i.filename)) for i in z.infolist()]
        with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED) as z:
            for i, data in items: z.writestr(i, buf.getvalue() if i.filename == 'media/main_topleft.jpg' else data)
        figure(png).save(f'/tmp/main_topleft_{lang}.jpg', quality=88)
        print(lang, 'ok')

if __name__ == '__main__': main()
