"""Localiza la vida de un enemigo en EDF6.exe y la instrucción que la escribe.

Uso:
    python hpscan.py snap  <pid> <snapshot>
    python hpscan.py diff  <pid> <snapshot> <daño> <candidatos.pkl> [tolerancia]
    python hpscan.py next  <pid> <candidatos.pkl> <daño> <candidatos.pkl> [tolerancia]
    python hpscan.py same  <pid> <candidatos.pkl> <candidatos.pkl>
    python hpscan.py show  <pid> <candidatos.pkl>
    python hpscan.py dump  <pid> <address_hex> <bytes_antes> <bytes_despues>
    python hpscan.py watch <pid> <address_hex|Modulo.dll+rva> <segundos>
    python hpscan.py trace_damage <pid> <Modulo.dll+rva> <segundos> <out.pkl>

snap/diff comparan contra una foto completa del heap escribible: la vida de
un enemigo tiene que haber bajado exactamente el daño que muestra el juego.
watch pone un breakpoint de hardware de escritura sobre la dirección y
reporta cada instrucción que la tocó (RIP es la instrucción siguiente).
trace_damage pone un breakpoint de ejecución en la escritura de la vida
(EDF.dll+0x54817a) y registra objetivo, atacante e info de cada impacto.
"""
import array
import ctypes
import os
import pickle
import struct
import sys
import time
from collections import Counter
from ctypes import wintypes as wt

PROCESS_ALL_ACCESS = 0x1F0FFF
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
MEM_PRIVATE = 0x20000
PAGE_GUARD = 0x100
WRITABLE_PROTECTIONS = {0x04, 0x08, 0x40, 0x80}
CHUNK = 1 << 16

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wt.DWORD),
        ("PartitionId", wt.WORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
    ]


def open_process(pid, access=PROCESS_QUERY_INFORMATION | PROCESS_VM_READ):
    h = k32.OpenProcess(access, False, pid)
    if not h:
        raise SystemExit("no pude abrir el proceso %d (correr como admin?)" % pid)
    return h


def read(h, base, size):
    buf = ctypes.create_string_buffer(size)
    n = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(base), buf, size, ctypes.byref(n)):
        return b""
    return buf.raw[: n.value]


def writable_regions(h):
    addr = 0
    mbi = MEMORY_BASIC_INFORMATION()
    while k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        base = mbi.BaseAddress or 0
        if (
            mbi.State == MEM_COMMIT
            and mbi.Type == MEM_PRIVATE
            and (mbi.Protect & 0xFF) in WRITABLE_PROTECTIONS
            and not (mbi.Protect & PAGE_GUARD)
        ):
            yield base, mbi.RegionSize
        addr = base + mbi.RegionSize
        if addr <= 0:
            break


def default_tolerance(damage):
    return max(0.51, damage * 0.005)


def damage_matches(before, after, damage, tolerance):
    return after >= 0 and before > after and abs((before - after) - damage) <= tolerance


def read_float(h, addr):
    data = read(h, addr, 4)
    return struct.unpack("<f", data)[0] if len(data) == 4 else None


def save_candidates(path, candidates):
    with open(path, "wb") as f:
        pickle.dump(candidates, f)


def load_candidates(path):
    with open(path, "rb") as f:
        return pickle.load(f)


def print_candidates(candidates, limit=40):
    print("candidatos: %d" % len(candidates))
    for addr, value in list(candidates.items())[:limit]:
        print("  %016x  %.3f" % (addr, value))


def cmd_snap(pid, snapshot):
    h = open_process(pid)
    index = []
    total = 0
    started = time.time()
    with open(snapshot + ".bin", "wb") as data_file:
        for base, size in writable_regions(h):
            data = read(h, base, size)
            if not data:
                continue
            index.append((base, len(data), data_file.tell()))
            data_file.write(data)
            total += len(data)
    with open(snapshot + ".idx", "wb") as f:
        pickle.dump(index, f)
    print("regiones: %d, %.0f MB, %.1f s" % (len(index), total / 2**20, time.time() - started))


