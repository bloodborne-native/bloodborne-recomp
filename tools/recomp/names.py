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
    if len(run) >= 2 and start in sites:
        vtables[start] = run
    if not run:
        i += 1
CLASSY = re.compile(r'^(?:CS|FD4|DL|Sprj|Fnd|MENU|Ez|GX|CG|Frpg|World|Msg|Rend|[A-Z])[A-Za-z0-9_]{3,}$')
GENERIC = {'Exception', 'Allocator', 'Unknown', 'Error', 'Failed', 'String', 'Object', 'Default', 'Invalid'}
class_of, class_methods = {}, collections.Counter()
class_names = {}
# A name in the messages of many classes is one they use (a singleton), not what they are.
spread = collections.defaultdict(set)
for table, run in vtables.items():
    for f in set(run) | ({owner(s) for s in sites[table]} - {None}):
        for s in by_function.get(f, ()):
            if CLASSY.match(s):
                spread[s].add(table)
for table, run in vtables.items():
    ctors = {owner(s) for s in sites[table]} - {None}
    votes = collections.Counter()
    for f in set(run) | ctors:
        if f in files and files[f].endswith(('.cpp', '.c')):
            stem = files[f].rsplit('.', 1)[0]
            if CLASSY.match(stem) and stem not in GENERIC:
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
    class_names[table] = name
    label = name or f'Class_{table:x}'
    for f in ctors:
        give(f, f'{label}::ctor_or_dtor', 'class' if name else 'vtable')
    for index, f in enumerate(run):
        class_of.setdefault(f, table)
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
    if '::' in name and name.split('::')[0] and how.get(f) in ('class', 'vtable', 'helper', 'message'):
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

