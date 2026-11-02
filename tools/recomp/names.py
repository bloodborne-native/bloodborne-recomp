#!/usr/bin/env python3
"""tools/recomp/names.py OUTDIR: names for the game's functions from what the executable itself says,
without reading any function: messages that give a function's name, source paths, tables of a name and
a code pointer, vtables and wrappers. Writes OUTDIR/names.tsv (address, name, how, library, source
file): game-derived, so it stays with the player's copy. annotations/functions.tsv wins over it, and
holds only what was checked. Prints what each step gave and how much of the target is covered."""
import bisect
import collections
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

OUT = Path(sys.argv[1])
TARGET = {'game', 'dantelion2', 'fd4', 'unknown', ''}


def rows(name):
    return [l.split('\t') for l in (OUT / name).read_text(errors='replace').splitlines()[1:] if l]


functions = {int(r[0], 16): (int(r[1]), int(r[2])) for r in rows('functions.tsv')}
starts = sorted(functions)
modules = {int(r[0], 16): (r[2], r[3] if len(r) > 3 else '') for r in rows('modules.tsv')}
# YEBIS's Direct3D 11 layer over Gnm (rule 23) sits among Havok's code, and is not Havok's.
for f, (library, source) in modules.items():
    if library == 'havok' and (0x11d0000 <= f < 0x11f6000 or 0x2a39000 <= f < 0x2a56000):
        modules[f] = ('d3d11', source)
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

code_text = loads_[0]
IDENT = re.compile(rb'[A-Za-z_][A-Za-z0-9_]{3,60}\0')
# Code ends at the import stubs; what follows in the segment is text and tables.
code_end = max(int(r[0], 16) for r in rows('imports.tsv')) + 16

# 3b. Static initialisers: the executable lists them in one long table (the longest run of pointers to
# code). Each source file that includes <system_error> initialises the same three error categories, in
# order: a set marks an object file, so what lies between two sets is one source file (of 195 stretches
# that name a .cpp, 180 name exactly one). They stay out of what there is to understand.
CATEGORY = re.compile(rb'\x48\x8d\x05.{4}\x80\x38\x00\x75.\x48\x8d\x0d(.{4})\x48\x83\xc1\x10\x48\x8d\x15.{4}\x48\x89\x0a\x48\xc7\x00\x01\0\0\0\xc3', re.S)
ordered_slots = sorted(slots)
longest = current = (0, 0)
for address in ordered_slots:
    current = (current[0], current[1] + 1) if address == current[0] + current[1] * 8 and slots[address] in functions else (address, 1 if slots[address] in functions else 0)
    if current[1] > longest[1]:
        longest = current
static_inits = {slots[longest[0] + i * 8] for i in range(longest[1])}
categories = collections.Counter()
for m in CATEGORY.finditer(code_, 0, code_end - code_text[3]):
    table_at = m.start(1) + 4 + struct.unpack('<i', m.group(1))[0]
    categories[table_at] += 1
    f = m.start() + code_text[3]
    if f in static_inits:
        give(f, f'std::_Error_objects::init_{table_at:x}', 'static')
for f in static_inits:
    give(f, f'staticInit_{f:x}', 'static')
print('static initialisers', len(static_inits), 'error categories', sum(categories.values()), 'source files', max(categories.values(), default=0))
unit_starts = []
last_category = 0
for m in CATEGORY.finditer(code_, 0, code_end - code_text[3]):
    f = m.start() + code_text[3]
    if f - last_category > 0x30:
        unit_starts.append(f)
    last_category = f
print('object files', len(unit_starts))

# 3a. Assertions: YEBIS passes the failed text, with its file, and the function's name to the C library's
# assert, so a function that asserts says what it is.
ASSERT_AT = {int(a, 16) for a, name in rows('imports.tsv') if name == '_Assert'}
TEXT_AT = re.compile(rb'[\x20-\x7e]{1,300}\0')
ARGUMENT = re.compile(rb'\x48\x8d([\x3d\x35])(.{4})', re.S)
asserted = collections.defaultdict(collections.Counter)
for m in re.finditer(rb'\xe8(.{4})', code_[:code_end - code_text[3]], re.S):
    if m.start(1) + 4 + struct.unpack('<i', m.group(1))[0] + code_text[3] not in ASSERT_AT:
        continue
    texts = {}
    for argument in ARGUMENT.finditer(code_, max(0, m.start() - 48), m.start()):
        texts[argument.group(1)] = TEXT_AT.match(code_, argument.start(2) + 4 + struct.unpack('<i', argument.group(2))[0])
    where, what = texts.get(b'\x3d'), texts.get(b'\x35')
    f = owner(m.start() + code_text[3])
    if f is not None and where and what:
        file_ = re.search(rb'([A-Za-z0-9_]+)\.(?:cpp|h)\(', where.group(0))
        asserted[f][(file_.group(1).decode() if file_ else None, what.group(0)[:-1].decode())] += 1
for f, seen in asserted.items():
    (file_, function), _ = seen.most_common(1)[0]
    if re.fullmatch(r'[A-Za-z_~][A-Za-z0-9_:]*', function):
        give(f, f'{file_}::{function}' if file_ and '::' not in function else function, 'assert')
print('named by assertions', sum(1 for h_ in how.values() if h_ == 'assert'))

# 3c. Step tables: state machines fill a table of a function, a zero and its name in wide text, the whole
# Class::STEP_name, in code. The pair of loads stores into the table.
WIDE = re.compile(rb'(?:[A-Za-z0-9_:<> ]\0){4,120}\0\0')
wide_names = {m.start(): m.group(0)[:-2].decode('utf-16-le') for m in WIDE.finditer(code_) if b':\0:\0' in m.group(0)}
STORE = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})[\x48\x49\x4c\x4d]\x89', re.S)
last_store = None
steps = collections.defaultdict(set)
for m in STORE.finditer(code_):
    target = m.start() + 7 + struct.unpack('<i', m.group(1))[0]
    if target in wide_names and last_store and m.start() - last_store[0] < 40:
        fn_at = last_store[1] + code_text[3]
        if fn_at in functions and fn_at < code_end:
            steps[wide_names[target]].add(fn_at)
    last_store = (m.start(), target)
named_steps = 0
for name, fs in steps.items():
    if len(fs) == 1 and not name.endswith('::THIS_CLASS'):
        named_steps += give(next(iter(fs)), name, 'step')
print('step tables', named_steps)

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

# 4a. DLRuntimeClass: the engine's own type information. A class's object is created once, behind a pointer
# in a global: code tests the pointer and skips creation if it is set, then stores two text pointers at
# +0x40 and +0x48 of the object and links it to its parent's. The class's vtable reads the same global in
# its first slot, and the object's own vtable in its tenth, which ties vtables to names.
text0 = loads[0]
code0 = image[text0[2]:text0[2] + text0[5]]
RUNTIME = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})[\x48\x49\x4c\x4d]\x89[\x40-\x7f]\x40', re.S)
SECOND = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})[\x48\x49\x4c\x4d]\x89[\x40-\x7f]\x48', re.S)
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
runtime_sites = []
for m in RUNTIME.finditer(code0):
    i = m.start()
    if SECOND.search(code0, i + 11, i + 40):
        first = wide_or_text(text0[3] + i + 7 + struct.unpack('<i', m.group(1))[0])
        if first:
            runtime_sites.append((i, first))
LOADED = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', re.S)
def pointer_loaded(low, high):
    # lea reg, [global]; mov reg, [reg]: the last before the test.
    best = None
    for m in LOADED.finditer(code0, low, high):
        target = m.start() + 7 + struct.unpack('<i', m.group(1))[0] + text0[3]
        if code0[m.end():m.end() + 2] in (b'\x49\x8b', b'\x48\x8b') and target >= loads[1][3]:
            best = target
    return best
def pointer_tested(site, low):
    # The first branch to the site tests the pointer, read directly or through a register.
    for p in range(max(low, site - 0x400), site):
        if code0[p:p + 2] == b'\x0f\x85' and p + 6 + struct.unpack_from('<i', code0, p + 2)[0] == site:
            near_jump = p
        elif code0[p] == 0x75 and p + 2 + struct.unpack_from('<b', code0, p + 1)[0] == site:
            near_jump = p
        else:
            continue
        q = near_jump - 3
        if code0[q - 7] in (0x48, 0x4c) and code0[q - 6] == 0x8b and code0[q - 5] & 0xc7 == 5:
            return q + struct.unpack_from('<i', code0, q - 4)[0] + text0[3]
        return pointer_loaded(low, near_jump)
    return None
runtime_globals = {}
runtime_getters = {}
previous = 0
for i, name_ in runtime_sites:
    low = max(order[bisect.bisect_right(order, i + text0[3]) - 1] - text0[3], previous)
    previous = i
    pointer = pointer_tested(i, low)
    if not pointer:
        # Out of line: the site calls a getter of its own.
        for c in range(i - 5, max(low, i - 40) - 1, -1):
            if code0[c] == 0xe8:
                t = c + 5 + struct.unpack_from('<i', code0, c + 1)[0] + text0[3]
                if t in functions and functions[t][0] < 1024:
                    body = (t - text0[3], t - text0[3] + functions[t][0])
                    read = re.compile(rb'\x48\x8b\x05(.{4})\x48\x85\xc0', re.S).search(code0, *body)
                    pointer = read.start() + 7 + struct.unpack('<i', read.group(1))[0] + text0[3] if read else pointer_loaded(*body)
                    runtime_getters[t] = name_
                break
    if pointer:
        runtime_globals[pointer] = name_
print('DLRuntimeClass classes', len(runtime_sites), 'with their pointer', len(runtime_globals))
pointer_readers = collections.defaultdict(set)
for m in re.finditer(rb'[\x48\x4c][\x8b\x8d][\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', code0, re.S):
    target = m.start() + 7 + struct.unpack('<i', m.group(1))[0] + text0[3]
    if target in runtime_globals:
        pointer_readers[order[bisect.bisect_right(order, m.start() + text0[3]) - 1]].add(runtime_globals[target])
for getter, name_ in runtime_getters.items():
    pointer_readers[getter].add(name_)

# 4a2. Method registry: a class registers a method by calling slot 11 of its class object (+0x58) with an
# invoker and the name. The invoker holds the function, stored just before; a getter called just before
# gives the class. These are the functions event and AI scripts call.
pairs = collections.defaultdict(set)
invoker_tables = set()
# lea rax, [function]; mov [global + 8], rax; ...; call getter; ...; lea rdx, [name]; lea rcx, [text]; ...; call [reg + 0x58]
NAMED = re.compile(rb'\x48\x8d\x15(.{4})\x48\x8d\x0d', re.S)
STORED = re.compile(rb'\x48\x8d\x05(.{4})\x48\x89\x05', re.S)
REGISTERED = re.compile(rb'\xff[\x50-\x57]\x58')
for m in NAMED.finditer(code_):
    name_at = m.start(1) + 4 + struct.unpack('<i', m.group(1))[0]
    hit = IDENT.match(code_, name_at) if 0 <= name_at < len(code_) else None
    if not hit:
        continue
    low = max(0, m.start() - 96)
    start = max([low] + [c.end() for c in REGISTERED.finditer(code_, low, m.start())])
    stores = [s_.start() + 7 + struct.unpack('<i', s_.group(1))[0] + code_text[3] for s_ in STORED.finditer(code_, start, m.start())]
    invoker_tables.update(a for a in stores if a not in functions)
    stores = [a for a in stores if a in functions and a < code_end]
    if not stores:
        continue
    owner_class = None
    for c in range(m.start() - 5, start - 1, -1):
        if code_[c] == 0xe8:
            readers = pointer_readers.get(c + 5 + struct.unpack_from('<i', code_, c + 1)[0] + code_text[3], ())
            if len(readers) == 1:
                owner_class = next(iter(readers))
                break
    pairs[(owner_class, hit.group(0)[:-1].decode())].add(stores[-1])