def cmd_diff(pid, snapshot, damage, out, tolerance):
    h = open_process(pid)
    with open(snapshot + ".idx", "rb") as f:
        index = pickle.load(f)
    candidates = {}
    changed_chunks = 0
    started = time.time()
    with open(snapshot + ".bin", "rb") as data_file:
        for base, size, offset in index:
            current = read(h, base, size)
            if len(current) != size:
                continue
            data_file.seek(offset)
            previous = data_file.read(size)
            for start in range(0, size - size % 4, CHUNK):
                end = min(start + CHUNK, size - size % 4)
                if previous[start:end] == current[start:end]:
                    continue
                changed_chunks += 1
                old = array.array("f", previous[start:end])
                new = array.array("f", current[start:end])
                for i, (before, after) in enumerate(zip(old, new)):
                    if before != after and damage_matches(before, after, damage, tolerance):
                        candidates[base + start + i * 4] = after
    save_candidates(out, candidates)
    print("bloques cambiados: %d, %.1f s" % (changed_chunks, time.time() - started))
    print_candidates(candidates)


def cmd_next(pid, infile, damage, out, tolerance):
    h = open_process(pid)
    candidates = {}
    for addr, before in load_candidates(infile).items():
        after = read_float(h, addr)
        if after is not None and damage_matches(before, after, damage, tolerance):
            candidates[addr] = after
    save_candidates(out, candidates)
    print_candidates(candidates)


def cmd_same(pid, infile, out):
    h = open_process(pid)
    candidates = {}
    for addr, before in load_candidates(infile).items():
        after = read_float(h, addr)
        if after == before:
            candidates[addr] = after
    save_candidates(out, candidates)
    print_candidates(candidates)


def cmd_show(pid, infile):
    h = open_process(pid)
    candidates = load_candidates(infile)
    print("candidatos: %d" % len(candidates))
    for addr, stored in list(candidates.items())[:40]:
        print("  %016x  guardado=%.3f  ahora=%s" % (addr, stored, read_float(h, addr)))


def cmd_dump(pid, addr_hex, before, after):
    h = open_process(pid)
    target = int(addr_hex, 16)
    start = target - int(before)
    data = read(h, start, int(before) + int(after))
    for offset in range(0, len(data) - 7, 8):
        addr = start + offset
        f1, f2 = struct.unpack_from("<ff", data, offset)
        q = struct.unpack_from("<Q", data, offset)[0]
        marker = "  <== target" if target in (addr, addr + 4) else ""
        print("%016x %+6x  q=%016x  f=%14.3f %14.3f%s" % (addr, addr - target, q, f1, f2, marker))


EXCEPTION_DEBUG_EVENT = 1
CREATE_THREAD_DEBUG_EVENT = 2
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_THREAD_DEBUG_EVENT = 4
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
EXCEPTION_BREAKPOINT = 0x80000003
EXCEPTION_SINGLE_STEP = 0x80000004
CONTEXT_DEBUG_REGISTERS = 0x00100010
CONTEXT_FULL_WITH_DEBUG = 0x0010001F
CONTEXT_SIZE = 1232
CONTEXT_OFFSETS = {
    "ContextFlags": 0x30, "Dr0": 0x48, "Dr6": 0x68, "Dr7": 0x70,
    "Rax": 0x78, "Rcx": 0x80, "Rdx": 0x88, "Rbx": 0x90, "Rsp": 0x98, "Rbp": 0xA0,
    "Rsi": 0xA8, "Rdi": 0xB0, "R8": 0xB8, "R9": 0xC0, "R10": 0xC8, "R11": 0xD0,
    "R12": 0xD8, "R13": 0xE0, "R14": 0xE8, "R15": 0xF0, "Rip": 0xF8,
}
GENERAL_REGISTERS = ["Rax", "Rcx", "Rdx", "Rbx", "Rsp", "Rbp", "Rsi", "Rdi",
                     "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"]
DR7_WRITE_4_BYTES_SLOT0 = 1 | (1 << 16) | (3 << 18)
DR7_EXECUTE_SLOT0 = 1
EFLAGS_OFFSET = 0x44
RESUME_FLAG = 0x10000
XMM0_OFFSET = 0x1A0


class DEBUG_EVENT(ctypes.Structure):
    _fields_ = [
        ("dwDebugEventCode", wt.DWORD),
        ("dwProcessId", wt.DWORD),
        ("dwThreadId", wt.DWORD),
        ("padding", wt.DWORD),
        ("u", ctypes.c_ubyte * 160),
    ]


