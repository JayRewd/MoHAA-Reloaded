#!/usr/bin/env python3
"""Lists the shaders of a MOHAA map with their properties and where their
surfaces are, to pick camera positions for tests (standard library only).

  bspinfo.py <map, e.g. dm/mohdm1> [keyword ...]

Prints the worldspawn keys (farplane fog, sky), then every shader used by
world surfaces whose script contains one of the keywords (default:
polygonoffset), with the centers of its first surfaces. Reads the pk3 files
of $MOH_BASEPATH/main (default ~/Documents/moh/main). The player spawn
points (for tele) are info_player_* entities in the entity lump.
"""

import glob
import os
import re
import struct
import sys
import zipfile

BASE = os.path.join(os.environ.get("MOH_BASEPATH", os.path.expanduser("~/Documents/moh")), "main")


def pk3s():
    # later pk3s override earlier ones, like the game (alphabetical order)
    return sorted(glob.glob(os.path.join(BASE, "*.pk3")), key=lambda p: os.path.basename(p).lower())


def read_file(name):
    data = None
    for p in pk3s():
        with zipfile.ZipFile(p) as z:
            for n in z.namelist():
                if n.lower() == name.lower():
                    data = z.read(n)
    return data


def shader_scripts():
    text = {}
    for p in pk3s():
        with zipfile.ZipFile(p) as z:
            for n in z.namelist():
                if n.lower().startswith("scripts/") and n.lower().endswith(".shader"):
                    text[n.lower()] = z.read(n).decode("latin-1")
    shaders = {}
    for body in text.values():
        body = re.sub(r"//[^\n]*", "", body)
        pos = 0
        while True:
            m = re.search(r"([^\s{}]+)\s*\{", body[pos:])
            if not m:
                break
            name = m.group(1).lower()
            start = pos + m.end()
            depth, i = 1, start
            while i < len(body) and depth:
                if body[i] == "{":
                    depth += 1
                elif body[i] == "}":
                    depth -= 1
                i += 1
            shaders.setdefault(name, body[start:i - 1])
            pos = i
    return shaders


def main():
    mapname = sys.argv[1]
    keywords = [k.lower() for k in sys.argv[2:]] or ["polygonoffset"]
    bsp = read_file(f"maps/{mapname}.bsp")
    if bsp is None:
        sys.exit("map not found")
    ident, version, checksum = struct.unpack_from("<4sii", bsp, 0)
    lumps = [struct.unpack_from("<ii", bsp, 12 + 8 * i) for i in range(28)]

    def lump(n):
        if version <= 18 and n > 12:
            n += 1
        return lumps[n]

    off, ln = lump(0)
    shaders = []
    for i in range(ln // 140):
        name = bsp[off + i * 140:off + i * 140 + 64].split(b"\0")[0].decode("latin-1")
        shaders.append(name)
    off, ln = lump(3)
    surfs = [struct.unpack_from("<3i2i2i5i3f9f2if", bsp, off + i * 108) for i in range(ln // 108)]
    voff, _ = lump(4)
    eoff, eln = lump(14)
    ents = bsp[eoff:eoff + eln].decode("latin-1")
    world = ents[:ents.find("}")]
    print(f"version {version}, {len(shaders)} shaders, {len(surfs)} surfaces")
    print("worldspawn:", " ".join(l.strip() for l in world.splitlines() if l.strip() not in ("{",)))

    # player spawn points: positions for tele (add the eye height, about 60)
    spawns = []
    for ent in re.findall(r"\{([^}]*)\}", ents):
        keys = dict(re.findall(r'"([^"]+)"\s+"([^"]*)"', ent))
        if keys.get("classname", "").startswith("info_player") and "origin" in keys:
            spawns.append(f"{keys['classname']} {keys['origin']} angle {keys.get('angle', keys.get('angles', '0'))}")
    print(f"{len(spawns)} spawn points:")
    for line in spawns[:8]:
        print("  ", line)

    scripts = shader_scripts()
    by_shader = {}
    for s in surfs:
        by_shader.setdefault(shaders[s[0]], []).append(s)
    for name, lst in sorted(by_shader.items()):
        body = scripts.get(name.lower(), "")
        low = body.lower()
        if not any(k in low for k in keywords):
            continue
        props = sorted({k for k in ("polygonoffset", "blendfunc", "alphafunc", "deformvertexes", "tcmod",
                                     "nofog", "fogonly", "sky", "portal", "cull", "sort", "rgbgen", "alphagen")
                        if k in low})
        print(f"\n{name}  [{', '.join(props)}]  {len(lst)} surfaces")
        for s in lst[:6]:
            first, count = s[3], s[4]
            pts = [struct.unpack_from("<3f", bsp, voff + (first + k) * 44) for k in range(count)]
            # drawVert_t: xyz, st, lightmap, normal, color
            nrm = struct.unpack_from("<3f", bsp, voff + first * 44 + 28)
            c = [sum(p[k] for p in pts) / len(pts) for k in range(3)]
            print(f"   type {s[2]} verts {count:4d} center {c[0]:8.1f} {c[1]:8.1f} {c[2]:8.1f}"
                  f"  normal {nrm[0]:5.2f} {nrm[1]:5.2f} {nrm[2]:5.2f}")


if __name__ == "__main__":
    main()
