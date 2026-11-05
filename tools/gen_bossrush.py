#!/usr/bin/env python3
# gen_bossrush.py — build world/bossrush.msn (Boss Rush arena).
# Same BOILERROOM geometry; EliteS1-6 become Boss1..Boss6, one per wave:
#   Boss1 Barrock (CSoldier), Boss2 Butcheress (heroType=3),
#   Boss3 Mynce (heroType=2), Boss4 RayneLA (4), Boss5 Rayne (1),
#   Boss6 the Act-1 Queen (CQueen). EliteS7/8 stay dormant reserves.
import re, sys

BASE = r"H:\GOG\BloodRayne Terminal Cut\world\bossbattle.msn"
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
    b = re.sub(r'(\n\t)\d+(    // spawnType)', r'\g<1>2\g<2>', b)
    if hp is not None:
        b = re.sub(r'(\n\t)\d+(\.0)?(    // hitPoints)', r'\g<1>%d\g<3>' % hp, b)
        b = re.sub(r'(\n\t)\d+(\.0)?(    // maxHitPoints)', r'\g<1>%d\g<3>' % hp, b)
    if hero is not None:
        b = re.sub(r'(\n\t)\d+(    // heroType)', r'\g<1>%d\g<2>' % hero, b)
    return b

txt = open(BASE, encoding="latin-1").read()
txt = txt.replace("\nbossbattle\n", "\nbossrush\n", 1)

barrock = get_block(BASE, "CSoldier", "Boss1")
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

# The old Boss1 (ex-EliteS8) is still in the file as a SECOND CSoldier
# "Boss1" block + decl — drop the last occurrence of each.
def drop_last(cls_name):
    global txt
    # block: last `{ CLS "Boss1" ... } CLS "Boss1"`
    pats = list(re.finditer(r'\{ %s "Boss1"\n.*?\} %s "Boss1"' % (cls_name, cls_name), txt, re.S))
    if len(pats) > 1:
        m = pats[-1]; txt = txt[:m.start()] + txt[m.end():]
    # decl: last `CLS "Boss1"` line at column 0
    pats = list(re.finditer(r'(?m)^%s "Boss1"\n' % cls_name, txt))
    if len(pats) > 1:
        m = pats[-1]; txt = txt[:m.start()] + txt[m.end():]
drop_last("CSoldier")

# EliteS1-6 no longer exist — their GunE1-6 would dangle. Remove the six
# gun blocks and their decls entirely.
for i in range(1, 7):
    txt = re.sub(r'\{ CGun "GunE%d"\n.*?\} CGun "GunE%d"\n' % (i, i), '', txt, flags=re.S)
    txt = re.sub(r'(?m)^CGun "GunE%d"\n' % i, '', txt)

# Header actor count must match the real decl list — recount it.
ndecl = len(re.findall(r'(?m)^[A-Za-z0-9_]+ "[^"]+"\s*$', txt))
txt = re.sub(r'(// actor count\n)\d+', r'\g<1>%d' % ndecl, txt, count=1)

open(DST, "w", encoding="latin-1").write(txt)
print("wrote", DST)
for k in ('"Boss1"','"Boss2"','"Boss3"','"Boss4"','"Boss5"','"Boss6"',
          "CMainCharacter", "CQueen"):
    print(k, txt.count(k))
