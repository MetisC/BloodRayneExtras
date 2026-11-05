#!/usr/bin/env python3
# gen_boss.py — build world/bossbattle.msn from world/challenge.msn.
# Same BOILERROOM geometry; enemy pool becomes officers (GGG_OFFICER +
# WALTHER pistols), sub-officers (SS_OFFICER + MG42), and EliteS8 is
# rebadged as the dropped-in boss "Boss1" (NAZI_BARROCK, big HP).
import re, sys

SRC = r"H:\GOG\BloodRayne Terminal Cut\world\challenge.msn"
DST = r"H:\GOG\BloodRayne Terminal Cut\world\bossbattle.msn"

txt = open(SRC, encoding="latin-1").read()

# 1) mission root name
txt = txt.replace("\nchallenge\n", "\nbossbattle\n", 1)

# 2) officer models: plain soldiers get GGG_OFFICER + WALTHER pistol
txt = txt.replace('"SOLDIER.DFM"    // modelName', '"GGG_OFFICER.DFM"    // modelName')
txt = txt.replace('"MP34.SMF"    // modelName', '"WALTHER.SMF"    // modelName')
# elite tier -> SS officers (their guns stay MG42)
txt = txt.replace('"GGG_ELITE.DFM"    // modelName', '"SS_OFFICER.DFM"    // modelName')

# 3) re-badge EliteS8 -> Boss1, GunE8 -> GunB1, group GE8 -> GB1.
#    Declaration list entry + block headers + carriedByActor/carry refs.
txt = txt.replace('"EliteS8"', '"Boss1"')
txt = txt.replace('"GunE8"', '"GunB1"')
txt = txt.replace('"GE8"', '"GB1"')
txt = txt.replace('"SS_OFFICER.DFM"    // modelName\n', '"SS_OFFICER.DFM"    // modelName\n', 1)

# 4) boss stats: inside the Boss1 block bump hitPoints and sizeScale.
def bump(m):
    blk = m.group(0)
    blk = re.sub(r'(\n\t250    // hitPoints)', '\n\t1200    // hitPoints', blk)
    blk = re.sub(r'(\n\t250    // maxHitPoints)', '\n\t1200    // maxHitPoints', blk)
    blk = re.sub(r'(\n\t)1    // sizeScale', r'\g<1>1.15    // sizeScale', blk)
    blk = blk.replace('"NAZI_BARROCK.DFM"__NO__', '"NAZI_BARROCK.DFM"')
    return blk

# swap the boss model inside its block only
blk_re = re.compile(r'\{ CSoldier "Boss1".*?\} CSoldier "Boss1"', re.S)
m = blk_re.search(txt)
if not m:
    sys.exit("Boss1 block not found")
blk = m.group(0)
blk = blk.replace('"SS_OFFICER.DFM"    // modelName', '"NAZI_BARROCK.DFM"    // modelName')
blk = re.sub(r'\n\t250    // hitPoints', '\n\t1200    // hitPoints', blk)
blk = re.sub(r'\n\t250    // maxHitPoints', '\n\t1200    // maxHitPoints', blk)
txt = blk_re.sub(lambda _: blk, txt, count=1)

open(DST, "w", encoding="latin-1").write(txt)
print("wrote", DST)
for k in ("bossbattle", "GGG_OFFICER", "SS_OFFICER", "NAZI_BARROCK", "Boss1", "GunB1", "GB1"):
    print(k, txt.count(k))
