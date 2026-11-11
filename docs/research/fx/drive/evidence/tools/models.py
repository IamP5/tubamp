import os
C = "/Library/Audio/Plug-Ins/Components"
GR6 = f"{C}/Guitar Rig 6.component/Contents/MacOS/Guitar Rig 6"
AT5 = f"{C}/AmpliTube 5.component/Contents/MacOS/AmpliTube 5"

def ni(ctor, proc, ctrl, params, defaults, drive="drive", drives=(0.1, 0.5, 1.0), size=0x4000, ctrl_n=16, extra_pre=(), **kw):
    """Guitar Rig 6 NI::DSP::EffectBase-derived class: ctor, EffectBase::init, setSampleRate(48000, ctrl_n, 1),
    setters, EffectBase::resume(false), then onControlRate every ctrl_n samples and processBuffer."""
    pre = [f"alloc {size:#x}", f"c {ctor:#x}", "c 0x60b960", f"cfii 0x60b9f0 48000 {ctrl_n} 1", *extra_pre]
    post = ["ci 0x60ba5c 0"]
    post += [s for k in params for s in ()]
    if ctrl: post.append(f"ctrl {ctrl:#x} {ctrl_n}")
    post.append(f"proc_ni {proc:#x}")
    return dict(kind="rig", bin=GR6, pre=pre, params=params, defaults=defaults, post=post, drive=drive, drives=list(drives), **kw)

MODELS = {}
MODELS["gr6_skreamer"] = ni(0x2f43d8, 0x2f50c8, 0x2f4f48,
    dict(drive="cf 0x2f4894 {v}", tone="cf 0x2f47f8 {v}", volume="cf 0x2f47c4 {v}", bass="cf 0x2f49e8 {v}", bright="cf 0x2f4a80 {v}", clean="cf 0x2f4b24 {v}"),
    dict(drive=0.5, tone=0.5, volume=0.5, bass=0.5, bright=0.5, clean=0.0), size=0x1000)

OURS = ("aufx", "Tamp", "Tuba")

def ni_vt(vt, proc, ctrl, params, defaults, **kw):
    """Class whose constructor is inlined: zeroed memory, EffectBase::EffectBase(), then the class vtable."""
    m = ni(0x60b924, proc, ctrl, params, defaults, **kw)
    m["pre"].insert(2, f"vt 0 {vt:#x}")
    return m

MODELS["gr6_distortion"] = ni(0x275724, 0x27631c, 0x2760f8,
    dict(drive="cf 0x275f7c {v}", tone="cf 0x275f04 {v}", volume="cf 0x275e88 {v}", bass="cf 0x275ce8 {v}", treble="cf 0x275d80 {v}", mid="cf 0x276060 {v}"),
    dict(drive=0.5, tone=0.5, volume=0.5, bass=0.5, treble=0.5, mid=0.5))
MODELS["gr6_cat"] = ni_vt(0x44891d8, 0x268768, 0x268544,
    dict(drive="cf 0x267fe8 {v}", filter="cf 0x2681b0 {v}", volume="cf 0x2683f0 {v}", bass="cf 0x267df4 {v}", treble="cf 0x267e98 {v}", tone="cf 0x267f38 {v}", balls="cf 0x268248 {v}"),
    dict(drive=0.5, filter=0.5, volume=0.5, bass=0.5, treble=0.5, tone=0.5, balls=0.5))
MODELS["gr6_bigfuzz"] = ni_vt(0x448aa18, 0x267554, 0x267330,
    dict(drive="cf 0x2671f4 {v}", tone="cf 0x267184 {v}", volume="cf 0x267114 {v}", bass="cf 0x266f74 {v}", treble="cf 0x267008 {v}"),
    dict(drive=0.5, tone=0.5, volume=0.5, bass=0.5, treble=0.5))
