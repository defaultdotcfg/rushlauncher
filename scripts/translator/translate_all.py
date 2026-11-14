"""translate_all.py <game_dir> [--version V]
Shader pipeline used by the launcher (also packaged as olauncher-translator.exe). One pass, in parallel worker processes:
  - every console shader in shaders/pso_h<hash>_<vs|ps|cs>.dxbc (extracted from the game's packs by the launcher, or
    dumped by the bridge) without a translation -> shaders/translated/<hash>.hlsl -> <hash>.cso (dxc, DXIL);
  - .hlsl without an up-to-date .cso (or with an old <hash>.unopt marker) -> recompiled;
  - every tessellation pair shaders/tess/vs<V>_ds<D>.dxbc -> tess_<V>_<D>.dxil.cso (tess_bypass.py).
Shaders that fail are listed in shaders/translated/failed.txt and not retried until the version changes.
Writes shaders/translated/skip.txt (compute kernels the bridge must leave untranslated) when missing.
Progress for the launcher on stdout: "PROGRESS 1 <done> <total>", last line "DONE <new> <failed>".
--optimize is still accepted (older launchers pass it); there is no separate optimization step any more.
Internal: translate_all --gcn2hlsl <src.dxbc> <out.hlsl> (used by tess_bypass inside the packaged exe)."""
import sys, os, json, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


# Bump TRANSLATOR_REV when a translator fix changes shaders that players already have translated, and list
# the kinds to redo for that revision ('vs', 'ps', 'cs', 'tess').
# 2: VS inputs typed from the signature (uint BLENDINDICES); unread VS attributes take no VGPRs (skinned
#    depth prepass/shadow VS -> invisible drivers); precise SV_Position.
# 3: descriptor/constant tracking restarts from the branch state on `else` sides and jump-only labels (TAA, volumetric
#    fog and bindless material PS: 10 refused shaders now translate, 18 read runtime values as 0); root UAV/SRV buffers;
#    f64 ops. A revision bump also retries the shaders listed in failed.txt.
# 4: s_buffer_load_dwordxN whose offset register is one of its own destinations read the later dwords at the new
#    value (material sampler index = garbage -> character faces black, wrong tree textures).
# 5: lens-flare VS (38d8ce79, c94ddda0, f2936dc4): structured vcc branches (`_scalar_if`), so the depth test of the sun position
#    is no longer overwritten by the off-screen else side (flare/bokeh no longer shows through walls and terrain).
# 6: UI backdrop PS (a7d882e2, 9d7f168d, 371650b1, 79f4...): the mip-3 sample of the scene copy is replaced by a blur of mip 0 (the PC target has one
#    mip, LOD 3 came out black: 'ESCOLHA UM VEICULO' / pause screens had a black background instead of the blurred game).
# 7: sky/terrain scene-capture PS (b517a509, 5beb3078, 39e86336): vccz branches structured (`_scalar_if`).
# 8: GPU particle emitters (the 22 counter kernels of the old skip.txt) are enabled: the ExecuteIndirect guard in the bridge made them
#    stable. Nitro jets, auras, wreck/splash particles come from them. An old skip.txt is emptied once.
# 9: sun cascade shadow mask CS (48facb18): its wave-level vccz/vccnz branches (GCN tests the whole 64-lane wave) are structured as
#    "any lane of the 64-thread group" (`_sif_any64`); before, far terrain came out fully shadowed and the pre-race floor was white.
# 10: SOPC with a literal operand (`s_cmp_gt_u32 s2, 0x800`) now consumes the literal dword, which was decoded as a stray
#     v_cndmask; loops holding a group barrier stay real loops when all their exits are uniform (scalar SCC). The GPU particle
#     bitonic sort (c2d308f5, ca754cf8, ...) ran one step only: unsorted keys gave the 16 particle draws 0..2 instances (or -1),
#     so dust, smoke and water splash were missing. 32 shaders change (1 PS, 31 CS).
# 11: SRV and UAV declarations with the same register number (t35 / u35) no longer share one key: the UAV stride
#     overwrote the SRV's, so structured SRVs became raw buffers indexed by the load size (4/8/12 bytes) instead of
#     the record stride (672 for the per-competitor table t35). Particle spawn kernels read the team, vehicle class
#     and transform of every competitor but the first from the wrong place: rush auras came out orange instead of
#     the team blue, and emitters were placed with garbage transforms.
# 12: `ds_read2_b32 v[13:14], v13` (LDS read whose address register is also its first destination) took the second
#     dword from the address just overwritten by the first. GCN reads the address once, at issue. The temporal
#     upscaler (CS 83717de2) got a garbage red value in its neighbourhood min/max: cyan and red fringes and trails
#     behind moving cars and around trees.
# 13: the SCC of 64-bit mask ops and s_cbranch_vcc*/exec* loop exits were per-lane values, so waterfall loops
#     (readfirstlane over a bindless texture index) let each lane leave as soon as its own texture was sampled. Later
#     iterations ran with quad neighbours gone and implicit-derivative samples got undefined mips: blocky, stair-stepped
#     explosions and particles. Now the whole wave stays in the loop while any lane is active, as on GCN.
# 14: tbuffer_load_format_x/xy/xyz with idxen on a constant buffer used the format width (4/8/12 bytes) as the index
#     stride; DXBC cbuffers are float4 registers, the V# stride is 16. The GPU particle lighting CS (31ff8b85) read
#     the sun cascade matrices from the wrong registers: every particle inside the shadow cascades was fully shadowed,
#     so water spray, dust and smoke came out dark, bluish and nearly invisible.
# 15: compute shaders: a forward s_cbranch_vccz/vccnz skips its block only when no lane of the wave has vcc set; it was
#     translated as predicated, so the block always ran, and an `s_branch` over an else side made that side always
#     overwrite the result. Now structured (whole 64-thread group = one GCN wave, or the wave where a group barrier is
#     not safe). 102 CS change: the sky-to-ambient SH (haze never added), sky/fog raymarch results replaced by 0/1, etc.
TRANSLATOR_REV = 15
RETRANSLATE = {2: ('vs', 'tess'), 3: ('vs', 'ps', 'cs'), 4: ('vs', 'ps', 'cs'), 5: ('vs',), 6: ('ps',), 7: ('ps',), 8: (), 9: ('cs',), 10: ('ps', 'cs'),
               11: ('vs', 'ps', 'cs'), 12: ('vs', 'ps', 'cs'), 13: ('vs', 'ps', 'cs'), 14: ('vs', 'ps', 'cs', 'tess'),
               15: ('cs',)}