bound = with_class = 0
for (owner_class, name), fs in pairs.items():
    if len(fs) == 1 and give(next(iter(fs)), f'{owner_class}::{name}' if owner_class else name, 'binding'):
        bound += 1
        with_class += owner_class is not None
print('registered methods', bound, 'with their class', with_class)

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
# The first slot reads the pointer, or jumps to a getter that does.
runtime_tables = {}
runtime_objects = {}
for table, run in vtables.items():
    f = run[0]
    at = f - text0[3]
    readers = pointer_readers.get(f, set())
    if code0[at] == 0xe9:
        readers = readers | pointer_readers.get(f + 5 + struct.unpack_from('<i', code0, at + 1)[0], set())
    if len(readers) == 1:
        runtime_tables[table] = next(iter(readers))
    if len(run) > 9 and len(pointer_readers.get(run[9], ())) == 1:
        runtime_objects[table] = next(iter(pointer_readers[run[9]]))
print('instance vtables named by DLRuntimeClass', len(runtime_tables), 'classes', len(set(runtime_tables.values())),
      'class objects', len(runtime_objects))
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
# Classes the executable does not name, named by reading their code.
hand_classes = {}
for line in (Path(__file__).resolve().parents[2] / 'annotations' / 'classes.tsv').read_text().splitlines():
    if line and not line.startswith('#'):
        hand_classes[int(line.split('\t')[0], 16)] = line.split('\t')[1]
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
# Debug pages: a destructor that removes a page labels it in wide text, usually with its class's name, but
# sometimes with a parent page or a topic, so a label must be one word in mixed case, of 8 letters or more,
# said by one class only, and the only page its destructor removes (one that removes several owns them as a
# group). Not constructors: what adds a page also builds small menu callbacks that are
# not the class (a sample read by hand).
DEBUG_MENU = {0xfa3260, 0xfa33a0, 0xfa33c0}
ANY_LABEL = re.compile(rb'(?:[\x20-\x7e]\0){3,60}\0\0')
PAGE_LABEL = re.compile(rb'(?:[A-Z]\0)(?:[a-z]\0)(?:[A-Za-z0-9]\0){6,48}\0\0')
page_callers = {int(r[0], 16) for r in rows('calls.tsv') if int(r[1], 16) in DEBUG_MENU}
page_labels = collections.defaultdict(set)
for table, run in vtables.items():
    for f in set(run[:2]) & page_callers:
        at = f - text[3]
        for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', code[at:at + functions[f][0]], re.S):
            target = f + m.end() + struct.unpack('<i', m.group(1))[0]
            for p_ in loads:
                if p_[3] <= target < p_[3] + p_[5]:
                    hit = ANY_LABEL.match(image, p_[2] + target - p_[3])
                    if hit:
                        page_labels[table].add(hit.group(0)[:-2].decode('utf-16-le'))
                    break
page_said = collections.Counter(l for labels in page_labels.values() for l in labels)
debug_pages = {}
for table, labels in page_labels.items():
    label = next(iter(labels))
    if len(labels) == 1 and page_said[label] == 1 and PAGE_LABEL.fullmatch(label.encode('utf-16-le') + b'\0\0'):
        debug_pages[table] = label
print('classes named by debug pages', len(debug_pages))
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
    name = name or debug_pages.get(table)
    if table in runtime_tables:
        name = runtime_tables[table]
        how_named[table] = 'runtime'
    elif table in runtime_objects:
        name = runtime_objects[table] + '::DLRuntimeClass'
        how_named[table] = 'runtime'
    name = hand_classes.get(table, name)
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
print('classes named', sum(1 for n in class_names.values() if n))
shared = collections.Counter(f for run in vtables.values() for f in set(run))
for table, run in vtables.items():
    label = class_names[table] or f'Class_{table:x}'
    for index, f in enumerate(run):
        if shared[f] == 1:
            give(f, f'{label}::vf{index}', 'class' if class_names[table] else 'vtable')
# A slot several classes share is inherited: it belongs to the class they all descend from.
holders = collections.defaultdict(list)
for table, run in vtables.items():
    for index, f in enumerate(run):
        holders[f].append((table, index))
def ancestors(table):
    seen = set()
    while table in parents and table not in seen:
        seen.add(table)
        table = parents[table]
        yield table
inherited = 0
for f, count in shared.items():
    if count > 1 and f in class_of:
        tables = {t for t, _ in holders[f]}
        tops = [t for t in tables if not tables & set(ancestors(t))]
        if len(tops) == 1 and len(tables) > 1:
            index = next(i for t, i in holders[f] if t == tops[0])
            inherited += give(f, f'{class_names[tops[0]] or f"Class_{tops[0]:x}"}::vf{index}', 'class' if class_names[tops[0]] else 'vtable')
        else:
            give(f, f'shared_{class_of[f]:x}', 'shared')
print('inherited slots', inherited)

# 4b2. Slots that mean the same in every class. A virtual function that writes its own class's vtable is
# its destructor, and it calls operator delete if it is the deleting one. The write must go to the object
# itself: one writing to its second argument is a clone. A class object's slots answer
# its name, label and type tags, destroy an instance and give its size.
OPERATOR_DELETE = {int(a, 16) for a, name in rows('imports.tsv') if name == '_ZdlPv'}
OBJECT_SLOTS = {2: 'getName', 3: 'getLabel', 4: 'typeTag1', 5: 'typeTag2', 6: 'typeTag3', 7: 'typeTag4',
                9: 'destroyInstance', 10: 'instanceSize'}
VTABLE_STORE = re.compile(rb'\x48\x8d[\x05\x0d\x15\x1d\x35\x3d](.{4})(?:\x48\x83[\xc0-\xc7]\x10|\x48\x8d[\x40-\x47]\x10)?([\x48\x49])\x89([\x00-\x07])', re.S)
def writes_itself(f, table):
    start = f - text0[3]
    end = start + functions[f][0]
    for m in VTABLE_STORE.finditer(code0, start, end):
        target = m.start() + 7 + struct.unpack('<i', m.group(1))[0] + text0[3]
        if target not in (table, table - 16):
            continue
        base = m.group(3)[0] | (8 if m.group(2) == b'\x49' else 0)
        if base == 7:  # rdi
            return True
        if base == 6:  # rsi
            return False
        # mov base, rdi
        move = bytes([0x48 | (base >> 3), 0x89, 0xf8 | (base & 7)])
        return code0.find(move, start, m.start()) >= 0
    return False
meaning = function_tables = 0
sites_writers = collections.defaultdict(set)
for f, tables in writers.items():
    for table in tables:
        sites_writers[table].add(f)
def rename(f, name):
    global meaning
    if f in names and (names[f].startswith('shared_') or re.search(r'^Class_.*::(?:vf\d+|writes_vtable)$|::(?:vf\d+|writes_vtable)$', names[f])):
        names[f] = name
        meaning += 1
for table, run in vtables.items():
    label = class_names[table] or f'Class_{table:x}'
    if table in runtime_tables:
        rename(run[0], f'{label}::getRuntimeClass')
    if table in runtime_objects:
        for index, slot in OBJECT_SLOTS.items():
            if index < len(run):
                rename(run[index], f'{label}::{slot}')
    for f in run:
        if table in writers.get(f, ()) and writes_itself(f, table):
            rename(f, f'{label}::{"deletingDestructor" if direct[f] & OPERATOR_DELETE else "destructor"}')
# Recipes the compiler repeats. A deleting destructor that calls a function and then operator delete has
# the destructor as that function. std::function's implementations hold, in order, copy, move, the call,
# the target's type and delete-this, which calls slot 5 and then operator delete if asked to. A class
# object's getter is the function that writes its vtable. Method invokers are templates by signature.
FUNCTION_SLOTS = ('_Copy', '_Move', '_Do_call', '_Target_type', '_Delete_this', 'destructor', 'deletingDestructor', '_Get')
INVOKER_SLOTS = {2: 'deletingDestructor', 3: 'argumentCount', 4: 'argumentTypes'}
DELETE_THIS = re.compile(rb'\xff[\x50-\x57\x60-\x67]\x28')
for table, run in vtables.items():
    label = class_names[table] or f'Class_{table:x}'
    for f in run:
        if functions[f][1] <= 12 and len(direct[f]) == 2 and direct[f] & OPERATOR_DELETE:
            other = next(iter(direct[f] - OPERATOR_DELETE))
            rename(f, f'{label}::deletingDestructor')
            if other in run:
                rename(other, f'{label}::destructor')
    if len(run) in (7, 8) and direct[run[4]] & OPERATOR_DELETE and functions[run[4]][1] <= 20 and \
            DELETE_THIS.search(code0, run[4] - text0[3], run[4] - text0[3] + functions[run[4]][0]):
        for f, slot in zip(run, FUNCTION_SLOTS):
            rename(f, f'std::_Func_impl_{table:x}::{slot}')
        function_tables += 1
    if table in runtime_objects:
        for f in sites_writers.get(table, ()):
            rename(f, f'{runtime_objects[table]}::getClassObject')
    if table in invoker_tables or table - 16 in invoker_tables:
        for index, f in enumerate(run):
            rename(f, f'DLMethodInvoker_{table:x}::{INVOKER_SLOTS.get(index, f"vf{index}")}')
pushes = 0
EZSTATE_OPS = {128: 'pushFloat', 129: 'pushDouble', 130: 'pushInt32', 131: 'loadVariable', 132: 'callFunction0',
               133: 'callFunction1', 134: 'callFunction2', 135: 'callFunction3', 136: 'callFunction4', 137: 'callFunction5',
               138: 'callFunction6', 139: 'callFunction7', 140: 'add', 141: 'negate', 142: 'subtract',
               143: 'multiply', 144: 'divide', 146: 'greater', 149: 'equal', 150: 'notEqual', 155: 'lessThanZero', 160: 'notZero'}
# EzState's expression evaluator: two tables of opcode handlers, on a stack of tagged values (int, float,
# object) at +0x480. EzStateEventImpl runs each event argument through it.
for table in (0x52c8e60, 0x52c8880):
    for index, f in enumerate(vtables.get(table, ())):
        # Opcodes 0 to 127 push their value less 64. The value is the last immediate store in the handler.
        stored = re.findall(rb'\xc7[\x00-\x07](.{4})', code0[f - text0[3]:f - text0[3] + min(functions[f][0], 260)], re.S)
        small = index < 128 and any(struct.unpack('<i', v)[0] == index - 64 for v in stored)
        pushes += small
        rename(f, f'EzStateExpression_{table:x}::' + (f'pushInt{index - 64}' if small else EZSTATE_OPS.get(index, f'op{index}')))
print('slots with a meaning', meaning, 'std::function implementations', function_tables, 'EzState pushes', pushes)
instance_sizes = {}
for table, name_ in runtime_objects.items():
    at = vtables[table][10] - text0[3]
    if code0[at] == 0xb8 and code0[at + 5] == 0xc3:
        instance_sizes[name_] = struct.unpack_from('<I', code0, at + 1)[0]
print('instance sizes', len(instance_sizes))
with open(OUT / 'classes.tsv', 'w') as out:
    out.write('vtable\tname\tslots\tparent\tchildren\tsize\n')
    for table, run in sorted(vtables.items()):
        out.write(f"{table:#x}\t{class_names.get(table) or ''}\t{len(run)}\t{parents.get(table, 0):#x}\t{children[table]}\t{instance_sizes.get(class_names.get(table), '')}\n")

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
# reaches further. Calls are direct ones only, so this is a guess at where a function belongs: where a
# function's source file names a class, only 47 of 207 helpers agree, against 561 of 587 vtable methods;
# of functions of known class, 43% have callers of one class that agree (54% with two callers or more).
callers = collections.defaultdict(set)
for f, targets in callees.items():
    for target in targets:
        callers[target].add(f)
def class_label(f):
    name = names.get(f, '')
    if '::' in name and name.split('::')[0] and how.get(f) in ('class', 'vtable', 'helper', 'message', 'singleton', 'step'):
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

