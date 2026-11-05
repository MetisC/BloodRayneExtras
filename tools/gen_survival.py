#!/usr/bin/env python3
"""Generate world\\challenge.msn + world\\challenge.scb for the B1 challenge arena.

The files are LOOSE (game dir\\world\\) — the engine falls back to CreateFileA
for names no pod claims, so nothing stock is touched. Geometry is the dev
BOILERROOM.PST; actors are cloned field-for-field from AR_MB_BOILER.MSN
templates. The .scb is a synthesized binary: [u32 ver][u32 count][u32 bloblen]
[NUL-separated commands][count x u32 line numbers].
"""
import math, struct, os

GAME = r"H:\GOG\BloodRayne Terminal Cut"
OUT  = os.path.join(GAME, "world")

# room bounds taken from the 4 CBarrier positions in BOILERROOM.MSN
CX, CZ = 250.0, 345.0
RING_R = 32.0
N_WP   = 12

def wp_ring():
    pts = []
    for k in range(N_WP):
        a = math.pi / 2 - k * (2 * math.pi / N_WP)
        pts.append((CX + RING_R * math.cos(a), 0.0, CZ + RING_R * math.sin(a)))
    pts.append((258.0, 0.0, 353.0))
    pts.append((242.0, 0.0, 337.0))
    return pts

SOLDIER_BLOCK = '''{{ CSoldier "{name}"
\t0,{x},{y},{z}    // location
\t0,0,{o}    // orient
\t0    // enabled
\t""    // group
\t0    // selfIlluminated
\t0    // attachCount
\t250    // hitPoints
\t1    // numCarryHands
\t1    // carry hand
\t"{gun}"\t\t// carry actor
\t0.307564,-0.132776,0.130445    // carry pos
\t0.0158968,1.42652,1.55902    // carry orient
\t{{ // clothList
\t\t0 // clothCount
\t}}
\t1    // sizeScale
\t250    // maxHitPoints
\t0    // spectralFlag
\t0    // invulnerableFlag
\t0    // etheralFlag
\t"(none)"\t\t// scriptWalkDest
\t3    // scriptWalkDist
\t1e+030    // scriptRunDist
\t-1    // scriptWalkTimer
\t0    // scriptWalkState
\t"(none)"\t\t// scriptDoorToOpen
\t"(none)"\t\t// whoIsControllingMe
\t1    // canInfest
\t0.99122    // speed
\t0    // poolMe
\t1    // randomizeMe
\t"(none)"\t\t// scriptVictim
\t""    // patrolNamePattern
\t200    // guardDistance
\t150    // victimHeight
\t"(none)"\t\t// victim
\t10    // smellDist
\t1    // loquaciousness
\t0.914121    // evasiveness
\t0.77485    // courage
\t0.57181    // loyalty
\t0.627583    // marksmanship
\t0.5    // awareness
\t0.5    // perception
\t"(none)"\t\t// ourPost
\t0.732719    // intelligence
\t0.716666    // aggression
\t0.686956    // valor
\t0    // postModeFlag
\t"none"    // postModeCond
\t0    // holdPosFlag
\t"none"    // holdPosCond
\t-1    // largestPatrolPointIndex
\t-1    // currentPatrolPointIndex
\t"(none)"\t\t// nextPatrolPoint
\t1    // spawnType
\t"{model}"    // modelName
\t{{ // motion state
\t\t"standambient",0 // current motion name, curFrameNumber
\t}}
\t1
\t0
\t2
}} CSoldier "{name}"'''

GUN_BLOCK = '''{{ CGun "{name}"
\t0,{x},{y},{z}    // location
\t0,0,0    // orient
\t{en}    // enabled
\t""    // group
\t0    // selfIlluminated
\t0    // attachCount
\t"{model}"    // modelName
\t"{carry}"\t\t// carriedByActor
\t0    // weaponState
\t{ammo}    // ammoCount
\t{atype}    // ammoType
\t0    // simTimer
\t{frate}    // fireRate
\t{dmin}    // minDamage
\t{dmax}    // maxDamage
\t0    // canAttachLight
\t{rng}    // effectiveRange
\t{gtype}    // gunType
\t0    // isCustomGun
}} CGun "{name}"'''