MODELS["gr6_fuzzace"] = ni_vt(0x448af90, 0x2878fc, 0x28777c,
    dict(drive="cf 0x287654 {v}", volume="cf 0x28756c {v}", bass="cf 0x2873d8 {v}", treble="cf 0x287470 {v}"),
    dict(drive=0.5, volume=0.5, bass=0.5, treble=0.5))
MODELS["gr6_demon"] = ni(0x274168, 0x275490, 0x27526c,
    dict(drive="cf 0x274e38 {v}", volume="cf 0x274d44 {v}", bass="cf 0x274f3c {v}", mid="cf 0x274ff0 {v}", treble="cf 0x2750d8 {v}"),
    dict(drive=0.5, volume=0.5, bass=0.5, mid=0.5, treble=0.5))
MODELS["gr6_jackhammer"] = ni_vt(0x448b158, 0x2911cc, 0x290fe4,
    dict(drive="cf 0x290568 {v}", volume="cf 0x290fbc {v}", bass="cf 0x290fc4 {v}", treble="cf 0x290fcc {v}", contour="cf 0x290fd4 {v}", frequency="cf 0x290fdc {v}"),
    dict(drive=0.5, volume=0.5, bass=0.5, treble=0.5, contour=0.5, frequency=0.5))
MODELS["gr6_gainboost"] = ni_vt(0x448b018, 0x287f00, 0x287d80, dict(drive="cf 0x287b7c {v}"), dict(drive=0.5))
MODELS["gr6_trebleboost"] = ni_vt(0x448b930, 0x2e5e2c, 0x2e5c08, dict(drive="cf 0x2e57a0 {v}", bright="cf 0x2e58d0 {v}"), dict(drive=0.5, bright=0.5))
MODELS["gr6_mulholland"] = ni_vt(0x448b350, 0x29a100, 0,
    dict(drive="cf 0x299748 {v}", tone="cf 0x299a9c {v}", dw="cf 0x299acc {v}"), dict(drive=0.5, tone=0.5, dw=1.0))

# tubamp's own AU (installed build): every block off except Drive, raw output mode.
_off = {1958342099: 0, 950482191: 0, 733639226: 0, 779398202: 0, 96736850: 0, 1078678588: 0, 816162331: 0, 389898092: 0,
        896045079: 0, 220875044: 0, 885066879: 0, 885096670: 0, 885126461: 0, 947001359: 0, 1549202612: 1}
MODELS["ours"] = dict(kind="au", codes=OURS, fixed=_off, params=dict(drive=577294612, tone=577695495, level=721012879),
                      defaults=dict(drive=0.3333, tone=0.628388, level=0.666667), drive="drive", drives=[0.1, 0.5, 1.0], fr_db=-70)
MODELS["ours_tone_open"] = dict(MODELS["ours"], defaults=dict(drive=0.3333, tone=1.0, level=0.666667))
_st = ["--state", os.path.join(os.environ.get("W", "."), "ours_drive_only.aupreset")]
for _k in ("ours", "ours_tone_open"):
    MODELS[_k] = dict(MODELS[_k], fixed={1549202612: 1, 947001359: 0}, extra=_st)

def ik(factory, proc, osp, names, defaults, drive, drives=(0.1, 0.5, 1.0), **kw):
    """AmpliTube 5 IK::KIS::FX module obtained from its IK::MAF::DynamicFactory (Instance, SetMaxBufferSize,
    SetSampleRate(48000), Acquire), parameters set through the stomp's own OnSetParameter(name, value)."""
    pre = ["ik_factory " + " ".join(f"{a:#x}" for a in factory)]
    params = {k: f"ik_set {osp:#x} {a:#x} {{v}}" for k, a in names.items()}
    return dict(kind="rig", bin=AT5, pre=pre, params=params, defaults=defaults, post=[f"proc_ik {proc:#x}"], drive=drive, drives=list(drives), **kw)