# 8. Neighbours: the linker keeps a source file's functions together, so what lies in a run between two
# functions of one class is that class's. Holding out functions of known class, 97% of runs of 9 to 64
# agree.
ordered_functions = sorted(functions)
def group(f):
    name = names.get(f, '')
    return name.split('::')[0] if '::' in name and how.get(f) not in ('shared', 'static') else None
for round_ in range(3):
    found = 0
    last_index, last_group = None, None
    for index, f in enumerate(ordered_functions):
        label = group(f)
        if label is None:
            continue
        if last_group == label and 1 < index - last_index <= 64:
            for g in ordered_functions[last_index + 1:index]:
                if g not in names:
                    found += give(g, f'{label}::near_{g:x}', 'near')
        last_index, last_group = index, label
    print('neighbours, pass', round_ + 1, found)
    if not found:
        break

# 8b. Shapes. A short function that tests a pointer-to-member's low bit and calls it is a delegate's call
# (in FromSoftware's code: middleware has forwarders of that form).
# Template methods compile to the same bytes but for addresses, so one read example names its group.
MEMBER_CALL = re.compile(rb'(?:\xf6[\xc0-\xc3]\x01|\xa8\x01)\x74.(?:[\x48\x49]\x8b.)?[\x48\x4c]\x8b[\x44\x4c\x54\x5c\x7c].{1}\xff', re.S)
MASK = re.compile(rb'\xe8.{4}|\xe9.{4}|[\x48\x4c\x49\x4d]?[\x8b\x8d\x89\x83\xc7\x3b\x39][\x05\x0d\x15\x1d\x25\x2d\x35\x3d].{4}', re.S)
EXAMPLES = {0x12a5050: 'push_back', 0x12b7550: 'treeErase', 0x12afa40: 'destroyInstance', 0x12f1c70: 'treeErase',
            0x12b7f20: 'treeInsertNode', 0xf85d60: 'getClassObject', 0x12226b0: 'addDebugItem', 0x144ea40: 'addDebugToggle',
            0x144e810: 'addDebugButton', 0x142ebe0: 'removeDebugPage', 0x122ef60: 'addDebugItem', 0x12dfa60: 'addDebugItem',
            0x1470f60: 'removeDebugPage', 0xfce990: 'eraseAndDelete', 0xff6f90: 'removeDebugEntry', 0x2007a70: 'setLogField',
            0x2008570: 'setLogField', 0x13c3d70: 'treeEraseRange', 0x12b75b0: 'treeEraseRange', 0x20ed680: 'treeFindString',
            0x1d445b0: 'findResourceByName', 0xfce860: 'findResource', 0x20088a0: 'setLogField', 0x20093c0: 'setLogField',
            0x2009260: 'setLogField', 0x2008740: 'setLogField', 0x1ac8640: 'makeResourcePath', 0x1ac8730: 'makeResourcePath',
            0x1fb2e60: 'callRemote', 0x1fb2be0: 'callRemote', 0x1fb3880: 'callRemote', 0x127afc0: 'lazyStaticList', 0x1d813f0: 'findTextureHandle',
            0x137d550: 'lazyStaticList'}
def wide_text_of(f):
    at = f - text0[3]
    for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', code0[at:at + functions[f][0]], re.S):
        target = f + m.end() + struct.unpack('<i', m.group(1))[0]
        for p_ in loads:
            if p_[3] <= target < p_[3] + p_[5]:
                hit = re.compile(rb'(?:[\x20-\x7e]\0){3,80}\0\0').match(image, p_[2] + target - p_[3])
                if hit:
                    return hit.group(0)[:-2].decode('utf-16-le')
    return None
def body_shape(f):
    body = code0[f - text0[3]:f - text0[3] + functions[f][0]]
    return MASK.sub(lambda m: m.group(0)[:-4] + b'\0\0\0\0', body)
example_shapes = {body_shape(f): name for f, name in EXAMPLES.items()}
shaped = collections.Counter()
for f, (size, count) in functions.items():
    if f >= code_end or f in static_inits or count < 6:
        continue
    known = how.get(f) in ('message', 'table', 'binding', 'step', 'singleton', 'static')
    name = names.get(f, '')
    if known or re.search(r'::[a-z_A-Z]*[Dd]estructor$|::_|getClassObject', name):
        continue
    what = None
    if f >= unit_starts[0] and count <= 60 and MEMBER_CALL.search(code0, f - text0[3], f - text0[3] + size):
        what = 'delegateCall'
    else:
        what = example_shapes.get(body_shape(f))
    if what:
        owner_name = name.split('::')[0] if '::' in name and not name.startswith(('Class_', 'shared_')) else 'Template'
        said = wide_text_of(f) if what in ('removeDebugEntry', 'setLogField', 'makeResourcePath', 'callRemote') else None
        if said and what == 'removeDebugEntry':  # the label is the class's name, as in a destructor
            owner_name = said
        elif said:
            what = f"{what}_{re.sub(r'[^A-Za-z0-9]+', '_', said).strip('_')}"
        names[f] = f'{owner_name}::{what}_{f:x}'
        how[f] = 'shape'
        shaped[what] += 1
print('shapes', dict(shaped))

# 8c. Thin callers: a registered method of a few instructions with one real callee, which no other method
# calls, is that callee's front: the callee does what the name says. Not steps, whose callees are generic.
fronts = collections.defaultdict(list)
for f in functions:
    if how.get(f) == 'binding' and functions[f][1] <= 40:
        real = [c for c in direct[f] if c in functions and functions[c][1] > 3 and c not in ASSERT_AT and c != 0x20b55b0 and c < code_end]
        if len(real) == 1:
            fronts[real[0]].append(f)
behind = 0
for callee, callers_ in fronts.items():
    if len(callers_) == 1 and how.get(callee) not in ('binding', 'step', 'message', 'table', 'singleton', 'static', 'shape', 'assert'):
        name = names.get(callee, '')
        if not name or re.search(r'::(?:helper_|near_|unit_|fn_|method_|vf\d+$)', name) or name.startswith(('Class_', 'shared_')):
            names[callee] = f'{names[callers_[0]]}#impl'
            how[callee] = 'front'
            behind += 1
print('behind a named front', behind)

# 9. Object files: in one where every class-named function has one class, the rest belong with it. Holding
# out functions of known class this agrees 98.4%, against 91.6% for taking the class of most of them.
# A stretch without a marker can be many object files: 13,874 functions in one, whose only .cpp named a
# Havok file over FromSoftware's debug menu code. Stretches of more than 500 are not one file.
unit_sizes = collections.Counter(bisect.bisect_right(unit_starts, f) - 1 for f in functions if f >= unit_starts[0])
HUGE_UNIT = 500
unit_classes = collections.defaultdict(collections.Counter)
for f, name in names.items():
    # The channels' builder sits in another file: that file is the bank's, not a channel's.
    if how[f] in ('class', 'binding', 'step') and not name.startswith(('Class_', 'GraphicsParamChannel_')) and f >= unit_starts[0]:
        unit_classes[bisect.bisect_right(unit_starts, f) - 1][name.split('::')[0]] += 1
# A class that is the only one in five or more object files is a base they share (read by hand: code of
# other systems), not what they hold.
class_spread = collections.Counter(c for classes in unit_classes.values() for c in classes)
placed_in_unit = 0
for f in functions:
    if f < unit_starts[0] or f >= code_end or f in static_inits:
        continue
    unit = bisect.bisect_right(unit_starts, f) - 1
    found = unit_classes.get(unit)
    if not found or len(found) != 1 or unit_sizes[unit] > HUGE_UNIT:
        continue
    only = next(iter(found))
    if class_spread[only] >= 5:
        continue
    if f not in names:
        placed_in_unit += give(f, f'{only}::unit_{f:x}', 'unit')
    elif names[f].startswith(('Class_', 'shared_')):
        names[f] = f'{only}::{names[f]}'
        how[f] = 'unit'
        placed_in_unit += 1
print('placed by object file', placed_in_unit)
# An object file that names one .cpp is that file, whatever classes it holds. Header paths do not count.
unit_files = collections.defaultdict(set)
for f, source in files.items():
    if f >= unit_starts[0] and f < code_end and source.endswith('.cpp') and not GENERIC_FILES.match(source):
        unit_files[bisect.bisect_right(unit_starts, f) - 1].add(source)
placed_by_file = 0
for f in functions:
    if f < unit_starts[0] or f >= code_end or f in static_inits:
        continue
    unit = bisect.bisect_right(unit_starts, f) - 1
    found = unit_files.get(unit)
    if not found or len(found) != 1 or unit_sizes[unit] > HUGE_UNIT:
        continue
    stem = tidy(next(iter(found)).rsplit('.', 1)[0])
    if f not in names:
        placed_by_file += give(f, f'{stem}::fn_{f:x}', 'unitfile')
    elif names[f].startswith(('Class_', 'shared_')):
        names[f] = f'{stem}::{names[f]}'
        how[f] = 'unitfile'
        placed_by_file += 1
print('placed by a file of the object file', placed_by_file)
with open(OUT / 'units.tsv', 'w') as out:
    out.write('start\tend\tfunctions\tclasses\n')
    for index, start in enumerate(unit_starts):
        end = unit_starts[index + 1] if index + 1 < len(unit_starts) else code_end
        count = bisect.bisect_left(order, end) - bisect.bisect_left(order, start)
        out.write(f"{start:#x}\t{end:#x}\t{count}\t{' '.join(unit_classes.get(index, ()))}\n")

# 9d. NexusRevolution2, Dantelion's network library: its thread names and source paths lie in this range, so
# what has no class of its own there is the library's.
nexus = 0
for f in functions:
    if 0xc83000 <= f < 0xcdd030 and modules.get(f, ('', ''))[0] == 'dantelion2':
        name = names.get(f, '')
        if not name or name.startswith(('Class_', 'shared_')):
            names[f] = f'NexusRevolution::{name or f"fn_{f:x}"}'
            how[f] = 'unit'
            nexus += 1
print('NexusRevolution', nexus)

# 9e. Dantelion's source directories: the functions between two that name a .cpp of one directory, within 200
# functions, are that directory's (held out, 91% of the functions between two of one directory agree).
DIRECTORY = re.compile(r'(?:Dantelion2|dantelion2_sprj)\\+(?:dist_ps4\\+)?source\\+(?:ClassLibrary|ApplicationLibrary)\\+(.*?)\\+[Ss]ource(?:\\.*)?\\+[^\\]+\.cpp$', re.I)
GAME_DIRECTORY = re.compile(r'\\+Source\\+SPRJ\\+Source\\+(.*?)\\+[^\\]+\.cpp$', re.I)
# A .cpp that many functions name is inlined (DLNew.cpp's message): it says nothing about where they lie.
named_by = collections.Counter(s_ for said in by_function.values() for s_ in set(said) if s_.endswith('.cpp'))
directories = {}
for f, said in by_function.items():
    if modules.get(f, ('', ''))[0] not in ('dantelion2', 'game', 'fd4'):
        continue
    for s_ in said:
        m = DIRECTORY.search(s_) or GAME_DIRECTORY.search(s_)
        if m and named_by[s_] <= 15:
            directories[f] = re.sub(r'\\+', '_', m.group(1))
            break
seeds = sorted(f for f in directories if f in functions)
place = {f: i for i, f in enumerate(ordered_functions)}
in_directory = 0
for a_, b_ in zip(seeds, seeds[1:]):
    if directories[a_] == directories[b_] and place[b_] - place[a_] <= 200:
        for g in ordered_functions[place[a_] + 1:place[b_]]:
            name = names.get(g, '')
            if modules.get(g, ('', ''))[0] == modules.get(a_, ('', ''))[0] and (not name or name.startswith(('Class_', 'shared_')) or how.get(g) == 'helper'):
                names[g] = f"{'DL' if modules.get(a_, ('', ''))[0] == 'dantelion2' else 'SPRJ'}_{directories[a_]}::{name.split('::')[-1] if name else f'fn_{g:x}'}"
                how[g] = 'unit'
                in_directory += 1
