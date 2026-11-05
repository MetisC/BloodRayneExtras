#!/usr/bin/env python3
# gen_bossrush.py — build world/bossrush.msn (Boss Rush arena).
# Same BOILERROOM geometry; EliteS1-6 become Boss1..Boss6, one per wave:
#   Boss1 Barrock (CSoldier), Boss2 Butcheress (heroType=3),
#   Boss3 Mynce (heroType=2), Boss4 RayneLA (4), Boss5 Rayne (1),
#   Boss6 the Act-1 Queen (CQueen). EliteS7/8 stay dormant reserves.
import re, sys

BASE = r"H:\GOG\BloodRayne Terminal Cut\world\challenge.msn"
DST  = r"H:\GOG\BloodRayne Terminal Cut\world\bossrush.msn"
MENTOR = r"H:\GOG\BloodRayne Terminal Cut\MODDING\extracted\b1_orig_world\WORLD\DE_C2START.MSN"
QUEEN  = r"H:\GOG\BloodRayne Terminal Cut\MODDING\extracted\b1_orig_world\WORLD\LA_SWAMP.MSN"

def get_block(path, cls, name):
    t = open(path, encoding="latin-1").read()
    m = re.search(r'\{ %s "%s"\n.*?\} %s "%s"' % (cls, name, cls, name), t, re.S)
    if not m: sys.exit("no %s %s" % (cls, name))
    return m.group(0)

def prep(b, cls, old, new, loc, group, hp=None, hero=None):
    b = b.replace('"%s"' % old, '"%s"' % new)
    b = re.sub(r'\n\t[^\n]*    // location', '\n\t%s    // location' % loc, b, count=1)
    b = re.sub(r'(\n\t)"[^"]*"(    // group)', r'\g<1>"%s"\g<2>' % group, b, count=1)
    b = re.sub(r'(\n\t)\d+(    // enabled)', r'\g<1>0\g<2>', b, count=1)
    b = re.sub(r'(\n\t)\d+(    // spawnType)', r'\g<1>1\g<2>', b)
    if hp is not None:
        b = re.sub(r'(\n\t)\d+(\.0)?(    // hitPoints)', r'\g<1>%d\g<3>' % hp, b)
        b = re.sub(r'(\n\t)\d+(\.0)?(    // maxHitPoints)', r'\g<1>%d\g<3>' % hp, b)
    if hero is not None:
        b = re.sub(r'(\n\t)\d+(    // heroType)', r'\g<1>%d\g<2>' % hero, b)
    return b

txt = open(BASE, encoding="latin-1").read()
txt = txt.replace("\nchallenge\n", "\nbossrush\n", 1)

# Boss1 (Barrock) is built from the dormant-reserve EliteS8 block itself:
# same CSoldier record, model swapped to NAZI_BARROCK.DFM.
barrock = get_block(BASE, "CSoldier", "EliteS8")
barrock = re.sub(r'"[^"]*"(    // modelName)', '"NAZI_BARROCK.DFM"\g<1>',
                 barrock, count=1)
mentor  = get_block(MENTOR, "CMainCharacter", "MentorBoss0")
queen   = get_block(QUEEN, "CQueen", "Queen")

def pos_of(b):
    return re.search(r'\n\t([^\n]*)    // location', b).group(1).strip()

specs = [  # (src block, class, heroType/None, hp)
    (barrock, "CSoldier", None, 900),
    (mentor,  "CMainCharacter", 3, 700),   # Butcheress
    (mentor,  "CMainCharacter", 2, 700),   # Mynce
    (mentor,  "CMainCharacter", 4, 700),   # RayneLA
    (mentor,  "CMainCharacter", 1, 700),   # Rayne
    (queen,   "CQueen",        None, 1600),
]

def repl(m):
    n = int(re.search(r'\d+', re.search(r'"(EliteS\d+)"', m.group(0)).group(1)).group(0))
    if n > 6: return m.group(0)          # EliteS7/8 stay dormant reserves
    src, cls, hero, hp = specs[n - 1]
    old = re.search(r'"([^"]+)"', src).group(1)
    return prep(src, cls, old, "Boss%d" % n, pos_of(m.group(0)),
                "GB%d" % n, hp=hp, hero=hero)

txt = re.sub(r'\{ CSoldier "EliteS\d+"\n.*?\} CSoldier "EliteS\d+"',
             repl, txt, flags=re.S)

# Fix the declaration list: EliteS1-6 -> Boss1-6 with their new classes.
for i in range(1, 7):
    cls = specs[i - 1][1]
    txt = txt.replace('CSoldier "EliteS%d"' % i, '%s "Boss%d"' % (cls, i))

# EliteS1-6 no longer exist — their GunE1-6 would dangle. Remove the six
# gun blocks and their decls entirely.
for i in range(1, 7):
    txt = re.sub(r'\{ CGun "GunE%d"\n.*?\} CGun "GunE%d"\n' % (i, i), '', txt, flags=re.S)
    txt = re.sub(r'(?m)^CGun "GunE%d"\n' % i, '', txt)

# Boss1 keeps a weapon: clone the reserve EliteS8's gun as GunB1 carried
# by Boss1 (EliteS8 stays dormant and keeps its own GunE8).
gunb = get_block(BASE, "CGun", "GunE8")
gunb = (gunb.replace('"GunE8"', '"GunB1"')
            .replace('"EliteS8"', '"Boss1"')
            .replace('"GE8"', '"GB1"'))
m = re.search(r'\{ CGun "GunE8"\n.*?\} CGun "GunE8"\n', txt, re.S)
txt = txt[:m.end()] + gunb + "\n" + txt[m.end():]
txt = re.sub(r'(?m)^CGun "GunE8"$', 'CGun "GunE8"\nCGun "GunB1"', txt, count=1)

# Header actor count must match the real decl list — recount it.
ndecl = len(re.findall(r'(?m)^[A-Za-z0-9_]+ "[^"]+"\s*$', txt))
txt = re.sub(r'(// actor count\n)\d+', r'\g<1>%d' % ndecl, txt, count=1)

open(DST, "w", encoding="latin-1").write(txt)
print("wrote", DST)
for k in ('"Boss1"','"Boss2"','"Boss3"','"Boss4"','"Boss5"','"Boss6"',
          '"GunB1"', "CMainCharacter", "CQueen"):
    print(k, txt.count(k))