MODELS["at5_tubescreamer"] = ik((0x14bafc4, 0x14bb12c, 0x14bb2e8, 0x14bc018), 0xdc7758, 0x187a748,
    dict(drive=0x37012f0, volume=0x3701308, tone=0x3701320, oversample=0x3701338), dict(drive=0.5, volume=0.5, tone=0.5, oversample=1.0), "drive")
MODELS["at5_tubescreamer_os_off"] = dict(MODELS["at5_tubescreamer"], defaults=dict(drive=0.5, volume=0.5, tone=0.5, oversample=0.0))
MODELS["at5_rat"] = ik((0x13eb0ac, 0x13eb214, 0x13eb3d0, 0x13ec028), 0xda22f0, 0x17d5e2c,
    dict(drive=0x36fbba8, filter=0x36fbbc0, gain=0x36fbbd8, oversample=0x36fbbf0), dict(drive=0.5, filter=0.5, gain=0.5, oversample=1.0), "drive")
MODELS["at5_bigmuff"] = ik((0x13e7ec0, 0x13e8028, 0x13e8200, 0x13e8f00), 0xc79294, 0x1531998,
    dict(drive=0x36ea4b8, volume=0x36ea4d0, tone=0x36ea4e8, oversample=0x36ea500), dict(drive=0.5, volume=0.5, tone=0.5, oversample=1.0), "drive")
MODELS["at5_ds1"] = ik((0x13f0f44, 0x13f10ac, 0x13f1268, 0x13f1cfc), 0xc7ce40, 0x153a6b0,
    dict(drive=0x36ea8c8, volume=0x36ea8e0, tone=0x36ea8f8, oversample=0x36ea910), dict(drive=0.5, volume=0.5, tone=0.5, oversample=1.0), "drive")
MODELS["at5_sd1"] = ik((0x13f3d7c, 0x13f3ee4, 0x13f40a0, 0x13f4bd4), 0xc80d0c, 0x153ed64,
    dict(drive=0x36eab48, tone=0x36eab60, level=0x36eab78, oversample=0x36eab90), dict(drive=0.5, tone=0.5, level=0.5, oversample=1.0), "drive")
MODELS["at5_distplus"] = ik((0x1456670, 0x14567d8, 0x14569b0, 0x1457490), 0xd87f14, 0x17870f4,
    dict(drive=0x36f9de0, output=0x36f9df8, mode=0x36f9e10, oversample=0x36f9e28), dict(drive=0.5, output=0.5, oversample=1.0), "drive")
MODELS["at5_fuzzface"] = ik((0x14dda84, 0x14ddbec, 0x14ddda8, 0x14deb18), 0xe34e80, 0x16b743c,
    dict(drive=0x36f3a90, level=0x36f3aa8, bias=0x36f3ac0, volume=0x36f3ad8, oversample=0x36f3af0), dict(drive=0.5, level=0.5, oversample=1.0), "drive")
MODELS["at5_overdrive"] = ik((0x14be668, 0x14be7d0, 0x14be99c, 0x14bf33c), 0, 0x17bf924,
    dict(drive=0x36fb4a8, volume=0x36fb4c0, tone=0x36fb4d8, oversample=0x36fb4f0), dict(drive=0.5, volume=0.5, tone=0.5, oversample=1.0), "drive")

def er(ctor, init, setsr, resume, setparam, proc, names, defaults, drive, size=0x80000, ctrl=0, **kw):
    """Guitar Rig 6 ni::effektrig::dsp core (IDSPCore): ctor, onInit, onSetSampleRate(48000, 16), onResume,
    setParameter(index, normalized), processAudio(in, out, n, nullptr, 0)."""
    pre = [f"alloc {size:#x}", f"c {ctor:#x}", f"c {init:#x}", f"cfi {setsr:#x} 48000 16", f"c {resume:#x}"]
    params = {k: f"cuf {setparam:#x} {i} {{v}}" for k, i in names.items()}
    return dict(kind="rig", bin=GR6, pre=pre, params=params, defaults=defaults, post=([f"ctrl {ctrl:#x} 16"] if ctrl else []) + [f"proc_er5 {proc:#x}"], drive=drive, drives=[0.1, 0.5, 1.0], **kw)

