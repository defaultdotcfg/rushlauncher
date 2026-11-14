"""gcn2hlsl.py shader.dxbc [out.hlsl]  -- GCN (Durango, SI/CI encoding) -> HLSL translator (PS/VS), v2.

Model
  * VGPRs  : `float v0..` (int ops go through asuint/asfloat).
  * SGPRs  : `uint  s0..` real HLSL variables for SALU math. SGPR content loaded from memory is symbolic:
             - s_load / s_buffer_load from user-data pointers become reads of constant buffers / descriptor tables:
               cb reads  -> `cbN[vec].c` (N is a stable name of the descriptor source), as asuint(...)
               descriptors -> remembered in `desc[sgpr]` = ('T'|'S'|'CB', base_name, index_expr)
  * exec / vcc / lane masks (SGPR pairs) : `bool` per-lane variables (m_<n>), exec is bool `ex`.
    Every VALU write is predicated by `ex` (v = ex ? new : v). s_cbranch_* are treated as no-ops (predication makes
    skipped regions harmless) except they are reported.
  * image_sample* are emitted at top level unpredicated (derivatives), results assigned predicated.
Unsupported things are emitted as `// TODO` and counted.
"""
import sys, re as _re, struct, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcn_dis as G

class NoGcnProgram(Exception):
    """The container carries no Xbox (GCN) program: the packs also hold containers the game never creates on the console."""


class _ReCache:
    """`re` with a plain dict of compiled patterns: the translator calls re.fullmatch/search/... with literal patterns
    hundreds of thousands of times per run, and re's own cache lookup (_compile) cost more than the matching."""
    def __init__(self): self._c = {}

    def _p(self, p, flags=0):
        r = self._c.get((p, flags))
        if r is None: r = self._c[(p, flags)] = _re.compile(p, flags)
        return r

    def fullmatch(self, p, s, flags=0): return self._p(p, flags).fullmatch(s)
    def match(self, p, s, flags=0): return self._p(p, flags).match(s)
    def search(self, p, s, flags=0): return self._p(p, flags).search(s)
    def findall(self, p, s, flags=0): return self._p(p, flags).findall(s)
    def finditer(self, p, s, flags=0): return self._p(p, flags).finditer(s)
    def sub(self, p, r, s, count=0, flags=0): return self._p(p, flags).sub(r, s, count)
    def subn(self, p, r, s, count=0, flags=0): return self._p(p, flags).subn(r, s, count)
    def split(self, p, s, maxsplit=0, flags=0): return self._p(p, flags).split(s, maxsplit)
    def __getattr__(self, n): return getattr(_re, n)


re = _ReCache()


class _Env:
    """os.environ.get for the translator's debug switches: a dict snapshot taken by translate() (os.environ lookups
    encode the key on every call and sat in the per-instruction paths)."""
    snap = None

    def get(self, k, d=None):
        s = self.snap
        return (os.environ if s is None else s).get(k, d)


_ENV = _Env()
_PJ = (None, None)   # (mtime, shader_patches.json) loaded once per change


def _patches_json():
    global _PJ
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'shader_patches.json')
    t = os.path.getmtime(p)
    if _PJ[0] != t: _PJ = (t, json.load(open(p)))
    return _PJ[1]
SIF_FORCE = False   # set per shader by build_shader_cache.py (shader_patches.json "_scalar_if")
SIF_ANY64 = False   # set per shader ("_sif_any64"): in a 64-thread CS, structured vccz/vccnz branches test the whole group (= one GCN wave), not each lane

def _lds_sync(body):
    """A 64-thread group is a single wave on the Xbox, so its compiler drops s_barrier around LDS.
    PC GPUs may run it as two wave32s, so add a group barrier wherever LDS use switches between
    writing and reading. Barriers only go at the function's top level (uniform control flow);
    a hazard whose two sides sit in the same nested block is counted and left alone."""
    depth = 0; anchor = 0; acc = []
    for idx, L in enumerate(body):
        if depth == 0: anchor = idx
        if 'lds[' in L:
            acc.append((anchor, 'W' if re.search(r'Interlocked\w+\(lds', L) else 'R'))
        depth += L.count('{') - L.count('}')
    at, bad, prev = set(), 0, None
    for anc, kind in acc:
        if prev is not None and kind != prev[1]:
            if anc != prev[0]: at.add(anc)
            else: bad += 1
        prev = (anc, kind)
    out = []
    for idx, L in enumerate(body):
        if idx in at: out.append("  GroupMemoryBarrierWithGroupSync();")
        out.append(L)
    return out, bad

def _sig_int(e):
    """Signature component type 1 = uint, 2 = int (3 = float). Integer attributes are flat in the PS."""
    return e.get('ct') in (1, 2) and e.get('sv', 0) == 0 and not _ENV.get('NO_INT_SIG')
def _sig_ty(e, nc):
    base = ('uint' if e.get('ct') == 1 else 'int') if _sig_int(e) else 'float'
    return base + (str(nc) if nc > 1 else '')

def parse_dxbc_sig(path):
    b = G.read_file(path)
    n = struct.unpack_from('<I', b, 0x1C)[0]
    chunks = {}
    for i in range(n):
        off = struct.unpack_from('<I', b, 0x20 + 4 * i)[0]
        chunks[b[off:off+4]] = (off + 8, struct.unpack_from('<I', b, off + 4)[0])
    so, ss = chunks[b'SHEX'] if b'SHEX' in chunks else chunks[b'SHDR']
    ver = struct.unpack_from('<I', b, so)[0]
    kind = {0: 'ps', 1: 'vs', 5: 'cs'}.get(ver >> 16, 'ps')
    def sig(tag):
        if tag not in chunks: return []
        o, _ = chunks[tag]
        cnt = struct.unpack_from('<I', b, o)[0]; els = []
        for k in range(cnt):
            e = o + 8 + 24 * k
            nm, idx, sv, ct, reg, mask = struct.unpack_from('<IIIIII', b, e)
            name = b[o + nm:b.index(b'\0', o + nm)].decode()
            # byte 21 = read mask (ISGN) / never-written mask (OSGN)
            els.append(dict(name=name, idx=idx, sv=sv, reg=reg, mask=mask & 0xF, ct=ct, rw=(mask >> 8) & 0xF))
        return els
    return kind, sig(b'ISGN'), sig(b'OSGN')

def parse_decls(path):
    """DXBC SHEX declarations -> {(space, reg): (kind, stride)} for SRVs ('SB' structured, 'RB' raw) and
    {('u', space, reg): (kind, stride)} for UAVs ('USB' structured, 'RUB' raw): t35 and u35 are different resources."""
    b = G.read_file(path)
    n = struct.unpack_from('<I', b, 0x1C)[0]
    out = {}
    for i in range(n):
        off = struct.unpack_from('<I', b, 0x20 + 4 * i)[0]
        if b[off:off+4] not in (b'SHEX', b'SHDR'): continue
        sz = struct.unpack_from('<I', b, off + 4)[0]; d = b[off + 8:off + 8 + sz]
        k = 2
        while k * 4 < sz:
            t = struct.unpack_from('<I', d, k * 4)[0]; op = t & 0x7ff; L = (t >> 24) & 0x7f
            if op == 0x35: k += struct.unpack_from('<I', d, (k + 1) * 4)[0]; continue
            if L == 0: L = 1
            toks = struct.unpack_from('<%dI' % L, d, k * 4)
            if op == 0x58 and L >= 7: out[('tex', toks[6], toks[3], toks[4])] = ((t >> 11) & 0x1f, 0)
            elif op == 0xa2 and L >= 7: out[(toks[6], toks[3])] = ('SB', toks[5])
            elif op == 0x9e and L >= 7: out[('u', toks[6], toks[3])] = ('USB', toks[5])
            elif op == 0x9c and L >= 7: out[('utex', toks[6], toks[3], toks[4])] = ((t >> 11) & 0x1f, 0)
            elif op == 0x9d and L >= 6: out[('u', toks[5], toks[3])] = ('RUB', 0)
            elif op == 0x9b and L >= 4: out['tg'] = (toks[1], toks[2], toks[3])
            elif op == 0x9f and L >= 3: out['tgsm'] = out.get('tgsm', 0) + toks[2]
            elif op == 0xa0 and L >= 4: out['tgsm'] = out.get('tgsm', 0) + toks[2] * toks[3]
            elif op == 0xa1 and L >= 6: out[(toks[5], toks[3])] = ('RB', 0)
            k += L
    return out

F32 = lambda x: f"asfloat({x})"
U32 = lambda x: f"asuint({x})"

LOD0 = False

