#!/usr/bin/env python3
"""tools/recomp/label.py OUTDIR [--check]: which library each function of the game belongs to, from
the tables of tools/recomp/scan.c and tools/recomp/pointers.py (docs/RECOMPILATION.md).

1. Seeds: a function that references a source file path of its own (.cpp or .c: assertion and
   log messages) takes that file and its library. Header paths (.h, .inl) are inlined into the
   callers and only count when the function has no .cpp path.
   Libraries without paths (Scaleform, zlib) are known by their messages (MESSAGES).
2. Neighbours: the linker keeps each library's object files together, so an unlabelled run of
   functions between two functions of the same library takes that library.
3. Vtables and calls, repeated until nothing changes: an unlabelled function takes the library of
   the other functions of its vtable (consecutive pointer slots, pointer_slots.tsv) when they agree,
   or the library of its callers, or of its callees, when all of them have one and it is the same.
   Most functions left after 1 and 2 are small and have no direct caller.
4. Neighbours again, for the runs 3 left between two functions of one library.

--check hides a tenth of the labelled functions, runs 3 again and prints how many of them it gives
back and how many of those match: a measure of 3's agreement with 1 and 2, not of its truth.

Writes OUTDIR/modules.tsv (address, size, module, source file, how it was labelled) and prints the
functions and bytes per module."""
import collections
import os
import random
import re
import sys

# Module of a source path (first match wins). Paths use either slash.
MODULES = [
    ('fmod', re.compile(r'fmod|FMOD', re.I)),
    ('yebis', re.compile(r'yebis|PPFX', re.I)),
    ('havok', re.compile(r'[/\\]hk\w*\.(?:cpp|h|inl)|[/\\]hcl\w*\.|Havok|p4[/\\]Release[/\\]2014_1')),
    ('lua', re.compile(r'Lua|SingleFileVM|[/\\]l\w+\.c$', re.I)),
    ('game', re.compile(r'SPRJ[/\\]Source[/\\]SPRJ[/\\]', re.I)),
    ('fd4', re.compile(r'[/\\]FD4[/\\]|FD4\w*\.(?:cpp|h|inl)', re.I)),
    ('dantelion2', re.compile(r'dantelion2', re.I)),
]
# Libraries without source paths, known by their messages and names.
MESSAGES = [
    ('scaleform', re.compile(r'GFx|Scaleform|scaleform\.gfx|^Error #\d+: ')),
    ('zlib', re.compile(r'invalid distance code|incorrect header check|invalid stored block lengths|'
                        r'too many length or distance symbols|unknown compression method')),
    ('havok', re.compile(r'^St(?:BuildJac|FirePostJacSetup)|^hk[A-Z]')),
    ('fd4', re.compile(r'^FD4[A-Z]\w+$')),
]
# A file's own functions span at most this many MiB of the image.
SPREAD_MIB = 3
SOURCE = re.compile(r'([A-Za-z0-9_./\\:-]+\.(cpp|c|cc|h|hpp|inl))\b')


def module_of(path):
    for name, pattern in MODULES:
        if pattern.search(path):
            return name
    return 'other'


def propagate(label, how, tables, callers, callees):
    """Rule 3 on label and how, in place: returns the number of functions it labelled."""
    in_tables = collections.defaultdict(list)
    for index, table in enumerate(tables):
        for address in table:
            in_tables[address].append(index)

    def agreed(group):
        """The one module of a group of functions, or None (any unlabelled, or several modules)."""
        modules = {label.get(a, 'unknown') for a in group} - {'other'}
        return modules.pop() if len(modules) == 1 and 'unknown' not in modules else None

    total = 0
    candidates = set(in_tables) | set(callers) | set(callees)
    while True:
        table_module = []
        for table in tables:
            modules = {label[a] for a in table if a in label} - {'other'}
            table_module.append(modules.pop() if len(modules) == 1 else None)
        found = {}
        for address in candidates:
            if address in label:
                continue
            votes = {table_module[i] for i in in_tables[address]} - {None}
            by_table = bool(votes)
            for group in (callers[address], callees[address]):
                module = agreed(group) if group else None
                if module:
                    votes.add(module)
            if len(votes) == 1:
                found[address] = (votes.pop(), 'vtable' if by_table else 'calls')
        if not found:
            return total
        for address, (module, kind) in found.items():
            label[address], how[address] = module, kind
        total += len(found)