print('in a Dantelion directory', in_directory)

# 9f. Wrappers of a system call: a short function whose only call is one SDK import, or the standard library's
# failure, says what it is. Memory and string calls and delete are left out: they say nothing.
imports_ = {int(r[0], 16): r[1] for r in rows('imports.tsv')}
wrappers = 0
for f, targets in callees.items():
    if f in imports_ or f not in functions or functions[f][1] > 60 or (names.get(f) and how.get(f) not in ('near', 'unit', 'unitfile', 'helper', 'vtable')):
        continue
    called = {imports_[t] for t in targets if t in imports_} - {'__stack_chk_fail'}
    if len(called) == 1 and all(t in imports_ for t in targets):
        import_ = next(iter(called))
        role = {'_ZSt14_Xlength_errorPKc': 'std::_Xlength', '_ZSt14_Xout_of_rangePKc': 'std::_Xran'}.get(import_)
        if not role and not import_.startswith('sce'):
            continue
        names[f] = f"{role or 'SDK::' + import_}_{f:x}"
        how[f] = 'shape'
        wrappers += 1
print('wrappers of a system call', wrappers)

# 10. What was named by hand (annotations/functions.tsv) wins, and is applied again after the last rule.
hand = {}
for line in (Path(__file__).resolve().parents[2] / 'annotations' / 'functions.tsv').read_text().splitlines():
    if line and not line.startswith('#'):
        f = int(line.split('\t')[0], 16)
        hand[f] = line.split('\t')[1]
        names[f], how[f] = hand[f], 'hand'

big = [f for f, (s, c) in functions.items() if c > 5 and modules.get(f, ('', ''))[0] in TARGET and f not in static_inits]
# 8c2. Settings: a short function that reads one setting says its dotted name in wide text.
SETTING = re.compile(rb'(?:[A-Z]\0)(?:[A-Za-z0-9_]\0)+(?:\.\0(?:[A-Za-z0-9_]\0)+)+\0\0')
settings = 0
for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', code, re.S):
    target = text[3] + m.end() + struct.unpack('<i', m.group(1))[0]
    f = owner(text[3] + m.start())
    if f is None or functions[f][1] > 60 or how.get(f) in ('step', 'binding', 'message'):
        continue
    for p_ in loads:
        if p_[3] <= target < p_[3] + p_[5]:
            hit = SETTING.match(image, p_[2] + target - p_[3])
            if hit:
                names[f] = 'Setting::' + hit.group(0)[:-2].decode('utf-16-le').replace('.', '_')
                how[f] = 'shape'
                settings += 1
            break
print('settings', settings)

# 8d. The standard library says what it is when it fails, and a short function that says only that is its
# template. Longer ones have the template inlined in real work (samples read by hand), so each has a limit.
STD_TEXT = {'vector<T> too long': 'vector::_Xlen', 'deque<T> too long': 'deque::_Xlen', 'string too long': 'string::_Xlen',
            'invalid vector<T> subscript': 'vector::_Xran', 'invalid string position': 'string::_Xran',
            'list<T> too long': 'list::insert', 'map/set<T> too long': 'tree::insert',
            'invalid map/set<T> iterator': 'tree::iterator', 'out of memory.': 'DLFixedVector::push_back',
            'access to an out of range.': 'DLFixedVector::access'}
STD_LIMIT = {'list::insert': 110, 'tree::insert': 260, 'tree::iterator': 420, 'vector::_Xlen': 100,
             'DLFixedVector::access': 70}
std_roles = 0
for f in functions:
    said = {STD_TEXT[s] for s in by_function.get(f, ()) if s in STD_TEXT}
    name = names.get(f, '')
    if len(said) == 1 and (not name or how.get(f) in ('near', 'unit', 'unitfile', 'helper', 'vtable', 'this') or name.startswith(('Class_', 'shared_'))):
        role = next(iter(said))
        if functions[f][1] > STD_LIMIT.get(role, 60):
            continue
        if role.endswith('_Xlen') and functions[f][1] > 8:
            role = role.replace('_Xlen', 'grow')
        names[f] = f'{role}_{f:x}' if role.startswith('DL') else f'std::{role}_{f:x}'
        how[f] = 'shape'
        std_roles += 1
print('standard library', std_roles)

# 9b. FD4Step is a template, so each step class has its own copy of its update and its debug page, which say
# the same wide text. They were read by hand; a copy keeps its class, and takes the template's name without one.
STEP_TEXT = {'State Finished.(No StepMethod is Executing.)': 'stepUpdate', 'StepMethodList': 'stepDebugPage',
             '//<※デバッグ強制ステップ遷移実行>(Stepが終了状態のため現在実行不可)': 'stepDebugForceItem'}
step_text = {}
for text_, role in STEP_TEXT.items():
    wide = text_.encode('utf-16-le') + b'\0\0'
    for m in re.finditer(re.escape(wide), image):
        for p_ in loads:
            if p_[2] <= m.start() < p_[2] + p_[5]:
                step_text[p_[3] + m.start() - p_[2]] = role
step_roles = 0
for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', code, re.S):
    role = step_text.get(text[3] + m.end() + struct.unpack('<i', m.group(1))[0])
    f = owner(text[3] + m.start()) if role else None
    if f is None or names.get(f, '').endswith(role):
        continue
    label = names.get(f, '').split('::')[0]
    if not label or re.search(r'Class_|shared_', names.get(f, '')):
        label = f'FD4Step_{f:x}'
    names[f] = f'{label}::{role}'
    how[f] = 'shape'
    step_roles += 1
print('step templates', step_roles)
# Its two small members have one shape around the failure "destination step undefined": one moves to the step it
# is given, the other to the next one.
STEP_FAIL = 0xf85b10
step_moves = 0
for r in rows('calls.tsv'):
    f = int(r[0], 16)
    role = {b'\x89\x77': 'setStep', b'\xff\x47': 'nextStep'}.get(code[f - text[3] + 27:f - text[3] + 29])
    if int(r[1], 16) != STEP_FAIL or functions[f][1] != 67 or role is None or how.get(f) == 'hand':
        continue
    label = names.get(f, '').split('::')[0]
    if not label or re.search(r'Class_|shared_', names.get(f, '')):
        label = f'FD4Step_{f:x}'
    names[f] = f'{label}::{role}'
    how[f] = 'shape'
    step_moves += 1
print('step moves', step_moves)
# 9b1. Three more templates of one body. A method invoker's argument types (the slot rule names those it finds
# in a table of its own; identical bodies are folded into one, so the others carry another class's name): a run
# of cleared slots, then the object stored after them. And a debug menu's: drop a page, make and add an item.
INVOKER_END = b'\x48\x89\x7e\x78\x31\xc0\xc3'
MENU_DETACH, MENU_ITEM = 0x1001720, 0x1001750
OWN_NAME = ('hand', 'message', 'table', 'step', 'binding', 'singleton', 'assert')
template_bodies = 0
for f, (size_, count_) in functions.items():
    if how.get(f) in OWN_NAME or not text[3] <= f < text[3] + text[5]:
        continue
    label = names.get(f, '').split('::')[0]
    if 18 <= count_ <= 23 and INVOKER_END in code[f - text[3]:f - text[3] + size_]:
        name = f'DLMethodInvoker::argumentTypes_{f:x}'
    elif count_ == 14 and callees[f] == {MENU_DETACH}:
        name = f'{label or "FD4DebugMenu"}::detachDebugPage' + ('' if label else f'_{f:x}')
    elif count_ == 47 and MENU_ITEM in callees[f]:
        name = f'FD4DebugMenu::addItem_{f:x}'
    else:
        continue
    if names.get(f, '').endswith('::argumentTypes'):
        continue
    names[f] = name
    how[f] = 'shape'
    template_bodies += 1
print('template bodies', template_bodies)
# 9b2. Message texts: a short getter falls back to the wide text ?WeaponName? when the entry is missing.
msg_text = {}
for m in re.finditer(rb'\?\0((?:[A-Za-z]\0){3,40})\?\0\0\0', image):
    for p_ in loads:
        if p_[2] <= m.start() < p_[2] + p_[5]:
            msg_text[p_[3] + m.start() - p_[2]] = m.group(1).decode('utf-16-le')
msg_getters = collections.defaultdict(list)
for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', code, re.S):
    stem = msg_text.get(text[3] + m.end() + struct.unpack('<i', m.group(1))[0])
    f = owner(text[3] + m.start()) if stem else None
    if f is not None and functions[f][1] <= 60 and how.get(f) != 'hand':
        msg_getters[stem].append(f)
for stem, fs in msg_getters.items():
    for f in fs:
        names[f] = f'MsgRepository::get{stem[0].upper()}{stem[1:]}' + (f'_{f:x}' if len(fs) > 1 else '')
        how[f] = 'shape'
print('message getters', sum(map(len, msg_getters.values())))
# 9b3. std::vector of pointers: one body, copied for every element type, around the allocator's failure text.
VECTOR_REALLOCATE = 0x12a5130
model = code[VECTOR_REALLOCATE - text[3]:VECTOR_REALLOCATE - text[3] + functions[VECTOR_REALLOCATE][0]]
vectors = 0
for f, (size_, count_) in functions.items():
    if count_ != 66 or size_ != len(model) or not text[3] <= f < text[3] + text[5] or how.get(f) == 'hand':
        continue
    body = code[f - text[3]:f - text[3] + size_]
    if body[:8] == model[:8] and sum(x != y for x, y in zip(body, model)) < 30:
        names[f] = f'std::vector::reallocate_{f:x}'
        how[f] = 'shape'
        vectors += 1
print('vector reallocate', vectors)
# 9c. Shader passes: a function that loads shader programs by name in wide text sets up that pass. Without a
# role of its own it takes the name the programs share, or its first.
PROGRAM = re.compile(rb'(?:[A-Za-z0-9_]\0){3,70}\.\0[a-z]\0p\0o\0\0\0')
programs = collections.defaultdict(set)
for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', code, re.S):
    target = text[3] + m.end() + struct.unpack('<i', m.group(1))[0]
    f = owner(text[3] + m.start())
    if f is None:
        continue
    for p_ in loads:
        if p_[3] <= target < p_[3] + p_[5]:
            hit = PROGRAM.match(image, p_[2] + target - p_[3])
            if hit:
                programs[f].add(hit.group(0)[:-2].decode('utf-16-le').rsplit('.', 1)[0])
            break
shader_passes = 0
for f, stems in programs.items():
    if how.get(f, '') in ('step', 'class', 'static', 'message') and not names[f].endswith('writes_vtable'):
        continue
    shared = os.path.commonprefix(sorted(stems)).rstrip('_')
    names[f] = f'Shaders::{shared if len(shared) >= 5 else min(stems)}_{f:x}'
    how[f] = 'shape'
    shader_passes += 1
