#!/usr/bin/env python3
"""Builds an AmigaGuide file (ISO-8859-1) from guide/<lang>.txt + guide/arexx.txt.
usage: mkguide.py LANG(en|de) OUT.guide

Source layout: text before the first heading is the start page; a line
underlined with ---- starts a node; at column 0 "1. Title" starts a child
node (First start).  arexx.txt: "--- Group ---" lines are group nodes, each
command line at column 0 (NAME TEMPLATE) starts a command node."""
import re, sys, os
here = os.path.dirname(os.path.abspath(__file__))
lang, out = sys.argv[1], sys.argv[2]
T = {'en': dict(title='Amatrix 0.2 - Matrix client for AmigaOS', contents='Contents',
                ax='ARexx reference', idx='Command index', tmpl='TEMPLATE', cmds='Commands',
                sub='Sections'),
     'de': dict(title='Amatrix 0.2 - Matrix-Client für AmigaOS', contents='Inhalt',
                ax='ARexx reference', idx='Befehlsindex', tmpl='TEMPLATE', cmds='Befehle',
                sub='Abschnitte')}[lang]
AXNOTE = {'en': '', 'de': 'Diese Referenz ist englisch.\n\n'}[lang]

def esc(s):
    return s.replace('\\', '\\\\').replace('@', '\\@')
def rd(n):
    return open(os.path.join(here, 'guide', n), encoding='utf-8').read().rstrip('\n').split('\n')

nodes = []   # dict(id,title,toc,body,links)
def node(id, title, toc, body):
    n = dict(id=id, title=title, toc=toc, body=body, kids=[])
    nodes.append(n); return n

def strip_blank(lines):
    while lines and not lines[0].strip(): lines = lines[1:]
    while lines and not lines[-1].strip(): lines = lines[:-1]
    return lines

# ---- main guide text -------------------------------------------------
L = rd(lang + '.txt')
secs = []   # (title, lines)
cur_t, cur = None, []
i = 0
while i < len(L):
    if i + 1 < len(L) and re.fullmatch(r'-{3,}', L[i + 1]) and L[i].strip() and (i == 0 or not L[i - 1].strip()):
        secs.append((cur_t, cur)); cur_t, cur = L[i], []; i += 2; continue
    cur.append(L[i]); i += 1
secs.append((cur_t, cur))

main = node('MAIN', T['title'], None, strip_blank(secs[0][1]))
prev = None
for k, (title, lines) in enumerate(secs[1:]):
    n = node('s%d' % k, title, 'MAIN', None)
    main['kids'].append(n)
    lines = strip_blank(lines)
    # child nodes "1. Title"
    parts, cur_c = [], None
    head = []
    for ln in lines:
        if re.match(r'\d\. \S', ln):
            cur_c = (ln, []); parts.append(cur_c)
        elif cur_c: cur_c[1].append(ln)
        else: head.append(ln)
    n['body'] = strip_blank(head)
    for j, (ct, cl) in enumerate(parts):
        c = node('s%d_%d' % (k, j), ct, n['id'], strip_blank(cl))
        n['kids'].append(c)

# ---- ARexx -------------------------------------------------------------
A = rd('arexx.txt')
axn = node('arexx', T['ax'], 'MAIN', None)
main['kids'].append(axn)
conv, groups, curg, curc = [], [], None, None
for ln in A[2:]:
    m = re.match(r'--- (.*?) -+$', ln)
    if m:
        curg = (m.group(1), []); groups.append(curg); curc = None; continue
    if curg is None: conv.append(ln); continue
    m = re.match(r'([A-Z][A-Z0-9]*)(?: (.*))?$', ln)
    if m:
        curc = [m.group(1), m.group(2) or '', []]; curg[1].append(curc)
    elif curc is not None:
        curc[2].append(ln)
    elif ln.strip():
        pass
axn['body'] = [AXNOTE.rstrip()] + [''] * (1 if AXNOTE else 0) + strip_blank(conv) if AXNOTE else strip_blank(conv)
allcmds = []
for gi, (gt, cmds) in enumerate(groups):
    g = node('ax_g%d' % gi, gt, 'arexx', None)
    axn['kids'].append(g)
    g['cmds'] = [(c[0], c[1]) for c in cmds]
    for name, tmpl, body in cmds:
        c = node('c_' + name, 'AMATRIX/' + name, g['id'], None)
        c['cmd'] = (name, tmpl, strip_blank(body))
        allcmds.append(name)
        g['kids'].append(c)
idxn = node('ax_idx', T['idx'], 'arexx', None)
axn['kids'].append(idxn)
idxn['allcmds'] = sorted(allcmds)

# ---- output ------------------------------------------------------------
def link(id, text):
    return '@{"%s" LINK %s}' % (text.replace('"', "'"), id)
def body_text(lines):
    s = esc('\n'.join(lines))
    s = s.replace('"ARexx reference"', link('arexx', 'ARexx reference'))
    return s
byid = {n['id']: n for n in nodes}
o = []
o.append('@database "Amatrix.guide"')
o.append('@$VER: Amatrix.guide 0.2 (15.11.2026)')
o.append('@author "Oliver Urbann"')
o.append('@(c) "Copyright (c) 2026 Oliver Urbann, MIT licence"')
o.append('@index ax_idx')
for n in nodes:
    o.append('')
    o.append('@node %s "%s"' % (n['id'], n['title'].replace('"', "'")))
    if n['toc']: o.append('@toc %s' % n['toc'])
    sib = byid[n['toc']]['kids'] if n['toc'] else []
    if n['id'] in [s['id'] for s in sib]:
        p = sib.index(n)
        if p > 0: o.append('@prev %s' % sib[p - 1]['id'])
        if p + 1 < len(sib): o.append('@next %s' % sib[p + 1]['id'])
    o.append('')
    if n['id'] == 'MAIN':
        o.append('@{b}%s@{ub}\n' % esc(n['title']))
    elif 'cmd' in n:
        pass
    else:
        o.append('@{b}%s@{ub}\n' % esc(n['title']))
    if 'cmd' in n:
        name, tmpl, body = n['cmd']
        o.append('@{b}NAME@{ub}\n    AMATRIX.1 / %s\n' % name)
        o.append('@{b}TEMPLATE@{ub}\n    %s\n' % esc(tmpl or '(none)'))
        o.append('@{b}FUNCTION@{ub}')
        o.append(esc('\n'.join(body)))
    elif 'allcmds' in n:
        o.append('  '.join(link('c_' + c, c) for c in n['allcmds']))
    else:
        if n['body']: o.append(body_text(n['body']))
        if 'cmds' in n:
            o.append('')
            for c, t in n['cmds']:
                o.append('  ' + link('c_' + c, c) + ' ' * max(1, 14 - len(c)) + esc(t))
        if n['kids'] and 'cmds' not in n and n['id'] != 'arexx':
            o.append('')
            if n['id'] == 'MAIN': o.append('@{b}%s@{ub}\n' % T['contents'])
            else: o.append('@{b}%s@{ub}\n' % T['sub'])
            for k in n['kids']:
                o.append('  ' + link(k['id'], k['title']))
        if n['id'] == 'arexx':
            o.append('')
            for k in n['kids']:
                o.append('  ' + link(k['id'], k['title']))
    o.append('@endnode')
data = '\n'.join(o) + '\n'
open(out, 'wb').write(data.encode('latin-1'))
print(out, len(nodes), 'nodes')
