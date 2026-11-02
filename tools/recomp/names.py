#!/usr/bin/env python3
"""tools/recomp/names.py OUTDIR: names for the game's functions from what the executable itself says,
without reading any function: messages that give a function's name, source paths, tables of a name and
a code pointer, vtables and wrappers. Writes OUTDIR/names.tsv (address, name, how, library, source
file): game-derived, so it stays with the player's copy. annotations/functions.tsv wins over it, and
holds only what was checked. Prints what each step gave and how much of the target is covered."""
import bisect
import collections
import re
import struct
import sys
from pathlib import Path

OUT = Path(sys.argv[1])
TARGET = {'game', 'dantelion2', 'fd4', 'unknown', ''}


def rows(name):
    return [l.split('\t') for l in (OUT / name).read_text(errors='replace').splitlines()[1:] if l]


functions = {int(r[0], 16): (int(r[1]), int(r[2])) for r in rows('functions.tsv')}
starts = sorted(functions)
modules = {int(r[0], 16): (r[2], r[3] if len(r) > 3 else '') for r in rows('modules.tsv')}
names, how = {}, {}


def give(function, name, why):
    if function in functions and function not in names and name:
        names[function] = name
        how[function] = why
        return True
    return False


def owner(address):
    i = bisect.bisect_right(starts, address) - 1
    if i >= 0 and address < starts[i] + functions[starts[i]][0]:
        return starts[i]
    return None


def tidy(text):
    return re.sub(r'[^A-Za-z0-9_:~]', '', text)


by_function = collections.defaultdict(list)
for r in rows('strings.tsv'):
    if len(r) >= 3:
        by_function[int(r[0], 16)].append(r[2])

# Every reference to text in the code (scan.c's strings.tsv keeps only some of them).
image_for_text = (OUT.parent / 'eboot.elf').read_bytes()
phoff_, = struct.unpack_from('<Q', image_for_text, 32)
phnum_, = struct.unpack_from('<H', image_for_text, 56)
loads_ = [p for p in (struct.unpack_from('<IIQQQQQQ', image_for_text, phoff_ + i * 56) for i in range(phnum_)) if p[0] == 1]
code_ = image_for_text[loads_[0][2]:loads_[0][2] + loads_[0][5]]
PRINTABLE = re.compile(rb'[\x20-\x7e\t\n]{4,200}\0')
added = 0
for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', code_):
    i = m.start()
    disp, = struct.unpack_from('<i', code_, i + 3)
    target = loads_[0][3] + i + 7 + disp
    for p in loads_:
        if p[3] <= target < p[3] + p[5]:
            hit = PRINTABLE.match(image_for_text, p[2] + target - p[3])
            if hit:
                f = owner(loads_[0][3] + i)
                text_ = hit.group(0)[:-1].decode('ascii')
                if f is not None and text_ not in by_function[f]:
                    by_function[f].append(text_)
                    added += 1
            break
print('text references added', added, 'functions with text', len(by_function))

# 1. A message that starts with a qualified name: the function that says it, if only one does.
QUALIFIED = re.compile(r'^!?\s*([A-Za-z_]\w+(?:::~?[A-Za-z_]\w+)+)(?:\(|\s*:|\s|$)')
sayers = collections.defaultdict(set)
for f, texts in by_function.items():
    for t in texts:
        m = QUALIFIED.match(t)
        if m:
            sayers[m.group(1)].add(f)
for name, fs in sayers.items():
    if len(fs) == 1:
        give(next(iter(fs)), name, 'message')
# 2. A message that starts with Name(.
CALL = re.compile(r'^!?\s*([A-Za-z_][A-Za-z0-9_]{3,})\(\s*\)?')
sayers = collections.defaultdict(set)
for f, texts in by_function.items():
    for t in texts:
        m = CALL.match(t)
        if m:
            sayers[m.group(1)].add(f)
for name, fs in sayers.items():
    if len(fs) == 1:
        give(next(iter(fs)), name, 'message')