class ThreadContext:
    def __init__(self):
        self.raw = ctypes.create_string_buffer(CONTEXT_SIZE + 16)
        self.offset = (-ctypes.addressof(self.raw)) % 16
        self.address = ctypes.addressof(self.raw) + self.offset

    def get(self, name):
        return ctypes.c_uint64.from_address(self.address + CONTEXT_OFFSETS[name]).value

    def set(self, name, value):
        ctypes.c_uint64.from_address(self.address + CONTEXT_OFFSETS[name]).value = value

    def load(self, thread, flags):
        ctypes.memset(self.address, 0, CONTEXT_SIZE)
        ctypes.c_uint32.from_address(self.address + CONTEXT_OFFSETS["ContextFlags"]).value = flags
        return k32.GetThreadContext(thread, ctypes.c_void_p(self.address))

    def store(self, thread):
        return k32.SetThreadContext(thread, ctypes.c_void_p(self.address))


def set_breakpoint(thread, address, dr7):
    ctx = ThreadContext()
    if not ctx.load(thread, CONTEXT_DEBUG_REGISTERS):
        return False
    ctx.set("Dr0", address)
    ctx.set("Dr6", 0)
    ctx.set("Dr7", dr7 if address else 0)
    return bool(ctx.store(thread))


def module_bases(h):
    modules = (ctypes.c_void_p * 1024)()
    needed = wt.DWORD(0)
    psapi.EnumProcessModulesEx(h, modules, ctypes.sizeof(modules), ctypes.byref(needed), 3)
    bases = {}
    for module in modules[: needed.value // ctypes.sizeof(ctypes.c_void_p)]:
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(h, ctypes.c_void_p(module), name, 260)
        bases[name.value] = module
    return bases


def describe_address(bases, address):
    best = None
    for name, base in bases.items():
        if base <= address and (best is None or base > best[1]):
            best = (name, base)
    return "%s+%#x" % (best[0], address - best[1]) if best else hex(address)


def resolve_address(bases, text):
    if "+" in text:
        module, offset = text.split("+")
        return bases[module] + int(offset, 16)
    return int(text, 16)


def debug_session(pid, process, address, dr7, seconds, on_hit):
    if not k32.DebugActiveProcess(pid):
        raise SystemExit("DebugActiveProcess falló: %d" % ctypes.get_last_error())
    k32.DebugSetProcessKillOnExit(False)
    threads = {}
    armed = False
    deadline = time.time() + float(seconds)
    event = DEBUG_EVENT()
    try:
        while time.time() < deadline:
            if not k32.WaitForDebugEvent(ctypes.byref(event), 100):
                continue
            code = event.dwDebugEventCode
            status = DBG_CONTINUE
            union = ctypes.addressof(event.u)
            if code == CREATE_PROCESS_DEBUG_EVENT:
                k32.CloseHandle(ctypes.c_void_p.from_address(union).value)
                threads[event.dwThreadId] = ctypes.c_void_p.from_address(union + 16).value
            elif code == CREATE_THREAD_DEBUG_EVENT:
                thread = ctypes.c_void_p.from_address(union).value
                threads[event.dwThreadId] = thread
                if armed:
                    set_breakpoint(thread, address, dr7)
            elif code == EXIT_THREAD_DEBUG_EVENT:
                threads.pop(event.dwThreadId, None)
            elif code == LOAD_DLL_DEBUG_EVENT:
                k32.CloseHandle(ctypes.c_void_p.from_address(union).value)
            elif code == EXIT_PROCESS_DEBUG_EVENT:
                print("el juego se cerró")
                return
            elif code == EXCEPTION_DEBUG_EVENT:
                exception_code = ctypes.c_uint32.from_address(union).value
                if exception_code == EXCEPTION_BREAKPOINT and not armed:
                    armed_threads = sum(set_breakpoint(t, address, dr7) for t in threads.values())
                    armed = True
                    print("breakpoint armado en %d/%d hilos sobre %016x" % (armed_threads, len(threads), address), flush=True)
                elif exception_code == EXCEPTION_SINGLE_STEP and event.dwThreadId in threads:
                    thread = threads[event.dwThreadId]
                    ctx = ThreadContext()
                    ctx.load(thread, CONTEXT_FULL_WITH_DEBUG)
                    if ctx.get("Dr6") & 1:
                        on_hit(ctx)
                        ctx.set("Dr6", 0)
                        if dr7 == DR7_EXECUTE_SLOT0:
                            eflags = ctypes.c_uint32.from_address(ctx.address + EFLAGS_OFFSET)
                            eflags.value |= RESUME_FLAG
                        ctx.store(thread)
                    else:
                        status = DBG_EXCEPTION_NOT_HANDLED
                else:
                    status = DBG_EXCEPTION_NOT_HANDLED
            k32.ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status)
    finally:
        for thread in threads.values():
            k32.SuspendThread(thread)
            set_breakpoint(thread, 0, 0)
            k32.ResumeThread(thread)
        k32.DebugActiveProcessStop(pid)