WP_BLOCK = '''{{ CWayPoint "{name}"
\t0,{x},{y},{z}    // location
\t0,0,0    // orient
\t1    // enabled
\t""    // group
\t0    // selfIlluminated
\t0    // attachCount
}} CWayPoint "{name}"'''

BARRIER_BLOCK = '''{{ CBarrier "{name}"
\t{loc}    // location
\t{ori}    // orient
\t1    // enabled
\t""    // group
\t0    // selfIlluminated
\t0    // attachCount
\t{sz}    // size
\t""    // effectClassNameList
\t1    // addToPathGrid
}} CBarrier "{name}"'''

# gun stat table lifted from real MSNs
GUNS = {
    "MP34.SMF":        dict(ammo=32, atype=12, frate=5,  dmin=20, dmax=60, rng=60,  gtype=3),
    "MG42.SMF":        dict(ammo=50, atype=11, frate=13, dmin=50, dmax=90, rng=200, gtype=12),
    "FALLSHIRMG42.SMF":dict(ammo=20, atype=11, frate=10, dmin=50, dmax=90, rng=120, gtype=17),
    "GREASEGUN.SMF":   dict(ammo=30, atype=12, frate=4,  dmin=20, dmax=60, rng=60,  gtype=0),
}

N_SOLD, N_ELITE = 40, 8