def say(*a):
    print(*a, flush=True)


def gcn2hlsl_cli(src, out):
    import gcn2hlsl as T
    txt, todo = T.translate(src)
    open(out, 'w', encoding='utf-8').write(txt)
    for pc, t in todo[:25]: print('  ', pc, t, file=sys.stderr)
    return 0


def main(argv):
    if len(argv) >= 4 and argv[1] == '--gcn2hlsl':
        return gcn2hlsl_cli(argv[2], argv[3])
    if len(argv) < 2:
        say(__doc__); return 2
    game = os.path.abspath(argv[1])
    version = argv[argv.index('--version') + 1] if '--version' in argv else 'dev'
    shaders = os.path.join(game, 'shaders'); tr = os.path.join(shaders, 'translated')
    if not os.path.isdir(shaders):
        say('DONE 0 0'); return 0
    os.makedirs(tr, exist_ok=True)
    os.environ['NOCHECK'] = '1'  # build_shader_cache: the workers compile (with dxc) right after translating
    t0 = time.time()

    import build_shader_cache as B, tess_bypass, compile_cso
    flag = os.path.join(tr, compile_cso.CANCEL_FLAG)  # created by the launcher on "Pular"
    if os.path.exists(flag): os.remove(flag)
    cancelled = lambda: os.path.exists(flag)
    patches = B._patches()
    skip_path = os.path.join(tr, 'skip.txt')
    if not os.path.exists(skip_path) and patches.get('_cs_skip'):
        open(skip_path, 'w').write('\n'.join(patches['_cs_skip']) + '\n')

    failed_path = os.path.join(tr, 'failed.txt')
    failed = set()
    if os.path.exists(failed_path):
        lines = open(failed_path).read().split()
        if lines[:2] == ['#version', version]: failed = set(lines[2:])

    # Translator revision: when a fix changes the output for shaders already translated, the kinds listed for
    # the new revisions are translated again; only shaders whose HLSL changed are rewritten (and recompiled).
    rev_path = os.path.join(tr, 'translator_rev.txt')
    try: old_rev = int(open(rev_path).read().split()[0])
    except Exception: old_rev = 1
    redo = sorted({k for r, ks in RETRANSLATE.items() if old_rev < r <= TRANSLATOR_REV for k in ks})
    if old_rev < 8 and os.path.exists(skip_path): open(skip_path, 'w').write('')   # emitters on (rev 8)
    if redo: failed = set()   # a translator fix may make them work: try them again

    # ---- one work list: translate + compile each console shader, recompile stale .cso, build tessellation pairs ----
    import re, collections
    from concurrent.futures import ProcessPoolExecutor, as_completed
    existing = {f[:-5] for f in os.listdir(tr) if f.endswith('.hlsl') and not f.startswith('tess_')}
    jobs, queued = [], set()   # (weight, (function, args...))
    for name in os.listdir(shaders):
        m = re.match(r'pso_h([0-9a-f]{16})_(vs|ps|cs)\.dxbc$', name)
        if not m or m.group(1) in queued: continue
        hx, kind = m.groups()
        f = os.path.join(shaders, name)
        if hx in existing:
            if kind in redo: jobs.append((os.path.getsize(f), (_job_shader, f, hx, kind, 'redo'))); queued.add(hx)
        elif hx not in failed:
            jobs.append((os.path.getsize(f), (_job_shader, f, hx, kind, 'new'))); queued.add(hx)
    for hx in existing - queued:   # translated before but without an up-to-date .cso (or an fxc-unoptimized one)
        f = os.path.join(tr, hx + '.hlsl')
        if compile_cso.needs_work(f): jobs.append((os.path.getsize(f), (_job_shader, f, hx, compile_cso._kind(f), 'compile')))
    for vs_h, ds_h, ds_path in tess_bypass.pairs(game):
        key = 'tess_%s_%s' % (vs_h, ds_h)
        have = os.path.exists(os.path.join(tr, key + '.dxil.cso'))
        if (have and 'tess' not in redo) or (not have and key in failed): continue
        jobs.append((1 << 20, (_job_tess, game, vs_h, ds_h, ds_path, have)))
    jobs.sort(key=lambda j: -j[0])   # big ones first, so the long compiles overlap with the short ones

    stat, reasons, new_failed, new_ok = collections.Counter(), collections.Counter(), set(), 0
    say('PROGRESS 1 0 %d' % len(jobs))
    if jobs and not cancelled():
        with ProcessPoolExecutor(compile_cso.workers(len(jobs))) as pool:
            futs = [pool.submit(*args) for _, args in jobs]
            for i, fut in enumerate(as_completed(futs)):
                key, res, why = fut.result()
                stat[res] += 1
                if res == 'ok': new_ok += 1
                elif res == 'fail': new_failed.add(key); reasons[why] += 1
                say('PROGRESS 1 %d %d' % (i + 1, len(jobs)))
                if cancelled():
                    for f2 in futs: f2.cancel()
    say('shaders: %s' % dict(stat))
    for r, n in reasons.most_common(5): say('  %d %s' % (n, r))
    if cancelled():  # leave failed.txt alone: the next run picks up where this one stopped
        os.remove(flag); say('CANCELLED'); return 1
    failed |= new_failed
    open(failed_path, 'w').write('#version %s\n' % version + '\n'.join(sorted(failed)) + '\n')
    open(rev_path, 'w').write('%d\n' % TRANSLATOR_REV)
    say('time %.1f s' % (time.time() - t0))
    say('DONE %d %d' % (new_ok, len(new_failed)))
    return 0