# 3. Tables in data: a pointer to a name beside a pointer to code.
slots = {int(a, 16): int(b, 16) for a, b in rows('pointer_slots.tsv')}
image = (OUT.parent / 'eboot.elf').read_bytes()
phoff, = struct.unpack_from('<Q', image, 32)
phnum, = struct.unpack_from('<H', image, 56)
loads = [p for p in (struct.unpack_from('<IIQQQQQQ', image, phoff + i * 56) for i in range(phnum)) if p[0] == 1]


def text_at(address):
    for p in loads:
        if p[3] <= address < p[3] + p[5]:
            at = p[2] + address - p[3]
            end = image.find(b'\0', at, at + 80)
            if end > at:
                try:
                    return image[at:end].decode('ascii')
                except UnicodeDecodeError:
                    return None
    return None


IDENTIFIER = re.compile(r'[A-Za-z_][A-Za-z0-9_:]{2,63}$')
tables = collections.defaultdict(set)
for slot, target in slots.items():
    if target in functions:
        for step in (-8, 8, -16, 16):
            other = slots.get(slot + step)
            if other is not None and other not in functions:
                text = text_at(other)
                if text and IDENTIFIER.match(text):
                    tables[text].add(target)
                    break
for name, fs in tables.items():
    if len(fs) == 1:
        give(next(iter(fs)), name, 'table')

# 3b. Script bindings: the function stored into a slot, then its name passed to the registrar.
code_text = loads_[0]
IDENT = re.compile(rb'[A-Za-z_][A-Za-z0-9_]{3,60}\0')
# Code ends at the import stubs; what follows in the segment is text and tables.
code_end = max(int(r[0], 16) for r in rows('imports.tsv')) + 16
pairs = collections.defaultdict(set)
# lea rax, [function]; mov [global + 8], rax; ...; lea rdx, [name]; lea rcx, [text]; ...; call [reg + 0x58]
# A slot may first get a descriptor the same way, so the function is the last store before the name.
NAMED = re.compile(rb'\x48\x8d\x15(.{4})\x48\x8d\x0d', re.S)
STORED = re.compile(rb'\x48\x8d\x05(.{4})\x48\x89\x05', re.S)
REGISTERED = re.compile(rb'\xff[\x50-\x57]\x58')
for m in NAMED.finditer(code_):
    name_at = m.start(1) + 4 + struct.unpack('<i', m.group(1))[0]
    hit = IDENT.match(code_, name_at) if 0 <= name_at < len(code_) else None
    if not hit:
        continue
    start = max(max(0, m.start() - 96), *(c.end() for c in REGISTERED.finditer(code_, max(0, m.start() - 96), m.start())), 0)
    stores = [s.start() + 7 + struct.unpack('<i', s.group(1))[0] + code_text[3] for s in STORED.finditer(code_, start, m.start())]
    stores = [a for a in stores if a in functions and a < code_end]
    if stores:
        pairs[hit.group(0)[:-1].decode()].add(stores[-1])
bound = 0
for name, fs in pairs.items():
    if len(fs) == 1:
        bound += give(next(iter(fs)), name, 'binding')
print('script bindings', bound)

# 4. Source files: the file a function's messages name, spread only over short runs between two
# functions of the same file (a file's functions stay together; a longer run can be anything).
files = {}
for a, (module, source) in modules.items():
    if source:
        files[a] = re.split(r'[\\/]', source)[-1]
order = sorted(functions)
last, gap = None, []
for a in order:
    if a in files:
        if last is not None and gap and len(gap) <= 16 and files[last] == files[a]:
            for g in gap:
                files[g] = files[a]
        last, gap = a, []
    elif last is not None:
        gap.append(a)

# 4a. DLRuntimeClass: the engine's own type information. Startup code calls a class's getter and stores
# two text pointers at +0x40 and +0x48 of the object it returns, then links it to its parent's. The
# getter is also a virtual function of the class, which ties a vtable to its name.
text0 = loads[0]
code0 = image[text0[2]:text0[2] + text0[5]]
RUNTIME = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})[\x48\x49\x4c\x4d]\x89[\x40-\x7f]\x40', re.S)
SECOND = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})[\x48\x49\x4c\x4d]\x89[\x40-\x7f]\x48', re.S)
runtime_getters = {}
def wide_or_text(address):
    for p_ in loads:
        if p_[3] <= address < p_[3] + p_[5]:
            at = p_[2] + address - p_[3]
            ascii_ = re.compile(rb'[\x20-\x7e]{2,120}\0').match(image, at)
            if ascii_:
                return ascii_.group(0)[:-1].decode()
            wide = re.compile(rb'(?:[\x20-\x7e]\0){2,120}\0\0').match(image, at)
            if wide:
                return wide.group(0)[:-2].decode('utf-16-le')
    return None