def cmd_watch(pid, addr_text, seconds):
    process = open_process(pid, PROCESS_ALL_ACCESS)
    bases = module_bases(process)
    address = resolve_address(bases, addr_text)
    hits = Counter()
    samples = {}

    def on_write(ctx):
        rip = ctx.get("Rip")
        hits[rip] += 1
        if rip not in samples:
            samples[rip] = {
                "registers": {r: ctx.get(r) for r in GENERAL_REGISTERS},
                "code": read(process, rip - 32, 64),
                "value": read_float(process, address),
            }

    debug_session(pid, process, address, DR7_WRITE_4_BYTES_SLOT0, seconds, on_write)
    print("escrituras: %d desde %d instrucciones" % (sum(hits.values()), len(hits)))
    for rip, count in hits.most_common():
        sample = samples[rip]
        print("\n== RIP %s (%016x)  x%d  valor después=%s" % (describe_address(bases, rip), rip, count, sample["value"]))
        print("   código [-32..+32]: %s | %s" % (sample["code"][:32].hex(" "), sample["code"][32:].hex(" ")))
        registers = sample["registers"]
        for i in range(0, len(GENERAL_REGISTERS), 4):
            print("   " + "  ".join("%s=%016x" % (r, registers[r]) for r in GENERAL_REGISTERS[i : i + 4]))


def read_u64(process, addr):
    data = read(process, addr, 8)
    return struct.unpack("<Q", data)[0] if len(data) == 8 else 0


def xmm_low_float(ctx, index):
    return ctypes.c_float.from_address(ctx.address + XMM0_OFFSET + 16 * index).value


def cmd_trace_damage(pid, addr_text, seconds, out):
    process = open_process(pid, PROCESS_ALL_ACCESS)
    bases = module_bases(process)
    address = resolve_address(bases, addr_text)
    records = []

    def on_execute(ctx):
        target = ctx.get("Rdi")
        info = ctx.get("R13")
        attacker = read_u64(process, info + 0x10)
        record = {
            "time": time.time(),
            "target": target,
            "target_vtable": read_u64(process, target),
            "target_blob": read(process, target, 0x400),
            "info": info,
            "info_blob": read(process, info, 0x80),
            "attacker": attacker,
            "attacker_vtable": read_u64(process, attacker) if attacker else 0,
            "attacker_blob": read(process, attacker, 0x400) if attacker else b"",
            "new_hp": xmm_low_float(ctx, 0),
            "delta": xmm_low_float(ctx, 6),
        }
        records.append(record)
        print("%7.2f  target=%016x (%s)  attacker=%016x (%s)  delta=%10.1f  hp=%10.1f" % (
            record["time"] % 1000, target, describe_address(bases, record["target_vtable"]),
            attacker, describe_address(bases, record["attacker_vtable"]) if attacker else "-",
            record["delta"], record["new_hp"]), flush=True)

    debug_session(pid, process, address, DR7_EXECUTE_SLOT0, seconds, on_execute)
    with open(out, "wb") as f:
        pickle.dump({"bases": bases, "records": records}, f)
    print("impactos: %d -> %s" % (len(records), out))


def tolerance_arg(args, position):
    return float(args[position]) if len(args) > position else default_tolerance(float(args[3]))


if __name__ == "__main__":
    args = sys.argv[1:]
    commands = {
        "snap": lambda: cmd_snap(int(args[1]), args[2]),
        "diff": lambda: cmd_diff(int(args[1]), args[2], float(args[3]), args[4], tolerance_arg(args, 5)),
        "next": lambda: cmd_next(int(args[1]), args[2], float(args[3]), args[4], tolerance_arg(args, 5)),
        "same": lambda: cmd_same(int(args[1]), args[2], args[3]),
        "show": lambda: cmd_show(int(args[1]), args[2]),
        "dump": lambda: cmd_dump(int(args[1]), args[2], args[3], args[4]),
        "watch": lambda: cmd_watch(int(args[1]), args[2], args[3]),
        "trace_damage": lambda: cmd_trace_damage(int(args[1]), args[2], args[3], args[4]),
    }
    if not args or args[0] not in commands:
        print(__doc__)
        raise SystemExit(1)
    commands[args[0]]()
