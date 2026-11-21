#!/usr/bin/env python3
"""Crash fuzzer for the SafeC compiler.

Takes every .safec test file and makes damaged copies of it, then runs the
compiler on each one. A compiler must reject bad input with an error: never
crash, hang, or (for input it accepts) produce invalid LLVM IR.

Two kinds of damage, mixed (FUZZ_MODE=token or FUZZ_MODE=semantic for only one):
  token     delete / duplicate / swap / replace / insert tokens, cut the file
            short, splice in tokens from another file. Mostly syntax errors:
            tests the lexer and parser.
  semantic  changes that usually keep the syntax valid: swap a name, type or
            operator, add or remove own/mut/*?, move, delete or duplicate a
            line, wrap one in trusted { }. Gets past the parser: tests sema,
            the borrow checker and codegen.

Reported:
  crash         killed by a signal, or a sanitizer report (ASan / UBSan)
  hang          no answer within the timeout
  throw         an uncaught C++ exception (terminate)
  codegen-fail  sema accepted the program but code generation failed without
                an error message (a clean "error: ..." from codegen is fine)
  bad-ir        code generation worked but `opt -passes=verify` rejects the IR

usage: fuzz.py <safec binary> <tests dir> <out dir> <count> [seed]
   or: bash tests/fuzz/run_fuzz.sh [count] [seed]   (builds safec with sanitizers first)
"""
import os, random, re, shutil, subprocess, sys, tempfile, hashlib
from concurrent.futures import ThreadPoolExecutor

TOKEN_RE = re.compile(r'''
    //[^\n]*            | /\*.*?\*/          |
    "(?:\\.|[^"\\\n])*" | '(?:\\.|[^'\\\n])*' |
    [A-Za-z_]\w*        | 0[xX][0-9a-fA-F]+  | \d+\.?\d*(?:[eE][+-]?\d+)?[fF]? |
    <<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=  |
    \S
''', re.X | re.S)

EXTRA = ['own', 'mut', 'raw', '*?', '*', '&', '&mut', 'new', 'free', 'defer', 'trusted', 'null',
         'struct', 'int', 'char', 'bool', 'float', 'void', '{', '}', '(', ')', '[', ']', ';', ',',
         '.', '->', '=', '+=', '++', '--', '!', '~', '-', 'return', 'if', 'else', 'while', 'for',
         'switch', 'case', 'default', ':', 'break', 'continue', '0', '1', '-1', '2147483647',
         '2147483648', '99999999999999999999', '1.5', "'a'", '"s"', 'true', 'false', 'x', 'p', 'main']


def tokens(src):
    return [m.group(0) for m in TOKEN_RE.finditer(src) if not m.group(0).startswith(('//', '/*'))]


def mutate(rng, toks, pool, other_lines):
    t = list(toks)
    for _ in range(rng.choice([1, 1, 1, 2, 3])):
        if not t:
            t = [rng.choice(pool)]
        i = rng.randrange(len(t))
        op = rng.randrange(8)
        if op == 0:   del t[i]
        elif op == 1: t.insert(i, t[i])
        elif op == 2 and len(t) > 1:
            j = min(i + 1, len(t) - 1); t[i], t[j] = t[j], t[i]
        elif op == 3: t[i] = rng.choice(pool)
        elif op == 4: t.insert(i, rng.choice(pool))
        elif op == 5: t = t[:i]
        elif op == 6:                             # delete a run of tokens
            del t[i:i + rng.randint(2, 8)]
        else:                                     # splice tokens from another file
            t[i:i] = tokens(rng.choice(other_lines))
    return ' '.join(t)


TYPES = ['int', 'char', 'bool', 'float', 'void']
QUALS = ['own', 'mut', 'raw']
BINOPS = ['+', '-', '*', '/', '%', '<', '>', '<=', '>=', '==', '!=', '&&', '||', '&', '|', '^', '<<', '>>']
ASSIGNS = ['=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=']
KEYWORDS = set(TYPES + QUALS + ['struct', 'return', 'if', 'else', 'while', 'for', 'switch', 'case',
           'default', 'break', 'continue', 'new', 'free', 'defer', 'trusted', 'null', 'true', 'false'])


def mutate_semantic(rng, src):
    """Mutations that usually keep the program parseable, to reach sema and codegen."""
    lines = src.splitlines()
    for _ in range(rng.choice([1, 1, 2, 3])):
        op = rng.randrange(6)
        if op <= 1 or not lines:                 # same-kind token swap inside one line
            if not lines: break
            li = rng.randrange(len(lines))
            toks = tokens(lines[li])
            if not toks: continue
            idents = [t for t in tokens(src) if re.fullmatch(r'[A-Za-z_]\w*', t) and t not in KEYWORDS]
            i = rng.randrange(len(toks)); t = toks[i]
            if t in TYPES: toks[i] = rng.choice(TYPES)
            elif t in QUALS: toks[i] = rng.choice(QUALS + [''])
            elif t in BINOPS: toks[i] = rng.choice(BINOPS)
            elif t in ASSIGNS: toks[i] = rng.choice(ASSIGNS)
            elif t == '*': toks[i] = rng.choice(['*?', '*', '* *'])
            elif t == '&': toks[i] = rng.choice(['&mut', '&', ''])
            elif re.fullmatch(r'\d+', t): toks[i] = rng.choice(['0', '1', '-1', '3', '255', '2147483647', '1.5'])
            elif re.fullmatch(r'[A-Za-z_]\w*', t) and t not in KEYWORDS and idents: toks[i] = rng.choice(idents)
            elif t in ('++', '--'): toks[i] = rng.choice(['++', '--', ''])
            else:
                toks.insert(i, rng.choice(QUALS + ['*', '&', '!', '-', '(', 'null', 'new int', 'trusted']))
            lines[li] = ' '.join(toks)
        elif op == 2: del lines[rng.randrange(len(lines))]
        elif op == 3:
            i = rng.randrange(len(lines)); lines.insert(rng.randrange(len(lines) + 1), lines[i])
        elif op == 4:
            i = rng.randrange(len(lines)); l = lines.pop(i); lines.insert(rng.randrange(len(lines) + 1), l)
        else:                                     # wrap a line in trusted { } or a block
            i = rng.randrange(len(lines))
            lines[i] = rng.choice(['trusted { ', '{ ', 'if (1) ', 'while (0) ', 'defer ']) + lines[i] + \
                       (' }' if rng.random() < 0.7 else '')
    return '\n'.join(lines)