MODELS["gr6_tsdeluxe"] = er(0x5edca0, 0x5ee498, 0x1c5d30, 0x1c5fc4, 0x1c6380, 0x1c6198,
    dict(p0=0, p1=1, p2=2, p3=3, p4=4, p5=5, mode=6), dict(p0=0.5, p1=0.5, p2=0.5, p3=0.5, p4=0.5, p5=0.5, mode=0.0), "p0", ctrl=0x5eeb28)

def variant(base, name, sweep=None, drives=None, **defaults):
    """A copy of a model with other defaults and/or another swept parameter."""
    m = dict(MODELS[base]); m["defaults"] = dict(m["defaults"], **defaults)
    if sweep: m["drive"] = sweep
    if drives: m["drives"] = list(drives)
    MODELS[name] = m

for _p in ("p1", "p2", "p3", "p4", "p5"):
    variant("gr6_tsdeluxe", f"gr6_tsdeluxe_{_p}", sweep=_p, drives=(0.0, 0.5, 1.0))
    variant("gr6_tsdeluxe", f"gr6_tsdeluxe_eq_{_p}", sweep=_p, drives=(0.0, 0.5, 1.0), mode=1.0)

MODELS["gr6_hm2"] = er(0x5e4338, 0x5e494c, 0x1e0cc8, 0x1e0f5c, 0x1e137c, 0x1e1208,
    dict(p0=0, p1=1, p2=2, p3=3), dict(p0=0.5, p1=0.5, p2=0.5, p3=0.5), "p0", ctrl=0x5e4db8)
MODELS["gr6_fuzzface"] = er(0x5e8bec, 0xf1f80, 0xf2064, 0xf22f8, 0x5e93cc, 0xf2584,
    dict(p0=0, p1=1, p2=2), dict(p0=0.5, p1=1.0, p2=0.0), "p0", ctrl=0x5ea174)
for _p in ("p1", "p2", "p3"): variant("gr6_hm2", f"gr6_hm2_{_p}", sweep=_p, drives=(0.0, 0.5, 1.0))
for _p in ("p1", "p2"): variant("gr6_fuzzface", f"gr6_fuzzface_{_p}", sweep=_p, drives=(0.0, 0.5, 1.0))

# Stand-alone AUs through auhost (normalised parameter values, ids from `auhost params`).
MODELS["ni_driver"] = dict(kind="au", codes=("aufx", "Ni$=", "-NI-"), fixed={3: 1.0, 2: 0.0}, params=dict(drive=4, color=5, input=0, output=7),
                           defaults=dict(drive=0.5, color=0.5), drive="drive", drives=[0.1, 0.5, 1.0])
MODELS["ni_dirt"] = dict(kind="au", codes=("aufx", "Ni$L", "-NI-"), fixed={7: 0.0, 9: 0.0},
                         params=dict(drive=3, amount=1, mode=0, bias=4, tilt=5, safety=2, amount_b=7, drive_b=9, mode_b=6, routing=13, mix=14),
                         defaults=dict(drive=0.5, amount=1.0, mode=0.5, bias=0.0, tilt=0.5, safety=1.0), drive="drive", drives=[0.1, 0.5, 1.0])
for _i, _n in ((0.0, "i"), (0.5, "ii"), (1.0, "iii")): variant("ni_dirt", f"ni_dirt_mode_{_n}", mode=_i)
variant("ni_dirt", "ni_dirt_bias", sweep="bias", drives=(0.0, 0.5, 1.0))
MODELS["ni_supercharger"] = dict(kind="au", codes=("aufx", "Ni$@", "-NI-"), fixed={}, params=dict(drive=2, dirt=4, punch=3, mix=5, trim=0),
                                 defaults=dict(drive=0.5, dirt=1.0), drive="drive", drives=[0.1, 0.5, 1.0])