for m in RUNTIME.finditer(code0):
    i = m.start()
    near = SECOND.search(code0, i + 11, i + 40)
    if not near:
        continue
    one, = struct.unpack('<i', m.group(1))
    two, = struct.unpack('<i', near.group(1))
    first = wide_or_text(text0[3] + i + 7 + one)
    second = wide_or_text(text0[3] + near.start() + 7 + two)
    if not first:
        continue
    call = code0.rfind(b'\xe8', max(0, i - 40), i)
    getter = None
    if call >= 0:
        rel, = struct.unpack_from('<i', code0, call + 1)
        getter = text0[3] + call + 5 + rel
    runtime_getters[getter or -i] = (first, second)
print('DLRuntimeClass classes', len(runtime_getters), 'distinct names', len({v[0] for v in runtime_getters.values()}), list(runtime_getters.values())[:14])
give_runtime = {g: v for g, v in runtime_getters.items()}

# 4b. Classes: a vtable is a run of slots that hold functions; a function that takes its address is
# one of the class's constructors or destructors. The class takes the name its functions agree on: a
# source file named as a class, or a class-like name in their messages.
text = loads[0]
code = image[text[2]:text[2] + text[5]]
sites = collections.defaultdict(set)
for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', code):
    i = m.start()
    disp, = struct.unpack_from('<i', code, i + 3)
    sites[text[3] + i + 7 + disp].add(text[3] + i)
vtables = {}
ordered = sorted(slots)
i = 0
while i < len(ordered):
    start = ordered[i]
    run = []
    while i < len(ordered) and ordered[i] == start + len(run) * 8 and slots[ordered[i]] in functions:
        run.append(slots[ordered[i]])
        i += 1
    # Objects hold the table's address past two header slots: `lea table; add 0x10`.
    if run and (start in sites or (start - 16) in sites):
        if len(run) >= 2 or (start - 16) in sites:
            vtables[start] = run
            sites[start] |= sites.get(start - 16, set())
    if not run:
        i += 1
CLASSY = re.compile(r'^(?:CS|FD4|DL|Sprj|Fnd|MENU|Ez|GX|CG|Frpg|World|Msg|Rend|[A-Z])[A-Za-z0-9_]{3,}$')
GENERIC = {'Exception', 'Allocator', 'Unknown', 'Error', 'Failed', 'String', 'Object', 'Default', 'Invalid'}
# A vtable's slot for its class is a stub of one or two instructions that goes to the getter.
runtime_stubs = {}
for run in vtables.values():
    for f in run:
        if functions[f][0] <= 16:
            at = f - text0[3]
            if code0[at] == 0xe9:
                rel, = struct.unpack_from('<i', code0, at + 1)
                got = runtime_getters.get(f + 5 + rel)
                if got and got[0] == got[1]:
                    runtime_stubs[f] = got[0]
print('runtime class stubs', len(runtime_stubs))
# Survey: the first slots of the vtables.
first = collections.Counter()
sizes = collections.Counter()
for table, run in vtables.items():
    f = run[0]
    first[(functions[f][1] if functions[f][1] < 6 else 6, code0[f - text0[3]])] += 1
print('slot 0 (instructions, first byte)', first.most_common(10))
class_of, class_methods = {}, collections.Counter()
how_named = {}
class_names = {}
# A name in the messages of many classes is one they use (a singleton), not what they are.
spread = collections.defaultdict(set)
for table, run in vtables.items():
    for f in set(run) | ({owner(s) for s in sites[table]} - {None}):
        for s in by_function.get(f, ()):
            if CLASSY.match(s):
                spread[s].add(table)
file_spread = collections.defaultdict(set)
for table, run in vtables.items():
    for f in set(run):
        if f in files:
            file_spread[files[f].rsplit('.', 1)[0]].add(table)