class Tr:
    def __init__(self, path, pmap=None):
        self.path = path
        self.pmap = pmap          # user-data sgpr -> root param index (None = collect mode)
        self.acc = []             # (ud, kind, static) accesses recorded in collect mode
        self.kind, self.ins, self.outs = parse_dxbc_sig(path)
        # DS_AS_VS=1: translate a domain shader as a plain function of ONE control point (tessellation bypass):
        # LDS control-point reads -> cp_[dword], patch-constant reads -> pc_[dword], domain coords = (0, 0)
        self.ds_cp = bool(_ENV.get('DS_AS_VS'))
        if self.ds_cp: self.kind = 'vs'; self.ins = []
        # LS_AS_FN=1 (with GCN_PROG=<LS program>): the LS writes its control point to LDS -> static cp_[dword]
        self.ls_cp = bool(_ENV.get('LS_AS_FN'))
        self.sg = {}            # sgpr -> ('u', hlsl uint expr) | ('desc', kind, base, idx)
        self.body = []; self.todo = []
        self.decl = set()       # declared resource names
        self.exps = []
        self.usgpr_used = set()
        self.tmp = 0
        self.origin = {}
        self.ranges = {}
        self.cval = {}
        self.embed = None
        self.embeds = []
        self.bufs = {}
        self.sym = {}
        self.decls = parse_decls(path)
        self.code_start = 0
        self.blob = b''
        import rootsig
        try:
            self.rs_ver, self.rs_flags, self.rs_params = rootsig.parse(rootsig.get_rts0(path))
        except Exception:
            self.rs_params = []
    # ---------------- helpers
    def emit(self, s): self.body.append(s)
    def t(self):
        self.tmp += 1; return f"t{self.tmp}"
    def sraw(self, n):
        """uint expression for sgpr n"""
        v = self.sg.get(n)
        if v is None:
            self.usgpr_used.add(n); return f"u{n}"
        if v[0] == 'u': return v[1]
        dd = self.desc_dims(n, v)
        if dd is not None: return dd
        return f"/*desc s{n}*/0u"
    def desc_dims(self, n, v):
        """T# dword 2 (width-1 | height-1 << 14) or dword 4 (depth-1 | pitch-1 << 13) read as data (s_bfe on the
        descriptor, e.g. to get the texture size): rebuilt from GetDimensions. None if not such a dword."""
        if _ENV.get('NO_DESC_DIMS') or v[0] != 'desc' or v[1] != 'T': return None
        e = getattr(self, 'desc_dw', {}).get(n)
        if e is None or e[1] is not v or e[0] % 8 not in (2, 4): return None
        try: tn, tidx = self.resolve('T', v)
        except ValueError: return None
        self.decl.add(('T', tn)); dim = self.tex_dim(tn)
        w, h, d = self.t(), self.t(), self.t()
        tex = f"{tn}[{tidx}]"
        if tn.startswith('Ux'):
            if dim == 5 or dim == 8: self.emit(f"uint {w}, {h}, {d}; {tex}.GetDimensions({w}, {h}, {d});")
            else: self.emit(f"uint {w}, {h}, {d} = 1u; {tex}.GetDimensions({w}, {h});")
        elif dim in (5, 8, 10): self.emit(f"uint {w}, {h}, {d}, {d}_l; {tex}.GetDimensions(0u, {w}, {h}, {d}, {d}_l);")
        else: self.emit(f"uint {w}, {h}, {d} = 1u, {d}_l; {tex}.GetDimensions(0u, {w}, {h}, {d}_l);")
        if dim != 5: self.emit(f"{d} = 1u;")      # arrays keep the slice count in dword 5, not in DEPTH
        if e[0] % 8 == 2: return f"((({w} - 1u) & 0x3FFFu) | ((({h} - 1u) & 0x3FFFu) << 14))"
        return f"((({d} - 1u) & 0x1FFFu) | ((({w} - 1u) & 0x3FFFu) << 13))"
    def lane(self, name):
        return name
    def src(self, t, kind='f'):
        """parse operand text -> HLSL expr of type kind ('f' float, 'u' uint, 'i' int)"""
        t = t.strip(); neg = False; ab = False
        if t.startswith('-') and not re.fullmatch(r'-\d+(\.\d+)?|-0x[0-9a-f]+', t): neg = True; t = t[1:]
        if t.startswith('|') and t.endswith('|'): ab = True; t = t[1:-1]
        if re.fullmatch(r'v\d+', t): base, bk = t, 'u'
        elif re.fullmatch(r's\d+', t): base, bk = self.sraw(int(t[1:])), 'u'
        elif t == 'vcc' or t == 'exec': base, bk = ('vcc' if t == 'vcc' else 'ex'), 'b'
        elif t in ('vcc_lo', 'vcc_hi', 'exec_lo', 'exec_hi', 'm0', 'scc'):
            base, bk = {'scc': 'scc', 'm0': 'm0'}.get(t, t), 'u'
        elif re.fullmatch(r's\[\d+:\d+\]', t):
            base, bk = 'sp_' + t[2:-1].split(':')[0], 'b'
        elif re.fullmatch(r'-?0x[0-9a-f]+', t):
            v = int(t, 0) & 0xFFFFFFFF; base, bk = f"0x{v:x}u", 'u'
        elif re.fullmatch(r'-?\d+\.\d+', t):
            base, bk = t, 'F'
        elif re.fullmatch(r'-?\d+', t):
            base, bk = str(int(t)), 'I'
        else:
            base, bk = f"/*{t}*/0", 'f'
        # convert base(bk) -> kind
        if bk == 'F': e = {'f': base, 'u': f"asuint({base})", 'i': f"asint({base})"}[kind]
        elif bk == 'I':
            iv = int(base)
            e = {'f': f"asfloat({iv & 0xFFFFFFFF}u)", 'u': f"{iv & 0xFFFFFFFF}u", 'i': f"({iv})"}[kind]
        elif bk == 'f': e = {'f': base, 'u': f"asuint({base})", 'i': f"asint({base})"}[kind]
        elif bk == 'u': e = {'f': f"asfloat({base})", 'u': base, 'i': f"asint({base})"}[kind]
        elif bk == 'b': e = {'f': f"asfloat({base} ? 0xffffffffu : 0u)", 'u': f"({base} ? 0xffffffffu : 0u)", 'i': f"({base} ? -1 : 0)"}[kind]
        else: e = base
        if ab: e = f"abs({e})"
        if neg:
            e = f"(-{e})" if kind == 'f' else (f"(0u-{e})" if kind == 'u' else f"(-{e})")
        return e
    def setv(self, d, expr):
        e = expr.strip()
        if e.startswith('asfloat(') and e.endswith(')'):
            dep = 0; ok = True
            for i_, ch_ in enumerate(e[7:], 7):
                if ch_ == '(': dep += 1
                elif ch_ == ')':
                    dep -= 1
                    if dep == 0 and i_ != len(e) - 1: ok = False; break
            e = e[8:-1] if ok else f"asuint({e})"
        else: e = f"asuint({e})"
        self.emit(f"v{d} = ex ? ({e}) : v{d};")
    def setu(self, d, expr, ty='u'):
        self.origin.pop(d, None); self.cval.pop(d, None); self.sym.pop(d, None)
        for _b in (d, d - 1):
            getattr(self, 'embed_regs', {}).pop(_b, None)
        self.sg[d] = ('u', f"({expr})")
        self.emit(f"u{d} = {expr};"); self.sg[d] = ('u', f"u{d}"); self.usgpr_used.add(d); self.decl.add(f"u{d}")
    def ps_input_ena(self):
        import struct as _st
        bl, s0 = getattr(self, 'blob', None), getattr(self, 'code_start', 0)
        if not bl or s0 < 868 or _st.unpack_from('<I', bl, 40)[0] != 0xd98: return 0x2
        return _st.unpack_from('<I', bl, 864)[0]
    @property
    def has_pos(self): return any(e['sv'] == 1 or e['name'].upper() == 'SV_POSITION' for e in self.ins)
    @property
    def has_ff(self): return any(e['name'].upper() == 'SV_ISFRONTFACE' for e in self.ins)
    # A loop holding a group barrier may stay a real loop when every exit/continue is uniform: its condition is
    # `true` or a SCC that was last set from scalar values only (s_cmp on SGPRs/constants, e.g. the bitonic-sort
    # stage counters `s_cmp_gt_u32 s2, 0x800`). Commenting such a loop out ran its body once and left the
    # particle sort keys unsorted (splash/spray draw ranges of 0..2 particles). NO_UNIFORM_LOOPS=1 = old behaviour.
    def _uniform_loop(self, at, extra):
        if _ENV.get('NO_UNIFORM_LOOPS'): return False
        for e in extra:
            L = self.body[e].strip()
            if L == '}': continue
            m = re.match(r'if \(!?\(?(.*?)\)?\) (break|continue);', L)
            if not m: return False
            cond = m.group(1).strip('() ')
            if cond == 'true': continue
            if cond not in ('scc != 0u', 'scc == 0u'): return False
            last = next((B for B in reversed(self.body[:e]) if B.lstrip().startswith('scc = ')), None)
            if last is None or re.search(r'\bex\b|\bvcc\b|\bsp_\d+|\bv\d+\b|\blane\b|\bflat\b', last): return False
        return True

    # ---------------- translation
    def run(self):
        raw = G.read_file(self.path)
        blob = G.extract_code_from_dxbc(self.path)
        if blob is None: raise NoGcnProgram()
        s0, e0 = G.find_code(blob)
        self.blob = blob; self.code_start = s0
        lines = G.disasm(blob[s0:e0])
        ins = []
        for ln in lines:
            m = re.match(r'([0-9a-f]+): ((?:[0-9a-f]{8} ?)+)\s+(\S+)\s*(.*)', ln)
            if not m: continue
            pc, _, op, args = m.groups()
            ins.append((pc, op, args.split(';')[0].strip()))
        # Structured scalar branches: a forward s_cbranch_scc*/vcc* becomes `if (not-taken) {`, closed at its label;
        # `s_branch L` as the last instruction of such a block turns it into `} else {` up to L.
        # opt-in for CS (globally it blacked out the compute lighting; see RESUMO); on by default for VS/PS,
        # where an ignored uniform branch runs both sides and the else side overwrites the result.
        structured = (_ENV.get('SCALAR_IF') is not None or SIF_FORCE
                      or (self.kind in ('vs', 'ps') and not _ENV.get('NO_GFX_SCALAR_IF')))
        blk = []   # stack of [end pc, index of the `if` line in self.body, [indices of `} else {` lines]]
        def close_block():
            end, at, elses = blk.pop()[:3]
            if any('GroupMemoryBarrier' in L for L in self.body[at:]):
                # a group barrier must not sit in (possibly) varying flow control: fall back to predication for this block
                self.body[at] = '// ' + self.body[at] + ' (unstructured: barrier inside)'
                for e in elses: self.body[e] = '// } else {'
            else:
                self.emit("}")
        def tgt(a):
            mm = re.search(r'label_([0-9a-f]+)', a); return int(mm.group(1), 16) if mm else None
        # Loops: a backward branch to label T becomes `[loop] for (...) { <T..P> if (!taken) break; }`.
        LOOP_OPS = {'s_cbranch_scc1': 'scc != 0u', 's_cbranch_scc0': 'scc == 0u', 's_cbranch_vccnz': 'vcc', 's_cbranch_vccz': '!vcc',
                    's_cbranch_execnz': 'ex', 's_cbranch_execz': '!ex', 's_branch': 'true'}
        loops_on = _ENV.get('NO_LOOPS') is None
        heads = {}
        if loops_on:
            for (pc_, op_, args_) in ins:
                T_ = tgt(args_) if op_ in LOOP_OPS else None
                if T_ is not None and T_ <= int(pc_, 16): heads[T_] = max(heads.get(T_, 0), int(pc_, 16))
            # keep only properly nested loops
            spans = sorted(heads.items(), key=lambda kv: (kv[0], -kv[1])); keep = {}; stack_ = []
            for h_, t_ in spans:
                while stack_ and stack_[-1][1] < h_: stack_.pop()
                if stack_ and t_ > stack_[-1][1]: continue      # overlaps its parent without nesting
                keep[h_] = t_; stack_.append((h_, t_))
            heads = keep
        lps = []   # stack of [head pc, tail pc, index of the `for` line, [extra line indices]]
        nloop = [0]
        # Else-gate (predicated mode): `s_cbranch_scc* L ... s_branch E ; L: <else> ; E:` - the <else> block is only
        # reachable through the scalar branch, so it runs only when that branch was taken (jk_L), instead of always
        # overwriting the `then` result. NO_ELSE_GATE=1 = old behaviour.
        egate_on = not _ENV.get('NO_ELSE_GATE')
        # per-shader exclusions: shader_patches.json "_else_gate_skip": {"<fnv>": ["<label hex>", ...]}
        # (e.g. volumetric-fog blocks whose input volume is not produced yet: gating them darkens everything)
        eg_skip = set()
        try:
            _pj = _patches_json()
            _mh = re.search(r'pso_h([0-9a-f]{16})_', os.path.basename(self.path))
            if _mh: eg_skip = {int(x, 16) for x in _pj.get('_else_gate_skip', {}).get(_mh.group(1), [])}
        except Exception: pass
        eg_skip |= {int(x, 16) for x in _ENV.get('ELSE_GATE_SKIP', '').split(',') if x}
        scc_tgts = {}
        for (pc_, op_, args_) in ins:
            if op_ in ('s_cbranch_scc0', 's_cbranch_scc1'):
                T_ = tgt(args_)
                if T_ is not None: scc_tgts[T_] = scc_tgts.get(T_, 0) + 1
        jk = set()       # targets whose jk_ flag was set by a uniform forward scc branch
        gates = []       # stack of [end pc, index of the `if` line]
        def close_gate():
            end, at = gates.pop()
            if any('GroupMemoryBarrier' in L for L in self.body[at:]): self.body[at] = '// ' + self.body[at] + ' (gate dropped: barrier inside)'
            else: self.emit("}")
        # Descriptor tracking and compile-time constants at labels reached only by a jump (the previous instruction is s_branch/s_endpgm): start from
        # the state at the branch that jumps there, not from the end of the code above it (that code may reuse the
        # SGPRs holding a T#/S# or a table pointer for something else). NO_LABEL_DESC=1 = old behaviour.
        fwd_snap = {}; label_desc = not _ENV.get('NO_LABEL_DESC')
        for k, (pc, op, args) in enumerate(ins):
            P = int(pc, 16)
            if label_desc:
                if P in fwd_snap and k > 0 and ins[k - 1][1] in ('s_branch', 's_endpgm'):
                    self.sg, self.origin, self.sym, self.cval = (dict(x) for x in fwd_snap[P])
                if op.startswith('s_cbranch'):
                    T_ = tgt(args)
                    if T_ is not None and T_ > P: fwd_snap[T_] = (dict(self.sg), dict(self.origin), dict(self.sym), dict(self.cval))
            while gates and gates[-1][0] == P:
                close_gate()
            while blk and blk[-1][0] == P:
                close_block()
            if P in heads:
                nloop[0] += 1; v_ = f"lp{nloop[0]}"
                self.emit(f"[loop] for (uint {v_} = 0; {v_} < 4096u; ++{v_}) {{   // loop head label_{P:04x}")
                lps.append([P, heads[P], len(self.body) - 1, []])
            if op in LOOP_OPS and lps:
                T = tgt(args)
                if T is not None and T <= P:
                    cond = LOOP_OPS[op]
                    # s_cbranch_vcc*/exec* test the whole wave's mask: keep every lane in the loop while any lane is
                    # still active (the body is predicated by ex), as on GCN (see the mask-op SCC note).
                    if cond in ('vcc', '!vcc', 'ex', '!ex') and not _ENV.get('PERLANE_LOOPS'):
                        cond = cond.replace(cond.lstrip('!'), f"WaveActiveAnyTrue({cond.lstrip('!')})")
                    if T == lps[-1][0] and P == lps[-1][1]:
                        self.emit(f"if (!({cond})) break;   // {op} {args}"); lps[-1][3].append(len(self.body) - 1)
                        self.emit("}"); lps[-1][3].append(len(self.body) - 1)
                        h_, t_, at_, extra_ = lps.pop()
                        if any('GroupMemoryBarrier' in L for L in self.body[at_:]) and not self._uniform_loop(at_, extra_):
                            self.body[at_] = '// ' + self.body[at_] + ' (loop unstructured: barrier inside)'
                            for e_ in extra_: self.body[e_] = '// ' + self.body[e_]
                        continue
                    if T == lps[-1][0]:
                        self.emit(f"if ({cond}) continue;   // {op} {args}"); lps[-1][3].append(len(self.body) - 1); continue
                elif T is not None and T > lps[-1][1]:
                    # forward branch leaving the innermost loop: break
                    c_ = LOOP_OPS[op]
                    if c_ in ('vcc', '!vcc', 'ex', '!ex') and not _ENV.get('PERLANE_LOOPS'):
                        c_ = c_.replace(c_.lstrip('!'), f"WaveActiveAnyTrue({c_.lstrip('!')})")
                    self.emit(f"if ({c_}) break;   // exit {op} {args}"); lps[-1][3].append(len(self.body) - 1); continue
            # Loop guard (CS too): a uniform forward s_cbranch_scc* that stays inside the current loop, e.g.
            # `s_cmp_ge i, n ; s_cbranch_scc0 L ; s_andn2 keep, keep, exec ; s_cbranch_scc0 exit ; L:` - predicating it ran the
            # andn2 every iteration and killed every lane at once (clustered light/probe lists came out empty). NO_LOOP_SIF=1 = old.
            loop_sif = (not structured and lps and not _ENV.get('NO_LOOP_SIF') and op in ('s_cbranch_scc0', 's_cbranch_scc1')
                        and tgt(args) is not None and P < tgt(args) <= lps[-1][1] and (not gates or tgt(args) <= gates[-1][0]))
            # CS (predicated mode): a forward s_cbranch_vccz/vccnz skips its block only when NO lane of the wave has vcc set; run
            # predicated, the block ran even when the branch was taken, and an `s_branch` over an else side let that side always
            # overwrite the result (SH ambient without haze, sky/fog raymarch results replaced by 0/1). Structured with the
            # wave-level test (or the whole 64-thread group, = one GCN wave, where a group barrier is safe). NO_CS_VCC_SIF=1 = old.
            vcc_sif = (not structured and not loop_sif and self.kind == 'cs' and not _ENV.get('NO_CS_VCC_SIF')
                       and op in ('s_cbranch_vccz', 's_cbranch_vccnz'))
            if vcc_sif:
                T = tgt(args)
                if (T is not None and T > P and (not blk or T <= blk[-1][0]) and (not gates or T <= gates[-1][0])
                        and (not lps or T <= lps[-1][1]) and not any(P < h_ < T <= t_ for h_, t_ in heads.items())):
                    grp64 = (lambda tg_: tg_[0] * tg_[1] * tg_[2] == 64)(self.decls.get('tg', (1, 1, 1)))
                    wave = not (grp64 and not lps and all(not b_[4] for b_ in blk if len(b_) > 4))
                    if wave: cond = 'WaveActiveAnyTrue(vcc)' if op == 's_cbranch_vccz' else '!WaveActiveAnyTrue(vcc)'
                    else: cond = 'any64_(vcc, flat)' if op == 's_cbranch_vccz' else '!any64_(vcc, flat)'; self.uses_any64 = True
                    self.emit(f"[branch] if ({cond}) {{   // {op} {args}")
                    blk.append([T, len(self.body) - 1, [], (dict(self.sym), dict(self.cval), dict(self.sg), dict(self.origin)), wave]); continue
            if (structured or loop_sif) and (op in ('s_cbranch_scc0', 's_cbranch_scc1') or ((_ENV.get('SCALAR_IF_VCC') or SIF_FORCE) and op in ('s_cbranch_vccz', 's_cbranch_vccnz'))):
                T = tgt(args)
                uniform = True
                if op.startswith('s_cbranch_scc'):
                    last = next((L for L in reversed(self.body) if L.lstrip().startswith('scc = ')), None)
                    # SCC computed from exec/vcc/lane masks or VGPRs is per-lane here: structuring would skip exec restores
                    if last is None or re.search(r'\bex\b|\bvcc\b|\bsp_\d+|\bv\d+\b', last): uniform = False
                if uniform and T is not None and T > P and (not blk or T <= blk[-1][0]):
                    cond = {'s_cbranch_scc0': 'scc != 0u', 's_cbranch_scc1': 'scc == 0u', 's_cbranch_vccz': 'vcc', 's_cbranch_vccnz': '!vcc'}[op]
                    if SIF_ANY64 and self.kind == 'cs' and op.startswith('s_cbranch_vcc') and (lambda tg_: tg_[0] * tg_[1] * tg_[2] == 64)(self.decls.get('tg', (1, 1, 1))):
                        cond = 'any64_(vcc, flat)' if op == 's_cbranch_vccz' else '!any64_(vcc, flat)'; self.uses_any64 = True
                    self.emit(f"[branch] if ({cond}) {{   // {op} {args}"); blk.append([T, len(self.body) - 1, [], (dict(self.sym), dict(self.cval), dict(self.sg), dict(self.origin))]); continue
            if (structured or (blk and len(blk[-1]) > 4)) and op == 's_branch' and blk:
                T = tgt(args); nxt = int(ins[k + 1][0], 16) if k + 1 < len(ins) else None
                if T is not None and nxt is not None and nxt == blk[-1][0] and T >= nxt and (len(blk) < 2 or T <= blk[-2][0]):
                    self.emit("} else {"); blk[-1][0] = T; blk[-1][2].append(len(self.body) - 1)
                    # the else side starts from the state at the branch, not from the end of the `then` side: restore the
                    # compile-time constants/symbolic offsets/descriptor tracking (a `then` that reused s2 for a float constant turned the
                    # else side's dynamic descriptor offset into a huge static one). NO_ELSE_SYM=1 = old behaviour.
                    if len(blk[-1]) > 3 and not _ENV.get('NO_ELSE_SYM'):
                        self.sym, self.cval, self.sg, self.origin = (dict(x) for x in blk[-1][3])
                    continue
            if egate_on and not structured:
                if op in ('s_cbranch_scc0', 's_cbranch_scc1'):
                    T = tgt(args)
                    last = next((L for L in reversed(self.body) if L.lstrip().startswith('scc = ')), None)
                    uni = last is not None and not re.search(r'\bex\b|\bvcc\b|\bsp_\d+|\bv\d+\b', last)
                    if uni and T is not None and T > P and scc_tgts.get(T) == 1 and T not in eg_skip:
                        self.emit(f"jk_{T:x} = (scc {'!=' if op == 's_cbranch_scc1' else '=='} 0u);   // {op} {args} taken")
                        jk.add(T)
                elif op == 's_branch':
                    T = tgt(args); nxt = int(ins[k + 1][0], 16) if k + 1 < len(ins) else None
                    if (T is not None and nxt is not None and T > nxt and nxt in jk
                            and not any(nxt <= h_ < T and t_ >= T for h_, t_ in heads.items())
                            and (not lps or T <= lps[-1][1]) and (not gates or T <= gates[-1][0])):
                        self.emit(f"[branch] if (jk_{nxt:x}) {{   // else-gate: s_branch {args} skips label_{nxt:04x}..")
                        gates.append([T, len(self.body) - 1]); continue
            try:
                self.one(pc, op, args)
            except Exception as ex:
                m_ = re.match(r'v(?:\[(\d+):(\d+)\]|(\d+))', args.strip()) if op.startswith('image_sample') and _ENV.get('STUB_IMAGE') else None
                if m_:
                    # STUB_IMAGE=1: a sample whose descriptor cannot be resolved (dead path in practice) returns 0
                    lo_ = int(m_.group(1) or m_.group(3)); hi_ = int(m_.group(2) or m_.group(3))
                    self.emit(f"// STUB {pc}: {op} {args} -> 0 ({ex})")
                    for r_ in range(lo_, hi_ + 1): self.setv(r_, "0.0")
                else:
                    self.todo.append((pc, f"{op} {args}  !! {type(ex).__name__}: {ex}"))
        while gates: close_gate()
        while blk: close_block()
        smpc = {x[1] for x in self.decl if isinstance(x, tuple) and x[0] == 'SC'}
        reps = []
        for key, sn_, orig, alt in getattr(self, 'samp_alt', []):
            rep = orig
            if sn_ in smpc:
                if alt is None or alt[0] in smpc: raise ValueError(f"sampler {sn_} used with and without compare, no plain fallback")
                rep = f"{alt[0]}[{alt[1]}]"; self.decl.discard(('S', sn_))
            reps.append((key, rep))
        res = self.assemble()
        txt = res[0] if isinstance(res, tuple) else res
        for key, rep in reps: txt = txt.replace(key, rep)
        return (txt,) + tuple(res[1:]) if isinstance(res, tuple) else txt

    def split_args(self, args):
        out = []; depth = 0; cur = ''
        for c in args:
            if c in '[(': depth += 1
            if c in '])': depth -= 1
            if c == ',' and depth == 0: out.append(cur.strip()); cur = ''
            else: cur += c
        if cur.strip(): out.append(cur.strip())
        return out

    def mask_target(self, t):
        """SGPR-pair / vcc destination -> bool variable name"""
        if t == 'vcc': return 'vcc'
        if t == 'exec': return 'ex'
        m = re.fullmatch(r's\[(\d+):\d+\]', t)
        return f"sp_{m.group(1)}" if m else None

    def one(self, pc, op, args):
        a = self.split_args(re.sub(r' (clamp|omod:\d)', '', args))
        def mods(a):
            clamp = 'clamp' in args.split(' ')[-2:] if args else False
            om = re.search(r'omod:(\d)', args)
            return ('clamp' in args), (int(om.group(1)) if om else 0)
        def fin(expr, args):
            clamp, om = mods(a)
            if om: expr = f"(({expr}) * {['1','2','4','0.5'][om]})"
            if clamp: expr = f"saturate({expr})"
            return expr
        A = [x for x in a if not (x.startswith('clamp') or x.startswith('omod') or x.startswith('dmask') or x == 'unorm')]
        def vd(): return int(A[0][1:])
        if op in ('s_add_u32', 's_addc_u32') and getattr(self, 'pcnext', None):
            def cvv(t):
                t = t.strip()
                if re.fullmatch(r's\d+', t):
                    r_ = int(t[1:]); v_ = self.cval.get(r_)
                    return v_ if v_ is not None else getattr(self, 'slit', {}).get(r_)
                if re.fullmatch(r'-?(0x[0-9a-f]+|\d+)', t): return int(t, 0) & 0xFFFFFFFF
                return None
            m1 = re.fullmatch(r's(\d+)', A[1].strip())
            if m1 and A[0].startswith('s') and not A[0].startswith('s['):
                d = int(A[0][1:]); r1 = int(m1.group(1))
                if op == 's_add_u32' and r1 in self.pcnext and cvv(A[2]) is not None:
                    self.emb_tmp = getattr(self, 'emb_tmp', {}); self.emb_tmp[d] = self.pcnext[r1] + cvv(A[2]); return
                if op == 's_addc_u32' and (r1 - 1) in self.pcnext and (d - 1) in getattr(self, 'emb_tmp', {}) and cvv(A[2]) is not None:
                    self.embed_regs = getattr(self, 'embed_regs', {})
                    self.embed_regs[d - 1] = dict(base=self.emb_tmp[d - 1], stride=(cvv(A[2]) >> 16) & 0x3FFF); return
        if op == 's_barrier':
            self.emit("GroupMemoryBarrierWithGroupSync();"); self.uses_lds = True; self.has_s_barrier = True; return
        if op in ('s_waitcnt', 's_nop', 's_endpgm', 's_branch', 's_sendmsg', 's_swappc_b64', 's_setpc_b64'): return
        if op.startswith('ds_'):
            return self.ds(op, args)
        if op in ('s_cbranch_execz', 's_cbranch_execnz', 's_cbranch_vccz', 's_cbranch_vccnz', 's_cbranch_scc0', 's_cbranch_scc1'):
            self.emit(f"// branch {op} {args} (ignored: predicated)"); return
        # ---- scalar
        if op == 's_bfm_b32' and re.fullmatch(r's\d+', A[0]):
            d = int(A[0][1:])
            self.setu(d, f"(((1u << ({self.src(A[1], 'u')} & 31u)) - 1u) << ({self.src(A[2], 'u')} & 31u))"); return
        if op == 's_addk_i32':
            d = int(A[0][1:]); self.setu(d, f"{self.sraw(d)} + {int(A[1]) & 0xFFFFFFFF}u"); return
        if op in ('s_cmovk_i32', 's_cmov_b32') and re.fullmatch(r's\d+', A[0]):
            d = int(A[0][1:])
            val = f"{int(A[1], 0) & 0xFFFFFFFF}u" if op == 's_cmovk_i32' else self.src(A[1], 'u')
            self.setu(d, f"(scc ? {val} : {self.sraw(d)})"); return
        if op in ('s_mov_b32', 's_movk_i32'):
            if A[0].startswith('s') and not A[0].startswith('s['):
                d = int(A[0][1:]); sv = A[1]
                er = getattr(self, 'embed_regs', {})
                self.slit = getattr(self, 'slit', {})   # s_mov_b32 literals (only for the s_getpc embedded-table pattern)
                if op == 's_mov_b32' and re.fullmatch(r'-?(0x[0-9a-f]+|\d+)', sv.strip()): self.slit[d] = int(sv.strip(), 0) & 0xFFFFFFFF
                else: self.slit.pop(d, None)
                msv = re.fullmatch(r's(\d+)', sv)
                if op == 's_mov_b32' and msv:
                    n_ = int(msv.group(1))
                    if (n_ - 1) in er and (d - 1) in er and er[d - 1] is er[n_ - 1]: return     # hi half of an embedded base copy
                    if n_ in er:
                        self.setu(d, self.src(sv, 'u')); self.embed_regs[d] = er[n_]; return
                # d no longer holds an embedded-table base (1.80 CS reuse s[16:19] for a UAV counter V# built from
                # the counter pointer at UAV descriptor +16; the stale entry made that atomic "embedded")
                if d in er and not _ENV.get('OLD_EMBED_KEEP'): er.pop(d, None)
                if op == 's_movk_i32': self.setu(d, f"{int(sv) & 0xFFFFFFFF}u"); self.cval[d] = int(sv) & 0xFFFFFFFF; self.sym[d] = (int(sv) & 0xFFFFFFFF, None, 1)
                elif re.fullmatch(r's\d+', sv) and self.sg.get(int(sv[1:]), ('u',))[0] == 'desc':
                    self.sg[d] = self.sg[int(sv[1:])]
                    if int(sv[1:]) in getattr(self, 'desc_dw', {}): self.desc_dw[d] = self.desc_dw[int(sv[1:])]
                else:
                    self.setu(d, self.src(sv, 'u'))
                    if re.fullmatch(r'-?(0x[0-9a-f]+|\d+)', sv): self.cval[d] = int(sv, 0) & 0xFFFFFFFF; self.sym[d] = (int(sv, 0) & 0xFFFFFFFF, None, 1)
                    if re.fullmatch(r's\d+', sv):
                        n = int(sv[1:])
                        if n not in self.sg or n in self.origin: self.origin[d] = self.origin.get(n, n)
                        if n not in self.sg or n in self.origin: self.origin_hist = getattr(self, 'origin_hist', {}); self.origin_hist[d] = self.origin.get(n, n)
            elif A[0] == 'm0':
                # m0 matters only for relative VGPR indexing (v_movrels); LDS-limit writes are harmless
                self.emit(f"m0 = {self.src(A[1], 'u')};")
            return
        if op in ('s_mov_b64', 's_wqm_b64'):
            dt = self.mask_target(A[0]); st = A[1]
            if dt is None: return
            if re.fullmatch(r's\[\d+:\d+\]', st) or st in ('vcc', 'exec'): sn = self.mask_target(st)
            else: sn = 'true' if st in ('0xffffffff', '-1') else 'false'
            self.emit(f"{dt} = {sn};"); self.decl.add(dt); return
        if op in ('s_and_b64', 's_or_b64', 's_xor_b64', 's_andn2_b64', 's_orn2_b64', 's_nand_b64', 's_nor_b64'):
            dt = self.mask_target(A[0]); x = self.src(A[1], 'b') if False else self.maskval(A[1]); y = self.maskval(A[2])
            e = {'s_and_b64': f"{x} && {y}", 's_or_b64': f"{x} || {y}", 's_xor_b64': f"{x} != {y}", 's_andn2_b64': f"{x} && !{y}",
                 's_orn2_b64': f"{x} || !{y}", 's_nand_b64': f"!({x} && {y})", 's_nor_b64': f"!({x} || {y})"}[op]
            # SCC of a 64-bit mask op is "result != 0" over the whole wave, not a per-lane value: a per-lane SCC made
            # waterfall loops (`s_andn2 todo, todo, exec ; s_cbranch_scc0 done`) exit lane by lane, so later
            # iterations ran with quad neighbours gone and implicit-derivative samples picked garbage mips
            # (blocky explosions / stair-stepped particle edges). PERLANE_LOOPS=1 = old behaviour.
            self.emit(f"{dt} = {e};"); self.decl.add(dt)
            self.emit(("scc = (" + dt + ");") if _ENV.get('PERLANE_LOOPS') else f"scc = WaveActiveAnyTrue({dt}) ? 1u : 0u;"); return
        if op == 's_and_saveexec_b64':
            dt = self.mask_target(A[0]); x = self.maskval(A[1])
            if re.search(rf"\b{dt}\b", x) and not _ENV.get('OLD_SAVEEXEC'):
                # s_and_saveexec s[N:N+1], s[N:N+1]: read the condition before the save overwrites it
                self.decl.add(dt)
                self.emit(f"sx_ = {x};"); self.emit(f"{dt} = ex;"); self.emit("ex = ex && sx_;"); return
            self.emit(f"{dt} = ex;"); self.emit(f"ex = ex && {x};"); self.decl.add(dt); return
        if op in ('s_lshl_b32', 's_lshr_b32', 's_ashr_i32', 's_add_u32', 's_sub_u32', 's_and_b32', 's_or_b32', 's_xor_b32', 's_mul_i32',
                  's_min_u32', 's_max_u32', 's_addc_u32', 's_bitset1_b32', 's_bitset0_b32', 's_lshl_b64', 's_andn2_b32', 's_cselect_b32', 's_bfe_u32', 's_bfe_i32', 's_min_i32', 's_max_i32'):
            if not A[0].startswith('s') or A[0].startswith('s['):
                self.todo.append((pc, f"{op} {args}")); return
            d = int(A[0][1:])
            if op == 's_bitset1_b32': self.setu(d, f"{self.sraw(d)} | (1u << {self.src(A[1],'u')})"); return
            if op == 's_bitset0_b32': self.setu(d, f"{self.sraw(d)} & ~(1u << {self.src(A[1],'u')})"); return
            x = self.src(A[1], 'u'); y = self.src(A[2], 'u')
            e = {'s_lshl_b32': f"{x} << ({y} & 31u)", 's_lshr_b32': f"{x} >> ({y} & 31u)", 's_ashr_i32': f"asuint(asint({x}) >> ({y} & 31u))",
                 's_add_u32': f"{x} + {y}", 's_sub_u32': f"{x} - {y}", 's_and_b32': f"{x} & {y}", 's_or_b32': f"{x} | {y}", 's_xor_b32': f"{x} ^ {y}",
                 's_mul_i32': f"asuint(asint({x}) * asint({y}))", 's_min_u32': f"min({x},{y})", 's_max_u32': f"max({x},{y})",
                 's_addc_u32': f"{x} + {y} + 0u", 's_andn2_b32': f"{x} & ~{y}", 's_min_i32': f"asuint(min(asint({x}),asint({y})))", 's_max_i32': f"asuint(max(asint({x}),asint({y})))",
                 's_cselect_b32': f"scc ? {x} : {y}",
                 's_bfe_u32': f"({x} >> ({y} & 31u)) & ((1u << (({y} >> 16) & 127u)) - 1u)",
                 's_bfe_i32': f"asuint(int({x} << ((32u - (({y} >> 16) & 31u) - ({y} & 31u)) & 31u)) >> ((32u - (({y} >> 16) & 31u)) & 31u))",
                 's_bfm_b32': f"((1u << ({x} & 31u)) - 1u) << ({y} & 31u)"}.get(op)
            if e is None: self.todo.append((pc, f"{op} {args}")); return
            if op == 's_lshl_b64': self.todo.append((pc, f"{op} {args}")); return
            try:
                def sy(t):
                    t = t.strip()
                    if re.fullmatch(r's\d+', t): return self.sym.get(int(t[1:])) or (0, f"u{int(t[1:])}", 1)
                    if re.fullmatch(r'-?(0x[0-9a-f]+|\d+)', t): return (int(t, 0) & 0xFFFFFFFF, None, 1)
                    return None
                x_, y_ = sy(A[1]), sy(A[2])     # read source symbols BEFORE setu clears the destination (d may equal a source)
                # the symbolic dynamic term names a register variable; if this instruction overwrites that register,
                # snapshot its current value so later index expressions still see the pre-shift/pre-add value
                # Any raw register named by the dynamic term is snapshotted (not only d): the code may reuse that
                # register before the descriptor load (e.g. 's_lshl s0, s2, 5' then 's_mov s2, s7'), and the index
                # would then read the new value (bloom downsample wrote into UAV 0 = the 1920x1080 scene buffer).
                # OLD_DYN_SNAP=1 keeps the old rule (snapshot only when d is that register).
                if op in ('s_lshl_b32', 's_add_u32'):
                    for z_i, z_ in enumerate((x_, y_)):
                        if not (z_ and z_[1] and re.fullmatch(r'u\d+', z_[1])): continue
                        if _ENV.get('OLD_DYN_SNAP') is not None and z_[1] != f"u{d}": continue
                        snap = self.t(); self.emit(f"uint {snap} = {z_[1]};")
                        if z_i == 0: x_ = (x_[0], snap, x_[2])
                        else: y_ = (y_[0], snap, y_[2])
            except Exception:
                x_ = y_ = None
            self.setu(d, e)
            try:
                if x_ and y_:
                    if op == 's_lshl_b32' and y_[1] is None: self.sym[d] = (x_[0] << y_[0], x_[1], x_[2] << y_[0])
                    elif op == 's_add_u32' and (x_[1] is None or y_[1] is None) and not (x_[1] and y_[1]):
                        dd = x_ if x_[1] else y_; cc = y_ if x_[1] else x_
                        self.sym[d] = (dd[0] + cc[0], dd[1], dd[2])
                    elif op == 's_add_u32' and x_[1] is None and y_[1] is None: self.sym[d] = (x_[0] + y_[0], None, 1)
            except Exception:
                pass
            return
        if op in ('s_not_b32', 's_add_i32', 's_sub_i32') and A[0].startswith('s') and not A[0].startswith('s['):
            d = int(A[0][1:])
            if op == 's_not_b32': self.setu(d, f"~{self.src(A[1], 'u')}")
            elif op == 's_add_i32': self.setu(d, f"{self.src(A[1], 'u')} + {self.src(A[2], 'u')}")
            else: self.setu(d, f"{self.src(A[1], 'u')} - {self.src(A[2], 'u')}")
            return
        if op == 's_bcnt1_i32_b64':
            d = int(A[0][1:])
            if _ENV.get('NO_SM6') is None:
                self.sm6 = True; c = self.t()
                self.emit(f"uint {c} = WaveActiveCountBits({self.maskval(A[1])});")
                self.setu(d, c); self.emit(f"scc = ({c} != 0u) ? 1u : 0u;"); return
            self.uses_wave = True
            c = self.t()
            self.emit(f"wv[flat] = ({self.maskval(A[1])}) ? 1u : 0u; GroupMemoryBarrierWithGroupSync();")
            self.emit(f"uint {c} = 0; for (uint q_ = 0; q_ < wlim; ++q_) {c} += wv[wbase + q_]; GroupMemoryBarrierWithGroupSync();")
            self.setu(d, c); self.emit(f"scc = ({c} != 0u) ? 1u : 0u;"); return
        if op == 's_mulk_i32':
            d = int(A[0][1:]); self.setu(d, f"asuint(asint({self.sraw(d)}) * ({int(A[1])}))"); return
        if op in ('s_cmp_eq_u32', 's_cmp_lg_u32', 's_cmp_lt_u32', 's_cmp_gt_u32', 's_cmp_le_u32', 's_cmp_ge_u32',
                  's_cmp_eq_i32', 's_cmp_lg_i32', 's_cmp_lt_i32', 's_cmp_gt_i32', 's_cmp_le_i32', 's_cmp_ge_i32'):
            c = {'eq': '==', 'lg': '!=', 'lt': '<', 'gt': '>', 'le': '<=', 'ge': '>='}[op.split('_')[2]]
            ty = 'u' if op.endswith('u32') else 'i'
            self.emit(f"scc = ({self.src(A[0], ty)} {c} {self.src(A[1], ty)});"); self.decl.add('scc'); return
        if op == 's_getpc_b64':
            mg = re.fullmatch(r's\[(\d+):(\d+)\]', A[0])
            self.pcnext = getattr(self, 'pcnext', {}); self.pcnext[int(mg.group(1))] = int(pc, 16) + 4; return
        mt = re.fullmatch(r'tbuffer_load_format_(x|xy|xyz|xyzw) v\[(\d+):\d+\], v(\d+), s\[(\d+):\d+\], (\S+) dfmt:(\d+) nfmt:(\d+) offset:(\d+)( offen)?( idxen)?', op + ' ' + args)
        if mt:
            comps, vd0, va, sr, so, dfmt, nfmt, off, offen, idxen = mt.groups()
            vd0 = int(vd0); sr = int(sr); nfmt = int(nfmt); dfmt = int(dfmt)
            if sr not in getattr(self, 'embed_regs', {}):
                return self.tbuffer_sb(comps, vd0, int(va), sr, dfmt, nfmt, int(off), bool(idxen), bool(offen))
            if nfmt != 7 or dfmt not in (4, 11, 13, 14) or not idxen or offen:
                raise ValueError("unsupported tbuffer load (embedded table)")
            name, st = self.emb_array(sr)
            ti_ = self.t(); self.emit(f"uint {ti_} = v{va} * {st // 4}u;")
            for k in range(len(comps)):
                self.setv(vd0 + k, f"asfloat({name}[{ti_} + {int(off) // 4 + k}u])")
            return
        mb = re.fullmatch(r'(buffer_(?:load|store|atomic)_\w+) v\[(\d+):(\d+)\], v(\d+), s\[(\d+):\d+\], (\S+) offset:(\d+)( offen)?( idxen)?( glc)?', op + ' ' + args)
        if mb:
            return self.mubuf(*mb.groups())
        # ---- scalar memory
        if op.startswith('s_load_dword') or op.startswith('s_buffer_load_dword'):
            mm = re.fullmatch(r's(?:\[(\d+):(\d+)\]|(\d+)), s\[(\d+):\d+\], (.+)', args.strip())
            dd0, dd1, dsingle, b0, off = mm.groups()
            d0 = int(dd0) if dd0 else int(dsingle); n = (int(dd1) - d0 + 1) if dd1 else 1
            b0 = int(b0)
            _er = getattr(self, 'embed_regs', {}); _keep = _er.get(b0)   # base may be overwritten by this very load
            for _r in range(d0, d0 + n):
                for _b in (_r, _r - 1): _er.pop(_b, None)
            if _keep is not None and op.startswith('s_buffer'): _er[b0] = _keep
            # byte offset expr
            dynx = None
            if off.startswith('lit:'): static = int(off[4:], 16) * 4; boff = f"{static}"
            elif off.startswith('0x'): static = int(off, 16) * 4; boff = f"{static}"
            else:
                boff = f"({self.sraw(int(off[1:]))})"; static = None; dynx = None
                sy = self.sym.get(int(off[1:]))
                if sy: static, dynx = sy[0], ((sy[1], sy[2]) if sy[1] else None)
            base = self.sg.get(b0)
            if base is not None and base[0] == 'desc': bname = base[2] + '_' + str(base[3]); bkind = base[1]
            else: bname = f"ud{self.origin.get(b0, b0)}"; bkind = 'UD'
            if op.startswith('s_buffer') and dynx is not None: static = None     # dynamic SGPR offset: use the runtime value
            if (op.startswith('s_buffer') and static is None and not off.startswith(('lit:', '0x')) and n > 1
                    and d0 <= int(off[1:]) < d0 + n - 1 and _ENV.get('NO_SELF_OFF') is None):
                # the offset register is one of the destinations (s_buffer_load_dwordx2 s[4:5], .., s4): the dwords
                # after it must still be read at the OLD offset. Freeze it first. NO_SELF_OFF=1 = old behaviour.
                so_ = self.t(); self.emit(f"{so_} = {boff};"); boff = f"({so_})"
                self.fn_uints = getattr(self, 'fn_uints', []); self.fn_uints.append(so_)
            if op.startswith('s_buffer'):
                if b0 in getattr(self, 'embed_regs', {}):
                    ename, est = self.emb_array(b0)
                    for k in range(n):
                        if static is not None: self.setu(d0 + k, f"{ename}[{static // 4 + k}]")
                        else: self.setu(d0 + k, f"{ename}[({boff}) / 4u + {k}u]")
                    for _r in range(d0, d0 + n):
                        for _b in (_r, _r - 1): self.embed_regs.pop(_b, None)
                    return
                try:
                    bk, nm, bstride = self.resolve_buf(base, b0)
                except ValueError:
                    if not getattr(self, 'ds_cp', False): raise
                    # DS_AS_VS: driver tess-constant buffer (only feeds LDS addressing, which is bypassed) -> 0
                    for k in range(n): self.setu(d0 + k, "0u")
                    return
                for k in range(n):
                    if static is not None:
                        by = static + 4 * k
                        if bk == 'cb': self.setu(d0 + k, f"asuint({nm}[{by // 16}].{'xyzw'[(by % 16) // 4]})")
                        elif bk == 'sb':
                            W = bstride // 4; self.setu(d0 + k, f"{nm}[{(by // 4) // W}].d[{(by // 4) % W}]")
                        else: self.setu(d0 + k, f"{nm}.Load({by})")
                    else:
                        if bk == 'cb': self.setu(d0 + k, f"asuint({nm}[({boff} + {4*k}u) / 16u][(({boff} + {4*k}u) % 16u) / 4u])")
                        elif bk == 'sb':
                            W = bstride // 4; wi = f"(({boff} + {4*k}u) / 4u)"
                            self.setu(d0 + k, f"{nm}[{wi} / {W}u].d[{wi} % {W}u]")
                        else: self.setu(d0 + k, f"{nm}.Load({boff} + {4*k}u)")
            else:
                stride = {8: 32, 4: 16}.get(n)
                if dynx is not None and n in (8, 4, 16) and _ENV.get('NO_DYN_SNAP') is None:
                    # the index register may be reused before the sample: freeze its value at descriptor-load time
                    # declared at function scope: the index may be used again from another block (texture size queries)
                    ds_ = self.t(); self.emit(f"{ds_} = {dynx[0]};"); dynx = (ds_, dynx[1])
                    self.fn_uints = getattr(self, 'fn_uints', []); self.fn_uints.append(ds_)
                if stride:
                    if static is not None: idx = str(static // stride)
                    else: idx = f"({boff}) / {stride}u"
                    kind = 'T' if n == 8 else 'S'
                    ud_n = self.origin.get(b0, b0) if base is None or base[0] != 'desc' else None
                    if ud_n is not None and self.pmap and self.pmap.get(ud_n) is None and b0 in getattr(self, 'origin_hist', {}) and self.pmap.get(self.origin_hist[b0]) is not None: ud_n = self.origin_hist[b0]   # pointer origin lost on another path
                    for k in range(n): self.sg[d0 + k] = ('desc', kind, bname, idx, ud_n, static, dynx)
                    self.desc_dw = getattr(self, 'desc_dw', {}); [self.desc_dw.__setitem__(d0 + k, (k, self.sg[d0 + k])) for k in range(n)]
                    self.sg_hist = getattr(self, 'sg_hist', {}); [self.sg_hist.__setitem__(d0 + k, self.sg[d0 + k]) for k in range(n)]
                elif n == 2 and static is not None and (base is None or base[0] != 'desc'):
                    # first half of a V# (base address + stride) read from a descriptor table slot; the shader fills in dwords 2,3
                    ud_n = self.origin.get(b0, b0)
                    for k in range(2): self.sg[d0 + k] = ('desc', 'S', bname, 0, ud_n, static, dynx)
                elif n == 16:
                    ud_n = self.origin.get(b0, b0) if base is None or base[0] != 'desc' else None
                    if ud_n is not None and self.pmap and self.pmap.get(ud_n) is None and b0 in getattr(self, 'origin_hist', {}) and self.pmap.get(self.origin_hist[b0]) is not None: ud_n = self.origin_hist[b0]   # pointer origin lost on another path
                    for k in range(16):
                        ex_ = {'T': (static + (k // 8) * 32) if static is not None else None, 'S': (static + (k // 4) * 16) if static is not None else None}
                        self.sg[d0 + k] = ('desc', 'T', bname, 0, ud_n, static, dynx, ex_)
                        self.desc_dw = getattr(self, 'desc_dw', {}); self.desc_dw[d0 + k] = (k, self.sg[d0 + k])
                else:
                    # plain pointer/data load from user-data table: treat as table base for next level
                    for k in range(n): self.sg[d0 + k] = ('desc', 'PTR', bname, f"{static // 4 + k if static is not None else 'dyn'}")
            return
        # ---- vector interpolation
        if op == 'v_interp_p1_f32':
            mm = re.fullmatch(r'v(\d+), v\d+, attr(\d+)\.(\w)', args.strip()); d, at, ch = mm.groups()
            self.interp_attrs = getattr(self, 'interp_attrs', set()) | {int(at)}
            self.emit(f"v{d} = asuint(A{at}.{ch});"); return
        if op in ('v_interp_p2_f32',): return
        if op == 'v_interp_mov_f32':
            # v_interp_mov reads one vertex's value (P0/P10/P20) without interpolation: often an
            # integer stored in float bits (instance id ...). Interpolating it flushes denormals to 0,
            # so such attributes are declared 'nointerpolation' (see PSIn emission).
            mm = re.fullmatch(r'v(\d+), v\d+, attr(\d+)\.(\w)', args.strip()); d, at, ch = mm.groups()
            self.flat_attrs = getattr(self, 'flat_attrs', set()) | {int(at)}
            self.emit(f"v{d} = asuint(A{at}.{ch});"); return
        # ---- image
        if op in ('image_store', 'image_load') :
            mm = re.fullmatch(r'v\[(\d+):(\d+)\], v(\d+)\.\., s\[(\d+):\d+\], s\[(\d+):\d+\] dmask:0x([0-9a-f])( unorm)?', args.strip())
            vd0, vd1, va, sr, ss, dm, un = mm.groups(); vd0 = int(vd0); va = int(va); dm = int(dm, 16)
            T = self.sg.get(int(sr))
            if T is None or T[0] != 'desc': raise ValueError("image T# not a descriptor")
            tn, tidx = self.resolve('T', T)
            if tn.startswith('Ux'):
                self.decl.add(('T', tn)); dim = self.tex_dim(tn)
                nco = {3: 2, 8: 3, 5: 3}.get(dim, 2)
                co = f"uint{nco}(" + ", ".join(f"v{va + q}" for q in range(nco)) + ")"
                if op == 'image_store':
                    if dm != 0xF: raise ValueError("partial-mask image_store")
                    if _ENV.get('NO_NAN_SCRUB'): self.emit(f"if (ex) {tn}[{tidx}][{co}] = float4(asfloat(v{vd0}), asfloat(v{vd0+1}), asfloat(v{vd0+2}), asfloat(v{vd0+3}));")
                    else:
                        tq = self.t(); self.emit(f"float4 {tq} = float4(asfloat(v{vd0}), asfloat(v{vd0+1}), asfloat(v{vd0+2}), asfloat(v{vd0+3})); {tq} = isnan({tq}) ? 0.0 : {tq};")
                        self.emit(f"if (ex) {tn}[{tidx}][{co}] = {tq};")
                else:
                    tmp = self.t(); self.emit(f"float4 {tmp} = {tn}[{tidx}][{co}];")
                    k = 0
                    for ch in range(4):
                        if dm & (1 << ch): self.setv(vd0 + k, f"{tmp}.{'xyzw'[ch]}"); k += 1
                return
            if op == 'image_store': raise ValueError("image_store to non-UAV")
        if op.startswith('image_sample') or op.startswith('image_load') or op in ('image_gather4_lz', 'image_gather4'):
            mm = re.fullmatch(r'v\[(\d+):(\d+)\], v(\d+)\.\., s\[(\d+):\d+\], s\[(\d+):\d+\] dmask:0x([0-9a-f])( unorm)?', args.strip())
            vd0, vd1, va, sr, ss, dm, un = mm.groups(); vd0 = int(vd0); va = int(va); dm = int(dm, 16)
            T = self.sg.get(int(sr)); S = self.sg.get(int(ss))
            if T is None or T[0] != 'desc': raise ValueError(f"texture s{sr} not a descriptor")
            c = op.startswith('image_sample_c')
            try: tn, tidx = self.resolve('T', T)
            except ValueError:
                Th = getattr(self, 'sg_hist', {}).get(int(sr))
                if Th is None or Th is T: raise
                T = Th; tn, tidx = self.resolve('T', T)
            self.decl.add(('T', tn))
            tdim = self.tex_dim(tn)
            sn = None
            if op.startswith('image_sample') or op.startswith('image_gather4'):
                if S is None or S[0] != 'desc': raise ValueError("sampler not a descriptor")
                try: sn, sidx = self.resolve('S', S)
                except ValueError:
                    Sh = getattr(self, 'sg_hist', {}).get(int(ss))
                    if Sh is None or Sh is S: raise
                    sn, sidx = self.resolve('S', Sh)
            ti = tidx
            lz = op.endswith('_lz') or op.endswith('_l'); c = op.endswith('_c') or '_c_' in op
            use_cmp = c and op.startswith('image_sample') and not _ENV.get('NO_SAMPLE_CMP')
            if sn is not None: self.decl.add(('SC' if use_cmp else 'S', sn))
            if sn is not None and not use_cmp and not _ENV.get('NO_SAMP_ALT'):
                # GCN ignores a sampler's compare function on non-compare ops, so the game may reuse the shadow
                # (comparison) sampler SGPRs for a plain sample; HLSL cannot. Remember the last plain sampler these
                # SGPRs held; assemble() swaps it in when sn also ends up declared as SamplerComparisonState.
                self.samp_alt = getattr(self, 'samp_alt', []); self.samp_last = getattr(self, 'samp_last', {})
                alt = self.samp_last.get(int(ss)); key = f"@@SALT{len(self.samp_alt)}@@"
                self.samp_alt.append((key, sn, f"{sn}[{sidx}]", alt)); self.samp_last[int(ss)] = (sn, sidx)
                sexpr = key
            else:
                sexpr = f"{sn}[{sidx}]" if sn is not None else None
            nco = {3: 2, 8: 3, 5: 3, 10: 3}.get(tdim, 2)
            def CO(st):
                if tdim == 10:   # cube array: GCN passes (u, v, slice*8 + face)
                    return (f"float4(cube_dir_of(asfloat(v{st}), asfloat(v{st + 1}), asfloat(v{st + 2})), "
                            f"floor(asfloat(v{st + 2}) / 8.0))")
                if tdim == 6:
                    # GCN passes (u, v, face) after the v_cube* math: rebuild the direction from them
                    return f"cube_dir_of(asfloat(v{st}), asfloat(v{st + 1}), asfloat(v{st + 2}))"
                return f"float{nco}(" + ", ".join(f"asfloat(v{st + q})" for q in range(nco)) + ")"
            tmp = self.t()
            cb = 1 if (c or op.endswith('_b')) else 0      # leading ref/bias register
            if op.startswith('image_gather4'):
                comp = {1: 'Red', 2: 'Green', 4: 'Blue', 8: 'Alpha'}.get(dm)
                if comp is None: raise ValueError("gather4 with multi-channel dmask")
                self.emit(f"float4 {tmp} = {tn}[{ti}].Gather{comp}({sexpr}, {CO(va)});")
                for q in range(4): self.setv(vd0 + q, f"{tmp}.{'xyzw'[q]}")
                return
            if op.startswith('image_load'):
                call = (f"{tn}[{ti}].Load(int4((int)v{va}, (int)v{va+1}, (int)v{va+2}, 0))" if tdim in (5,) else f"{tn}[{ti}].Load(int4((int)v{va}, (int)v{va+1}, 0, 0))" if tdim == 8 and False else f"{tn}[{ti}].Load(int3((int)v{va}, (int)v{va+1}, 0))")
            elif c and use_cmp:
                # the comparison function comes from the (translated) game sampler
                call = f"float4({tn}[{ti}].SampleCmpLevelZero({sn}[{sidx}], {CO(va + 1)}, asfloat(v{va})), 0, 0, 0)"
            elif c:
                call = f"float4((asfloat(v{va}) <= {tn}[{ti}].SampleLevel({sn}[{sidx}], {CO(va + 1)}, 0).x) ? 1.0 : 0.0, 0, 0, 0)"
            elif op == 'image_sample_lz': call = f"{tn}[{ti}].SampleLevel({sexpr}, {CO(va)}, 0)"
            elif op == 'image_sample_l': call = f"{tn}[{ti}].SampleLevel({sexpr}, {CO(va)}, asfloat(v{va + nco}))"
            elif op == 'image_sample_b' and LOD0: call = f"{tn}[{ti}].SampleLevel({sexpr}, {CO(va + 1)}, 0)"
            elif op == 'image_sample_b': call = f"{tn}[{ti}].SampleBias({sexpr}, {CO(va + 1)}, asfloat(v{va}))"
            elif op == 'image_sample': call = (f"{tn}[{ti}].SampleLevel({sexpr}, {CO(va)}, 0)" if LOD0 else f"{tn}[{ti}].Sample({sexpr}, {CO(va)})")
            else: raise ValueError("unsupported sample variant " + op)
            self.emit(f"float4 {tmp} = {call};")
            k = 0
            for ch in range(4):
                if dm & (1 << ch): self.setv(vd0 + k, f"{tmp}.{'xyzw'[ch]}"); k += 1
            return
        # ---- export
        if op == 'exp':
            mm = re.fullmatch(r'(\w+) v(\d+), v(\d+), v(\d+), v(\d+) en:0x([0-9a-f])( compr)?( done)?( vm)?', args.strip())
            tgt, r0, r1, r2, r3, en, compr, done, vm = mm.groups()
            if compr:
                e = (f"float4(f16tof32(v{r0} & 0xFFFF), f16tof32(v{r0} >> 16), "
                     f"f16tof32(v{r1} & 0xFFFF), f16tof32(v{r1} >> 16))")
            else: e = f"float4(asfloat(v{r0}), asfloat(v{r1}), asfloat(v{r2}), asfloat(v{r3}))"
            if tgt.startswith('mrt') and not _ENV.get('NO_NAN_SCRUB'):
                tq = self.t(); self.emit(f"float4 {tq} = {e}; {tq} = isnan({tq}) ? 0.0 : {tq};"); e = tq
            self.emit(f"{tgt}_o = ex ? {e} : {tgt}_o;"); self.exps.append(tgt)
            # 'done vm': exec is the pixel valid mask -> lanes switched off by a kill (alpha test) are discarded.
            if self.kind == 'ps' and done and vm and not _ENV.get('NO_PS_KILL'): self.emit("if (!ex) discard;")
            return
        # ---- vector compare
        mc = re.fullmatch(r'v_cmp(x?)_(\w+?)_(f32|i32|u32)', op)
        if mc:
            x, c, ty = mc.groups()
            dt = self.mask_target(A[0])
            k = 'f' if ty == 'f32' else ('i' if ty == 'i32' else 'u')
            ops = {'lt': '<', 'eq': '==', 'le': '<=', 'gt': '>', 'ne': '!=', 'lg': '!=', 'ge': '>=', 'neq': '!=', 'nlt': '>=', 'ngt': '<=', 'nge': '<', 'nle': '>', 'nlg': '=='}
            xx = self.src(A[1], k); yy = self.src(A[2], k)
            if c == 'f': e = 'false'
            elif c == 't' or c == 'tru': e = 'true'
            elif c == 'o': e = f"(!isnan({xx}) && !isnan({yy}))"
            elif c == 'u': e = f"(isnan({xx}) || isnan({yy}))"
            else: e = f"({xx} {ops[c]} {yy})"
            self.emit(f"{dt} = ex && {e};"); self.decl.add(dt)
            if x: self.emit(f"ex = {dt};")
            return
        if op in ('v_cndmask_b32', 'v_cndmask_b32_e64'):
            d = vd(); mk = self.maskval(A[3]) if len(A) > 3 else 'vcc'
            # v_cndmask is a bitwise select: select the raw bits as uint. Selecting asfloat(int) values on the GPU
            # flushes denormals (small integers such as voxel coordinates) to 0. NO_UINT_CNDMASK=1 = old float select.
            if _ENV.get('NO_UINT_CNDMASK') is None and not any(a.strip().startswith('|') or (a.strip().startswith('-') and not re.fullmatch(r'-\d+(\.\d+)?|-0x[0-9a-f]+', a.strip())) for a in A[1:3]):
                self.setv(d, f"asfloat({mk} ? {self.src(A[2], 'u')} : {self.src(A[1], 'u')})"); return
            y0 = self.src(A[1], 'f'); y1 = self.src(A[2], 'f')
            self.setv(d, f"{mk} ? {y1} : {y0}"); return
        if op in ('v_addc_u32_e64', 'v_addc_u32'):
            d = vd(); x = self.src(A[1], 'u'); y = self.src(A[2], 'u')
            c = self.maskval(A[3]) if len(A) > 3 else 'vcc'
            self.setv(d, f"asfloat({x} + {y} + ({c} ? 1u : 0u))")
            if len(A) > 3: pass
            return
        # ---- VALU float
        name = op[:-4] if op.endswith('_e64') else op
        F = lambda s: self.src(s, 'f')
        U = lambda s: self.src(s, 'u')
        I = lambda s: self.src(s, 'i')
        flt2 = {'v_add_f32': '{0} + {1}', 'v_sub_f32': '{0} - {1}', 'v_subrev_f32': '{1} - {0}', 'v_mul_f32': '{0} * {1}', 'v_mul_legacy_f32': 'mul_legacy({0}, {1})',
                'v_max_f32': 'max({0}, {1})', 'v_min_f32': 'min({0}, {1})', 'v_max_legacy_f32': 'max({0}, {1})', 'v_min_legacy_f32': 'min({0}, {1})'}
        if name in flt2 and len(A) >= 3:
            self.setv(vd(), fin(flt2[name].format(F(A[1]), F(A[2])), args)); return
        if name in ('v_mac_f32', 'v_mac_legacy_f32'):
            d = vd(); mul = 'mul_legacy' if 'legacy' in name else ''
            self.setv(d, fin(f"asfloat(v{d}) + {mul}({F(A[1])}, {F(A[2])})" if mul else f"asfloat(v{d}) + {F(A[1])} * {F(A[2])}", args)); return
        if name in ('v_mad_f32', 'v_fma_f32', 'v_mad_legacy_f32'):
            f = 'mul_legacy({0}, {1}) + {2}' if 'legacy' in name else '{0} * {1} + {2}'
            self.setv(vd(), fin(f.format(F(A[1]), F(A[2]), F(A[3])), args)); return
        if name in ('v_madak_f32',):
            self.setv(vd(), f"{F(A[1])} * {F(A[2])} + {F(A[3])}"); return
        if name in ('v_madmk_f32',):
            self.setv(vd(), f"{F(A[1])} * {F(A[3])} + {F(A[2])}"); return
        one = {'v_mov_b32': '{0}', 'v_rcp_f32': 'rcp({0})', 'v_rsq_f32': 'rsqrt({0})', 'v_sqrt_f32': 'sqrt({0})', 'v_exp_f32': 'exp2({0})', 'v_log_f32': 'log2({0})',
              'v_fract_f32': 'frac({0})', 'v_floor_f32': 'floor({0})', 'v_ceil_f32': 'ceil({0})', 'v_trunc_f32': 'trunc({0})', 'v_rndne_f32': 'round({0})',
              'v_sin_f32': 'sin(({0}) * 6.283185307)', 'v_cos_f32': 'cos(({0}) * 6.283185307)', 'v_not_b32': 'asfloat(~asuint({0}))'}
        if name in one and len(A) >= 2:
            self.setv(vd(), fin(one[name].format(F(A[1])), args)); return
        if name in ('v_max3_f32','v_min3_f32','v_med3_f32'):
            x,y,z=F(A[1]),F(A[2]),F(A[3])
            e={'v_max3_f32':f"max({x}, max({y}, {z}))",'v_min3_f32':f"min({x}, min({y}, {z}))",'v_med3_f32':f"max(min({x},{y}), min(max({x},{y}), {z}))"}[name]
            self.setv(vd(), fin(e, args)); return
        if name in ('v_writelane_b32', 'v_readlane_b32'):
            # SGPR spill into VGPR lanes: model each (vgpr, constant lane) as a uniform scalar slot wl_<v>_<lane>
            ls = A[2]
            if ls.isdigit(): lane = int(ls)
            elif ls.startswith('v') and ls[1:].isdigit() and int(ls[1:]) >= 128: lane = int(ls[1:]) - 128
            else: raise ValueError('dynamic lane select')
            self.wl_sym = getattr(self, 'wl_sym', {})
            if name == 'v_writelane_b32':
                key = (int(A[0][1:]), lane)
                s_ = int(A[1][1:]) if A[1].startswith('s') and A[1][1:].isdigit() else None
                sym = self.sg.get(s_) if s_ is not None else None
                if sym is not None and sym[0] == 'desc': self.wl_sym[key] = sym; return   # descriptor dword: symbolic only
                self.wl_sym.pop(key, None)
                self.emit(f"wl_{key[0]}_{lane} = {U(A[1])};")
            elif (int(A[1][1:]), lane) in self.wl_sym:
                self.sg[int(A[0].split(':')[0].lstrip('sv['))] = self.wl_sym[(int(A[1][1:]), lane)]
            else:
                # VOP2 readlane keeps the SGPR destination in the vdst field: gcn_dis prints it as vN
                self.setu(int(A[0].split(':')[0].lstrip('sv[')), f"wl_{int(A[1][1:])}_{lane}")
            return
        if name in ('v_max3_u32', 'v_min3_u32', 'v_med3_u32', 'v_max3_i32', 'v_min3_i32', 'v_med3_i32'):
            sg_ = name.endswith('i32'); cv = (lambda a: f"asint({U(a)})") if sg_ else U
            x, y, z = cv(A[1]), cv(A[2]), cv(A[3])
            e = {'max3': f"max({x}, max({y}, {z}))", 'min3': f"min({x}, min({y}, {z}))",
                 'med3': f"max(min({x}, {y}), min(max({x}, {y}), {z}))"}[name[2:6]]
            self.setv(vd(), f"asfloat({'asuint(' + e + ')' if sg_ else e})"); return
        if name in ('v_cubeid_f32', 'v_cubesc_f32', 'v_cubetc_f32', 'v_cubema_f32'):
            # Direction built per instruction: a cached temp could be out of scope (declared in another
            # block) or stale (same register names, values rewritten in between).
            fn = {'v_cubeid_f32': 'cube_id', 'v_cubesc_f32': 'cube_sc', 'v_cubetc_f32': 'cube_tc', 'v_cubema_f32': 'cube_ma'}[name]
            self.setv(vd(), f"{fn}(float3({F(A[1])}, {F(A[2])}, {F(A[3])}))"); return
        if name in ('v_mad_u32_u24', 'v_mad_i32_i24'):
            self.setv(vd(), f"asfloat((({U(A[1])} & 0xFFFFFFu) * ({U(A[2])} & 0xFFFFFFu)) + {U(A[3])})"); return
        if name in ('v_mul_u32_u24', 'v_mul_i32_i24'):
            self.setv(vd(), f"asfloat(({U(A[1])} & 0xFFFFFFu) * ({U(A[2])} & 0xFFFFFFu))"); return
        if name == 'v_cvt_flr_i32_f32': self.setv(vd(), f"asfloat(asuint((int)floor({F(A[1])})))"); return
        if name == 'v_cvt_rpi_i32_f32': self.setv(vd(), f"asfloat(asuint((int)floor({F(A[1])} + 0.5)))"); return
        if name == 'v_subrev_i32': self.setv(vd(), f"asfloat({U(A[2])} - {U(A[1])})"); return
        if name == 'v_lshl_b32': self.setv(vd(), f"asfloat({U(A[1])} << ({U(A[2])} & 31u))"); return
        if name == 'v_lshr_b32': self.setv(vd(), f"asfloat({U(A[1])} >> ({U(A[2])} & 31u))"); return
        if name == 'v_ashr_i32': self.setv(vd(), f"asfloat(asuint({I(A[1])} >> ({U(A[2])} & 31u)))"); return
        if name == 'v_ffbh_u32':
            self.setv(vd(), f"asfloat(({U(A[1])} == 0u) ? 0xFFFFFFFFu : (31u - firstbithigh({U(A[1])})))"); return
        if name == 'v_bfm_b32':
            self.setv(vd(), f"asfloat(((1u << ({U(A[1])} & 31u)) - 1u) << ({U(A[2])} & 31u))"); return
        if name in ('v_mbcnt_lo_u32_b32', 'v_mbcnt_hi_u32_b32'):
            lo = name.endswith('lo_u32_b32')
            self.need_lane = True
            m0 = A[1].strip()
            if m0 in ('-1', '0xffffffff'):
                lane_part = "min(lane, 32u)" if lo else "(lane > 32u ? lane - 32u : 0u)"
                self.setv(vd(), f"asfloat({lane_part} + {U(A[2])})"); return
            mr = re.fullmatch(r's(\d+)', m0)
            if not mr: raise ValueError("mbcnt mask operand " + m0)
            base = int(mr.group(1)) if lo else int(mr.group(1)) - 1
            mname = f"sp_{base}"
            if mname not in self.decl: raise ValueError("mbcnt on non-lane-mask sgpr")
            if _ENV.get('NO_SM6') is None:
                # SM6: the lo half yields the full prefix count, the hi half adds nothing
                self.sm6 = True
                if lo: self.setv(vd(), f"asfloat(WavePrefixCountBits({mname}) + {U(A[2])})")
                else: self.setv(vd(), f"asfloat({U(A[2])})")
                return
            self.uses_wave = True
            c = self.t()
            self.emit(f"wv[flat] = {mname} ? 1u : 0u; GroupMemoryBarrierWithGroupSync();")
            lo_b, hi_b = ("0u", "min(lane, 32u)") if lo else ("32u", "lane")
            self.emit(f"uint {c} = 0; for (uint q_ = {lo_b}; q_ < {hi_b}; ++q_) {c} += wv[wbase + q_]; GroupMemoryBarrierWithGroupSync();")
            self.setv(vd(), f"asfloat({c} + {U(A[2])})"); return
        if name in ('v_mul_lo_u32', 'v_mul_lo_i32'): self.setv(vd(), f"asfloat({U(A[1])} * {U(A[2])})"); return
        if name == 'v_mul_hi_u32': self.setv(vd(), f"asfloat(mulhi_u32({U(A[1])}, {U(A[2])}))"); return
        if name == 'v_mul_hi_i32': self.setv(vd(), f"asfloat(mulhi_u32({U(A[1])}, {U(A[2])}) - ((asint({U(A[1])}) < 0) ? {U(A[2])} : 0u) - ((asint({U(A[2])}) < 0) ? {U(A[1])} : 0u))"); return
        if name == 'v_movrels_b32':
            # vD = v[S + m0]: expanded into a switch over every declared VGPR once the register count is known
            sb = re.fullmatch(r'v(\d+)', A[1].strip())
            if not sb: raise ValueError("movrels source " + A[1])
            self.emit(f"@@MOVRELS {vd()} {sb.group(1)}@@"); return
        if name == 'v_readfirstlane_b32' and re.fullmatch(r'v(\d+)', A[1].strip()) and int(A[1].strip()[1:]) in getattr(self, 'counter_orig', {}):
            d = int(A[0][1:]); self.counter_base = getattr(self, 'counter_base', {}); self.counter_base[d] = self.counter_orig[int(A[1].strip()[1:])]
            self.setu(d, "0u"); return
        if name == 'v_add_i32' and getattr(self, 'counter_base', None):
            for t_ in (A[1].strip(), A[2].strip()):
                mm_ = re.fullmatch(r's(\d+)', t_)
                if mm_ and int(mm_.group(1)) in self.counter_base:
                    nmc = self.counter_base[int(mm_.group(1))]; tt = self.t()
                    mreg = re.fullmatch(r'SB_u(\d+)_sp(\d+)', nmc); cidx = ((int(mreg.group(1)) & 1023) | ((int(mreg.group(2)) & 7) << 10)) if mreg else 0
                    if _ENV.get('CTR_MODE', 'inc') == 'inc':
                        self.emit(f"uint {tt} = 0; if (ex) {tt} = {nmc}.IncrementCounter();")
                    else:
                        self.uses_ctr = True
                        self.emit(f"uint {tt} = 0; if (ex) g_ctr.InterlockedAdd({cidx * 4}u, 1u, {tt});")
                    self.setv(vd(), f"asfloat({tt})"); return
        if name == 'v_readfirstlane_b32':
            d = int(A[0][1:])
            if _ENV.get('NO_SM6') is None:
                self.sm6 = True; self.setu(d, f"rfl_({U(A[1])}, ex)"); return
            if self.kind == 'cs':
                self.uses_wave = True
                f_ = self.t()
                self.emit(f"wv[flat] = {U(A[1])}; wx[flat] = ex ? 1u : 0u; GroupMemoryBarrierWithGroupSync();")
                self.emit(f"uint {f_} = 0; for (uint q_ = 0; q_ < wlim; ++q_) if (wx[wbase + q_]) {{ {f_} = wv[wbase + q_]; break; }} GroupMemoryBarrierWithGroupSync();")
                self.setu(d, f_); return
            self.setu(d, U(A[1])); return
        mu = re.fullmatch(r'v_cvt_f32_ubyte([0-3])', name)
        if mu:
            self.setv(vd(), f"(float)(({U(A[1])} >> {8 * int(mu.group(1))}) & 0xFFu)"); return
        if name in ('v_subb_u32', 'v_sub_u32'):
            c = self.maskval(A[3]) if len(A) > 3 else 'false'
            self.setv(vd(), f"asfloat({U(A[1])} - {U(A[2])} - ({c} ? 1u : 0u))"); return
        if name == 'v_bfi_b32':
            self.setv(vd(), f"asfloat(({U(A[1])} & {U(A[2])}) | (~{U(A[1])} & {U(A[3])}))"); return
        if name == 'v_cvt_f32_f16':
            self.setv(vd(), f"f16tof32({U(A[1])} & 0xFFFF)"); return
        if name == 'v_cvt_f16_f32':
            self.setv(vd(), f"asfloat(f32tof16({F(A[1])}))"); return
        if name in ('v_min_i32', 'v_max_i32', 'v_min_u32', 'v_max_u32'):
            ty = 'i' if name.endswith('i32') else 'u'; fn = 'min' if 'min' in name else 'max'
            cv = (lambda x: f"asuint({fn}({self.src(A[1], 'i')}, {self.src(A[2], 'i')}))") if ty == 'i' else (lambda x: f"{fn}({U(A[1])}, {U(A[2])})")
            self.setv(vd(), f"asfloat({cv(0)})"); return
        if name in ('v_cvt_f64_f32', 'v_add_f64', 'v_mul_f64', 'v_max_f64', 'v_min_f64', 'v_cvt_f32_f64') and not _ENV.get('NO_F64'):
            # 64-bit float ops (a register pair holds the double); seen in a one-off lookup-table compute shader
            raw = args.strip(); clamp_ = raw.endswith(' clamp')
            if clamp_: raw = raw[:-6]
            ops = [x.strip() for x in raw.split(',')]
            def D(t):
                ng = t.startswith('-'); t = t[1:] if ng else t
                ab_ = t.startswith('|') and t.endswith('|'); t = t[1:-1] if ab_ else t
                mv = re.fullmatch(r'([vs])(\d+)', t)
                if mv:
                    r_ = int(mv.group(2))
                    e = (f"asdouble(v{r_}, v{r_ + 1})" if mv.group(1) == 'v' else f"asdouble({self.sraw(r_)}, {self.sraw(r_ + 1)})")
                elif re.fullmatch(r'-?\d+(\.\d+)?', t): e = f"((double){t})"
                else: raise ValueError("f64 operand " + t)
                if ab_: e = f"abs({e})"
                return f"(-{e})" if ng else e
            d_ = int(re.fullmatch(r'v(\d+)', ops[0]).group(1))
            if name == 'v_cvt_f32_f64':
                self.setv(d_, f"(float)({D(ops[1])})"); return
            e = {'v_cvt_f64_f32': lambda: f"(double)({F(ops[1])})", 'v_add_f64': lambda: f"({D(ops[1])} + {D(ops[2])})",
                 'v_mul_f64': lambda: f"({D(ops[1])} * {D(ops[2])})", 'v_max_f64': lambda: f"max({D(ops[1])}, {D(ops[2])})",
                 'v_min_f64': lambda: f"min({D(ops[1])}, {D(ops[2])})"}[name]()
            if clamp_: e = f"saturate({e})"
            dt, lo_, hi_ = self.t(), self.t(), self.t()
            self.emit(f"double {dt} = {e}; uint {lo_}, {hi_}; asuint({dt}, {lo_}, {hi_});")
            self.setv(d_, f"asfloat({lo_})"); self.setv(d_ + 1, f"asfloat({hi_})"); return
        if name == 'v_cvt_f32_i32': self.setv(vd(), f"(float)({I(A[1])})"); return
        if name == 'v_cvt_f32_u32': self.setv(vd(), f"(float)({U(A[1])})"); return
        # GCN saturates out-of-range values and maps NaN to 0; a bare HLSL cast is undefined there
        # (a negative float -> huge uint made a particle loop run 4096x per thread and stall the GPU).
        if name == 'v_cvt_i32_f32': self.setv(vd(), f"asfloat(asuint((int)clamp({F(A[1])}, -2147483648.0, 2147483520.0)))"); return
        if name == 'v_cvt_u32_f32': self.setv(vd(), f"asfloat((uint)clamp({F(A[1])}, 0.0, 4294967040.0))"); return
        if name == 'v_cvt_pkrtz_f16_f32':
            self.setv(vd(), f"asfloat(f32tof16({F(A[1])}) | (f32tof16({F(A[2])}) << 16))"); return
        ib = {'v_and_b32': '{0} & {1}', 'v_or_b32': '{0} | {1}', 'v_xor_b32': '{0} ^ {1}', 'v_add_i32': '{0} + {1}', 'v_sub_i32': '{0} - {1}',
              'v_lshlrev_b32': '{1} << ({0} & 31u)', 'v_lshrrev_b32': '{1} >> ({0} & 31u)', 'v_mul_lo_u32': '{0} * {1}', 'v_mul_lo_i32': '{0} * {1}'}
        if name in ib and len(A) >= 3:
            self.setv(vd(), f"asfloat({ib[name].format(U(A[1]), U(A[2]))})"); return
        if name == 'v_ashrrev_i32': self.setv(vd(), f"asfloat(asuint({I(A[2])} >> ({U(A[1])} & 31u)))"); return
        if name == 'v_bfe_u32': self.setv(vd(), f"asfloat(({U(A[1])} >> ({U(A[2])} & 31u)) & ((1u << ({U(A[3])} & 31u)) - 1u))"); return
        raise ValueError("unhandled op")


    def param_of(self, ud):
        if self.pmap is None: return None
        return self.pmap.get(ud)

    def find_range(self, pidx, static, kind):
        P = [p for p in self.rs_params if p['i'] == pidx]
        if not P or P[0]['type'] != 'TABLE': return None
        pos = 0
        for r in P[0]['ranges']:
            sz = 16 if r['t'] == 'SAMP' else 32
            span = r['n'] * sz
            if pos <= static < pos + span:
                if (static - pos) % sz:
                    if kind in ('B', 'W') and r['t'] == 'UAV' and (static - pos) % sz == 16: self._counter_hit = True
                    else: return None
                ok = {'T': ('SRV', 'UAV'), 'S': ('SAMP',), 'B': ('CBV', 'SRV', 'UAV'), 'W': ('UAV',)}[kind]
                return (r, pos, sz) if r['t'] in ok else None
            pos += span
        return None

    def infer_pmap(self):
        """choose root-param for each user-data sgpr from collected accesses (increasing order, lowest params first)"""
        uds = sorted(set(a[0] for a in self.acc))
        tables = [p['i'] for p in self.rs_params if p['type'] == 'TABLE']
        cbvs = [p['i'] for p in self.rs_params if p['type'] == 'CBV']
        need = {u: [a for a in self.acc if a[0] == u] for u in uds}
        res = {}
        def fits(u, pidx):
            for (_, kind, st) in need[u]:
                if kind == 'R':
                    if pidx not in cbvs: return False
                elif pidx in cbvs or self.find_range(pidx, st, kind) is None: return False
            return True
        def dfs(k, last):
            if k == len(uds): return True
            u = uds[k]
            for pidx in sorted(tables + cbvs):
                if pidx <= last: continue
                if fits(u, pidx):
                    res[u] = pidx
                    if dfs(k + 1, pidx): return True
                    del res[u]
            return False
        if not dfs(0, -1): raise ValueError("no consistent root-param assignment for user data " + str(uds))
        return res

    def resolve(self, kind, desc):
        """(kind 'T'|'S', desc tuple) -> (array name, index expr) using the root signature; user-data sN -> param (N-4 >= 1)"""
        ud, static = desc[4], desc[5]
        if len(desc) > 7 and desc[7].get(kind) is not None: static = desc[7][kind]
        if ud is None or static is None: raise ValueError("unresolved descriptor table (nested pointer)")
        if ud == 4 and False: raise ValueError("inline CBV used as table")
        if self.pmap is None:
            self.acc.append((ud, kind, static)); return f"{'Tx' if kind == 'T' else 'Sm'}_dummy", 0
        pidx = self.pmap.get(ud)
        if pidx is None: raise ValueError(f"user data s{ud} has no root param")
        P = [p for p in self.rs_params if p['i'] == pidx]
        if not P or P[0]['type'] != 'TABLE': raise ValueError(f"user data s{ud} -> param {pidx} is not a table")
        pos = 0
        for r in P[0]['ranges']:
            sz = 16 if r['t'] == 'SAMP' else 32
            span = r['n'] * sz
            if pos <= static < pos + span:
                want = ('SRV', 'UAV') if kind == 'T' else ('SAMP',)
                if r['t'] not in want: raise ValueError(f"offset {static} lands on {r['t']} range, wanted {want}")
                nm = f"{('Ux' if r['t'] == 'UAV' else 'Tx') if kind == 'T' else 'Sm'}_sp{r['space']}_r{r['reg']}"
                self.ranges[nm] = r
                dyn = desc[6] if len(desc) > 6 else None
                if dyn and len(desc) > 7: dyn = (dyn[0], dyn[1] * (1 if kind == 'T' else 1))
                if dyn:
                    if dyn[1] % sz == 0: return nm, f"{(static - pos) // sz} + {dyn[0]} * {dyn[1] // sz}"
                    return nm, f"(({static - pos}u + {dyn[0]} * {dyn[1]}u) / {sz}u)"
                return nm, (static - pos) // sz
            pos += span
        raise ValueError(f"offset {static} beyond table param {pidx}")

    def resolve_buf(self, base, b0):
        """descriptor source -> (kind 'cb'|'ba'|'sb', name, stride)"""
        if (base is None or base[0] != 'desc') and b0 in getattr(self, 'sg_hist', {}) and _ENV.get('NO_DESC_HIST') is None:
            # linear translation lost the descriptor on another control-flow path: use the last descriptor loaded here
            pidx_ = self.pmap.get(self.origin.get(b0, b0)) if self.pmap else None
            P_ = [p for p in self.rs_params if p['i'] == pidx_] if pidx_ is not None else []
            if not (P_ and P_[0]['type'] in (('CBV',) if _ENV.get('NO_ROOT_UAV') else ('CBV', 'UAV', 'SRV'))): base = self.sg_hist[b0]
        if base is None or base[0] != 'desc':
            ud = self.origin.get(b0, b0)
            if self.pmap is None:
                self.acc.append((ud, 'R', 0)); return 'cb', 'Dummy', 0
            pidx = self.pmap.get(ud)
            P = [p for p in self.rs_params if p['i'] == pidx]
            if P and P[0]['type'] == 'CBV':
                nm = f"RootCBV_b{P[0]['reg']}_sp{P[0]['space']}"; self.bufs[nm] = ('CBV', P[0]['reg'], P[0]['space']); return 'cb', nm, 0
            if P and P[0]['type'] in ('UAV', 'SRV') and not _ENV.get('NO_ROOT_UAV'):
                # root UAV/SRV (a raw GPU address in user data; the shader builds the rest of the V# itself)
                reg, space = P[0]['reg'], P[0]['space']; dk = self.decls.get(('u', space, reg) if P[0]['type'] == 'UAV' else (space, reg))
                if P[0]['type'] == 'UAV' and dk and dk[0] == 'USB':
                    nm = f"SB_u{reg}_sp{space}"; self.bufs[nm] = ('USB', reg, space, dk[1]); return 'sb', nm, dk[1]
                if P[0]['type'] == 'SRV' and dk and dk[0] == 'SB':
                    nm = f"SB_t{reg}_sp{space}"; self.bufs[nm] = ('SB', reg, space, dk[1]); return 'sb', nm, dk[1]
                if P[0]['type'] == 'UAV': nm = f"BA_u{reg}_sp{space}"; self.bufs[nm] = ('UAV', reg, space); return 'ba', nm, 0
                nm = f"BA_t{reg}_sp{space}"; self.bufs[nm] = ('SRV', reg, space); return 'ba', nm, 0
            raise ValueError(f"buffer base s{b0} (user data {ud}) not a root CBV")
        if len(base) < 6: raise ValueError("descriptor from pointer chase")
        ud, static = base[4], base[5]
        if ud is None or static is None: raise ValueError("nested buffer descriptor")
        if self.pmap is None:
            self.acc.append((ud, 'W' if getattr(self, '_buf_store', False) and not _ENV.get('NO_STORE_UAV') else 'B', static)); return 'cb', 'Dummy', 0
        pidx = self.pmap.get(ud)
        self._counter_hit = False
        f = self.find_range(pidx, static, 'B') if pidx is not None else None
        if f is None: raise ValueError(f"buffer at ud{ud}+{static} not resolvable")
        r, pos, sz = f
        reg = r['reg'] + (static - pos) // sz
        if self._counter_hit:
            dk = self.decls.get(('u', r['space'], reg))
            if not (dk and dk[0] == 'USB'): raise ValueError("counter of a non-structured UAV")
            nm = f"SB_u{reg}_sp{r['space']}"; self.bufs[nm] = ('USB', reg, r['space'], dk[1]); return 'ctr', nm, dk[1]
        if r['t'] == 'CBV':
            nm = f"CB_sp{r['space']}_r{reg}"; self.bufs[nm] = ('CBV', reg, r['space']); return 'cb', nm, 0
        dk = self.decls.get(('u', r['space'], reg) if r['t'] == 'UAV' else (r['space'], reg))
        if r['t'] == 'SRV' and dk and dk[0] == 'SB':
            nm = f"SB_t{reg}_sp{r['space']}"; self.bufs[nm] = ('SB', reg, r['space'], dk[1]); return 'sb', nm, dk[1]
        if r['t'] == 'UAV' and dk and dk[0] == 'USB':
            nm = f"SB_u{reg}_sp{r['space']}"; self.bufs[nm] = ('USB', reg, r['space'], dk[1]); return 'sb', nm, dk[1]
        if r['t'] == 'SRV':
            nm = f"BA_t{reg}_sp{r['space']}"; self.bufs[nm] = ('SRV', reg, r['space']); return 'ba', nm, 0
        nm = f"BA_u{reg}_sp{r['space']}"; self.bufs[nm] = ('UAV', reg, r['space']); return 'ba', nm, 0

    def tex_dim(self, tn):
        r = self.ranges.get(tn)
        if not r: return 3
        if tn.startswith('Ux'):
            for k, v in self.decls.items():
                if isinstance(k, tuple) and k[0] == 'utex' and k[1] == r['space'] and k[2] <= r['reg'] <= k[3]: return v[0]
            return 3
        for k, v in self.decls.items():
            if k[0] == 'tex' and k[1] == r['space'] and k[2] <= r['reg'] <= k[3]: return v[0]
        return 3

    def space(self, n):
        if not hasattr(self, '_sp'): self._sp = {}
        return self._sp.setdefault(n, 100 + len(self._sp))

    def tbuffer_sb(self, comps, vd0, va, sr, dfmt, nfmt, off, idxen, offen):
        bk, nm, stride = self.resolve_buf(self.sg.get(sr), sr)
        # formats: 32-bit float comps, or 16-bit float pairs
        if nfmt == 7 and dfmt in (4, 11, 13, 14): nf = {4: 1, 11: 2, 13: 3, 14: 4}[dfmt]; esz = nf * 4; half = False
        elif nfmt == 7 and dfmt in (5, 12): nf = {5: 2, 12: 4}[dfmt]; esz = nf * 2; half = True
        else: raise ValueError(f"unsupported tbuffer format dfmt:{dfmt} nfmt:{nfmt}")
        parts = [f"{off}u"]; r_ = va
        if idxen:
            # V# index stride: the structured record for SBs; 16 for constant buffers (DXBC cbuffers are float4
            # registers, so `cb0[r0.x + 96].xyz` is tbuffer_load_format_xyz idxen with a 16-byte stride, whatever the
            # format width); the element size for typed buffers.
            parts.append(f"v{r_} * {stride if bk == 'sb' else (16 if bk == 'cb' else esz)}u"); r_ += 1
        if offen: parts.append(f"v{r_}")
        ad = self.t(); self.emit(f"uint {ad} = " + " + ".join(parts) + ";")
        nreq = len(comps)
        def word(wk):
            wi = f"(({ad} + {4 * wk}u) / 4u)"
            if bk == 'sb': W = stride // 4; return f"{nm}[{wi} / {W}u].d[{wi} % {W}u]"
            if bk == 'ba': return f"{nm}.Load({ad} + {4 * wk}u)"
            return f"asuint({nm}[{wi} / 4u][{wi} % 4u])"
        if not half:
            for k in range(min(nreq, nf)): self.setv(vd0 + k, f"asfloat({word(k)})")
        else:
            if off % 4: raise ValueError("unaligned half tbuffer")
            for k in range(min(nreq, nf)): self.setv(vd0 + k, f"f16tof32(({word(k // 2)} >> {16 * (k % 2)}) & 0xFFFF)")
        for k in range(nf, nreq): self.setv(vd0 + k, "0.0" if k < 3 else "1.0")

    def mubuf(self, name, vd0, vd1, va, sr, so, off, offen, idxen, glc):
        vd0 = int(vd0); va = int(va); sr = int(sr); off = int(off)
        if sr in getattr(self, 'embed_regs', {}):
            if not name.startswith('buffer_load') or not idxen or offen: raise ValueError("unsupported embedded buffer access")
            ename, est = self.emb_array(sr)
            mm0 = re.search(r'dword(?:x(\d))?$', name); n0 = int(mm0.group(1)) if mm0 and mm0.group(1) else 1
            ti_ = self.t(); self.emit(f"uint {ti_} = v{va} * {est // 4}u + {off // 4}u;")
            for k in range(n0): self.setv(vd0 + k, f"asfloat({ename}[{ti_} + {k}u])")
            return
        self._buf_store = not name.startswith('buffer_load')
        try: bk, nm, stride = self.resolve_buf(self.sg.get(sr), sr)
        finally: self._buf_store = False
        if bk == 'ctr':
            if name != 'buffer_atomic_add' or not glc: raise ValueError("unsupported counter access " + name)
            self.counter_orig = getattr(self, 'counter_orig', {}); self.counter_orig[vd0] = nm
            self.emit(f"v{vd0} = ex ? 0u : v{vd0};"); return
        # byte address
        parts = [f"{off}u"]
        r_ = va
        if idxen:
            if bk != 'sb': raise ValueError("idxen on non-structured buffer")
            parts.append(f"v{r_} * {stride}u"); r_ += 1
        if offen: parts.append(f"v{r_}")
        sov = self.src(so, 'u')
        if sov not in ('0u', '0'): parts.append(sov)
        ad = self.t(); self.emit(f"uint {ad} = " + " + ".join(parts) + ";")
        isload = name.startswith('buffer_load'); isstore = name.startswith('buffer_store')
        mm = re.search(r'dword(?:x(\d))?$', name)
        if isload or isstore:
            if not mm: raise ValueError("unsupported buffer width " + name)
            n = int(mm.group(1)) if mm.group(1) else 1
            for k in range(n):
                wi = f"(({ad} + {4 * k}u) / 4u)"
                if bk == 'sb':
                    W = stride // 4; ref = f"{nm}[{wi} / {W}u].d[{wi} % {W}u]"
                    if isload: self.setv(vd0 + k, f"asfloat({ref})")
                    else:
                        if not self.bufs[nm][0] == 'USB': raise ValueError("store to read-only structured buffer")
                        self.emit(f"if (ex) {ref} = v{vd0 + k};")
                elif bk == 'ba':
                    if isload: self.setv(vd0 + k, f"asfloat({nm}.Load({ad} + {4 * k}u))")
                    else:
                        if self.bufs[nm][0] != 'UAV': raise ValueError("store to read-only raw buffer")
                        self.emit(f"if (ex) {nm}.Store({ad} + {4 * k}u, v{vd0 + k});")
                else:   # constant buffer
                    if not isload: raise ValueError("store to CBV")
                    self.setv(vd0 + k, f"{nm}[{wi} / 4u][{wi} % 4u]")
            return
        # atomics
        fn = {'buffer_atomic_add': 'InterlockedAdd', 'buffer_atomic_and': 'InterlockedAnd', 'buffer_atomic_or': 'InterlockedOr', 'buffer_atomic_xor': 'InterlockedXor',
              'buffer_atomic_umin': 'InterlockedMin', 'buffer_atomic_umax': 'InterlockedMax', 'buffer_atomic_swap': 'InterlockedExchange'}.get(name)
        if fn is None or bk not in ('sb', 'ba'): raise ValueError("unsupported atomic " + name)
        wi = f"({ad} / 4u)"
        if bk == 'sb':
            W = stride // 4; ref = f"{nm}[{wi} / {W}u].d[{wi} % {W}u]"
            if self.bufs[nm][0] != 'USB': raise ValueError("atomic on read-only buffer")
        else: ref = None
        orig = self.t()
        self.emit(f"uint {orig} = 0;")
        if bk == 'sb': self.emit(f"if (ex) {fn}({ref}, v{vd0}, {orig});")
        else: self.emit(f"if (ex) {nm}.{fn}({ad}, v{vd0}, {orig});")
        if glc: self.emit(f"v{vd0} = ex ? {orig} : v{vd0};")

    def emb_array(self, sr):
        e = self.embed_regs[sr]; st = e['stride']; nrec = self.cval.get(sr + 2)
        if nrec is None or st == 0: raise ValueError("embedded table size unknown")
        key = (e['base'], st, nrec)
        for nm_, key2, words in getattr(self, '_emb_keys', []):
            if key2 == key: return nm_, st
        nd = st // 4 * nrec; o0 = self.code_start + e['base']
        words = struct.unpack_from(f'<{nd}I', self.blob, o0)
        name = f"EMB{len(self.embeds)}"; self.embeds.append((name, words))
        self._emb_keys = getattr(self, '_emb_keys', []) + [(name, key, words)]
        return name, st

    def ds(self, op, args):
        m = re.fullmatch(r'(ds_\w+) vdst:v(\d+) addr:v(\d+) data0:v(\d+) data1:v(\d+) off0:(\d+) off1:(\d+)( gds)?', op + ' ' + args)
        if not m: raise ValueError("ds format")
        name, vd, va, d0, d1, o0, o1, gds = m.groups(); vd = int(vd); va = int(va); d0 = int(d0); d1 = int(d1); o0 = int(o0); o1 = int(o1)
        if gds: raise ValueError("gds")
        if name == 'ds_swizzle_b32':
            # no LDS access: cross-lane permute of the value in ADDR. QDMode (bit 15): lane i of each quad reads quad lane
            # sel_i = (offset >> 2i) & 3 -> SM6 QuadReadLaneAt (GCN and D3D share the quad layout TL, TR, BL, BR).
            off_ = o0 | (o1 << 8)
            if not (off_ & 0x8000): raise ValueError("ds_swizzle bitmask mode")
            sels = [(off_ >> (2 * q)) & 3 for q in range(4)]
            self.sm6 = True
            if len(set(sels)) == 1:
                self.setv(vd, f"asfloat(QuadReadLaneAt(v{va}, {sels[0]}u))")
            else:
                r_ = [f"QuadReadLaneAt(v{va}, {k}u)" for k in range(4)]
                qi = "((uint(i.pos.x) & 1u) | ((uint(i.pos.y) & 1u) << 1))" if self.kind == 'ps' else "(lane & 3u)"
                pick = {k: r_[sels[k]] for k in range(4)}
                self.setv(vd, f"asfloat({qi} == 0u ? {pick[0]} : {qi} == 1u ? {pick[1]} : {qi} == 2u ? {pick[2]} : {pick[3]})")
            return
        if getattr(self, 'ls_cp', False):
            if name == 'ds_write2_b32':
                self.emit(f"if (ex) {{ cp_[{o0 & 63}] = v{d0}; cp_[{o1 & 63}] = v{d1}; }}"); return
            if name == 'ds_write_b32':
                self.emit(f"if (ex) cp_[{((o0 | (o1 << 8)) // 4) & 63}] = v{d0};"); return
            if name == 'ds_write_b64':
                k_ = ((o0 | (o1 << 8)) // 4) & 63; self.emit(f"if (ex) {{ cp_[{k_}] = v{d0}; cp_[{(k_ + 1) & 63}] = v{d0 + 1}; }}"); return
            raise ValueError("LS_AS_FN: unsupported " + name)
        if getattr(self, 'ds_cp', False):
            # control-point reads use the per-CP base registers (also used by ds_read2_b32); a single read from
            # another base is the patch-constant read
            self._cp_base = getattr(self, '_cp_base', set())
            if name in ('ds_read2_b32',):
                self._cp_base.add(va)
                self.setv(vd, f"asfloat(cp_[{o0 & 63}])"); self.setv(vd + 1, f"asfloat(cp_[{o1 & 63}])"); return
            if name == 'ds_read_b32':
                if va in self._cp_base: self.setv(vd, f"asfloat(cp_[{((o0 | (o1 << 8)) // 4) & 63}])"); return
                self.setv(vd, f"asfloat(pc_[{((o0 | (o1 << 8)) // 4) & 15}])"); return
            if name == 'ds_read_b64':
                k_ = ((o0 | (o1 << 8)) // 4) & 63; self.setv(vd, f"asfloat(cp_[{k_}])"); self.setv(vd + 1, f"asfloat(cp_[{(k_ + 1) & 63}])"); return
            raise ValueError("DS_AS_VS: unsupported " + name)
        self.uses_lds = True
        off = o0 | (o1 << 8)
        def W(byteexpr): return f"lds[({byteexpr}) / 4u]"
        base = f"v{va}"
        if name in ('ds_read2_b32', 'ds_read2st64_b32', 'ds_read_b64') and va == vd:
            # GCN reads ADDR when the instruction issues: `ds_read2_b32 v[13:14], v13` loads both dwords from the old v13.
            # The first assignment below overwrites v13, so the second would use the loaded value as its address.
            # NO_DS_ADDR_SNAP=1 = old behaviour.
            if not _ENV.get('NO_DS_ADDR_SNAP'):
                ta_ = self.t(); self.emit(f"uint {ta_} = v{va};"); base = ta_
        if name == 'ds_read_b32': self.setv(vd, f"asfloat({W(f'{base} + {off}u')})"); return
        if name == 'ds_write_b32': self.emit(f"if (ex) {{ uint dm_; InterlockedExchange({W(f'{base} + {off}u')}, v{d0}, dm_); }}"); return
        if name in ('ds_read2_b32', 'ds_read2st64_b32'):
            sc = 4 if name == 'ds_read2_b32' else 256
            self.setv(vd, f"asfloat({W(f'{base} + {o0 * sc}u')})"); self.setv(vd + 1, f"asfloat({W(f'{base} + {o1 * sc}u')})"); return
        if name in ('ds_write2_b32', 'ds_write2st64_b32'):
            sc = 4 if name == 'ds_write2_b32' else 256
            self.emit(f"if (ex) {{ uint dm_; InterlockedExchange({W(f'{base} + {o0 * sc}u')}, v{d0}, dm_); }}"); self.emit(f"if (ex) {{ uint dm_; InterlockedExchange({W(f'{base} + {o1 * sc}u')}, v{d1}, dm_); }}"); return
        if name == 'ds_read_b64':
            self.setv(vd, f"asfloat({W(f'{base} + {off}u')})"); self.setv(vd + 1, f"asfloat({W(f'{base} + {off + 4}u')})"); return
        if name == 'ds_write_b64':
            self.emit(f"if (ex) {{ uint dm_; InterlockedExchange({W(f'{base} + {off}u')}, v{d0}, dm_); }}"); self.emit(f"if (ex) {{ uint dm_; InterlockedExchange({W(f'{base} + {off + 4}u')}, v{d0 + 1}, dm_); }}"); return
        atom = {'ds_add_u32': 'InterlockedAdd', 'ds_and_b32': 'InterlockedAnd', 'ds_or_b32': 'InterlockedOr', 'ds_xor_b32': 'InterlockedXor',
                'ds_min_u32': 'InterlockedMin', 'ds_max_u32': 'InterlockedMax'}
        nm2 = name.replace('_rtn', '')
        if nm2 in atom:
            o = self.t(); self.emit(f"uint {o} = 0;")
            self.emit(f"if (ex) {atom[nm2]}(lds[({base} + {off}u) / 4u], v{d0}, {o});")
            if '_rtn_' in name: self.setv(vd, f"asfloat({o})")
            return
        raise ValueError("unsupported ds op " + name)

    def maskval(self, t):
        t = t.strip()
        if t in ('vcc',): return 'vcc'
        if t == 'exec': return 'ex'
        m = re.fullmatch(r's\[(\d+):\d+\]', t)
        if m: return f"sp_{m.group(1)}"
        m = re.fullmatch(r's(\d+)', t)
        if m and f"sp_{m.group(1)}" in self.decl: return f"sp_{m.group(1)}"
        if t in ('0',): return 'false'
        if t in ('-1', '0xffffffff'): return 'true'
        raise ValueError("mask operand " + t)

    def assemble(self):
        h = [f"// generated by gcn2hlsl.py from {os.path.basename(self.path)} ({self.kind})",
             "float mul_legacy(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }",
             "uint mulhi_u32(uint a, uint b) { uint a0 = a & 0xFFFFu, a1 = a >> 16, b0 = b & 0xFFFFu, b1 = b >> 16; uint t = a1 * b0 + ((a0 * b0) >> 16); uint w1 = (t & 0xFFFFu) + a0 * b1; return a1 * b1 + (t >> 16) + (w1 >> 16); }",
             "float cube_id(float3 v) { float3 a = abs(v); if (a.z >= a.x && a.z >= a.y) return v.z < 0 ? 5 : 4; if (a.y >= a.x) return v.y < 0 ? 3 : 2; return v.x < 0 ? 1 : 0; }",
             "float cube_sc(float3 v) { float3 a = abs(v); if (a.z >= a.x && a.z >= a.y) return v.z < 0 ? -v.x : v.x; if (a.y >= a.x) return v.x; return v.x < 0 ? v.z : -v.z; }",
             "float cube_tc(float3 v) { float3 a = abs(v); if (a.z >= a.x && a.z >= a.y) return -v.y; if (a.y >= a.x) return v.y < 0 ? -v.z : v.z; return -v.y; }",
             "float cube_ma(float3 v) { float3 a = abs(v); if (a.z >= a.x && a.z >= a.y) return 2 * v.z; if (a.y >= a.x) return 2 * v.y; return 2 * v.x; }",
             # GCN cube coords (sc/|2ma|+1.5, tc/|2ma|+1.5, slice*8+face) -> direction (inverse of cube_sc/tc above)
             "float3 cube_dir_of(float u, float w, float f) { float s = (u - 1.5) * 2.0, t = (w - 1.5) * 2.0; int fc = (int)(f - 8.0 * floor(f / 8.0) + 0.5); "
             "if (fc == 0) return float3(1, -t, -s); if (fc == 1) return float3(-1, -t, s); if (fc == 2) return float3(s, 1, t); if (fc == 3) return float3(s, -1, -t); if (fc == 4) return float3(s, -t, 1); return float3(-s, -t, -1); }"]
        if getattr(self, 'sm6', False):
            h.append("// sm6 (wave intrinsics; compiled with dxc)")
            h.append("uint rfl_(uint x, bool e) { uint4 b = WaveActiveBallot(e); if (!any(b != 0u)) return x;  /* helper lanes are not in the ballot: with only helpers left, use own value so waterfall loops exit (else 4096 spins -> GPU TDR) */ uint l = b.x ? firstbitlow(b.x) : (b.y ? 32u + firstbitlow(b.y) : (b.z ? 64u + firstbitlow(b.z) : (b.w ? 96u + firstbitlow(b.w) : 0u))); return WaveReadLaneAt(x, l); }")
        tex = sorted(x for x in self.decl if isinstance(x, tuple) and x[0] == 'T')
        smp = sorted(x for x in self.decl if isinstance(x, tuple) and x[0] == 'S')
        smpc = sorted(x for x in self.decl if isinstance(x, tuple) and x[0] == 'SC')
        for _, n in tex:
            r = self.ranges[n]
            if n.startswith('Ux'):
                ty = {8: 'RWTexture2DArray<float4>', 5: 'RWTexture3D<float4>', 3: 'RWTexture2D<float4>'}.get(self.tex_dim(n), 'RWTexture2D<float4>')
                h.append(f"{ty} {n}[{r['n']}] : register(u{r['reg']}, space{r['space']});")
                continue
            ty = {6: 'TextureCube', 10: 'TextureCubeArray', 8: 'Texture2DArray', 5: 'Texture3D', 3: 'Texture2D'}.get(self.tex_dim(n), 'Texture2D')
            h.append(f"{ty} {n}[{r['n']}] : register(t{r['reg']}, space{r['space']});")
        for _, n in smpc:
            r = self.ranges[n]; h.append(f"SamplerComparisonState {n}[{r['n']}] : register(s{r['reg']}, space{r['space']});")
        for _, n in smp:
            r = self.ranges[n]; h.append(f"SamplerState {n}[{r['n']}] : register(s{r['reg']}, space{r['space']});")
        for n, bt in sorted(self.bufs.items()):
            kind, reg, space = bt[0], bt[1], bt[2]
            if kind == 'CBV': h.append(f"cbuffer {n}_b : register(b{reg}, space{space}) {{ float4 {n}[1024]; }}")
            elif kind == 'SRV': h.append(f"ByteAddressBuffer {n} : register(t{reg}, space{space});")
            elif kind == 'UAV': h.append(f"RWByteAddressBuffer {n} : register(u{reg}, space{space});")
            elif kind == 'SB' or kind == 'USB':
                stride = self.bufs[n][3]; W = stride // 4
                if W not in getattr(self, '_sbt', set()):
                    self._sbt = getattr(self, '_sbt', set()) | {W}; h.append(f"struct SBT{W} {{ uint d[{W}]; }};")
                h.append(f"{'StructuredBuffer' if kind == 'SB' else 'RWStructuredBuffer'}<SBT{W}> {n} : register({'t' if kind == 'SB' else 'u'}{reg}, space{space});")
        for name, words in self.embeds:
            h.append(f"static const uint {name}[{len(words)}] = {{ " + ", ".join(f"0x{w:x}u" for w in words) + " };")
        sg_decl = sorted(self.usgpr_used)
        if self.kind == 'ps':
            h.append("struct PSIn {")
            # 'nointerpolation' on float inputs changes fxc's register packing vs the VS -> PSO rejected;
            # only used when FLAT_FLOAT=1. Integer attributes (sig ct 1/2) are declared uint/int instead.
            flat = (getattr(self, 'flat_attrs', set()) - getattr(self, 'interp_attrs', set())) if _ENV.get('FLAT_FLOAT') else set()
            ak = 0
            for e in self.ins:
                if e['sv'] == 0:
                    nc = bin(e['mask']).count('1'); ty = _sig_ty(e, nc)
                    h.append(f"  {'nointerpolation ' if ak in flat else ''}{ty} {e['name']}{e['idx']} : {e['name']}{e['idx']};")
                    ak += 1
                elif e['sv'] == 1 or e['name'].upper() == 'SV_POSITION': h.append("  float4 pos : SV_POSITION;")
                elif e['name'].upper() == 'SV_ISFRONTFACE': h.append(f"  bool sv_ff : SV_IsFrontFace;")
                else:
                    nc = bin(e['mask']).count('1')
                    h.append(f"  {'uint' if nc == 1 else 'uint%d' % nc} sv_{e['name']}{e['idx']} : {e['name']}{e['idx'] if e['idx'] else ''};")
            h.append("};")
            mrts = sorted(int(e[3:]) for e in set(self.exps) if e.startswith('mrt') and e[3:].isdigit())
            self.nmrt = (mrts[-1] + 1) if mrts else 1
            if self.nmrt > 1:
                h.append("struct PSOut {")
                for k_ in range(self.nmrt): h.append(f"  float4 c{k_} : SV_Target{k_};")
                h.append("};")
                h.append("PSOut main(PSIn i) {")
            else:
                h.append("float4 main(PSIn i) : SV_TARGET {")
        elif self.kind == 'cs':
            tg = self.decls.get('tg', (1, 1, 1))
            if getattr(self, 'uses_wave', False):
                ntot = tg[0] * tg[1] * tg[2]
                h.append(f"groupshared uint wv[{ntot}]; groupshared uint wx[{ntot}];")
            if getattr(self, 'uses_lds', False):
                words = 8192
                h.append(f"groupshared uint lds[{words}];")
            if getattr(self, 'uses_any64', False):
                h.append("#define any64_f lds[8191]" if getattr(self, 'uses_lds', False) else "groupshared uint any64_f;")   # LDS is already declared at the 32 KB limit
                h.append("bool any64_(bool v, uint flat) { GroupMemoryBarrierWithGroupSync(); if (flat == 0u) any64_f = 0u; GroupMemoryBarrierWithGroupSync(); if (v) { uint d_; InterlockedOr(any64_f, 1u, d_); } GroupMemoryBarrierWithGroupSync(); bool r = any64_f != 0u; GroupMemoryBarrierWithGroupSync(); return r; }")
            if getattr(self, 'uses_ctr', False):
                h.append("RWByteAddressBuffer g_ctr : register(u200, space99);")
            h.append(f"[numthreads({tg[0]}, {tg[1]}, {tg[2]})]")
            h.append("void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {")
        else:
            for e in self.ins:
                if e['sv'] not in (0, 6, 8) and e['name'] not in ('SV_InstanceID', 'SV_VertexID'):
                    raise ValueError("VS input " + e['name'] + " not supported")
            h.append("struct VSOut {")
            for e in self.outs:
                nc = bin(e['mask']).count('1'); ty = _sig_ty(e, nc)
                if e['sv'] == 1: h.append("  float4 pos : SV_POSITION;")
                elif e['sv'] != 0: h.append(f"  float{nc if nc > 1 else ''} sv_{e['name']}{e['idx']} : {e['name']}{e['idx']};")
                else: h.append(f"  {ty} {e['name']}{e['idx']} : {e['name']}{e['idx']};")
            h.append("};")
            params = ["uint vid : SV_VertexID"]
            self.vs_attrs = []
            for e in self.ins:
                if e['sv'] == 0:
                    nc = bin(e['mask']).count('1'); ty = _sig_ty(e, nc) if not _ENV.get('NO_INT_VSIN') else 'float' + (str(nc) if nc > 1 else '')
                    params.append(f"{ty} in_{e['name']}{e['idx']} : {e['name']}{e['idx']}"); self.vs_attrs.append((e, nc))
            if any(e['name'] == 'SV_InstanceID' for e in self.ins): params.insert(1, "uint iid : SV_InstanceID")
            h.append("VSOut main(" + ", ".join(params) + ") {")
        import re as _re
        if self.kind == 'cs' and getattr(self, 'uses_lds', False) and not getattr(self, 'has_s_barrier', False) \
                and not _ENV.get('NO_LDS_SYNC'):
            tg_ = self.decls.get('tg', (1, 1, 1))
            if tg_[0] * tg_[1] * tg_[2] <= 64:
                self.body, bad_ = _lds_sync(self.body)
                h.append(f"  // lds sync: 64-thread group was one Xbox wave (no s_barrier); barriers added for wave32" +
                         (f", {bad_} hazard(s) inside divergent blocks left unsynced" if bad_ else ""))
        _mx = max([int(m_) for m_ in _re.findall(r"v(\d+)", " ".join(self.body))] + [63])
        nv_ = min(255, _mx + 1)
        def _movrels(m_):
            d_, b_ = int(m_.group(1)), int(m_.group(2))
            cases = " ".join(f"case {k - b_}u: r_ = v{k}; break;" for k in range(b_, nv_))
            return f"{{ uint r_ = 0; switch (m0) {{ {cases} default: break; }} v{d_} = ex ? r_ : v{d_}; }}"
        self.body = [_re.sub(r"@@MOVRELS (\d+) (\d+)@@", _movrels, l_) for l_ in self.body]
        h.append("  uint " + ", ".join(f"v{k}" for k in range(nv_)) + ";")
        for k in range(nv_): h.append(f"  v{k} = 0;")
        if self.kind == 'ps' and not _ENV.get('NO_PS_INPUT_ENA'):
            # PS input VGPRs follow SPI_PS_INPUT_ENA (dword 216 of the Xbox shader header): I/J pairs first, then
            # POS_X/Y/Z/W, FRONT_FACE... Before, all were 0, so shaders using the pixel position read texel (0,0).
            ena = self.ps_input_ena()
            vk_ = 0
            for bit_, n_ in enumerate((2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1)):
                if not (ena >> bit_) & 1: continue
                if vk_ < nv_ and bit_ in (8, 9, 10) and self.has_pos:
                    h.append(f"  v{vk_} = asuint(i.pos.{'xyz'[bit_ - 8]});  // PS input POS_{'XYZ'[bit_ - 8]}")
                elif vk_ < nv_ and bit_ == 12 and self.has_ff:
                    h.append(f"  v{vk_} = asuint(i.sv_ff ? 1.0 : -1.0);  // PS input FRONT_FACE")
                elif bit_ >= 8:
                    h.append(f"  // PS input bit {bit_} (v{vk_}) not provided")
                vk_ += n_
        if self.kind == 'cs':
            h.append("  v0 = gtid.x; v1 = gtid.y; v2 = gtid.z;")
            tg_ = self.decls.get('tg', (1, 1, 1))
            ntot = tg_[0] * tg_[1] * tg_[2]
            h.append(f"  uint flat = gtid.x + gtid.y * {tg_[0]}u + gtid.z * {tg_[0] * tg_[1]}u; uint lane = flat & 63u; uint wbase = flat & ~63u; uint wlim = min(64u, {ntot}u - wbase);")
        if self.kind == 'vs' and getattr(self, 'ds_cp', False):
            h.append("  v0 = asuint(0.0); v1 = asuint(0.0); v2 = 0u;   // domain coords (0,0) -> weight 1 on control point 0")
        elif self.kind == 'vs':
            h.append("  v0 = vid;")
            if any(e['name'] == 'SV_InstanceID' for e in self.ins): h.append("  v3 = iid;")
            # the Xbox fetch shader gives every vertex attribute 4 VGPRs (v4 + 4*i), whatever its width
            # (checked against all dumped multi-attribute VS); NO_ALIGNED_VS_IN=1 = old packed layout
            packed_in = bool(_ENV.get('NO_ALIGNED_VS_IN'))
            # attributes the shader never reads (signature rw mask 0) are not fetched and take no VGPRs
            # (skinned depth-prepass VS: POSITION v4, BLENDWEIGHTS v8, BLENDINDICES v12 with NORMAL/TANGENT/
            # TEXCOORD unused); OLD_UNUSED_SLOTS=1 = old layout that gave every declared attribute a slot
            keep_unused = bool(_ENV.get('OLD_UNUSED_SLOTS'))
            vreg = 4; slot = 0
            for ai, (e, nc) in enumerate(self.vs_attrs):
                if not keep_unused and e.get('rw', 1) == 0: continue
                if not packed_in: vreg = 4 + 4 * slot
                slot += 1
                for c in range(nc):
                    h.append(f"  v{vreg} = asuint(in_{e['name']}{e['idx']}{'' if nc == 1 else '.' + 'xyzw'[c]});"); vreg += 1
        if sg_decl:
            rc = {}
            nn = 3
            for pp in self.rs_params:
                if pp['type'] == 'CONST32':
                    for k in range(pp['n']): rc[nn + k] = (pp, k)
                    nn += pp['n']
                else: nn += 1 if pp['type'] == 'TABLE' else 2
            used_rc = {pp['i']: pp for (pp, k) in (rc[n] for n in sg_decl if n in rc)}
            for pp in used_rc.values():
                h.insert(2, f"cbuffer RootConsts{pp['i']} : register(b{pp['reg']}, space{pp['space']}) {{ " + " ".join(f"uint rc{pp['i']}_{k};" for k in range(pp['n'])) + " }")
            tgb = nn
            def _init(n):
                if n in rc: return f"u{n} = rc{rc[n][0]['i']}_{rc[n][1]}"
                if self.kind == 'cs' and n in (tgb, tgb + 1, tgb + 2): return f"u{n} = gid.{'xyz'[n - tgb]}"
                return f"u{n} = 0"
            h.append("  uint " + ", ".join(_init(n) for n in sg_decl) + ";   // user-data SGPRs (table pointers are resolved symbolically)")
        if getattr(self, 'fn_uints', None): h.append("  uint " + ", ".join(f"{x} = 0" for x in self.fn_uints) + ";   // descriptor index snapshots")
        h.append("  uint scc = 0, m0 = 0;")
        h.append("  bool ex = true, vcc = false, sx_ = false;")
        masks = {x for x in self.decl if isinstance(x, str) and x.startswith('sp_')}
        # masks read before any write in the translated order (e.g. restored after a branch) still need a declaration
        # one pass over the body for the three kinds of names (sp_ masks, jk_ branch flags, wl_ v_writelane spill slots)
        names = set(re.findall(r'\b(?:sp_\d+|jk_[0-9a-f]+|wl_\d+_\d+)\b', "\n".join(self.body)))
        masks |= {x for x in names if x.startswith('sp_')}
        masks = sorted(masks)
        if masks: h.append("  bool " + ", ".join(f"{m} = false" for m in masks) + ";")
        jks = sorted(x for x in names if x.startswith('jk_'))
        if jks: h.append("  bool " + ", ".join(f"{m} = false" for m in jks) + ";")
        wls = sorted(x for x in names if x.startswith('wl_'))
        if wls: h.append("  uint " + ", ".join(f"{m} = 0" for m in wls) + ";")
        for e in sorted(set(self.exps)):
            # 'precise' position: depth-EQUAL passes pair a position-only prepass VS with the full G-buffer VS;
            # without it the driver may fuse/reorder their math differently -> 1 ULP depth mismatch (black trees).
            pr = 'precise ' if e == 'pos0' and self.kind != 'ps' and not _ENV.get('NO_PRECISE_POS') else ''
            h.append(f"  {pr}float4 {e}_o = 0;")
        if self.kind == 'vs':
            for e in self.outs:
                if e['sv'] == 0 and f"param{self._preg(e)}" not in self.exps: h.append(f"  float4 param{self._preg(e)}_o = 0;")
        if self.kind == 'ps':
            tex_in = [e for e in self.ins if e['sv'] == 0]
            if _ENV.get('NO_REG_ATTR'):
                groups = [[e] for e in tex_in]   # old behaviour: one A{k} per signature element
            else:
                # GCN attrN is the N-th interpolated *register* (VS param export N); several signature
                # elements can share one register (e.g. TEXCOORD1.xy + TEXCOORD2.zw), so build A{k}
                # per register, taking each component from the element that owns it.
                regs = sorted({e['reg'] for e in tex_in})
                groups = [[e for e in tex_in if e['reg'] == r] for r in regs]
                # The Xbox compiler only gives an attribute slot to registers the PS actually reads
                # (ISGN read mask != 0); an unread register before a read one shifts every attrN after it.
                if not _ENV.get('NO_SKIP_UNREAD_ATTR'):
                    groups = [g for g in groups if any(e.get('rw', 0xF) for e in g)]
            for k, grp in enumerate(groups):
                vals = []
                for c in range(4):
                    v = '0'
                    for e in grp:
                        comps = [cc for cc in range(4) if (e['mask'] >> cc) & 1]
                        if c in comps:
                            fld = f"i.{e['name']}{e['idx']}"
                            v = fld + ('' if len(comps) == 1 else '.' + 'xyzw'[comps.index(c)])
                            if _sig_int(e): v = f"asfloat({v})"
                            break
                    vals.append(v)
                h.append(f"  float4 A{k} = float4({', '.join(vals)});")
        for ln in self.body: h.append("  " + ln)
        if self.kind == 'ps':
            if getattr(self, 'nmrt', 1) > 1:
                h.append("  PSOut po;")
                for k_ in range(self.nmrt): h.append(f"  po.c{k_} = {'mrt%d_o' % k_ if ('mrt%d' % k_) in self.exps else 'float4(0, 0, 0, 0)'};")
                h.append("  return po;")
            else: h.append("  return mrt0_o;" if 'mrt0' in self.exps else "  return 0;")
        elif self.kind == 'cs': pass
        else:
            h.append("  VSOut o;")
            for e in self.outs:
                if e['sv'] == 1:
                    h.append("  o.pos = pos0_o;")
                    # Old workaround (CLAMP_Z_W=1): it pushed far-plane passes (sky at z==w, depth test EQUAL) off the far plane.
                    if _ENV.get('CLAMP_Z_W'): h.append("  o.pos.z = min(o.pos.z, o.pos.w * 0.99999);   // bridge clips z==w exactly")
                    continue
                comps = [c for c in range(4) if (e['mask'] >> c) & 1]
                if e['sv'] != 0:   # other system values (clip distances...): GCN exports them via pos1..3
                    src_ = 'pos1_o' if 'pos1' in self.exps else 'float4(0, 0, 0, 0)'
                    vv = [f"{src_}.{'xyzw'[c]}" for c in comps]
                    h.append(f"  o.sv_{e['name']}{e['idx']} = " + (vv[0] if len(vv) == 1 else f"float{len(vv)}({', '.join(vv)})") + ";"); continue
                sr = f"param{self._preg(e)}_o"
                vals = [f"{sr}.{'xyzw'[c]}" for c in comps]
                if _sig_int(e): vals = [f"{'asuint' if e['ct'] == 1 else 'asint'}({v})" for v in vals]
                h.append(f"  o.{e['name']}{e['idx']} = " + (vals[0] if len(vals) == 1 else f"{_sig_ty(e, len(vals))}({', '.join(vals)})") + ";")
            h.append("  return o;")
        h.append("}")
        for pc, t in self.todo: h.append(f"// TODO {pc}: {t}")
        out = "\n".join(h)
        # Append-based emitters (IncrementCounter inside a loop) compute their per-thread count from
        # frame constants; when those are off the count explodes and the 4096 cap still stalls the GPU
        # for seconds (compute queue hang entering gameplay). Cap these loops lower (EMIT_LOOP_CAP).
        if self.kind == 'cs' and 'IncrementCounter' in out and '[loop]' in out:
            cap = _ENV.get('EMIT_LOOP_CAP', '64')
            out = out.replace('< 4096u; ++', f'< {cap}u; ++') + f"\n// emitter loop cap {cap}\n"
        out = harden_waterfalls(out)
        return out, self.todo

def harden_waterfalls(out):
    """GPU-hang guards (tutorial-end / loading TDRs in the UI shaders):
    - waterfall loops (readfirstlane over a descriptor index) need at most one pass per lane: cap 128, not 4096;
    - data-driven texture-array indices are clamped to the declared array size (an out-of-range
      descriptor read during level loads can fault/hang the GPU)."""
    import re as _re
    lines = out.split('\n')
    for i, l in enumerate(lines):
        if '[loop] for (uint lp' in l and '< 4096u' in l and any('rfl_(' in x for x in lines[i + 1:i + 3]):
            lines[i] = l.replace('< 4096u', '< 128u')
    out = '\n'.join(lines)
    sizes = {m.group(1): int(m.group(2)) for m in _re.finditer(r'\b(Tx_sp\d+_r\d+)\[(\d+)\]\s*:\s*register', out)}
    def _clamp(m):
        name, idx = m.group(1), m.group(2)
        if name not in sizes or idx.startswith('min(') or '[' in idx: return m.group(0)
        return f"{name}[min({idx}, {sizes[name] - 1}u)]"
    return _re.sub(r'\b(Tx_sp\d+_r\d+)\[([^\]]*[A-Za-z_][^\]]*)\]', _clamp, out)

def _preg(self, e):
    """GCN param export index of a VS output: rank of its register among the non-SV output registers
    (was reg-1, which assumed SV_Position is register 0)."""
    regs = sorted({o['reg'] for o in self.outs if o['sv'] == 0})
    return regs.index(e['reg'])
Tr._preg = _preg

def sgpr_map(rs_params):
    """Deterministic Durango user-data layout: allocation starts at s3, in root-param order:
    CONST32 = n dwords, root CBV/SRV/UAV = 2 sgprs, descriptor table = 1 sgpr."""
    m = {}; n = 3
    for p in rs_params:
        m[n] = p['i']
        n += p.get('n', 1) if p['type'] == 'CONST32' else (1 if p['type'] == 'TABLE' else 2)
    return m

def translate(path):
    G.cache_reads(True)
    _ENV.snap = dict(os.environ)   # the switches cannot change during one translation
    try:
        t0 = Tr(path)
        return Tr(path, pmap=sgpr_map(t0.rs_params)).run()
    finally:
        _ENV.snap = None
        G.cache_reads(False)

if __name__ == '__main__':
    txt, todo = translate(sys.argv[1])
    if len(sys.argv) > 2: open(sys.argv[2], 'w').write(txt)
    else: print(txt)
    if todo:
        print(f"{len(todo)} untranslated instructions:", file=sys.stderr)
        for pc, t in todo[:25]: print("  ", pc, t, file=sys.stderr)