MODELS["pa_hg2"] = dict(kind="au", codes=("aufx", "BAH2", "Brwx"), fixed={2: 1.0}, params=dict(drive=3, pentode=5, triode=6, density=10, alt=4, satfreq=1),
                        defaults=dict(drive=0.5, pentode=0.5, triode=0.5), drive="drive", drives=[0.1, 0.5, 1.0])
variant("pa_hg2", "pa_hg2_pentode", sweep="pentode", drives=(0.0, 0.5, 1.0))
variant("pa_hg2", "pa_hg2_triode", sweep="triode", drives=(0.0, 0.5, 1.0))

# Tone-control sweeps (drive at its default), for the tone-stack numbers in the report.
variant("at5_rat", "at5_rat_filter", sweep="filter", drives=(0.0, 0.5, 1.0))
variant("at5_bigmuff", "at5_bigmuff_tone", sweep="tone", drives=(0.0, 0.5, 1.0))
variant("gr6_bigfuzz", "gr6_bigfuzz_tone", sweep="tone", drives=(0.0, 0.5, 1.0))
variant("at5_tubescreamer", "at5_tubescreamer_tone", sweep="tone", drives=(0.0, 0.5, 1.0))
variant("gr6_skreamer", "gr6_skreamer_tone", sweep="tone", drives=(0.0, 0.5, 1.0))
variant("at5_ds1", "at5_ds1_tone", sweep="tone", drives=(0.0, 0.5, 1.0))

# Guitar Rig 6 as an Audio Unit, one rack component, parameters from the factory INIT preset unless overridden.
def gr6au(name, doc, params, defaults, drive, fixed=None):
    return dict(kind="gr6au", name=name, doc=doc, params=params, defaults=defaults, fixed=fixed or {}, drive=drive, drives=[0.1, 0.5, 1.0])
MODELS["gr6au_skreamer"] = gr6au("Skreamer", "ngrc_Skreamer_doc0.xml", dict(drive="Dr", tone="Tn", volume="Vol", bass="Bs", clean="Cl", bright="Br"), {}, "drive")
# same knob positions as the in-process model gr6_skreamer, to validate the class harness against the real plugin
MODELS["gr6au_skreamer_xcheck"] = gr6au("Skreamer", "ngrc_Skreamer_doc0.xml", dict(drive="Dr", tone="Tn", volume="Vol", bass="Bs", clean="Cl", bright="Br"),
                                        dict(tone=0.5, volume=0.5, bass=0.5, clean=0.0, bright=0.5), "drive")
MODELS["gr6au_cat"] = gr6au("Cat", "ngrc_Cat_doc0.xml", dict(drive="D", filter="F", volume="V", bass="B", balls="Bl", treble="T", tone="O"), {}, "drive")
MODELS["gr6au_cat_xcheck"] = gr6au("Cat", "ngrc_Cat_doc0.xml", dict(drive="D", filter="F", volume="V", bass="B", balls="Bl", treble="T", tone="O"),
                                   dict(filter=0.5, volume=0.5), "drive")
MODELS["gr6au_distortion"] = gr6au("Distortion", "ngrc_Distortion_doc0.xml", dict(drive="Dst", tone="tn", volume="vol", bass="Bs", mid="M", treble="T"), {}, "drive")
MODELS["gr6au_bigfuzz"] = gr6au("Big Fuzz", "ngrc_BigFuzz_doc0.xml", dict(drive="S", tone="T", volume="V", bass="B", treble="R"), {}, "drive")
MODELS["gr6au_skreamer_off"] = gr6au("Skreamer", "ngrc_Skreamer_doc0.xml", dict(drive="Dr", power="Pwr"), dict(power=0), "drive")