def build_msn():
    wps = wp_ring()
    decls, blocks = [], []
    decls.append('CHeroPlaceholder "HeroPlaceholder0"')
    blocks.append('''{ CHeroPlaceholder "HeroPlaceholder0"
\t0,242.392,0,272.733    // location
\t0,0,0.518797    // orient
\t1    // enabled
\t""    // group
\t0    // selfIlluminated
\t0    // attachCount
\t0    // index
\t"Hero"    // heroName
\t0
\t1    // heroType
} CHeroPlaceholder "HeroPlaceholder0"''')
    # verbatim barrier geometry from BOILERROOM.MSN (pos + size + orient)
    bar = [("0,308.7,20,339.78", "0,0,0", "3.01266,20.1835,15.3865"),
           ("0,309.447,-1.78814e-007,294.502", "0,0,0", "1.29444,8.14302,6.29097"),
           ("0,195.353,-1.78814e-007,343.998", "0,0,0", "1.29444,8.14302,6.29097"),
           ("0,224.864,0.0307615,394.847", "0,0,-1.52763", "1.01604,19.6298,16.6113")]
    for i,(loc, ori, sz) in enumerate(bar):
        n = "Barrier%d" % i
        decls.append('CBarrier "%s"' % n)
        blocks.append(BARRIER_BLOCK.format(name=n, loc=loc, ori=ori, sz=sz))
    for i,(x,y,z) in enumerate(wps):
        n = "WPHero" if i == N_WP else "WPS%d" % (i + 1 if i < N_WP else i)
        if i == N_WP: n = "WPS13"
        if i == N_WP + 1: n = "WPS14"
        if i < N_WP: n = "WPS%d" % (i + 1)
        decls.append('CWayPoint "%s"' % n)
        blocks.append(WP_BLOCK.format(name=n, x="%.3f" % x, y=y, z="%.3f" % z))
    decls.append('CWayPoint "WPHero"')
    blocks.append(WP_BLOCK.format(name="WPHero", x=CX, y=0, z=CZ))
    for i in range(1, N_SOLD + 1):
        x, y, z = wps[(i - 1) % len(wps)]
        n, g = "SoldierS%d" % i, "GunS%d" % i
        decls += ['CSoldier "%s"' % n, 'CGun "%s"' % g]
        blocks.append(SOLDIER_BLOCK.format(name=n, x="%.3f"%x, y=y, z="%.3f"%z, o=0,
                                           gun=g, model="SOLDIER.DFM"))
        blocks.append(GUN_BLOCK.format(name=g, x="%.3f"%x, y=y, z="%.3f"%z, en=0,
                                       carry=n, model="MP34.SMF", **GUNS["MP34.SMF"]))
    for i in range(1, N_ELITE + 1):
        x, y, z = wps[(i * 3) % len(wps)]
        n, g = "EliteS%d" % i, "GunE%d" % i
        decls += ['CSoldier "%s"' % n, 'CGun "%s"' % g]
        blocks.append(SOLDIER_BLOCK.format(name=n, x="%.3f"%x, y=y, z="%.3f"%z, o=0,
                                           gun=g, model="GGG_ELITE.DFM"))
        blocks.append(GUN_BLOCK.format(name=g, x="%.3f"%x, y=y, z="%.3f"%z, en=0,
                                       carry=n, model="MG42.SMF", **GUNS["MG42.SMF"]))
    floor = [("GunFloor1","MG42.SMF",245,0,340), ("GunFloor2","MG42.SMF",255,0,350),
             ("GunFloor3","FALLSHIRMG42.SMF",250,0,360), ("GunFloor4","GREASEGUN.SMF",240,0,355)]
    for n, mod, x, y, z in floor:
        decls.append('CGun "%s"' % n)
        blocks.append(GUN_BLOCK.format(name=n, x=x, y=y, z=z, en=1,
                                       carry="(none)", model=mod, **GUNS[mod]))
    out = ["// mission version, actor version", "8,4",
           "// Root name of mission", "challenge",
           "// setList", "1", "BOILERROOM.PST",
           "// actorTypeList", "55",
           # verbatim schema list from AR_MB_BOILER.MSN (production —
           # BOILERROOM's own 50-entry list is stale: CCharacter 15
           # wrongly enables the grabbedBy/grabbedType fields)
           "CWeapon 11","CWayPoint 3","CWaterActor 12","CVampire 2","CNest 3",
           "CTVBat 1","CTrigger 15","CThule 1","CTempleDetail 1","CTank 1",
           "CSpotLight 1","CSpinner 3","CSoldier 13","CRopeLadder 2","CRail 4",
           "CQueen 3","CPlatform 21","CPendulum 8","CNPC 7","CMutate 5",
           "CMech 4","CMaraisreq 8","CMauler 1","CMainCharacter 2",
           "CLiteBulb 6","CLever 9","CHero 14","CHeroPlaceholder 4",
           "CHeinrich 4","CHealthItem 3","CGun 2","CGlass 12","CGemini 1",
           "CFrog 1","CFlies 2","CFlame 11","CEnemy 19","CEmitter 23",
           "CDoor 21","CDecal 3","CDaemiteCluster 1","CDaemWar 3","CDaemite 4",
           "CCurtain 10","CConveyor 2","CChain 4","CCharacter 16",
           "CCameraPathActor 1","CBreakable 12","CBoxActor 36","CBeliar 1",
           "CBatCreature 6","CBarrier 2","CAmmo 1","CDemonActor 14",
           "// actor count", str(len(decls)),
           "// actor name list"]
    out += decls
    out.append("// actor data list")
    out += blocks
    return ("\r\n".join(out) + "\r\n").replace("\r\n", "\n").replace("\n", "\r\n")

SCB_CMDS = ["idle", "setFog(0,0,0,10,240,0)", "flagon(DrawHud)",
            ":mainloop", "idle", "goto mainloop"]

def build_scb():
    blob = "\x00".join(SCB_CMDS) + "\x00"
    b = blob.encode("latin1")
    hdr = struct.pack("<III", 1, len(SCB_CMDS), len(b))
    tab = b"".join(struct.pack("<I", i + 1) for i in range(len(SCB_CMDS)))
    return hdr + b + tab

def main():
    os.makedirs(OUT, exist_ok=True)
    msn = os.path.join(OUT, "challenge.msn")
    scb = os.path.join(OUT, "challenge.scb")
    open(msn, "w", newline="").write(build_msn())
    open(scb, "wb").write(build_scb())
    print("wrote", msn)
    print("wrote", scb)

if __name__ == "__main__":
    main()
