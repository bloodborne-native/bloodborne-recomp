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
    if how[f] in ('class', 'binding', 'step') and not name.startswith('Class_') and f >= unit_starts[0]:
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
            'invalid map/set<T> iterator': 'tree::iterator'}
STD_LIMIT = {'list::insert': 70, 'tree::insert': 260, 'tree::iterator': 420}
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
        names[f] = f'std::{role}_{f:x}'
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
SHAPE_ROLES = {0x26f4cb0: 'destructor', 0x26f4d00: 'deletingDestructor', 0x26f1910: 'create',
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
               0x2ae91c0: 'release', 0x2aedf60: 'evaluate'}
WEAK_ROLE = ('helper', 'near', 'unit', 'method', 'vf', 'writes_vtable', 'shared', 'fn', '_Do_call')
role_of = lambda f: re.sub(r'_[0-9a-f]{5,}$', '', names.get(f, '').split('::')[-1])
objdump = shutil.which('llvm-objdump') or shutil.which('objdump')
elf = OUT / 'eboot_ghidra.elf'
shapes = collections.defaultdict(list)
writers = {}
vtable_stores = collections.Counter()
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
            this_regs, new_regs, table_regs, stores = {'%rdi'}, set(), set(), set()
        if current is not None and names.get(current, '').endswith('writes_vtable'):
            # Who the vtable is stored into: the first argument, a new object or a local.
            copy = re.match(r'movq\s+(%\w+), (%\w+)$', hit.group(2).strip())
            load = re.match(r'leaq\s+\S+\(%rip\), (%\w+)\s+# (0x[0-9a-f]+)', hit.group(2))
            store = re.match(r'movq\s+(%\w+), \((%\w+)\)$', hit.group(2).strip())
            if copy and copy.group(1) == '%rdi':
                this_regs.add(copy.group(2))
            elif copy and copy.group(1) == '%rax':
                new_regs.add(copy.group(2))
            if load:
                (table_regs.add if int(load.group(2), 16) in vtables else table_regs.discard)(load.group(1))
            if store and store.group(1) in table_regs:
                stores.add('this' if store.group(2) in this_regs else
                           'new' if store.group(2) in new_regs | {'%rax'} else 'other')
                writers[current] = stores
                vtable_stores[current] += 1
        code_text = hit.group(2).split('#')[0].split('<')[0].strip()
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
built = 0
for f, stores in writers.items():
    if how.get(f) in OWN_NAME or not names.get(f, '').endswith('writes_vtable'):
        continue
    if 'this' in stores or ('new' in stores and vtable_stores[f] == 1 and functions[f][1] <= 120):
        names[f] = names[f].rsplit('::', 1)[0] + ('::constructor' if 'this' in stores else '::create')
        how[f] = 'shape'
        built += 1
print('constructors', built)
names.update(hand)
how.update(dict.fromkeys(hand, 'hand'))
with open(OUT / 'names.tsv', 'w') as out:
    out.write('address\tname\thow\tlibrary\tsource\n')
    for f in sorted(set(names) | set(files)):
        out.write(f'{f:#x}\t{names.get(f, "")}\t{how.get(f, "")}\t{modules.get(f, ("", ""))[0]}\t{files.get(f, "")}\n')
tally = collections.Counter(how.values())
named_target = {f for f in names if modules.get(f, ('', ''))[0] in TARGET}
print('named', len(names), dict(tally))
print('named in FromSoftware or unlabelled code', len(named_target), 'of which longer than 5 instructions', len(named_target & set(big)), 'of', len(big))
print('with a source file', len(files), 'of', len(functions))