def main():
    args = [a for a in sys.argv[1:] if a != '--check']
    out = args[0] if args else 'out/recomp'
    functions = []
    for line in open(f'{out}/functions.tsv'):
        if line.startswith('address'):
            continue
        address, size, _, _ = line.split('\t')
        functions.append((int(address, 16), int(size)))
    own, headers, messages = {}, collections.defaultdict(list), collections.defaultdict(list)
    for line in open(f'{out}/strings.tsv', encoding='latin1'):
        if line.startswith('function\t'):
            continue
        function, _, text = line.rstrip('\n').split('\t', 2)
        address = int(function, 16)
        match = SOURCE.search(text)
        if not match:
            for name, pattern in MESSAGES:
                if pattern.search(text):
                    messages[address].append((name, text))
                    break
            continue
        path = match.group(1).replace('\\\\', '\\')
        if match.group(2) in ('cpp', 'c', 'cc'):
            own.setdefault(address, path)
        else:
            headers[address].append(path)

    # A .cpp path referenced from all over the image comes from a macro inlined into callers (the
    # allocator's DLNew.cpp, the mutexes' PthreadMutex.cpp), not from that file's own functions,
    # which the linker keeps together: such paths are not seeds.
    spread = collections.defaultdict(set)
    for address, path in own.items():
        spread[path].add(address >> 20)
    for address in [a for a, p in own.items() if len(spread[p]) > SPREAD_MIB]:
        headers[address].append(own.pop(address))

    message_spread = collections.defaultdict(set)
    for address, found in messages.items():
        for _, text in found:
            message_spread[text].add(address >> 20)

    label, source, how = {}, {}, {}
    for address, _ in functions:
        if address in own:
            label[address], source[address], how[address] = module_of(own[address]), own[address], 'source'
        elif address in messages:
            modules = {m for m, text in messages[address] if len(message_spread[text]) <= SPREAD_MIB}
            if len(modules) == 1:
                label[address], how[address] = modules.pop(), 'message'

    # Neighbours: unlabelled runs between two functions of the same module.
    run = []
    previous = None
    for address, _ in functions:
        if address not in label:
            run.append(address)
            continue
        if previous is not None and run and label[previous] == label[address]:
            for a in run:
                label[a], how[a] = label[address], 'neighbours'
        run, previous = [], address

    # Header paths only for functions still unlabelled, and not spread to neighbours: the engine's
    # templates (FD4Singleton.h, DLFixedVector.inl) are inlined into game code.
    for address, paths in headers.items():
        modules = {module_of(p) for p in paths}
        if address not in label and len(modules) == 1:
            label[address], how[address] = modules.pop(), 'header'

    # Vtables and calls (rule 3).
    entries = {address for address, _ in functions}
    tables = []
    if os.path.exists(f'{out}/pointer_slots.tsv'):
        previous = None
        for line in open(f'{out}/pointer_slots.tsv'):
            if line.startswith('slot'):
                continue
            slot, target = (int(field, 16) for field in line.split('\t'))
            if target not in entries:
                continue
            if previous is not None and slot == previous + 8:
                tables[-1].append(target)
            else:
                tables.append([target])
            previous = slot
    else:
        print(f'no {out}/pointer_slots.tsv (tools/recomp/pointers.py writes it): vtables not used')
    callers, callees = collections.defaultdict(set), collections.defaultdict(set)
    for line in open(f'{out}/calls.tsv'):
        if line.startswith('caller'):
            continue
        caller, callee = (int(field, 16) for field in line.split('\t'))
        if caller in entries and callee in entries and caller != callee:
            callers[callee].add(caller)
            callees[caller].add(callee)
    if '--check' in sys.argv:
        random.seed(1)
        known = sorted(a for a, m in label.items() if m != 'other')
        hidden = set(random.sample(known, len(known) // 10))
        test_label = {a: m for a, m in label.items() if a not in hidden}
        propagate(test_label, {}, tables, callers, callees)
        back = [a for a in hidden if a in test_label]
        same = sum(test_label[a] == label[a] for a in back)
        print(f'check: {len(hidden)} labels hidden, {len(back)} given back, {same} the same '
              f'({100 * same / max(1, len(back)):.1f}%)')
    propagate(label, how, tables, callers, callees)
    # A run with the same library on both sides is that library: 99.7% of hidden runs of up to 20 agree.
    last, run = None, []
    for address, _ in functions:
        if address not in label:
            run.append(address)
            continue
        if run and last == label[address]:
            for a in run:
                label[a], how[a] = last, 'between'
        last, run = label[address], []

    totals = collections.Counter()
    sizes = collections.Counter()
    with open(f'{out}/modules.tsv', 'w') as f:
        f.write('address\tsize\tmodule\tsource\thow\n')
        for address, size in functions:
            module = label.get(address, 'unknown')
            totals[module] += 1
            sizes[module] += size
            f.write(f'{address:#x}\t{size}\t{module}\t{source.get(address, "")}\t{how.get(address, "")}\n')
    all_bytes = sum(sizes.values())
    print(f'{"module":12} {"functions":>9} {"KiB":>8} {"share":>6}')
    for module, count in totals.most_common():
        print(f'{module:12} {count:9} {sizes[module] // 1024:8} {100 * sizes[module] / all_bytes:5.1f}%')
    print('labelled by: ' + ', '.join(f'{k} {v}' for k, v in collections.Counter(how.values()).most_common()))


if __name__ == '__main__':
    main()