print('shader passes', shader_passes)
# 11. Identical code: functions with the same instructions, constants aside, are copies of one template. Where a
# group's known members mostly agree on a role the others take it (99.3% of 15,664 held-out names), and a group's
# role can be read once in the code (SHAPE_ROLES). Short bodies say too little.
SHAPE_ROLES = {0x1495250: 'std::vector::push_back', 0x2acd200: 'FXResource::relocateAndSwap', 0x16f3170: 'std::vector::reallocate', 0x12da780: 'std::vector::reallocate', 0x127db90: 'std::tree::eraseNodes',
               0x1ea54d0: 'std::map::extract', 0x230dc40: 'RenderPass::bindCommonTextures',
               0x29116a0: 'std::vector::eraseRange', 0x2911940: 'GUIComboTweaker::populateItems', 0x2188630: 'std::unordered_map::constructor',
               0x21e17a0: 'GUICallbackObject::connectSignals', 0x19b2d60: 'std::tree::constructor', 0x13ec140: 'std::vector::push_back', 0x1785460: 'SprjSessionManager::findMember',
               0xe6c840: 'ShaderParamSet::release', 0xe6c950: 'ShaderParamSet::reset', 0xe6c9b0: 'ShaderParamSet::commit',
               0x1f11da0: 'SoloParamRepository::findRow', 0x1f189f0: 'SoloParamRepository::setRowFlag', 0x2b75af0: 'FX::serializeChildren',
               0x2b3a3f0: 'FX::serializeMember', 0x2ae99d0: 'FX::freeKeys', 0x2935d50: 'std::unordered_map::erase', 0x2911290: 'std::vector::push_back',
               0x29117e0: 'std::vector::reallocate', 0xe73800: 'Template::destroyMembers', 0x2b767e0: 'FX::serializeChildren', 0x2aebdd0: 'FX::allocateKeys',
               0x2936200: 'std::unordered_map::insertUnique', 0x21e0120: 'std::unordered_map::clearNested',
               0x2b385c0: 'FX::serializeMember', 0x2904800: 'std::unordered_map::insert', 0x2967530: 'FD4DebugMenu::addCallbackItem',
               0x1d86ba0: 'ResRepository::findByPath', 0x1ae12c0: 'Menu::fixedMessageText',
               0x2905050: 'std::unordered_map::constructor', 0x2904af0: 'std::vector::insertN', 0x26f4cb0: 'destructor', 0x26f4d00: 'deletingDestructor', 0x26f1910: 'create',
               0x2880000: 'deletingDestructor', 0x1dea0: 'deletingDestructorThunk',
               0x2405a00: 'deletingDestructorThunk', 0x2611020: 'std::tree::eraseRange',
               0x13e7880: 'std::vector::insert', 0x21e2490: 'std::unordered_map::insert',
               0x21e21b0: 'std::unordered_map::erase', 0x1f11c80: 'ParamRowRef::find',
               0x1003f00: 'FD4DebugMenu::addItem', 0x1007250: 'FD4DebugMenu::addItem',
               0x2b38490: 'serialize', 0x2a84710: 'copyTo', 0x26f4c40: 'create', 0x2717050: 'std::vector::swap',
               0x23eef90: 'Scaleform::AS3::thunkGetString', 0x23eee90: 'Scaleform::AS3::thunkGetObject',
               0x2439370: 'Scaleform::AS3::thunkGetValue', 0x240ef50: 'Scaleform::AS3::ForEachChild_GC',
               0x240ed90: 'Scaleform::AS3::ForEachChild_GC', 0x29c2150: 'debugDrawLine',
               0x29c3100: 'debugDrawLine', 0x29c4d40: 'debugDrawLine', 0x1ed6c30: 'DL::tryTransaction',
               0xd06670: 'NexusRevolution::callById', 0xd1c560: 'NexusRevolution::callById',
               0x1048540: 'std::wstring::clear', 0x1448c90: 'DL::deleteObject', 0x290ab00: 'destructor',
               0x1df5fb0: 'std::tree::eraseNodes', 0x1ce80a0: 'create', 0x19cca60: 'constructor',
               0x13ec240: 'std::vector::reallocate', 0x268f8c0: 'std::vector::tryReserve',
               0x272e7c0: 'std::vector::moveAssign', 0x2717330: 'std::vector::insertRange',
               0x2ae91c0: 'release', 0x2aedf60: 'evaluate', 0x27021c0: 'destructor',
               0x2702220: 'deletingDestructor', 0x2aed080: 'deletingDestructor', 0x2ae9270: 'clone',
               0x2ae9710: 'resize', 0x26aaed0: 'CG::replayCommand', 0x2af9440: 'copyTo', 0x2ae97b0: 'allocate',
               0x26f4d40: 'equals', 0x1fc3be0: 'getLocked', 0x29070f0: 'destructor',
               0x2907120: 'deletingDestructor', 0x291b050: 'removeByKey', 0x26d47d0: 'CG::setShaderResources',
               0x290fa80: 'std::vector::clear', 0x2702160: 'create', 0x13e9e40: 'deletingDestructor',
               0x1fc4970: 'lockedForward', 0x1fc4ba0: 'lockedForward', 0x285aaa0: 'create',
               0xe2f290: 'constructor', 0x189ea10: 'flushIfDirty', 0x1fbed10: 'unsupportedMethod',
               0x26abdd0: 'CG::recordCommand', 0x123ce90: 'std::list::constructor',
               0x12da6a0: 'std::vector::push_back', 0x2a82b60: 'resize', 0x2a82da0: 'release',
               0x2aebf40: 'release', 0x17c9010: 'DL::deleteObject', 0x1460c50: 'DL::deleteThis',
               0x2aecf20: 'destructor', 0x15d9fe0: 'setProperty', 0x26700b0: 'cacheError',
               0xc96820: 'deletingDestructorThunk', 0x27221f0: 'std::vector::tryReserve',
               0x1003b80: 'FD4DebugMenu::addItem', 0x1833db0: 'FD4DeleyDeleteManager::request',
               0x1eae7a0: 'std::tree::eraseNodes', 0x2172880: 'freeBuffers', 0x2afabc0: 'forwardToParam',
               0x2afad00: 'forwardToParam', 0x2911b10: 'syncSelection', 0xc96e80: 'getLocked',
               0xcda980: 'std::vector::free', 0xf29e00: 'deleteMember', 0xf5d810: 'lockedForward',
               0xe59ee0: 'releaseBuffers', 0x101a980: 'forwardToSingleton', 0x1509630: 'releaseRef',
               0x1fc49d0: 'lockedForward', 0xcf3100: 'setName', 0x158a0c0: 'std::tree::eraseNodes',
               0xc8de50: 'lockedForward', 0xc97a20: 'lockedForward', 0x2a84920: 'getMaxValue',
               0x2a84990: 'getMinValue', 0x2705a90: 'copyFrom', 0x2969f50: 'lockedFree',
               0x26abb00: 'CG::recordCommand', 0x2708100: 'copyFrom', 0x2708240: 'equals',
               0x2885ea0: 'forwardToParam', 0x2a5f900: 'clone', 0x2645710: 'setLocked',
               0xc91d30: 'setLocked', 0x1001390: 'deleteSingleton', 0xff6020: 'removeDebugPage'}
WEAK_ROLE = ('helper', 'near', 'unit', 'method', 'vf', 'writes_vtable', 'shared', 'fn', '_Do_call')
role_of = lambda f: re.sub(r'_[0-9a-f]{5,}$', '', names.get(f, '').split('::')[-1])
objdump = shutil.which('llvm-objdump') or shutil.which('objdump')
elf = OUT / 'eboot_ghidra.elf'
shapes = collections.defaultdict(list)
writers = {}
vtable_stores = collections.Counter()
stored_tables = collections.defaultdict(list)
first_stores = set()
lock_calls = collections.defaultdict(lambda: [0, 0, 0, 0])
if objdump and elf.exists():
    current, digest = None, None
    constants = re.compile(r'-?0x[0-9a-f]+|\b\d+\b')
    listing = subprocess.Popen([objdump, '-d', '--no-show-raw-insn', str(elf)], stdout=subprocess.PIPE, text=True,
                               errors='replace')
    for line in listing.stdout:
        hit = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
        if not hit:
            continue
        at = int(hit.group(1), 16)
        if at in functions:
            if current is not None and functions[current][1] >= 8:
                shapes[digest.digest()].append(current)
            current, digest = at, hashlib.blake2b(digest_size=8)
            this_regs, new_regs, table_regs, stores = {'%rdi'}, set(), {}, set()
            loaded = {}
        if current is not None and at < current + functions[current][0] and \
                re.search(r'writes_vtable$|::vf\d+$', names.get(current, '')):
            # Who the vtable is stored into: the first argument, a new object or a local.
            copy = re.match(r'movq\s+(%\w+), (%\w+)$', hit.group(2).strip())
            load = re.match(r'leaq\s+\S+\(%rip\), (%\w+)\s+# (0x[0-9a-f]+)', hit.group(2))
            store = re.match(r'movq\s+(%\w+), \((%\w+)\)$', hit.group(2).strip())
            if copy and copy.group(1) == '%rdi':
                this_regs.add(copy.group(2))
            elif copy and copy.group(1) == '%rax':
                new_regs.add(copy.group(2))
            if load:
                table_regs.pop(load.group(1), None)
                loaded[load.group(1)] = int(load.group(2), 16)
                if loaded[load.group(1)] in vtables:
                    table_regs[load.group(1)] = loaded[load.group(1)]
            # The address is often loaded early, with the offset added after (or in a second lea).
            added = re.match(r'addq\s+\$(0x[0-9a-f]+), (%\w+)$', hit.group(2).strip())
            moved = re.match(r'leaq\s+(0x[0-9a-f]+)\((%\w+)\), (%\w+)$', hit.group(2).strip())
            if added and added.group(2) in loaded:
                loaded[added.group(2)] += int(added.group(1), 16)
                if loaded[added.group(2)] in vtables:
                    table_regs[added.group(2)] = loaded[added.group(2)]
            if moved and moved.group(2) in loaded and loaded[moved.group(2)] + int(moved.group(1), 16) in vtables:
                table_regs[moved.group(3)] = loaded[moved.group(2)] + int(moved.group(1), 16)
            if store and store.group(1) in table_regs:
                stores.add('this' if store.group(2) in this_regs else
                           'new' if store.group(2) in new_regs | {'%rax'} else 'other')
                writers[current] = stores
                vtable_stores[current] += 1
                if store.group(2) in this_regs:
                    stored_tables[current].append(table_regs[store.group(1)])
                if store.group(2) in this_regs and at < current + 48:
                    first_stores.add(current)
        code_text = hit.group(2).split('#')[0].split('<')[0].strip()
        if current is not None and functions[current][1] <= 30 and at < current + functions[current][0]:
            lock_seen = lock_calls[current]
            lock_seen[0] += '$0xffffffff, %esi' in code_text
            lock_seen[1] += bool(re.match(r'(callq|jmpq)\s+\*0x18\(%r', code_text))
            lock_seen[2] += bool(re.match(r'(callq|jmpq)\s+\*0x28\(%r', code_text))
            lock_seen[3] += bool(re.match(r'(callq|jmpq)\s+\*', code_text))
        if current is not None and not code_text.startswith('nop'):
            digest.update(constants.sub('#', code_text).encode())
identical = 0
for group in shapes.values():
    known = collections.Counter(role_of(f) for f in group
                                if role_of(f) and not role_of(f).startswith(WEAK_ROLE))
    seeded = next((SHAPE_ROLES[f] for f in group if f in SHAPE_ROLES), None)
    total = sum(known.values())
    mixed = set(known) == {'destructor', 'deletingDestructor'}
    role = seeded or (known.most_common(1)[0][0] if total >= 3 and known.most_common(1)[0][1] / total >= 0.8 else None)
    if not role and not mixed:
        continue
    for f in group:
        if how.get(f) in OWN_NAME or (role_of(f) and not role_of(f).startswith(WEAK_ROLE)):
            continue
        this = role or ('deletingDestructor' if direct[f] & OPERATOR_DELETE else 'destructor')
        label = names.get(f, '').split('::')[0]
        names[f] = this + f'_{f:x}' if '::' in this else f'{label}::{this}' if label else f'Copy::{this}_{f:x}'
        how[f] = 'shape'
        identical += 1
