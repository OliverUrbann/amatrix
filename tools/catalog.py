#!/usr/bin/env python3
"""Amiga catalogs for Amatrix, without FlexCat.

  catalog.py header  catalogs/amatrix.cd gui/amatrix_strings.h
  catalog.py catalog catalogs/amatrix.cd catalogs/deutsch.ct out/amatrix.catalog
  catalog.py check   catalogs/amatrix.cd catalogs/deutsch.ct

amatrix.cd holds the built-in (English) strings, one per ID:
    MSG_NAME (number//)
    text
    ;
deutsch.ct the translation (FlexCat style):
    ## version $VER: amatrix.catalog 1.0 (14.11.2026)
    ## language deutsch
    MSG_NAME
    Text
    ; comment
Text uses C escapes (\\n, \\33, \\", \\xNN).  The files are UTF-8; the
catalog is ISO-8859-1 (codeset 0).  A translation must have the same printf
conversions in the same order as the original - otherwise it is refused,
since a wrong %s crashes the program.
"""
import re
import struct
import sys

FMT = re.compile(r'%[-+ #0]*(\d+|\*)?(\.(\d+|\*))?(hh|h|ll|l)?[diouxXcsfgeEp%]')


def read_cd(path):
    """[(name, number, text)]"""
    out, lines, i = [], open(path, encoding='utf-8').read().split('\n'), 0
    while i < len(lines):
        ln = lines[i]
        m = re.match(r'^(MSG_\w+)\s*\((\d*)/\d*/\d*\)\s*$', ln)
        if m:
            text = lines[i + 1]
            i += 2
            while i < len(lines) and lines[i].endswith('\\') and False:
                i += 1
            out.append((m.group(1), int(m.group(2)) if m.group(2) else len(out), text))
            continue
        i += 1
    nums = [n for _, n, _ in out]
    if len(set(nums)) != len(nums):
        sys.exit(f'{path}: an ID number is used twice')
    names = [n for n, _, _ in out]
    if len(set(names)) != len(names):
        sys.exit(f'{path}: an ID name is used twice')
    return out


def read_ct(path):
    """({name: text}, version, language)"""
    tr, ver, lang = {}, None, None
    lines = open(path, encoding='utf-8').read().split('\n')
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith('## version'):
            ver = ln[len('## version'):].strip()
        elif ln.startswith('## language'):
            lang = ln[len('## language'):].strip()
        elif re.match(r'^MSG_\w+\s*$', ln):
            name = ln.strip()
            if i + 1 < len(lines) and lines[i + 1] and not lines[i + 1].startswith(';'):
                tr[name] = lines[i + 1]
            i += 2
            continue
        i += 1
    return tr, ver, lang


def cbytes(text):
    """C-escaped UTF-8 text -> ISO-8859-1 bytes"""
    out, i = bytearray(), 0
    while i < len(text):
        c = text[i]
        if c == '\\' and i + 1 < len(text):
            d = text[i + 1]
            if d in 'nrt"\\\'':
                out += {'n': b'\n', 'r': b'\r', 't': b'\t', '"': b'"', '\\': b'\\', "'": b"'"}[d]
                i += 2
            elif d == 'x':
                m = re.match(r'[0-9a-fA-F]{1,2}', text[i + 2:])
                out.append(int(m.group(0), 16))
                i += 2 + len(m.group(0))
            elif d in '01234567':
                m = re.match(r'[0-7]{1,3}', text[i + 1:])
                out.append(int(m.group(0), 8))
                i += 1 + len(m.group(0))
            elif d == 'e':
                out.append(27)
                i += 2
            else:
                sys.exit(f'unknown escape \\{d} in: {text}')
        else:
            out += c.encode('iso-8859-1')
            i += 1
    return bytes(out)


def fmts(text):
    return [m.group(0)[-1] + (m.group(4) or '') for m in FMT.finditer(text.replace('%%', ''))]


def check(cd, tr):
    ok, names = True, {n for n, _, _ in cd}
    for name in tr:
        if name not in names:
            print(f'unknown ID in translation: {name}')
            ok = False
    missing = 0
    for name, _, text in cd:
        if name not in tr:
            missing += 1
            continue
        if fmts(text) != fmts(tr[name]):
            print(f'{name}: % conversions differ: {fmts(text)} vs {fmts(tr[name])}')
            ok = False
        try:
            cbytes(tr[name])
        except UnicodeEncodeError:
            print(f'{name}: not ISO-8859-1: {tr[name]}')
            ok = False
    if missing:
        print(f'{missing} strings untranslated (the English ones are used)')
    return ok


def header(cd, out):
    with open(out, 'w', encoding='utf-8') as f:
        f.write('/* made by tools/catalog.py from catalogs/amatrix.cd - do not edit */\n'
                '#ifndef AMATRIX_STRINGS_H\n#define AMATRIX_STRINGS_H\n\nenum {\n')
        for name, num, _ in cd:
            f.write(f'    {name} = {num},\n')
        top = max(n for _, n, _ in cd) + 1
        f.write(f'    MSG_COUNT = {top}\n}};\n\nstatic const char *const amx_builtin[MSG_COUNT] = {{\n')
        for name, num, text in sorted(cd, key=lambda x: x[1]):
            f.write(f'    [{name}] = "{text}",\n')
        f.write('};\n\n#endif\n')


def catalog(cd, tr, ver, lang, out):
    def chunk(tag, data):
        if len(data) & 1:
            data += b'\0'
        return tag + struct.pack('>I', len(data) - (0)) + data
    nums = {n: num for n, num, _ in cd}
    strs = bytearray()
    for name, num, _ in sorted(cd, key=lambda x: x[1]):
        if name not in tr:
            continue
        b = cbytes(tr[name]) + b'\0'
        strs += struct.pack('>II', nums[name], len(b)) + b
        strs += b'\0' * (-len(b) % 4)
    body = b'CTLG'
    body += chunk(b'FVER', ver.encode('iso-8859-1') + b'\0')
    body += chunk(b'LANG', lang.encode('iso-8859-1') + b'\0')
    body += chunk(b'CSET', struct.pack('>I', 0) + b'\0' * 28)
    body += chunk(b'STRS', bytes(strs))
    with open(out, 'wb') as f:
        f.write(b'FORM' + struct.pack('>I', len(body)) + body)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    cmd, cd = sys.argv[1], read_cd(sys.argv[2])
    if cmd == 'header':
        header(cd, sys.argv[3])
    elif cmd in ('catalog', 'check'):
        tr, ver, lang = read_ct(sys.argv[3])
        if not check(cd, tr):
            sys.exit(1)
        if cmd == 'catalog':
            if not ver or not lang:
                sys.exit('## version and ## language are needed')
            catalog(cd, tr, ver, lang, sys.argv[4])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
