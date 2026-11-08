"""Validate portable project invariants without installing an Android SDK."""
from pathlib import Path
import re, struct, zlib, zipfile, xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
assert re.fullmatch(r"\d+\.\d+\.\d+", (root / "VERSION").read_text().strip())
version=None;section=None;unreleased=False;released=False
for line in (root / "CHANGELOG.txt").read_text(encoding="utf-8").splitlines():
    if not line or line.startswith("#"):continue
    if line == "Unreleased":
        assert not released and not unreleased, "Only one leading Unreleased section is allowed"
        unreleased=True;section=None
    elif re.fullmatch(r"\d+\.\d+\.\d+(?:\s*\|.*)?",line):
        if not released:assert line.split("|")[0].strip() == (root / "VERSION").read_text().strip()
        released=True;unreleased=False;version=line.split("|")[0].strip();section=None
    elif line in {"[New]","[Improved]","[Fixed]"} or unreleased and line == "[Linux development]":
        assert version or unreleased;section=line
    else:assert (version or unreleased) and section and line.startswith("- "), "Changelog entry must belong to a release section"
assert released, "The current release must remain in the changelog"
for p in list((root / "helper").rglob("*.xml")) + list((root / "companion").rglob("*.xml")):
    ET.parse(p)
catalog = (root / "src/localization.inc").read_text(encoding="utf-8")
entries = re.findall(r'\{L"((?:[^"\\]|\\.)*)",', catalog)
assert entries and entries == sorted(set(entries)), "Translation keys must be sorted and unique"
for p in (root / "wallpapers").glob("*.png"):
    raw=p.read_bytes();assert raw[:8] == b"\x89PNG\r\n\x1a\n";at=8
    while at < len(raw):
        size=struct.unpack_from(">I",raw,at)[0];end=at+size+12;assert end <= len(raw)
        assert zlib.crc32(raw[at+4:at+8+size]) == struct.unpack_from(">I",raw,at+8+size)[0]
        assert raw[at+4:at+8] not in {b"caBX",b"tEXt",b"zTXt",b"iTXt",b"eXIf",b"tIME"}
        at=end
    assert at == len(raw)
with zipfile.ZipFile(root / "navigation/VeloxNavOrder.apk") as apk:
    assert apk.testzip() is None
    assert not any(n.endswith(".dex") for n in apk.namelist()), "Navigation overlay must remain resource-only"
for name in ["LICENSE.txt","release/THIRD-PARTY-NOTICES.txt","third_party/rootAVD/LICENSE","navigation/LICENSE.txt"]:
    assert (root / name).is_file()
print("PASS: version, Android XML, translation ordering, wallpaper integrity, resource-only overlay and license files")