print('identical code', identical)
# 12. A function that stores its class's vtable and is no slot of it builds an object: its first argument
# (a constructor) or, when short with one store, what it allocated (a factory: longer ones build debug pages).
built = virtual_destructors = 0
for f, stores in writers.items():
    if how.get(f) in OWN_NAME:
        continue
    # A virtual method that starts by storing a vtable into its own object is a destructor.
    if re.search(r'::vf\d+$', names.get(f, '')):
        if f in first_stores:
            names[f] = names[f].rsplit('::', 1)[0] + (
                '::deletingDestructor' if direct[f] & OPERATOR_DELETE else '::destructor')
            how[f] = 'shape'
            virtual_destructors += 1
        continue
    if not names.get(f, '').endswith('writes_vtable'):
        continue
    # Constructors end on their own class's vtable; destructors start on it and end on the base's, or free the
    # object. One that only opens with its vtable was a constructor in every sample.
    label = names[f].rsplit('::', 1)[0]
    own = lambda table: label in (class_names[table], f'Class_{table:x}')
    tables = stored_tables[f]
    if 'this' in stores and f in first_stores:
        if direct[f] & OPERATOR_DELETE:
            names[f], how[f] = f'{label}::deletingDestructor', 'shape'
            built += 1
        elif len(set(tables)) > 1 and own(tables[0]) and not own(tables[-1]):
            names[f], how[f] = f'{label}::destructor', 'shape'
            built += 1
        else:
            names[f], how[f] = f'{label}::constructor', 'shape'
            built += 1
        continue
    if 'this' in stores or ('new' in stores and vtable_stores[f] == 1 and functions[f][1] <= 120):
        names[f] = names[f].rsplit('::', 1)[0] + ('::constructor' if 'this' in stores else '::create')
        how[f] = 'shape'
        built += 1
print('constructors', built, 'virtual destructors', virtual_destructors)
# 13. A short function that calls the debug menu's item makers fills a debug page.
ITEM_MAKERS = ('addItem', 'addDebugItem', 'addDebugToggle', 'addDebugButton')
page_fillers = 0
for f, (size_, count_) in functions.items():
    if count_ <= 120 and (not role_of(f) or role_of(f).startswith(WEAK_ROLE)) and how.get(f) not in OWN_NAME \
            and any(role_of(c) in ITEM_MAKERS for c in callees[f]):
        label = names.get(f, '').split('::')[0]
        names[f] = f'{label}::fillDebugPage' if label else f'FD4DebugMenu::fillDebugPage_{f:x}'
        how[f] = 'shape'
        page_fillers += 1
print('debug page fillers', page_fillers)
# 14. A short function that takes a mutex (slot 3, waiting forever), does one thing and releases it (slot 5): a
# getter, a setter or a forward under the lock.
locked = 0
for f, seen in lock_calls.items():
    if seen[0] and seen[1:] == [1, 1, 2] and how.get(f) not in OWN_NAME and (not role_of(f) or role_of(f).startswith(WEAK_ROLE)):
        label = names.get(f, '').split('::')[0]
        names[f] = f'{label}::lockedAccessor' + ('' if '::' in names.get(f, '') else f'_{f:x}') if label else f'Locked::accessor_{f:x}'
        how[f] = 'shape'
        locked += 1
print('locked accessors', locked)
names.update(hand)
how.update(dict.fromkeys(hand, 'hand'))
# 16. Bound methods: scripts and debug menus register a method with its name in wide text, the text loaded just
# before the code pointer (samples read by hand: setters, getters and flags that fit their names).
BOUND_TEXT = re.compile(rb'(?:[\x20-\x7e]\0){3,80}\0\0')
BOUND_NAME = re.compile(r'(?:[A-Z]\w+::)?(?:Set|Get|Is|Has|Can|Send|On|Add|Remove|Start|Stop|Reset|Clear|Load|Save|Update|Create|Delete|Enable|Disable|Check|Find|Request)[A-Z]\w*')
BOUND_WEAK = ('', 'vtable', 'near', 'helper', 'unit', 'unitfile', 'this')
bound, last_text = 0, None
for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', code, re.S):
    target = text[3] + m.end() + struct.unpack('<i', m.group(1))[0]
    if target in functions:
        if last_text and m.start() - last_text[0] < 48 and how.get(target, '') in BOUND_WEAK and modules.get(target, ('', ''))[0] in TARGET:
            word = last_text[1]
            owner_name = names.get(target, '').split('::')[0]
            if word.endswith('[push enter]'):
                names[target] = 'DebugAction::' + re.sub(r'\W+', '_', word[:-12]).strip('_') + f'_{target:x}'
            elif '::' in word or not owner_name or owner_name.startswith(('Class_', 'shared_', 'Says')):
                names[target] = word if '::' in word else f'Bound::{word}_{target:x}'
            else:
                names[target] = f'{owner_name}::{word}_{target:x}'
            how[target] = 'shape'
            bound += 1
        continue
    for p_ in loads:
        if p_[3] <= target < p_[3] + p_[5]:
            hit = BOUND_TEXT.match(image, p_[2] + target - p_[3])
            if hit:
                word = hit.group(0)[:-2].decode('utf-16-le')
                last_text = (m.start(), word) if BOUND_NAME.fullmatch(word) or word.endswith('[push enter]') else None
            break