for table, run in vtables.items():
    ctors = {owner(s) for s in sites[table]} - {None}
    votes = collections.Counter()
    for f in set(run) | ctors:
        if f in files and files[f].endswith(('.cpp', '.c')):
            stem = files[f].rsplit('.', 1)[0]
            if CLASSY.match(stem) and stem not in GENERIC and len(file_spread[stem]) <= 4:
                votes[stem] += 3
        for s in by_function.get(f, ()):
            if CLASSY.match(s) and s not in GENERIC and len(s) < 48 and len(spread[s]) <= 2:
                votes[s] += 1
    name = None
    # A constructor that says a name no other class says is naming its own class.
    own = {s for f in ctors for s in by_function.get(f, ()) if CLASSY.match(s) and s not in GENERIC and len(spread[s]) == 1}
    if len(own) == 1:
        name = next(iter(own))
    elif votes:
        top, count = votes.most_common(1)[0]
        if count >= 3 and (len(votes) == 1 or count >= 2 * votes.most_common(2)[1][1]):
            name = top
    for f in run:
        known = runtime_stubs.get(f)
        if known:
            name = known
            how_named[table] = 'runtime'
            break
    class_names[table] = name
    label = name or f'Class_{table:x}'
    # Many functions writing one vtable are its users (objects built in place), not its constructors.
    if len(ctors) <= 8:
        for f in ctors:
            give(f, f'{label}::writes_vtable', 'class' if name else 'vtable')
    for index, f in enumerate(run):
        class_of.setdefault(f, table)
print('named by DLRuntimeClass', len(how_named))
print('vtables', len(vtables), 'named', sum(1 for n in class_names.values() if n), 'distinct names', len({n for n in class_names.values() if n}),
      'functions in them', len(class_of))
# THIS_CLASS: the function that sets up a class's runtime description says its name and takes the
# address of the vtable it uses.
for f, texts in by_function.items():
    for s in texts:
        m = re.match(r'([A-Za-z_]\w+)::THIS_CLASS$', s)
        if m:
            near = [table for table in vtables if any(owner(x) == f for x in sites[table])]
            if len(near) == 1 and not class_names.get(near[0]):
                class_names[near[0]] = m.group(1)
# Inheritance: a class overrides some of its parent's slots and keeps the others, so two vtables that
# share slots at the same index are relatives. The parent is the shorter, or the one more share with.
slot_users = collections.defaultdict(set)
for table, run in vtables.items():
    for index, f in enumerate(run):
        slot_users[(index, f)].add(table)
parents = {}
for table, run in vtables.items():
    scores = collections.Counter()
    for index, f in enumerate(run):
        users = slot_users[(index, f)]
        if len(users) < 64:
            for other in users:
                if other != table and len(vtables[other]) <= len(run):
                    scores[other] += 1
    if scores:
        best, count = scores.most_common(1)[0]
        if count >= 2 and count * 2 >= len(vtables[best]):
            parents[table] = best
# A constructor calls its parent's constructor (the function that writes the parent's vtable).
writers = collections.defaultdict(set)
for table in vtables:
    for s in sites[table]:
        f = owner(s)
        if f is not None:
            writers[f].add(table)
direct = collections.defaultdict(set)
for r in rows('calls.tsv'):
    direct[int(r[0], 16)].add(int(r[1], 16))
by_ctor = 0
for f, tables in writers.items():
    if len(tables) != 1:
        continue
    table = next(iter(tables))
    bases = {b for callee in direct[f] for b in writers.get(callee, ()) if b != table}
    if len(bases) == 1 and table not in parents:
        parents[table] = next(iter(bases))
        by_ctor += 1
print('parents from constructors', by_ctor)
# The vtables named above are often those of a class's DLRuntimeClass object (their first slot goes to
# DLRuntimeClass's). Such an object's slots create the class, so they write the class's real vtable.
linked = 0
for table in list(vtables):
    name = class_names.get(table)
    if not name or vtables[table][0] != 0x20b5a60 and functions[vtables[table][0]][1] > 1:
        continue
    made = set()
    for f in set(vtables[table]):
        for w in writers.get(f, ()):
            made.add(w)
        for callee in direct[f]:
            for w in writers.get(callee, ()):
                made.add(w)
    made -= {table}
    made = {m for m in made if not class_names.get(m)}
    if 1 <= len(made) <= 2:
        for m in made:
            class_names[m] = name
            linked += 1
