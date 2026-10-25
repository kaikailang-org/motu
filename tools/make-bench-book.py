#!/usr/bin/env python3
"""Writes a book project of about 150 000 words in 400 documents, for measuring."""
import os, re, sys

out = sys.argv[1]
src = open(os.path.join(os.path.dirname(__file__), "..", "tests", "data", "harbour.md")).read()
paras = [p for p in re.split(r"\n\s*\n", src) if p.strip() and not p.startswith(("#", "[^"))]
for d in ["docs", "notes", "snapshots", "research"]:
    os.makedirs(os.path.join(out, d), exist_ok=True)
nodes = [("manuscript", "", "folder", "Manuscript"), ("research", "", "folder", "Research"), ("trash", "", "folder", "Trash")]
words, i = 0, 0
for part in range(20):
    pid = f"part{part}"
    nodes.append((pid, "manuscript", "folder", f"Part {part + 1}"))
    for ch in range(20):
        cid = f"ch{part:02d}{ch:02d}"
        nodes.append((cid, pid, "doc", f"Chapter {part * 20 + ch + 1}"))
        body = []
        while sum(len(b.split()) for b in body) < 375:
            body.append(paras[i % len(paras)]); i += 1
        text = "\n\n".join(body) + "\n"
        words += len(text.split())
        with open(os.path.join(out, "docs", cid + ".md"), "w") as f:
            f.write(f'+++\ntitle = "Chapter {part * 20 + ch + 1}"\nsynopsis = ""\nlabel = "draft"\nstatus = "first draft"\nkeywords = []\ntarget = 400\n+++\n\n' + text)
with open(os.path.join(out, "project.toml"), "w") as f:
    f.write('title = "Bench"\nauthor = "Motu"\nlanguage = "en"\ntarget = 150000\nlabels = ["none", "draft", "revised", "final"]\nstatuses = ["to do", "first draft", "revised", "done"]\n\n')
    for n in nodes:
        f.write('[[node]]\nid = "%s"\nparent = "%s"\nkind = "%s"\ntitle = "%s"\nfile = ""\n\n' % n)
print(f"{out}: {sum(1 for n in nodes if n[2] == 'doc')} documents, about {words} words")