print('bound methods', bound)
# 16b. Script natives: Havok's Lua callbacks and ActionScript's natives are registered with their names in plain
# text beside the code pointer, in either order (hkbFireEvent read by hand).
NATIVE_TEXT = re.compile(rb'(?:hkb[A-Z][A-Za-z0-9]+|[a-z]+[A-Z][A-Za-z0-9]+|[a-z]{6,})\0')
leas = [(m.start(), text[3] + m.end() + struct.unpack('<i', m.group(1))[0]) for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', code, re.S)]
natives = 0
for i, (at_, target) in enumerate(leas):
    if target not in functions or functions[target][1] < 5 or how.get(target, '') not in BOUND_WEAK:
        continue
    for j in (i - 1, i + 1):
        if not 0 <= j < len(leas) or abs(leas[j][0] - at_) >= 48 or leas[j][1] in functions:
            continue
        hit = None
        for p_ in loads:
            if p_[3] <= leas[j][1] < p_[3] + p_[5]:
                hit = NATIVE_TEXT.match(image, p_[2] + leas[j][1] - p_[3])
                break
        if hit:
            word = hit.group(0)[:-1].decode()
            names[target] = f'hkbScript::{word}_{target:x}' if word.startswith('hkb') else f'Native::{word}_{target:x}'
            how[target] = 'shape'
            natives += 1
            break
print('script natives', natives)
# 16c. The same natives in data: tables of names and function pointers (Lua's name, function records; ActionScript's
# with the name after the pointer). Samples read by hand: Lua's sin and setfenv.
TABLE_TEXT = re.compile(rb'[a-z_][A-Za-z0-9_]{2,60}\0')
slots = {int(r[0], 16): int(r[1], 16) for r in rows('pointer_slots.tsv')}
tabled = 0
for slot, target in slots.items():
    if target not in functions or how.get(target, '') not in BOUND_WEAK:
        continue
    for step in (-8, 16, -24):
        other = slots.get(slot + step)
        if other is None or other in functions:
            continue
        hit = None
        for p_ in loads:
            if p_[3] <= other < p_[3] + p_[5]:
                hit = TABLE_TEXT.match(image, p_[2] + other - p_[3])
                break
        if hit:
            names[target] = f'Native::{hit.group(0)[:-1].decode()}_{target:x}'
            how[target] = 'shape'
            tabled += 1
            break
print('natives in tables', tabled)
# 17. Slot roles: a family of classes shares what a virtual slot does, read once in the code. An evaluatable's slot 1
# evaluates: the operators call slot 1 of both operands and combine the results.
SLOT_ROLES = [(re.compile(r'FXEvaluatable.*::vf1'), 'evaluate'),
              # Menu dialogs: slot 24 binds named parts to child classes, slot 27 takes a message id and a value (three read each).
              (re.compile(r'FrpgMenuDlg\w*::vf24'), 'attachChildren'), (re.compile(r'FrpgMenuDlg\w*::vf27'), 'onMessage'),
              # Slot 17 passes the frame on to the base dialog's update (three read).
              (re.compile(r'FrpgMenuDlg\w*::vf17'), 'update'),
              # A GUI object's slot 3 drops what it owns and calls the base's slot 3 (five read).
              (re.compile(r'GUI\w*::vf3'), 'releaseChildren'),
              # Debug tweakers: slot 5 compares the bound value with the shown one (combo and on/off read), a combo's slot 7
              # stores the chosen item's value, a range's data parses text into its limits.
              (re.compile(r'GUI(?:Combo|OnOff)Tweaker::vf5'), 'syncValue'), (re.compile(r'GUIComboTweaker::vf7'), 'selectItem'),
              (re.compile(r'GUIRangeDataBase::vf5'), 'parseValue'),
              # FX sequences: slot 11 evaluates at a tick, slot 18 finds the smallest key, slot 19 beside it the largest.
              (re.compile(r'FX\w*SequenceParamReal::vf11'), 'evaluate'), (re.compile(r'FX\w*SequenceParamReal::vf18'), 'getMinValue'),
              (re.compile(r'FX\w*SequenceParamReal::vf19'), 'getMaxValue'),
              # File capsules: slot 7 passes the loaded data on (two read). Location nodes: slot 8 copies a range of 0x30-byte
              # matrices out (one read).
              (re.compile(r'\w+FileCap::vf7'), 'onFileLoaded'), (re.compile(r'SprjFD4LocationNode_\w+::vf8'), 'getMatrices'),
              # Effect drawing: ten setup slots for the effect kinds read their curves, then 32 copies the scene region and 33 draws.
              (re.compile(r'FXRenderMaterial::vf(1[4-9]|2[0-3])'), 'setupEffect'), (re.compile(r'FXRenderMaterial::vf32'), 'copySceneRegion'),
              (re.compile(r'FXRenderMaterial::vf33'), 'draw'), (re.compile(r'FXRenderMaterial::vf34'), 'setMatrices'),
              # FX runtime sequences (one read each): slot 11 evaluates, 9 sizes the stored value, 4 frees the keys; constants: 8 copies to a like object.
              (re.compile(r'FX\w*SequenceParamRuntime::vf11'), 'evaluate'), (re.compile(r'FX\w*ParamRuntime::vf9'), 'getStoredSize'),
              (re.compile(r'FX\w*SequenceParamRuntime::vf4'), 'freeKeys'), (re.compile(r'FX(?:(?!Sequence)\w)*ParamReal::vf8'), 'copyTo'),
              # Location nodes combine two inputs: slot 5 one transform, 6 its inverse (a determinant, read), 7 a range of them.
              (re.compile(r'SprjFD4LocationNode_\w+::vf5'), 'getTransform'), (re.compile(r'SprjFD4LocationNode_\w+::vf6'), 'getInverseTransform'),
              (re.compile(r'SprjFD4LocationNode_\w+::vf7'), 'getTransforms'),
              (re.compile(r'FD4DebugMenuItem_\w+::vf2'), 'getLabel'), (re.compile(r'FD4DebugMenuItem_\w+::vf15'), 'onEvent'),
              # Debug menu items: slot 7 edits the value from the pad, slot 8 writes the item's text (two read).
              (re.compile(r'FD4DebugMenuItem_\w+::vf7'), 'onInput'), (re.compile(r'FD4DebugMenuItem_\w+::vf8'), 'writeText'),
              # Fading setting channels (one class per value type): a list of named states, a list of bound targets.
              # Read in one class: the slots do the same in the rest, only the value's size differs.
              *[(re.compile(r'GraphicsParamChannel_\w+::vf%d' % slot), role) for slot, role in
                ((13, 'setStateValue'), (14, 'getStateCount'), (15, 'getStateIdAt'), (16, 'getStateValue'), (20, 'getStateAt'),
                 (2, 'addTarget'), (4, 'fadeTargetsIn'), (5, 'fadeTargetsOut'), (6, 'setTargetsScaled'), (7, 'update'),
                 (8, 'updateTargets'), (9, 'fadeIn'), (10, 'fadeOut'), (11, 'setScaled'), (12, 'hasState'))],
              # Menu dialogs: slot 32 takes a control event (2, 6, 8) and a value (two read, same comparisons).
              (re.compile(r'FrpgMenuDlg\w*::vf32'), 'onControlEvent'),
              # FX constant parameters: slot 9 gives the size of the stored value, slot 10 describes it (one read each); a parent
              # reference's slot 11 forwards to the parent's slot 11.
              (re.compile(r'FX(?:(?!Sequence)\w)*ParamReal::vf9'), 'getStoredSize'), (re.compile(r'FX(?:(?!Sequence)\w)*ParamReal::vf10'), 'describeValue'),
              (re.compile(r'FX\w*ParamParentRef::vf11'), 'evaluate'),
              # A method invoker calls its member function pointer and stores the result.
              (re.compile(r'DLMethodInvoker_\w+::vf0'), 'invoke')]
slot_named = 0
for f, name in list(names.items()):
    if how.get(f) in ('class', 'vtable'):
        for pattern, role in SLOT_ROLES:
            if pattern.fullmatch(name):
                names[f] = name.rsplit('::', 1)[0] + '::' + role + (f'_{f:x}' if role == 'setupEffect' else '')
                how[f] = 'shape'
                slot_named += 1
print('slot roles', slot_named)
# 18. Parameter tables: SoloParamRepository keeps its tables 72 bytes apart from offset 112, in the order of a table of
# wide names at 0x5355ad0. A function that reaches one table works with that parameter (read against the names of the
# code using them: lock cameras, character creation, face data). Short ones are its accessors; on longer unnamed ones
# the table is only a hint, like rule 15.
PARAM_TABLE, PARAM_REPOSITORY = 0x5355ad0, 0x5540340
param_names = []
while PARAM_TABLE + len(param_names) * 16 in slots:
    q = slots[PARAM_TABLE + len(param_names) * 16]
    for p_ in loads:
        if p_[3] <= q < p_[3] + p_[5]:
            param_names.append(image[p_[2] + q - p_[3]:p_[2] + q - p_[3] + 80].split(b'\0\0')[0].replace(b'\0', b'').decode('ascii', 'replace'))
            break
param_users = set()
for m in re.finditer(rb'[\x48\x4c][\x8b\x83\x8d][\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', code, re.S):
    if PARAM_REPOSITORY in (text[3] + m.end() + struct.unpack('<i', m.group(1))[0], text[3] + m.end() + 1 + struct.unpack('<i', m.group(1))[0]):
        f = owner(text[3] + m.start())
        if f is not None:
            param_users.add(f)
param_named = 0
for f in param_users:
    at = f - text[3]
    body = code[at:at + functions[f][0]]
    seen = {struct.unpack('<i', m.group(1))[0] for m in re.finditer(rb'\x83[\xb8-\xbf](.{4})\x00', body, re.S)}
    seen |= {struct.unpack('<i', m.group(1))[0] - 8 for m in re.finditer(rb'[\x48\x4c]\x8b[\x80-\xbf](.{4})', body, re.S)}
    seen = [x for x in seen if 112 <= x < 112 + 72 * len(param_names) and (x - 112) % 72 == 0]
    if len(seen) != 1:
        continue
    table = param_names[(seen[0] - 112) // 72]
    name = names.get(f, '')
    if name.startswith('SoloParamRepository::'):
        names[f] = f"SoloParam::{table}::{name.split('::')[1].rsplit('_', 1)[0]}_{f:x}"
    elif not name and functions[f][1] <= 60:
        names[f], how[f] = f'SoloParam::{table}::access_{f:x}', 'shape'
    elif not name:
        names[f], how[f] = f'Says::{table}_{f:x}', 'text'
    else:
        continue
    param_named += 1
print('parameter tables', len(param_names), 'functions', param_named)
# 19. Repository lookups: a short function that names one repository in its singleton check and calls the common
# find works on that repository's resources (read: FlverRepository, CSEntryfilelistRepository, SprjEvdRepository).
RES_FIND = 0xfe68f0
find_callers = {int(r[0], 16) for r in rows('calls.tsv') if int(r[1], 16) == RES_FIND}
repo_named = 0
for f in find_callers:
    repos = {s_ for s_ in by_function.get(f, ()) if s_.endswith('Repository')}
    if len(repos) == 1 and f in functions and functions[f][1] <= 90 and how.get(f, '') in ('', 'near', 'helper', 'unit', 'vtable', 'class', 'text'):
        names[f], how[f] = f'{repos.pop()}::findResource_{f:x}', 'shape'
        repo_named += 1
print('repository lookups', repo_named)
# 21. Import stubs carry their library name, whatever the rules above made of the callers around them.
for r in rows('imports.tsv'):
    f = int(r[0], 16)
    if f in functions or f in names:
        names[f], how[f] = 'Import::' + r[1], 'binding'
# 15. Text: a function no rule placed but that loads wide text says what it deals with. That is a hint, not its
# role, so the name says only that (Says::), it never replaces another name, and it counts as placed, not identified.
SAYS = re.compile(rb'(?:[\x20-\x7e]\0){5,60}\0\0')
SAYS_WORD = re.compile(r'[A-Za-z][A-Za-z0-9_.:/ ]{4,}')
SAYS_GENERIC = {'unknown', 'Enable', 'value', 'Dummy', 'cursor', 'common', 'debug', 'Memory allocation failed!'}
said = collections.defaultdict(set)
for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', code, re.S):
    target = text[3] + m.end() + struct.unpack('<i', m.group(1))[0]
    f = owner(text[3] + m.start())
    if f is None or names.get(f) or f in static_inits or modules.get(f, ('', ''))[0] not in TARGET:
        continue
    for p_ in loads:
        if p_[3] <= target < p_[3] + p_[5]:
            hit = SAYS.match(image, p_[2] + target - p_[3])
            if hit:
                word = SAYS_WORD.search(hit.group(0)[:-2].decode('utf-16-le'))
                if word and word.group(0) not in SAYS_GENERIC:
                    said[f].add(word.group(0))
            break
# The same for plain text: most often a singleton's name in its inlined check. The rarest word says the most.
said_plain = collections.Counter(s_ for f in by_function for s_ in set(by_function[f]) if re.fullmatch(r'[A-Z][A-Za-z0-9_]{4,}', s_))
for f, texts in by_function.items():
    if f not in names and f not in said and f not in static_inits and modules.get(f, ('', ''))[0] in TARGET:
        words = [s_ for s_ in texts if s_ in said_plain and not s_.startswith('Text_')]
        if words:
            said[f].add(min(words, key=lambda s_: (said_plain[s_], s_)))
for f, words in said.items():
    names[f] = 'Says::' + re.sub(r'\W+', '_', max(sorted(words), key=len))[:40].strip('_') + f'_{f:x}'
    how[f] = 'text'
print('functions named by their text', len(said))
# 20. Singleton fields: a short function whose only call is the failed check of one singleton reads or writes a field
# of it (two read: SprjWorldAiManager, FrpgNetMan). A mention with other calls proves nothing, so those stay hints.
SINGLETON_FAIL = 0x20b55b0
callees = collections.defaultdict(set)
for r in rows('calls.tsv'):
    callees[int(r[0], 16)].add(int(r[1], 16))
SINGLETON_NAME = re.compile(r'[A-Z][A-Za-z0-9]*(?:Man|Manager|Repository|System|Sfx|Gaitem|File)')
field_named = 0
for f, name in list(names.items()):
    if how.get(f) == 'text' and functions[f][1] <= 30 and callees[f] <= {SINGLETON_FAIL}:
        words = {s_ for s_ in by_function.get(f, ()) if SINGLETON_NAME.fullmatch(s_)}
        if len(words) == 1:
            names[f], how[f] = f'{words.pop()}::fieldAccess_{f:x}', 'shape'
            field_named += 1
print('singleton fields', field_named)
# 23. YEBIS carries a Direct3D 11 layer over Gnm (ppfxPlatformContextGnmShim calls it; CoreGraphics2 does not): Map returns DXGI_ERROR_WAS_STILL_DRAWING, UpdateSubresource tiles
# through GpuAddress, and the slots keep Microsoft's published order (the sizes agree: constant buffer and sampler setters
# are one template per stage, shader resource setters another, the private data pair is the same size in every class).
D3D_UNKNOWN = ['QueryInterface', 'AddRef', 'Release']
D3D_CHILD = D3D_UNKNOWN + ['GetDevice', 'GetPrivateData', 'SetPrivateData', 'SetPrivateDataInterface']
STAGE_SETTERS = lambda stage: [f'{stage}SetShaderResources', f'{stage}SetShader', f'{stage}SetSamplers', f'{stage}SetConstantBuffers']
STAGE_GETTERS = lambda stage: [f'{stage}GetShaderResources', f'{stage}GetShader', f'{stage}GetSamplers', f'{stage}GetConstantBuffers']
D3D_TABLES = {
    0x530a380: ('D3D11DeviceContext', D3D_CHILD + """VSSetConstantBuffers PSSetShaderResources PSSetShader PSSetSamplers VSSetShader DrawIndexed Draw
        Map Unmap PSSetConstantBuffers IASetInputLayout IASetVertexBuffers IASetIndexBuffer DrawIndexedInstanced DrawInstanced
        GSSetConstantBuffers GSSetShader IASetPrimitiveTopology VSSetShaderResources VSSetSamplers Begin End GetData SetPredication
        GSSetShaderResources GSSetSamplers OMSetRenderTargets OMSetRenderTargetsAndUnorderedAccessViews OMSetBlendState
        OMSetDepthStencilState SOSetTargets DrawAuto DrawIndexedInstancedIndirect DrawInstancedIndirect Dispatch DispatchIndirect
        RSSetState RSSetViewports RSSetScissorRects CopySubresourceRegion CopyResource UpdateSubresource CopyStructureCount
        ClearRenderTargetView ClearUnorderedAccessViewUint ClearUnorderedAccessViewFloat ClearDepthStencilView GenerateMips
        SetResourceMinLOD GetResourceMinLOD ResolveSubresource ExecuteCommandList""".split()
        + STAGE_SETTERS('HS') + STAGE_SETTERS('DS') + ['CSSetShaderResources', 'CSSetUnorderedAccessViews', 'CSSetShader', 'CSSetSamplers', 'CSSetConstantBuffers']
        + """VSGetConstantBuffers PSGetShaderResources PSGetShader PSGetSamplers VSGetShader PSGetConstantBuffers IAGetInputLayout
        IAGetVertexBuffers IAGetIndexBuffer GSGetConstantBuffers GSGetShader IAGetPrimitiveTopology VSGetShaderResources VSGetSamplers
        GetPredication GSGetShaderResources GSGetSamplers OMGetRenderTargets OMGetRenderTargetsAndUnorderedAccessViews OMGetBlendState
        OMGetDepthStencilState SOGetTargets RSGetState RSGetViewports RSGetScissorRects""".split()
        + STAGE_GETTERS('HS') + STAGE_GETTERS('DS') + ['CSGetShaderResources', 'CSGetUnorderedAccessViews', 'CSGetShader', 'CSGetSamplers', 'CSGetConstantBuffers']
        + ['ClearState', 'Flush', 'GetType', 'GetContextFlags', 'FinishCommandList']),
    0x5308810: ('D3D11Device', D3D_UNKNOWN + """CreateBuffer CreateTexture1D CreateTexture2D CreateTexture3D CreateShaderResourceView
        CreateUnorderedAccessView CreateRenderTargetView CreateDepthStencilView CreateInputLayout CreateVertexShader CreateGeometryShader
        CreateGeometryShaderWithStreamOutput CreatePixelShader CreateHullShader CreateDomainShader CreateComputeShader CreateClassLinkage
        CreateBlendState CreateDepthStencilState CreateRasterizerState CreateSamplerState CreateQuery CreatePredicate CreateCounter
        CreateDeferredContext OpenSharedResource CheckFormatSupport CheckMultisampleQualityLevels CheckCounterInfo CheckCounter
        CheckFeatureSupport GetPrivateData SetPrivateData SetPrivateDataInterface GetFeatureLevel GetCreationFlags GetDeviceRemovedReason
        GetImmediateContext SetExceptionMode GetExceptionMode""".split()),
    0x530b800: ('D3D11Texture1D', D3D_CHILD + ['GetType', 'SetEvictionPriority', 'GetEvictionPriority', 'GetDesc']),
    0x530cdb0: ('D3D11Buffer', D3D_CHILD + ['GetType', 'SetEvictionPriority', 'GetEvictionPriority', 'GetDesc']),
    0x530bc00: ('D3D11Texture2D', D3D_CHILD + ['GetType', 'SetEvictionPriority', 'GetEvictionPriority', 'GetDesc']),
    0x530be80: ('D3D11Texture3D', D3D_CHILD + ['GetType', 'SetEvictionPriority', 'GetEvictionPriority', 'GetDesc']),
}
D3D_TABLES.update({table: (label, D3D_CHILD + extra) for table, label, extra in (
    (0x530ae10, 'D3D11RenderTargetView', ['GetResource', 'GetDesc']), (0x530d0e0, 'D3D11DepthStencilView', ['GetResource', 'GetDesc']),
    (0x530b5c0, 'D3D11ShaderResourceView', ['GetResource', 'GetDesc']), (0x530c100, 'D3D11UnorderedAccessView', ['GetResource', 'GetDesc']),
    (0x530cc80, 'D3D11BlendState', ['GetDesc']), (0x530cfb0, 'D3D11DepthStencilState', ['GetDesc']), (0x530ace0, 'D3D11RasterizerState', ['GetDesc']),
    (0x530afe0, 'D3D11SamplerState', ['GetDesc']), (0x530a870, 'D3D11InputLayout', []), (0x530ab90, 'D3D11Query', ['GetDataSize', 'GetDesc']),
    *((t, 'D3D11' + n, []) for t, n in ((0x5309920, 'VertexShader'), (0x53097a0, 'PixelShader'), (0x5309860, 'GeometryShader'),
                                        (0x53096e0, 'HullShader'), (0x5309620, 'DomainShader'), (0x5309560, 'ComputeShader'))))})
d3d_named = 0
for table, (label, slots) in D3D_TABLES.items():
    for f, slot in zip(vtables.get(table, ()), slots):
        if how.get(f) == 'hand':
            continue
        names[f], how[f] = f'{label}::{slot}', 'shape'
        d3d_named += 1
        # Most of the device's methods are a jump to the real one.
        if functions[f][1] <= 2 and len(direct.get(f, ())) == 1:
            target = next(iter(direct[f]))
            if how.get(target) != 'hand' and functions.get(target, (0, 0))[1] > 2:
                names[target], how[target] = f'{label}::{slot}Impl', 'shape'
                d3d_named += 1
print('Direct3D slots', d3d_named)
# 24. The SDK's command buffer writes PM4 packets with their headers as constants, so the opcode says what a short function
# does, and for a register write the next constant says which register.
PM4_OPS = {0x10: 'nop', 0x12: 'clearState', 0x13: 'indexBufferSize', 0x15: 'dispatchDirect', 0x16: 'dispatchIndirect', 0x20: 'setPredication',
           0x24: 'drawIndirect', 0x25: 'drawIndexIndirect', 0x26: 'indexBase', 0x27: 'drawIndex2', 0x28: 'contextControl', 0x2a: 'indexType',
           0x2d: 'drawIndexAuto', 0x2f: 'numInstances', 0x33: 'indirectBufferConst', 0x34: 'strmoutBufferUpdate', 0x35: 'drawIndexOffset2',
           0x37: 'writeData', 0x39: 'memSemaphore', 0x3c: 'waitRegMem', 0x3f: 'indirectBuffer', 0x40: 'copyData', 0x42: 'pfpSyncMe',
           0x43: 'surfaceSync', 0x45: 'condWrite', 0x46: 'eventWrite', 0x47: 'eventWriteEop', 0x48: 'eventWriteEos', 0x49: 'releaseMem',
           0x50: 'dmaData', 0x58: 'acquireMem', 0x59: 'rewind', 0x68: 'setConfigReg', 0x69: 'setContextReg', 0x76: 'setShReg', 0x79: 'setUconfigReg',
           0x83: 'dumpConstRam', 0x84: 'incrementCeCounter', 0x85: 'incrementDeCounter', 0x86: 'waitOnCeCounter', 0x88: 'waitOnDeCounterDiff'}
PM4_HEADER = re.compile(rb'\xc7[\x00\x40-\x47]([\x00-\xf8]?)\x00([\x10-\x88])([\x00-\x3f])\xc0')
PM4_NEXT = re.compile(rb'\xc7[\x40-\x47][\x00-\xf8](....)', re.S)
pm4_named = collections.Counter()
for f, (size, count, *_) in functions.items():
    if size > 1200 or modules.get(f, ('', ''))[0] in ('havok', 'scaleform', 'fmod', 'lua', 'yebis') or how.get(f) == 'hand':
        continue
    start = f - text0[3]
    packets = []
    for m in PM4_HEADER.finditer(code0, start, start + size):
        if m.group(2)[0] in PM4_OPS:
            operand = PM4_NEXT.match(code0, m.end())
            packets.append((m.group(2)[0], int.from_bytes(operand.group(1), 'little') if operand else None))
    if not packets or not (names.get(f, '') == '' or re.search(r'::(?:near_|helper_|unit_|vf\d+$)|^shared_', names[f])):
        continue
    op, register = next((p for p in packets if p[0] != 0x10), packets[0])
    if op == 0x10:  # a padding packet says nothing
        continue
    label = PM4_OPS[op]
    if op in (0x68, 0x69, 0x76, 0x79) and register is not None and register < 0x10000:
        label += f'_{register:x}'
    names[f], how[f] = f'Gnm::DrawCommandBuffer::{label}_{f:x}', 'shape'
    pm4_named[PM4_OPS[op]] += 1
print('command buffer packets', sum(pm4_named.values()), dict(pm4_named))
# 25. Unplaced functions. Most have no reference at all: no call, jump, address taken or data pointer, so nothing can
# reach them (Ghidra agrees on a sample) and they are only counted. The rest go with the one class that calls them, or
# with the function that takes their address.
FOREIGN_CLASS = re.compile(r'^(?:|Says|std|shared_\w+|Template|Import|Shaders|DL|staticInit_\w+)$')
def weak_class(name):
    return FOREIGN_CLASS.match(name.split('::')[0]) is not None
callers_of = collections.defaultdict(set)
for f, targets in callees.items():
    for target in targets:
        callers_of[target].add(f)
ordered = sorted(functions)
code_start = text0[3]
address_takers = collections.defaultdict(set)
for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', code0):
    at = code_start + m.start()
    target = at + 7 + int.from_bytes(code0[m.start() + 3:m.start() + 7], 'little', signed=True)
    if target in functions:
        address_takers[target].add(ordered[bisect.bisect_right(ordered, at) - 1])
data_pointers = {int(r[1], 16) for r in rows('pointer_slots.tsv')}
placed_late = collections.Counter()
WEAK_HOW = ('helper', 'near', 'unit', 'unitfile', 'shared', 'text')
before = dict(names)
unused = 0
for f in functions:
    if names.get(f) or modules.get(f, ('', ''))[0] not in TARGET or functions[f][1] <= 5:
        continue
    if not callers_of[f] and not address_takers[f] and f not in data_pointers:
        unused += 1
        continue
    # Only functions known by more than where they sit count, and none placed here: helpers of helpers ran together (read).
    # A template used by one class still lands on it, so this says who uses the code, not what it does.
    users = callers_of[f] | address_takers[f]
    owners = {before.get(c, '').split('::')[0] for c in users if how.get(c) not in WEAK_HOW} - {''}
    if len(owners) == 1 and not weak_class(next(iter(owners))) and all(before.get(c) for c in users):
        kind = 'callback' if not callers_of[f] else 'helper'
        names[f], how[f] = f'{next(iter(owners))}::{kind}_{f:x}', 'helper'
        placed_late[kind] += 1
print('no reference at all', unused, 'placed by their one owner', dict(placed_late))
# 26. Event conditions: slot 5 writes the object's state for a debug page and starts with its name, "Condition:EndFlow".
# Slot 2 tests the condition, slot 6 raises the event (two read).
CONDITION_TEXT = re.compile(rb'Condition:'.decode().encode('utf-16-le') + rb'((?:[A-Za-z0-9_]\0)+)\0\0')
conditions = 0
for table, run in vtables.items():
    if len(run) != 7 or run[5] not in functions:
        continue
    start = run[5] - text0[3]
    label = None
    for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d](.{4})', code0[start:start + functions[run[5]][0]], re.S):
        target = run[5] + m.end() + int.from_bytes(m.group(1), 'little', signed=True) - text0[3] + text0[2]
        text = CONDITION_TEXT.match(image, target)
        if text:
            label = 'SprjEventCondition_' + text.group(1).decode('utf-16-le')
            break
    if not label:
        continue
    conditions += 1
    old = f'Class_{table:x}::'
    for f, name in list(names.items()):
        if name.startswith(old) and how.get(f) != 'hand':
            names[f] = label + '::' + name[len(old):]
    for index, role in ((2, 'check'), (5, 'describe'), (6, 'raise')):
        if how.get(run[index]) != 'hand':
            names[run[index]], how[run[index]] = f'{label}::{role}', 'shape'
print('event conditions', conditions)
# 24b. A helper or near label far from everything solid in its class is a template or a shared utility that a class happens
# to use (two of three read were: a vector assignment, a number parser). Those lose the label: 1 MiB, the width of a large
# object file, measured against vtable slots, constructors and hand names.
SOLID_HOW = ('class', 'vtable', 'shape', 'hand', 'step', 'message', 'binding')
solid = collections.defaultdict(list)
for f, name in names.items():
    if how.get(f) in SOLID_HOW and '::' in name:
        solid[name.split('::')[0]].append(f)
for anchors in solid.values():
    anchors.sort()
far_labels = 0
for f, name in list(names.items()):
    if how.get(f) in ('helper', 'near') and '::' in name and modules.get(f, ('', ''))[0] in TARGET:
        anchors = solid.get(name.split('::')[0])
        if anchors:
            i = bisect.bisect_left(anchors, f)
            nearest = min((abs(anchors[j] - f) for j in (i - 1, i) if 0 <= j < len(anchors)))
            if nearest > 0x100000:
                del names[f]
                how.pop(f, None)
                far_labels += 1
print('labels dropped for distance', far_labels)
# 22. A std::function is built where its table is written: the only writer outside the table's own slots says whose lambda it is.
WEAK_OWNER = re.compile(r'^(?:Class_|shared_|fn_|Says::|Import::|std::|DL::)|::(?:near_|helper_|unit_|vf\d+$|fn_|writes_vtable)')
lambdas = collections.Counter()
for table, run in vtables.items():
    if len(run) < 3 or not names.get(run[2], '').startswith(f'std::_Func_impl_{table:x}::_Do_call'):
        continue
    owners = sites_writers.get(table, set()) - set(run)
    if len(owners) != 1:
        lambdas['writers %s' % min(len(owners), 2)] += 1
        continue
    owner = next(iter(owners))
    owner_name = names.get(owner, '')
    if not owner_name or WEAK_OWNER.search(owner_name):
        # Only the owner's class is worth keeping, when it has one.
        owner_name = owner_name.split('::')[0]
        if not owner_name or WEAK_OWNER.search(owner_name + '::'):
            lambdas['weak owner'] += 1
            continue
    names[run[2]] = f'{owner_name}::lambda_{run[2]:x}'
    lambdas['named'] += 1
print('lambdas', dict(lambdas))
with open(OUT / 'names.tsv', 'w') as out:
    out.write('address\tname\thow\tlibrary\tsource\n')
    for f in sorted(set(names) | set(files)):
        out.write(f'{f:#x}\t{names.get(f, "")}\t{how.get(f, "")}\t{modules.get(f, ("", ""))[0]}\t{files.get(f, "")}\n')
tally = collections.Counter(how.values())
named_target = {f for f in names if modules.get(f, ('', ''))[0] in TARGET}
print('named', len(names), dict(tally))
print('named in FromSoftware or unlabelled code', len(named_target), 'of which longer than 5 instructions', len(named_target & set(big)), 'of', len(big))
print('with a source file', len(files), 'of', len(functions))