print('instance vtables named through their DLRuntimeClass', linked)
children = collections.Counter(parents.values())
roots = collections.Counter()
for table in vtables:
    seen, node = set(), table
    while node in parents and node not in seen:
        seen.add(node)
        node = parents[node]
    roots[node] += 1
print('inheritance', len(parents), 'classes with a parent;', len(roots), 'families; the largest',
      [(class_names.get(r) or hex(r), n, len(vtables[r])) for r, n in roots.most_common(10)])
print('named after THIS_CLASS', sum(1 for n in class_names.values() if n))
with open(OUT / 'classes.tsv', 'w') as out:
    out.write('vtable\tname\tslots\tparent\tchildren\n')
    for table, run in sorted(vtables.items()):
        out.write(f"{table:#x}\t{class_names.get(table) or ''}\t{len(run)}\t{parents.get(table, 0):#x}\t{children[table]}\n")

shared = collections.Counter(f for run in vtables.values() for f in set(run))
for table, run in vtables.items():
    label = class_names[table] or f'Class_{table:x}'
    for index, f in enumerate(run):
        if shared[f] == 1:
            give(f, f'{label}::vf{index}', 'class' if class_names[table] else 'vtable')
for f, count in shared.items():
    if count > 1 and f in class_of:
        give(f, f'shared_{class_of[f]:x}', 'shared')

# 4c. Singletons: FD4Singleton checks a global and says the class's name if it is null. That ties the
# global to the name, and a function called with that global as its first argument is a method of it.
singleton_votes = collections.defaultdict(collections.Counter)
targets_of_text = collections.defaultdict(list)
for at, where in sites.items():
    for p_ in loads:
        if p_[3] <= at < p_[3] + p_[5]:
            hit = re.compile(rb'[A-Za-z_][A-Za-z0-9_]{3,60}\0').match(image, p_[2] + at - p_[3])
            if hit:
                targets_of_text[hit.group(0)[:-1].decode()] = where
            break
base_va = text[3]
LEA = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})[\x48\x49]\x8b', re.S)
ASSERTS = collections.Counter()
methods = collections.defaultdict(collections.Counter)
for name, where in targets_of_text.items():
    if not CLASSY.match(name) or name in GENERIC:
        continue
    for site in where:
        i = site - base_va
        if code[i:i + 3] != b'\x48\x8d\x0d':  # the name is the assert's fourth argument
            continue
        start = max(0, i - 72)
        loads_here = list(LEA.finditer(code, start, i))
        if not loads_here:
            continue
        m = loads_here[-1]
        disp, = struct.unpack('<i', m.group(1))
        singleton_votes[base_va + m.start() + 7 + disp][name] += 1
        # The assert's call, then the call that follows, with the singleton still the first argument.
        first = code.find(b'\xe8', i + 7, i + 24)
        if first < 0:
            continue
        after = first + 5
        call = code.find(b'\xe8', after, after + 56)
        if call < 0 or re.search(rb'\x48\x8d[\x3d\x7c\x7d\xbd]|\x48\x89[\xc7\xd7\xdf\xcf\xf7\xef]|\x4c\x89[\xc7\xcf\xd7\xdf\xe7\xef\xf7\xff]|\x31\xff', code[after:call]):
            continue
        rel, = struct.unpack_from('<i', code, call + 1)
        target = base_va + call + 5 + rel
        if target in functions:
            methods[target][name] += 1
globals_ = {a: v.most_common(1)[0][0] for a, v in singleton_votes.items() if v.most_common(1)[0][1] * 3 >= sum(v.values()) * 2}
print('singleton globals', len(globals_), 'names', len(set(globals_.values())))
# Later calls on the same object: a register that holds the global's address stays in the function,
# and each `mov rdi, [register]` before a call makes the next call one of the class's methods.
MOV_RDI = {b'\x49\x8b\x3e': 'r14', b'\x49\x8b\x3f': 'r15', b'\x48\x8b\x3b': 'rbx', b'\x49\x8b\x3c\x24': 'r12',
           b'\x49\x8b\x7d\x00': 'r13', b'\x48\x8b\x38': 'rax', b'\x48\x8b\x39': 'rcx', b'\x48\x8b\x3a': 'rdx'}
