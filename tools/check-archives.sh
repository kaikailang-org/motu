#!/bin/sh
# Writes the zip and EPUB fixtures through the test suite, then checks them
# with tools that do not share our code: unzip, zipinfo, Python's zipfile
# and XML parser, and epubcheck when it is installed.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/archives"
cd "$root"

kai test tests/zip_test.kai
kai test tests/epub_test.kai

for z in "$out/stored.zip" "$out/deflated.zip" "$out/book.epub" "$out/book-cover.epub"; do
  echo "== $z"
  unzip -t "$z"
  zipinfo -v "$z" | grep -E '^Central directory entry|compression method|32-bit CRC|compressed size|uncompressed size|length of extra field'
done

python3 -I - "$out" <<'PY'
import os, sys, xml.dom.minidom, zipfile
out = sys.argv[1]
for name in ("stored.zip", "deflated.zip", "book.epub", "book-cover.epub"):
    z = zipfile.ZipFile(os.path.join(out, name))
    bad = z.testzip()
    print(name, "testzip:", bad, z.namelist())
    assert bad is None, name
with zipfile.ZipFile(os.path.join(out, "deflated.zip")) as z:
    original = open(os.path.join(out, "prose.txt"), "rb").read()
    info = z.getinfo("prose.txt")
    assert z.read("prose.txt") == original
    print("prose.txt round-trips: %d bytes, %d compressed (%.1f%%)"
          % (info.file_size, info.compress_size, 100.0 * info.compress_size / info.file_size))
for name in ("book.epub", "book-cover.epub"):
    with zipfile.ZipFile(os.path.join(out, name)) as z:
        first = z.infolist()[0]
        assert first.filename == "mimetype" and first.compress_type == 0 and first.extra == b""
        for n in z.namelist():
            if n.endswith((".xhtml", ".opf", ".ncx", ".xml")):
                xml.dom.minidom.parseString(z.read(n))
        print(name, "mimetype first and stored; every XML document parses")
PY

if command -v epubcheck >/dev/null 2>&1; then
  epubcheck "$out/book.epub"
  epubcheck "$out/book-cover.epub"
else
  echo "epubcheck not installed; skipped"
fi
echo "all archive checks passed"
