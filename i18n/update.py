#!/usr/bin/env python3
"""Collects the strings marked with i18n("…") / I18N_NOOP("…") in src/ into
i18n/luna_<lang>.ts (Qt Linguist format, one context "Luna").

    i18n/update.py                 update every i18n/luna_*.ts
    i18n/update.py fr              also start i18n/luna_fr.ts

Translations already in a file are kept; new strings come in as
"unfinished", strings gone from the code are marked "vanished". Edit the
files with Qt Linguist or any text editor, then rebuild: Luna embeds them.
"""
import glob
import os
import re
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src')
STR = r'"(?:[^"\\\n]|\\.)*"'
CALL = re.compile(r'\b(?:i18n|I18N_NOOP)\(\s*((?:' + STR + r'\s*)+)\)')
ESC = {'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'"}


def unescape(lit):
    return re.sub(r'\\(.)', lambda m: ESC.get(m.group(1), m.group(0)), lit)


def collect():
    found = {}   # source -> first "file:line"
    for f in sorted(glob.glob(os.path.join(SRC, '*.cpp')) + glob.glob(os.path.join(SRC, '*.h'))):
        text = open(f, encoding='utf-8').read()
        for m in CALL.finditer(text):
            s = ''.join(unescape(p[1:-1]) for p in re.findall(STR, m.group(1)))
            if s and s not in found:
                found[s] = (os.path.basename(f), text.count('\n', 0, m.start()) + 1)
    return found


def update(path, lang, found):
    old = {}
    if os.path.exists(path):
        for msg in ET.parse(path).getroot().iter('message'):
            src, tr = msg.find('source').text or '', msg.find('translation')
            old[src] = (tr.text or '', tr.get('type'))
    ts = ET.Element('TS', version='2.1', language=lang, sourcelanguage='es_ES')
    ctx = ET.SubElement(ts, 'context')
    ET.SubElement(ctx, 'name').text = 'Luna'
    done = todo = 0
    for src, (fname, line) in found.items():
        msg = ET.SubElement(ctx, 'message')
        ET.SubElement(msg, 'location', filename='../src/' + fname, line=str(line))
        ET.SubElement(msg, 'source').text = src
        text, kind = old.get(src, ('', 'unfinished'))
        tr = ET.SubElement(msg, 'translation')
        if kind in ('vanished', 'obsolete'):
            kind = None if text else 'unfinished'
        if kind:
            tr.set('type', kind)
        tr.text = text
        if text and not kind:
            done += 1
        else:
            todo += 1
    for src, (text, kind) in old.items():   # keep gone strings' work around
        if src not in found and text:
            msg = ET.SubElement(ctx, 'message')
            ET.SubElement(msg, 'source').text = src
            ET.SubElement(msg, 'translation', type='vanished').text = text
    ET.indent(ts)
    with open(path, 'w', encoding='utf-8') as out:
        out.write('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n')
        out.write(ET.tostring(ts, encoding='unicode'))
        out.write('\n')
    print(f'{os.path.basename(path)}: {done} traducidas, {todo} pendientes')


def main():
    found = collect()
    print(f'{len(found)} textos en el código')
    langs = {os.path.basename(p)[5:-3] for p in glob.glob(os.path.join(HERE, 'luna_*.ts'))}
    langs.update(sys.argv[1:])
    for lang in sorted(langs):
        update(os.path.join(HERE, f'luna_{lang}.ts'), lang, found)


if __name__ == '__main__':
    main()