# ---- worker processes ----
_PATCHES = None


def _job_shader(f, hx, kind, mode):
    """mode 'new': translate + compile; 'redo': translate, compile only if the text changed; 'compile': compile only.
    Returns (key, result, reason), result 'ok', 'unchanged', 'fail' or 'cancel'."""
    global _PATCHES
    import build_shader_cache as B, compile_cso
    tr = os.path.dirname(f) if mode == 'compile' else os.path.join(os.path.dirname(f), 'translated')
    if os.path.exists(os.path.join(tr, compile_cso.CANCEL_FLAG)): return hx, 'cancel', ''
    if mode == 'compile':
        if compile_cso.compile_one(f) == 'ok': return hx, 'ok', ''
        os.remove(f)   # does not compile: dropped, like a failed translation
        return hx, 'fail', '%s: compile' % kind
    if _PATCHES is None: _PATCHES = B._patches()
    body, why = B.translate_one(f, hx, kind, _PATCHES)
    if body is None: return hx, 'fail', why
    dst = os.path.join(tr, hx + '.hlsl')
    if mode == 'redo' and os.path.exists(dst) and os.path.exists(dst[:-5] + '.cso'):
        with open(dst, encoding='utf-8') as fh:
            if fh.read() == body: return hx, 'unchanged', ''
    tmp = dst + '.%d.tmp' % os.getpid()
    with open(tmp, 'w', encoding='utf-8') as fh: fh.write(body)
    os.replace(tmp, dst)
    if compile_cso.compile_one(dst) == 'ok': return hx, 'ok', ''
    os.remove(dst)
    for ext in ('.cso', '.dxil.cso'):   # an older build of a shader that no longer compiles must not stay in use
        if os.path.exists(dst[:-5] + ext): os.remove(dst[:-5] + ext)
    return hx, 'fail', '%s: compile' % kind


def _job_tess(game, vs_h, ds_h, ds_path, changed_only):
    import tess_bypass, compile_cso
    key = 'tess_%s_%s' % (vs_h, ds_h)
    if os.path.exists(os.path.join(game, 'shaders', 'translated', compile_cso.CANCEL_FLAG)): return key, 'cancel', ''
    try:
        return key, tess_bypass.build_pair(game, vs_h, ds_h, ds_path, changed_only), ''
    except Exception as e:
        return key, 'fail', 'tess: ' + str(e)[:80]


if __name__ == '__main__':
    import multiprocessing
    multiprocessing.freeze_support()  # compile_cso uses worker processes (also inside the packaged exe)
    sys.exit(main(sys.argv))