LEA_REG = {(0x4c, 0x35): 'r14', (0x4c, 0x3d): 'r15', (0x48, 0x1d): 'rbx', (0x4c, 0x25): 'r12', (0x4c, 0x2d): 'r13',
           (0x48, 0x05): 'rax', (0x48, 0x0d): 'rcx', (0x48, 0x15): 'rdx'}
FIRST = re.compile(rb'\x48\x8d[\x3d\x7c\x7d\xbd]|\x48\x89[\xc7\xd7\xdf\xcf\xf7\xef]|\x4c\x89[\xc7\xcf\xd7\xdf\xe7\xef\xf7\xff]|\x31\xff')
later = 0
for address, name in globals_.items():
    for site in sites.get(address, ()):
        i = site - base_va
        register = LEA_REG.get((code[i], code[i + 2]))
        f = owner(site)
        if not register or f is None:
            continue
        end = f + functions[f][0] - base_va
        for form, reg in MOV_RDI.items():
            if reg != register:
                continue
            at = code.find(form, i + 7, end)
            while at >= 0:
                after = at + len(form)
                call = code.find(b'\xe8', after, after + 40)
                # The jump over the assert, a load for the assert's message, then the call itself.
                if call >= 0 and not FIRST.search(code[after:call]):
                    rel, = struct.unpack_from('<i', code, call + 1)
                    target = base_va + call + 5 + rel
                    if target in functions and target != 0x20b55b0:
                        methods[target][name] += 1
                        later += 1
                at = code.find(form, after, end)
print('later calls on singletons', later)
found = 0
# One call proves little: a singleton is passed to mutexes and containers too (a sample read by hand
# was mostly wrong). Two calls are asked for, from callers of one class, and a function whose own
# messages name a source file belongs to that file.
FOREIGN = re.compile(r'[\\\\/]([A-Za-z0-9_]+)\.(?:cpp|c|h|inl)$')
def foreign_file(f):
    return any(FOREIGN.search(s) and 'Singleton' not in s for s in by_function.get(f, ()))
for f, votes in methods.items():
    top, count = votes.most_common(1)[0]
    if count >= 2 and count * 5 >= sum(votes.values()) * 4 and not foreign_file(f):
        if f in names and how.get(f) != 'message':
            del names[f]
        found += give(f, f'{top}::method_{f:x}', 'singleton')
print('singleton methods', found, 'classes', len({n.split('::')[0] for f, n in names.items() if how.get(f) == 'singleton'}))

# 5. Wrappers: a function of a few instructions with one callee takes the callee's name.
callees = collections.defaultdict(set)
for r in rows('calls.tsv'):
    callees[int(r[0], 16)].add(int(r[1], 16))
for _ in range(2):
    for f, (size, count) in functions.items():
        if f not in names and count <= 6 and len(callees[f]) == 1:
            target = next(iter(callees[f]))
            if target in names and how[target] != 'wrapper':
                give(f, names[target] + '#wrapper', 'wrapper')

# 6. Helpers: a function whose callers all belong to one class is that class's, repeated while it
# reaches further. Calls are direct ones only, so this is a guess at where a function belongs.
callers = collections.defaultdict(set)
for f, targets in callees.items():
    for target in targets:
        callers[target].add(f)
def class_label(f):
    name = names.get(f, '')
    if '::' in name and name.split('::')[0] and how.get(f) in ('class', 'vtable', 'helper', 'message', 'singleton'):
        return name.split('::')[0]
    return None
for depth in range(8):
    found = 0
    for f in functions:
        if f in names or not callers[f]:
            continue
        labels = {class_label(c) for c in callers[f]}
        if len(labels) == 1 and None not in labels:
            give(f, f'{labels.pop()}::helper_{f:x}', 'helper')
            found += 1
    print('helpers, pass', depth + 1, found)
    if not found:
        break

