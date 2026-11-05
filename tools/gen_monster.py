#!/usr/bin/env python3
# gen_monster.py — build world/monster.msn (Monster Mash arena).
# Same BOILERROOM geometry as challenge.msn; the human pool is replaced by
# native monster classes cloned from campaign MSNs:
#   SoldierS1-40 -> CMaraisreq (marai3.dfm swamp packs)
#   EliteS1-8    -> CMutate    (mutate.dfm church monsters)
#   Boss1        -> CQueen     (Queen.dfm, the Act-1 queen)
# Actor *names* stay SoldierS*/EliteS*/Boss1 so the wave engine is untouched.
import re, sys

BASE = r"H:\GOG\BloodRayne Terminal Cut\world\challenge.msn"
DST  = r"H:\GOG\BloodRayne Terminal Cut\world\monster.msn"
NATIVE = {
    "CMaraisreq": (r"H:\GOG\BloodRayne Terminal Cut\MODDING\extracted\b1_orig_world\WORLD\LA_SWAMP.MSN", "Maraisreq0"),
    "CMutate":    (r"H:\GOG\BloodRayne Terminal Cut\MODDING\extracted\b1_orig_world\WORLD\LA_CHURCH.MSN", "Mutate0"),
    "CQueen":     (r"H:\GOG\BloodRayne Terminal Cut\MODDING\extracted\b1_orig_world\WORLD\LA_SWAMP.MSN", "Queen"),
}

def get_block(path, cls, name):
    t = open(path, encoding="latin-1").read()
    m = re.search(r'\{ %s "%s"\n.*?\} %s "%s"' % (cls, name, cls, name), t, re.S)
    if not m: sys.exit("no %s %s in %s" % (cls, name, path))
    return m.group(0)

def clone(block, cls, oldname, newname, loc, group, hp=None, model=None):
    b = block.replace('"%s"' % oldname, '"%s"' % newname)
    b = re.sub(r'\n\t[^\n]*    // location', '\n\t%s    // location' % loc, b, count=1)
    b = re.sub(r'(\n\t)"[^"]*"(    // group)', r'\g<1>"%s"\g<2>' % group, b, count=1)
    b = re.sub(r'(\n\t)\d+(    // spawnType)', r'\g<1>1\g<2>', b)
    b = re.sub(r'(\n\t)\d+(    // enabled)', r'\g<1>0\g<2>', b, count=1)
    if hp is not None:
        b = re.sub(r'(\n\t)\d+(\.0)?(    // hitPoints)', r'\g<1>%d\g<3>' % hp, b)
        b = re.sub(r'(\n\t)\d+(\.0)?(    // maxHitPoints)', r'\g<1>%d\g<3>' % hp, b)
    if model is not None:
        b = re.sub(r'"[^"]*"(    // modelName)', '"%s"\g<1>' % model, b, count=1)
    return b

txt = open(BASE, encoding="latin-1").read()
txt = txt.replace("\nchallenge\n", "\nmonster\n", 1)

mar = get_block(*[NATIVE["CMaraisreq"][0], "CMaraisreq", NATIVE["CMaraisreq"][1]])
mut = get_block(NATIVE["CMutate"][0], "CMutate", NATIVE["CMutate"][1])
queen = get_block(NATIVE["CQueen"][0], "CQueen", NATIVE["CQueen"][1])

# soldier blocks -> CMaraisreq clones (drop their carried guns: replace gun
# decl/blocks' class CSoldier->CGun stays, but numCarryHands actor-side is in
# the soldier block; maraisreq template has 0 already).
def pos_of(blk):
    m = re.search(r'\n\t([^\n]*)    // location', blk)
    return m.group(1).strip()

def grp_of(blk):
    m = re.search(r'\n\t"([^"]*)"\s*// group', blk)
    return m.group(1) if m else ""

n_s = n_e = 0
def repl_soldier(m):
    global n_s
    n_s += 1
    return clone(mar, "CMaraisreq", "Maraisreq0", "SoldierS%d" % n_s,
                 pos_of(m.group(0)), grp_of(m.group(0)), hp=80)

def repl_elite(m):
    global n_e
    n_e += 1
    if n_e == 8:  # EliteS8 slot -> the Queen boss
        return clone(queen, "CQueen", "Queen", "Boss1",
                     pos_of(m.group(0)), grp_of(m.group(0)).replace("GE8", "GB1"),
                     hp=1500)
    return clone(mut, "CMutate", "Mutate0", "EliteS%d" % n_e,
                 pos_of(m.group(0)), grp_of(m.group(0)), hp=220)

txt = re.sub(r'\{ CSoldier "SoldierS\d+"\n.*?\} CSoldier "SoldierS\d+"',
             repl_soldier, txt, flags=re.S)
txt = re.sub(r'\{ CSoldier "EliteS\d+"\n.*?\} CSoldier "EliteS\d+"',
             repl_elite, txt, flags=re.S)

# Declaration-list class fixes (the actor decl lines keep the old class).
txt = re.sub(r'CSoldier "SoldierS\d+"', lambda m: m.group(0).replace("CSoldier", "CMaraisreq"), txt)
def _ed(m):
    n = int(re.search(r'\d+', m.group(0)).group(0))
    return ('CQueen "Boss1"' if n == 8 else 'CMutate "EliteS%d"' % n)
txt = re.sub(r'CSoldier "EliteS\d+"', _ed, txt)

# NOTE: the number after each class in the header table is the class's
# serialized format *version*, NOT an instance count — leave it untouched.

open(DST, "w", encoding="latin-1").write(txt)
print("wrote", DST)
for k in ("CMaraisreq", "CMutate", "CQueen", "Boss1", "SoldierS40"):
    print(k, txt.count(k))