def run(binary, path, args, timeout):
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=0',
               UBSAN_OPTIONS='print_stacktrace=1')
    try:   # in the program's own temp folder: the driver writes prog.ll next to where it runs
        p = subprocess.run([binary, path] + args, capture_output=True, text=True,
                           timeout=timeout, env=env, errors='replace', cwd=os.path.dirname(path))
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return None, 'timeout'


def classify(rc, out):
    if rc is None: return 'hang'
    if 'AddressSanitizer' in out or 'runtime error:' in out: return 'crash'
    if 'terminate called' in out: return 'throw'
    if rc < 0 or rc >= 128: return 'crash'
    return None


HAVE_OPT = shutil.which('opt') is not None


def one(job):
    binary, src, timeout = job
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, 'prog.safec')
        with open(path, 'w') as f:
            f.write(src)
        rc, out = run(binary, path, ['--till-sema'], timeout)
        kind = classify(rc, out)
        if kind or rc != 0:
            return kind, out, src
        rc, out = run(binary, path, ['--emit-llvm'], timeout)   # accepted: codegen must work too
        kind = classify(rc, out)
        if kind: return kind, out, src
        if rc != 0:
            # a clean rejection at codegen (e.g. codegen/fail/reserved_allocator_names)
            # is fine; failing without saying why is not
            return (None if 'error:' in out else 'codegen-fail'), out, src
        if HAVE_OPT:
            v = subprocess.run(['opt', '-passes=verify', '-disable-output', os.path.join(d, 'prog.ll')],
                               capture_output=True, text=True)
            if v.returncode != 0:
                return 'bad-ir', v.stderr, src
    return None, '', src


def signature(kind, out):
    """Group findings by where they happen, so one bug isn't reported a thousand times."""
    for pat in [r'runtime error: [^\n]*', r'SUMMARY: AddressSanitizer: \S+ [^\n]*',
                r'terminate called[^\n]*\n[^\n]*', r'error: [^\n]*']:
        m = re.search(pat, out)
        if m:
            s = re.sub(r'/tmp/\S+', '', m.group(0))
            s = re.sub(r"'%?[\w.]+'", "'_'", s)
            s = re.sub(r'\d+', 'N', s)
            return kind + ': ' + s.strip()
    return kind + ': ' + (out.strip().splitlines() or ['?'])[-1][:120]


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    binary, tests_dir, out_dir, iters = os.path.abspath(sys.argv[1]), sys.argv[2], sys.argv[3], int(sys.argv[4])
    seed = int(sys.argv[5]) if len(sys.argv) > 5 else 1
    rng = random.Random(seed)
    os.makedirs(out_dir, exist_ok=True)

    files = []
    for root, _, names in os.walk(tests_dir):
        files += [os.path.join(root, n) for n in names if n.endswith('.safec')]
    corpus = [open(f).read() for f in sorted(files)]
    pool = sorted(set(t for src in corpus for t in tokens(src))) + EXTRA
    lines = [l for src in corpus for l in src.splitlines() if l.strip()]

    mode = os.environ.get('FUZZ_MODE', 'mixed')
    jobs = []
    for _ in range(iters):
        base = rng.choice(corpus)
        if mode == 'token' or (mode == 'mixed' and rng.random() < 0.3):
            src = mutate(rng, tokens(base), pool, lines)
        else:
            src = mutate_semantic(rng, base)
        jobs.append((binary, src, 10))

    found = {}
    counts = {}
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as ex:
        for n, (kind, out, src) in enumerate(ex.map(one, jobs), 1):
            if kind:
                sig = signature(kind, out)
                counts[sig] = counts.get(sig, 0) + 1
                if sig not in found or len(src) < len(found[sig][0]):
                    found[sig] = (src, out)
            if n % 1000 == 0 and n < iters:
                print(f'  {n} of {iters} done, {len(found)} distinct problems so far', flush=True)

    for i, (sig, (src, out)) in enumerate(sorted(found.items())):
        h = hashlib.md5(sig.encode()).hexdigest()[:8]
        with open(os.path.join(out_dir, f'{h}.safec'), 'w') as f: f.write(src + '\n')
        with open(os.path.join(out_dir, f'{h}.log'), 'w') as f: f.write(sig + '\n\n' + out)
        print(f'[{counts[sig]:5d}x] {h}  {sig}')
    print(f'{iters} damaged programs, {len(found)} distinct problems')
    if found:
        print(f'each one saved in {out_dir}/ as <id>.safec (the smallest input that shows it) and <id>.log')
    if not HAVE_OPT:
        print('(opt not found: the generated IR was not verified)')
    sys.exit(1 if found else 0)


if __name__ == '__main__':
    main()