# 6b. this: a method keeps its first argument in a register and passes it on, so the function it calls
# with that register as the first argument is a method of the same class (or a parent's).
SAVES = {b'\x48\x89\xfb': b'\x48\x89\xdf', b'\x49\x89\xfe': b'\x4c\x89\xf7', b'\x49\x89\xff': b'\x4c\x89\xff',
         b'\x49\x89\xfc': b'\x4c\x89\xe7', b'\x49\x89\xfd': b'\x4c\x89\xef'}
THIS_LABEL = ('class', 'vtable', 'helper', 'singleton', 'this', 'file', 'message')
passes = collections.defaultdict(set)
for f, (size, _) in functions.items():
    i = f - base_va
    end = i + size
    if end > len(code):
        continue
    head = code[i:i + 48]
    for save, load in SAVES.items():
        if head.find(save) < 0:
            continue
        at = code.find(load, i, end)
        while at >= 0:
            after = at + 3
            call = code.find(b'\xe8', after, after + 28)
            if call >= 0 and call + 5 <= end and not FIRST.search(code[after:call]):
                rel, = struct.unpack_from('<i', code, call + 1)
                target = base_va + call + 5 + rel
                if target in functions and target != f:
                    passes[target].add(f)
            at = code.find(load, after, end)
print('this passes', sum(len(v) for v in passes.values()), 'to', len(passes))
for round_ in range(6):
    found = 0
    for target, fromf in passes.items():
        if target in names and not names[target].startswith('shared_'):
            continue
        labels = collections.Counter(names[g].split('::')[0] for g in fromf if g in names and how.get(g) in THIS_LABEL and '::' in names[g])
        if labels:
            top, count = labels.most_common(1)[0]
            if count >= 2 and count * 4 >= sum(labels.values()) * 3 and not foreign_file(target):
                if target in names:
                    del names[target]
                found += give(target, f'{top}::method_{target:x}', 'this')
    print('this, pass', round_ + 1, found)
    if not found:
        break

# 7. Source files: what has no better name takes its file's, and a Class_ label gives way to it.
GENERIC_FILES = re.compile(r'^(?:xstring|xmemory|vector|list|map|xtree|new|string|FD4Singleton|FD4DerivedSingleton|DLFormat)\.', re.I)
for f, source in files.items():
    if GENERIC_FILES.match(source) or not source.endswith(('.cpp', '.c')):
        continue
    stem = tidy(source.rsplit('.', 1)[0])
    if f not in names:
        give(f, f'{stem}::fn_{f:x}', 'file')
    elif names[f].startswith(('Class_', 'shared_')):
        names[f] = f'{stem}::{names[f].split("::")[-1]}'
        how[f] = 'file'

# 8. Neighbours: the linker keeps a source file's functions together, so what lies in a short run
# between two functions of one class is that class's.
ordered_functions = sorted(functions)
def group(f):
    name = names.get(f, '')
    return name.split('::')[0] if '::' in name and how.get(f) != 'shared' else None
for round_ in range(3):
    found = 0
    last_index, last_group = None, None
    for index, f in enumerate(ordered_functions):
        label = group(f)
        if label is None:
            continue
        if last_group == label and 1 < index - last_index <= 8:
            for g in ordered_functions[last_index + 1:index]:
                if g not in names:
                    found += give(g, f'{label}::near_{g:x}', 'near')
        last_index, last_group = index, label
    print('neighbours, pass', round_ + 1, found)
    if not found:
        break

tally = collections.Counter(how.values())
named_target = {f for f in names if modules.get(f, ('', ''))[0] in TARGET}
big = [f for f, (s, c) in functions.items() if c > 5 and modules.get(f, ('', ''))[0] in TARGET]
with open(OUT / 'names.tsv', 'w') as out:
    out.write('address\tname\thow\tlibrary\tsource\n')
    for f in sorted(set(names) | set(files)):
        out.write(f'{f:#x}\t{names.get(f, "")}\t{how.get(f, "")}\t{modules.get(f, ("", ""))[0]}\t{files.get(f, "")}\n')
print('named', len(names), dict(tally))
print('named in FromSoftware or unlabelled code', len(named_target), 'of which longer than 5 instructions', len(named_target & set(big)), 'of', len(big))
print('with a source file', len(files), 'of', len(functions))

