set pagination off
set confirm off
set debuginfod enabled off
set print thread-events off
set logging file reports/resolve-session.log
set logging overwrite on
set logging enabled on
set disable-randomization off
set non-stop off
handle SIGABRT stop print pass
handle SIGSEGV stop print pass
break *0x00ec0c70
run
set scheduler-locking on
python
import gdb
import hashlib
import os
import struct
from collections import deque

ROOT = os.path.abspath(os.getcwd())
RESOLVE = "/opt/resolve/bin/resolve"
CONTROLLER = ROOT + "/build/clang-libcxx/libdavinci_av1_session_controller.so.0.1.0"
BOOTSTRAP = ROOT + "/build/clang-libcxx/libdavinci_av1_bootstrap.so.0.1.0"
RUNTIME = ROOT + "/build/clang-libcxx/libdavinci_codec_runtime.so.0.1.0"
FFMPEG_ROOT = ROOT + "/build/private-ffmpeg"
MANIFEST = ROOT + "/build/clang-libcxx/artifact-manifest.txt"
with open(MANIFEST, "r", encoding="ascii") as manifest_stream:
    ARTIFACT_HASHES = dict(
        line.strip().split("=", 1)
        for line in manifest_stream
        if "=" in line
    )
CONTROLLER_SHA256 = ARTIFACT_HASHES["controller_sha256"]
RESOLVE_SHA256 = "d083cf9f1eb875bdb928db54b20425b4111b53a16cae72a1aef1c3eeb603a121"

passed = []
errors = []

def require(condition, message):
    if condition:
        passed.append(message)
        print("PASS:", message)
    else:
        errors.append(message)
        print("FAIL:", message)

def abort_probe(stage):
    print("SESSION_CONTROLLER_PROBE_FAILED_AT", stage)
    print("SESSION_CONTROLLER_PROBE_CHECKS", len(passed))
    print("SESSION_CONTROLLER_PROBE_FAILURES", len(errors))
    gdb.execute("kill")
    gdb.execute("quit 1")

def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def u64(inferior, address):
    return int.from_bytes(inferior.read_memory(address, 8).tobytes(), "little")

def u32(inferior, address):
    return int.from_bytes(inferior.read_memory(address, 4).tobytes(), "little")

def u8(inferior, address):
    return inferior.read_memory(address, 1).tobytes()[0]

def capture_map(inferior):
    map_address = 0x29537cf0
    sentinel = map_address + 8
    root = u64(inferior, sentinel)
    reported = u64(inferior, map_address + 0x10)
    nodes = set()
    av01_values = []

    def walk(node):
        if node == 0:
            return
        if node in nodes:
            raise RuntimeError("red-black map contains a cycle or duplicate node")
        nodes.add(node)
        key = u32(inferior, node + 0x1c)
        value = u32(inferior, node + 0x20)
        if key == 0x61763031:
            av01_values.append(value)
        walk(u64(inferior, node))
        walk(u64(inferior, node + 8))

    walk(root)
    if reported != 71 or len(nodes) != 71 or av01_values:
        raise RuntimeError("pre-controller map is not the expected 71-node/no-av01 tree")
    header = inferior.read_memory(map_address, 0x18).tobytes()
    records = tuple((node, inferior.read_memory(node, 0x28).tobytes())
                    for node in sorted(nodes))
    return (header, records)

inferior = gdb.selected_inferior()
threads = inferior.threads()
require(inferior.pid > 0, "Resolve diagnostic child is alive at main entry")
require(len(threads) == 1, "exactly one Resolve thread exists before controller load")
require(all(t.is_stopped() and not t.is_running() for t in threads),
        "all Resolve threads are stopped at the validated main entry")
if errors:
    abort_probe("main-entry thread gate")

require(sha256(RESOLVE) == RESOLVE_SHA256,
        "Resolve executable fingerprint matches the inspected build")
require(sha256(CONTROLLER) == CONTROLLER_SHA256,
        "session-controller DSO SHA-256 matches the audited artifact")
if errors:
    abort_probe("artifact fingerprint gate")

try:
    map_before = capture_map(inferior)
    callback_before = inferior.read_memory(0x05e3deb0, 8).tobytes()
    require(callback_before == bytes.fromhex("53 83 ff 01 75 28 48 89"),
            "AV1 callback entry has the expected pristine eight bytes")
except Exception as exc:
    require(False, "initial Resolve map/callback snapshot readable: " + repr(exc))
    abort_probe("initial target snapshot")

controller_handle = int(gdb.parse_and_eval(
    '(unsigned long long)dlopen("' + CONTROLLER + '", 2)'))
require(controller_handle != 0,
        "session-controller DSO loads into the Resolve base namespace")
if not controller_handle:
    abort_probe("controller dlopen")

controller_lines = [line for line in
                    gdb.execute("info sharedlibrary", to_string=True).splitlines()
                    if "libdavinci_av1_session_controller.so.0.1.0" in line]
require(len(controller_lines) == 1,
        "controller appears once in the Resolve base link-map")

def symbol(name):
    address = int(gdb.parse_and_eval(
        '(unsigned long long)dlsym((void*)' + hex(controller_handle) +
        ', "' + name + '")'))
    require(address != 0, "controller exports " + name)
    return address

abi_fn = symbol("davinci_av1_session_controller_abi_version")
initialize_fn = symbol("davinci_av1_session_controller_initialize")
status_fn = symbol("davinci_av1_session_controller_get_status")
plan_fn = symbol("davinci_av1_session_controller_get_plan")
prepare_fn = symbol("davinci_av1_session_controller_prepare")
execute_fn = symbol("davinci_av1_session_controller_execute")
rollback_fn = symbol("davinci_av1_session_controller_rollback")

abi_version = int(gdb.parse_and_eval(
    '((unsigned int (*)(void))' + hex(abi_fn) + ')()')) if abi_fn else 0
require(abi_version == 1, "controller reports ABI version 1")
if errors:
    abort_probe("controller ABI")

# Config V1 is a fixed 856-byte record. This scratch mapping belongs only to
# the disposable Resolve child and is not part of the transaction target.
scratch = int(gdb.parse_and_eval(
    '(unsigned long long)mmap(0, 16384, 3, 0x22, -1, 0)'))
require(scratch not in (0, 0xffffffffffffffff),
        "temporary anonymous config/status scratch mapping allocated")
if scratch in (0, 0xffffffffffffffff):
    abort_probe("scratch allocation")

project_bytes = ROOT.encode() + b"\0"
resolve_bytes = RESOLVE.encode() + b"\0"
config = (
    struct.pack("<QIIII", 0x4156315343434631, 1, 856, 0, 0) +
    project_bytes.ljust(512, b"\0") +
    resolve_bytes.ljust(256, b"\0") +
    bytes(64)
)
require(len(config) == 856, "controller config ABI has exact 856-byte size")
inferior.write_memory(scratch, config)

initialize_result = int(gdb.parse_and_eval(
    '((int (*)(const void *))' + hex(initialize_fn) +
    ')((void*)' + hex(scratch) + ')')) if initialize_fn else -999
require(initialize_result == 0,
        "controller verifies artifacts, loads bootstrap/provider/private FFmpeg, "
        "validates Resolve target owners, and binds the callback")

status_address = scratch + 1024
status_result = int(gdb.parse_and_eval(
    '((int (*)(void *))' + hex(status_fn) +
    ')((void*)' + hex(status_address) + ')')) if status_fn else -999
require(status_result == 0, "controller status query succeeds")

if status_result == 0:
    raw_status = inferior.read_memory(status_address, 48).tobytes()
    fields = struct.unpack("<QIIiIIIIII4x", raw_status)
    (magic, version, struct_size, result, state, provider_ready,
     callback_configured, reserved, private_version, reserved2) = fields
    require(magic == 0x4156315343535431 and version == 1 and struct_size == 48,
            "controller status ABI has the expected magic/version/size")
    require(result == 0 and state == 2,
            "controller reaches BOUND state without entering transaction execution")
    require(provider_ready == 1 and private_version == 3940198,
            "private FFmpeg 6.1.6/libavcodec 60.31.102 provider is ready")
    require(callback_configured == 1,
            "callback policy has immutable bindings to the real Resolve factories")
    require(reserved == 0 and reserved2 == 0,
            "controller status reserved fields remain zero")

# Build the final mapped-image attestation only after the controller has loaded
# bootstrap, provider and both private FFmpeg DSOs. The helper reads maps/files
# only; the packed 192-byte record is written into our anonymous scratch page.
import sys
sys.path.insert(0, ROOT + "/installer/gdb")
from resolve_attestation_builder import build_preflight_attestation

operator_new = u64(inferior, 0x2904e6e8)
sized_delete = u64(inferior, 0x2904c688)
proxy_base = operator_new - 0x26e0
scalable_malloc = u64(inferior, proxy_base + 0x5058)
safe_free = u64(inferior, proxy_base + 0x5030)

attestation = build_preflight_attestation(
    inferior.pid, 0x29537cf0, 0x05e3deb0,
    operator_new, sized_delete, scalable_malloc, safe_free)
attestation_address = scratch + 2048
inferior.write_memory(attestation_address, attestation)
require(len(attestation) == 192,
        "final mapped-image ABI-v3 attestation has exact 192-byte layout")

prepare_status_address = scratch + 2560
prepare_result = int(gdb.parse_and_eval(
    '((int (*)(const void *, void *))' + hex(prepare_fn) +
    ')((void*)' + hex(attestation_address) + ', (void*)' +
    hex(prepare_status_address) + ')')) if prepare_fn else -999
require(prepare_result == 0,
        "controller final image-set validation, Resolve structural validation "
        "and one-shot read-only transaction snapshot preparation succeed")

if prepare_result == 0:
    raw_prepare = inferior.read_memory(prepare_status_address, 48).tobytes()
    tx = struct.unpack("<QIIiIII16s", raw_prepare)
    (tx_magic, tx_version, tx_size, tx_result, tx_state,
     tx_stage, tx_reserved, tx_nonce) = tx
    require(tx_magic == 0x4156315458535331 and
            tx_version == 1 and tx_size == 48,
            "bootstrap transaction status ABI has exact magic/version/size")
    require(tx_result == 0 and tx_state == 2 and tx_stage == 2,
            "bootstrap reached PREFLIGHT_READY with the complete snapshot captured")
    require(tx_reserved == 0 and len(tx_nonce) == 16 and any(tx_nonce),
            "transaction status contains a nonzero session nonce and zero reserved field")

    plan_address = scratch + 2816
    plan_result = int(gdb.parse_and_eval(
        '((int (*)(const void *, void *))' + hex(plan_fn) +
        ')((void*)' + hex(attestation_address) + ', (void*)' +
        hex(plan_address) + ')')) if plan_fn else -999
    require(plan_result == 0,
            "controller returns the immutable production transaction binding plan")
    if plan_result == 0:
        plan = inferior.read_memory(plan_address, 192).tobytes()
        plan_header = struct.unpack_from("<QIIII", plan, 0)
        (plan_magic, plan_version, plan_size, plan_state, plan_reserved) = plan_header
        require(plan_magic == 0x4156315343504c31 and
                plan_version == 1 and plan_size == 192 and
                plan_state == 3 and plan_reserved == 0,
                "controller plan ABI is exact and reports PREFLIGHT_READY")
        context, plan_new, plan_delete, plan_rebalancer, plan_callback_entry, plan_callback = struct.unpack_from(
            "<QQQQQQ", plan, 24)
        wrappers = struct.unpack_from("<8Q", plan, 72)
        require(context == 0 and plan_new == operator_new and
                plan_delete == sized_delete and
                plan_rebalancer == 0x00ecdba0 and
                plan_callback_entry == 0x05e3deb0,
                "transaction plan carries the exact attested allocator, rebalancer and callback entry")
        require(plan_callback != 0 and all(wrappers),
                "transaction plan contains the callback and all eight provider wrappers")
        callback_api = struct.unpack_from("<IIQQQQQQ", plan, 136)
        (callback_abi, callback_size, config_getter, software_helper,
         nvidia_factory, generic_factory, aom_factory,
         callback_reserved) = callback_api
        require(callback_abi == 1 and callback_size == 56 and
                callback_reserved == 0,
                "callback binding plan has the expected v1 ABI and zero reserved field")
        require((config_getter, software_helper, nvidia_factory,
                 generic_factory, aom_factory) ==
                (0x00ef4cf0, 0x05e444f0, 0x05f2cb30,
                 0x05eeef00, 0x05f90830),
                "callback plan targets the exact Resolve singleton, setting helper and three factories")

        def mapped_executable_owner(address, maps_text):
            for line in maps_text.splitlines():
                fields = line.split(None, 5)
                if len(fields) < 6 or "x" not in fields[1]:
                    continue
                start_text, end_text = fields[0].split("-", 1)
                if int(start_text, 16) <= address < int(end_text, 16):
                    return os.path.realpath(fields[5])
            return None

        plan_maps = open("/proc/%d/maps" % inferior.pid, "r").read()
        require(mapped_executable_owner(plan_callback, plan_maps) ==
                os.path.realpath(RUNTIME),
                "replacement callback pointer belongs to the pinned codec-runtime DSO")
        require(all(mapped_executable_owner(address, plan_maps) ==
                    os.path.realpath(RUNTIME) for address in wrappers),
                "all eight wrapper targets belong to the pinned codec-runtime DSO")

# This probe is intentionally write-capable only in a disposable child. Its
# shell runner requires the explicit --authorize-live-av1-patch flag. The child
# is stopped at Resolve main entry and independently proven to have one thread.
authorization = b"I AUTHORIZE AV1 PATCH IN THIS RESOLVE PROCESS"
token_address = scratch + 3072
execution_address = scratch + 3136
inferior.write_memory(token_address, authorization)
inferior.write_memory(execution_address, bytes(64))

call_sites = [
    0x05eef886, 0x05eef92c, 0x05eefa86, 0x05eefbc4, 0x05eefbce,
    0x05ef0006, 0x05ef005d, 0x05ef030f, 0x05ef066f, 0x05ef0678,
]
dispatch_indices = [0, 1, 2, 3, 4, 5, 6, 7, 3, 4]
call_bytes_before = tuple(inferior.read_memory(address, 5).tobytes()
                          for address in call_sites)
expected_original = [
    bytes.fromhex("e8 35 7c 90 fa"), bytes.fromhex("e8 df bc 91 fa"),
    bytes.fromhex("e8 15 57 91 fa"), bytes.fromhex("e8 67 03 91 fa"),
    bytes.fromhex("e8 9d 96 91 fa"), bytes.fromhex("e8 d5 87 90 fa"),
    bytes.fromhex("e8 3e 91 91 fa"), bytes.fromhex("e8 bc 72 90 fa"),
    bytes.fromhex("e8 bc f8 90 fa"), bytes.fromhex("e8 f3 8b 91 fa"),
]
require(call_bytes_before == tuple(expected_original),
        "all ten original generic-video-decoder CALL instructions match the pinned bytes")
if errors:
    abort_probe("live transaction CALL-site baseline")

# Check that the pre-main stop is also outside dynamic-loader activity. The
# controller/provider were loaded earlier; they must have returned to CONSISTENT.
try:
    r_debug = int(gdb.parse_and_eval("(unsigned long long)&_r_debug"))
    require(u32(inferior, r_debug + 24) == 0,
            "dynamic loader is RT_CONSISTENT immediately before live transaction")
except Exception as exc:
    require(False, "loader state readable before live transaction: " + repr(exc))
if errors:
    abort_probe("live transaction loader gate")

selected_pc = int(gdb.parse_and_eval("(unsigned long long)$pc"))
current_threads = inferior.threads()
require(selected_pc == 0x00ec0c70,
        "transaction is still at the exact first-instruction main-entry breakpoint")
require(len(current_threads) == 1 and
        all(thread.is_stopped() and not thread.is_running()
            for thread in current_threads),
        "transaction begins with exactly one stopped Resolve thread and no TBB worker")
if errors:
    abort_probe("pre-commit single-thread main-entry gate")

# Call the same exported bootstrap transaction function the controller
# forwards to, so a pre-executor rejection retains its precise bootstrap error
# code instead of being flattened to the controller's generic -15 result.
bootstrap_handle = int(gdb.parse_and_eval(
    '(unsigned long long)dlopen("' + BOOTSTRAP + '", 2)'))
require(bootstrap_handle != 0,
        "bootstrap DSO resolves to the same loaded transaction instance")
bootstrap_execute_fn = int(gdb.parse_and_eval(
    '(unsigned long long)dlsym((void*)' + hex(bootstrap_handle) +
    ', "davinci_av1_bootstrap_execute_prepared_transaction")'))
bootstrap_rollback_fn = int(gdb.parse_and_eval(
    '(unsigned long long)dlsym((void*)' + hex(bootstrap_handle) +
    ', "davinci_av1_bootstrap_rollback_prepared_transaction")'))
require(bootstrap_execute_fn != 0 and bootstrap_rollback_fn != 0,
        "bootstrap exposes its authorized production execute and rollback entrypoints")
if errors:
    abort_probe("bootstrap transaction entrypoint resolution")

bootstrap_base = None
for line in open("/proc/%d/maps" % inferior.pid, "r", encoding="ascii"):
    fields = line.rstrip("\n").split(None, 5)
    if (len(fields) == 6 and
            os.path.basename(fields[5].removesuffix(" (deleted)")) ==
            os.path.basename(BOOTSTRAP) and int(fields[2], 16) == 0):
        bootstrap_base = int(fields[0].split("-", 1)[0], 16)
        break
require(bootstrap_base is not None,
        "bootstrap mapping base is available for rollback diagnostics")
if bootstrap_base is None:
    abort_probe("bootstrap mapping base")
copy_prepared_fn = bootstrap_base + 0x34e0
revalidate_prepared_fn = bootstrap_base + 0x3640
copied_attestation = scratch + 4096
copied_snapshot = scratch + 4352
copied_nonce = copied_snapshot + 8528
copy_prepared_result = int(gdb.parse_and_eval(
    '((int (*)(void *, void *, unsigned long, void *))' +
    hex(copy_prepared_fn) + ')((void *)' + hex(copied_attestation) +
    ', (void *)' + hex(copied_snapshot) + ', 8528, (void *)' +
    hex(copied_nonce) + ')'))
require(copy_prepared_result == 0,
        "read-only copy of transaction snapshot is available for rollback diagnostics")
if errors:
    abort_probe("copy prepared snapshot before commit")

execute_result = int(gdb.parse_and_eval(
    '((int (*)(const char *, unsigned long, const void *, void *))' +
    hex(bootstrap_execute_fn) + ')((const char *)' + hex(token_address) +
    ', ' + str(len(authorization)) + ', (const void *)' +
    hex(plan_address + 24) + ', (void *)' + hex(execution_address) + ')'))
print("LIVE_EXECUTE_RESULT", execute_result)
execution_raw = inferior.read_memory(execution_address, 64).tobytes()
execution_fields = struct.unpack("<Q9I4xQQ", execution_raw)
(exec_magic, exec_version, exec_size, exec_state, call_mask, writable_mask,
 node_allocated, node_linked, callback_published, exec_reserved,
 relay_page, inserted_node) = execution_fields
print("LIVE_EXECUTION_RECORD", execution_fields)
if execute_result != 0 or exec_state != 2:
    # Never dereference a relay/node pointer after a failed execute. Capture the
    # surviving transaction/controller state and exact target bytes instead.
    status_diag = scratch + 3520
    status_diag_result = int(gdb.parse_and_eval(
        '((int (*)(void *))' + hex(status_fn) +
        ')((void *)' + hex(status_diag) + ')'))
    if status_diag_result == 0:
        diag_fields = struct.unpack(
            "<QIIiIIIIII4x", inferior.read_memory(status_diag, 48).tobytes())
        print("LIVE_CONTROLLER_STATUS_AFTER_EXECUTE", diag_fields)
    print("LIVE_CALLBACK_AFTER_EXECUTE",
          inferior.read_memory(0x05e3deb0, 8).tobytes().hex())
    print("LIVE_CALLS_AFTER_EXECUTE", tuple(
        inferior.read_memory(address, 5).tobytes().hex()
        for address in call_sites))
    try:
        print("LIVE_MAP_AFTER_EXECUTE", capture_map(inferior))
    except Exception as exc:
        print("LIVE_MAP_DIAGNOSTIC_ERROR", repr(exc))
    print("LIVE_EXECUTE_FAILURE_STAGE: transaction is not committed; stopping disposable inferior")
    gdb.execute("kill")
    gdb.execute("quit 1")

require(execute_result == 0,
        "production executor commits the AV1 map/CALL/callback transaction in the disposable live Resolve child")

require(exec_magic == 0x4156314558454331 and exec_version == 1 and
        exec_size == 64 and exec_state == 2 and exec_reserved == 0,
        "live execution record has the expected committed ABI state")
require(call_mask == 0x3ff and writable_mask == 0 and
        node_allocated == 1 and node_linked == 1 and callback_published == 1 and
        relay_page != 0 and inserted_node != 0,
        "live commit reports ten CALL patches, linked map node, callback publication and RX-restored pages")

callback_after_commit = inferior.read_memory(0x05e3deb0, 8).tobytes()
require(callback_after_commit[:2] == b"\xff\x25" and
        callback_after_commit[6:] == b"\x90\x90",
        "live AV1 callback entry contains the aligned indirect-jump detour")
callback_relative = struct.unpack("<i", callback_after_commit[2:6])[0]
callback_cell = 0x05e3deb0 + 6 + callback_relative
require(callback_cell == relay_page + 0x100 and
        u64(inferior, callback_cell) == plan_callback,
        "live callback detour resolves through the relay cell to the pinned runtime callback")

# Verify the real Resolve map now has exactly one additional av01 -> 226 node.
def capture_live_map(expected_count, expected_av1):
    map_address = 0x29537cf0
    root = u64(inferior, map_address + 8)
    reported = u64(inferior, map_address + 0x10)
    nodes = set()
    av1_values = []
    def walk(node):
        if node == 0:
            return
        if node in nodes:
            raise RuntimeError("Resolve map contains a cycle or duplicate node")
        nodes.add(node)
        key = u32(inferior, node + 0x1c)
        value = u32(inferior, node + 0x20)
        if key == 0x61763031:
            av1_values.append(value)
        walk(u64(inferior, node))
        walk(u64(inferior, node + 8))
    walk(root)
    if reported != expected_count or len(nodes) != expected_count or av1_values != expected_av1:
        raise RuntimeError("live Resolve codec map has unexpected size or av01 mapping")
    header = inferior.read_memory(map_address, 0x18).tobytes()
    records = tuple((node, inferior.read_memory(node, 0x28).tobytes())
                    for node in sorted(nodes))
    return header, records

try:
    committed_map = capture_live_map(72, [226])
    require(True, "real Resolve FFmpeg map contains 72 valid nodes and av01 -> codec ID 226")
except Exception as exc:
    committed_map = None
    require(False, "real Resolve map after commit: " + repr(exc))

# The CALL instructions remain E8 rel32 but now land on the eight relay stubs.
patched_calls_ok = True
for index, (address, dispatch) in enumerate(zip(call_sites, dispatch_indices)):
    current = inferior.read_memory(address, 5).tobytes()
    if current[0] != 0xe8:
        patched_calls_ok = False
        break
    relative = struct.unpack("<i", current[1:5])[0]
    target = address + 5 + relative
    if target != relay_page + dispatch * 16:
        patched_calls_ok = False
        break
require(patched_calls_ok,
        "all ten live CALL sites resolve to their intended relay dispatch stubs")

wrappers_ok = True
for index, target in enumerate(wrappers):
    stub = relay_page + index * 16
    stub_bytes = inferior.read_memory(stub, 6).tobytes()
    if stub_bytes[:2] != b"\xff\x25":
        wrappers_ok = False
        break
    relative = struct.unpack("<i", stub_bytes[2:6])[0]
    cell = stub + 6 + relative
    if cell != relay_page + 0x180 + index * 8 or u64(inferior, cell) != target:
        wrappers_ok = False
        break
require(wrappers_ok,
        "all eight live relay stubs point to the exact pinned provider wrapper targets")

# Verify the relay is executable/read-only and the loader remains quiescent.
def mapping_permissions(address):
    with open("/proc/%d/maps" % inferior.pid, "r", encoding="ascii") as stream:
        for line in stream:
            fields = line.split(None, 2)
            begin, end = (int(part, 16) for part in fields[0].split("-", 1))
            if begin <= address < end:
                return fields[1]
    return None
require(mapping_permissions(relay_page) is not None and
        mapping_permissions(relay_page).startswith("r-x"),
        "relay page is mapped RX after successful commit")
try:
    require(u32(inferior, r_debug + 24) == 0,
            "dynamic loader remains RT_CONSISTENT after live commit")
except Exception as exc:
    require(False, "loader state readable after commit: " + repr(exc))

status_after_commit = int(gdb.parse_and_eval(
    '((int (*)(void *))' + hex(status_fn) +
    ')((void *)' + hex(status_address) + ')'))
if status_after_commit == 0:
    committed_status = struct.unpack(
        "<QIIiIIIIII4x", inferior.read_memory(status_address, 48).tobytes())
    controller_state_after_commit = committed_status[4]
else:
    controller_state_after_commit = -1
require(status_after_commit == 0 and controller_state_after_commit == 3,
        "controller remains at PREFLIGHT_READY while the bootstrap owns the direct transaction test")

# Keep the committed patch installed for this Resolve session. Lightweight
# entry breakpoints count actual callback/factory/provider activity without
# stopping the UI thread (stop() returns False immediately).
TRACE_DETAIL_FILE = "reports/resolve-codec-session-trace-details.log"
TRACE_RECENT_EVENTS = deque(maxlen=256)
with open(TRACE_DETAIL_FILE, "w", encoding="utf-8") as trace_file:
    trace_file.write("Resolve codec runtime trace details\n")

def trace_log(*parts):
    line = " ".join(str(part) for part in parts)
    TRACE_RECENT_EVENTS.append(line)
    try:
        with open(TRACE_DETAIL_FILE, "a", encoding="utf-8") as trace_file:
            trace_file.write(line + "\n")
    except Exception as exc:
        print("AV1_TRACE_FILE_ERROR", repr(exc))
    print(line)

def context_state(context_address, label):
    if not context_address:
        trace_log("AV1_CONTEXT_STATE", label, "context=NULL")
        return
    try:
        codec = u64(inferior, context_address + 0x10)
        extradata = u64(inferior, context_address + 0x58)
        extradata_size = u32(inferior, context_address + 0x60)
        subtitle_header = u64(inferior, context_address + 0x2b0)
        subtitle_header_size = u32(inferior, context_address + 0x2b8)
        internal = u64(inferior, context_address + 0x18)
        trace_log("AV1_CONTEXT_STATE", label, "context=" + hex(context_address),
                  "codec=" + hex(codec), "internal=" + hex(internal),
                  "extradata=" + hex(extradata),
                  "extradata_size=" + str(extradata_size),
                  "subtitle_header=" + hex(subtitle_header),
                  "subtitle_header_size=" + str(subtitle_header_size))
    except Exception as exc:
        trace_log("AV1_CONTEXT_STATE_ERROR", label, hex(context_address), repr(exc))

class RuntimeReturnBreakpoint(gdb.FinishBreakpoint):
    def __init__(self, frame, trace_name, context_address=0):
        super().__init__(frame, internal=True)
        self.trace_name = trace_name
        self.context_address = context_address

    def stop(self):
        try:
            result = int(gdb.parse_and_eval("$rax"))
            trace_log("AV1_RUNTIME_RETURN", self.trace_name, "result=" + str(result))
            if self.trace_name in ("avcodec_open2", "avcodec_close"):
                context_state(self.context_address,
                              self.trace_name + "_after_return")
        except Exception as exc:
            trace_log("AV1_RUNTIME_RETURN_ERROR", self.trace_name, repr(exc))
        return False

class RuntimeHitBreakpoint(gdb.Breakpoint):
    def __init__(self, name, address, argument_register=None):
        super().__init__("*" + hex(address), internal=True)
        self.trace_name = name
        self.argument_register = argument_register
        self.trace_hits = 0

    def stop(self):
        try:
            self.trace_hits += 1
            argument = 0
            if self.argument_register:
                try:
                    argument = int(gdb.parse_and_eval("$" + self.argument_register))
                except Exception:
                    argument = 0
            if self.trace_hits <= 20:
                try:
                    caller = u64(inferior, int(gdb.parse_and_eval("$rsp")))
                except Exception:
                    caller = 0
                trace_log("AV1_RUNTIME_TRACE", self.trace_name,
                          "count=" + str(self.trace_hits),
                          "arg=" + hex(argument),
                          "caller=" + hex(caller))

            if self.trace_name == "avcodec_close":
                context_state(argument, "before_close")
            elif self.trace_name == "avcodec_free_context":
                context_address = u64(inferior, argument) if argument else 0
                context_state(context_address, "before_free")
            elif self.trace_name in ("avcodec_open2", "avcodec_send_packet",
                                     "avcodec_receive_frame"):
                context_address = argument
                try:
                    RuntimeReturnBreakpoint(gdb.newest_frame(), self.trace_name,
                                             context_address)
                except Exception as exc:
                    trace_log("AV1_RETURN_TRACE_ARM_ERROR", self.trace_name, repr(exc))
        except Exception as exc:
            trace_log("AV1_RUNTIME_TRACE_HANDLER_ERROR", self.trace_name, repr(exc))
        return False

trace_points = [
    ("replacement_callback", plan_callback, "rdi"),
    ("resolve_nvidia_factory", 0x05f2cb30, "rdi"),
    ("resolve_generic_ffmpeg_factory", 0x05eeef00, "rdi"),
    ("resolve_aom_factory", 0x05f90830, "rdi"),
    ("avcodec_find_decoder", wrappers[0], "rdi"),
    ("avcodec_alloc_context3", wrappers[1], "rdi"),
    ("avcodec_open2", wrappers[2], "rdi"),
    ("avcodec_close", wrappers[3], "rdi"),
    ("avcodec_free_context", wrappers[4], "rdi"),
]
trace_breakpoints = []
for trace_name, trace_address, argument_register in trace_points:
    try:
        trace_breakpoints.append(RuntimeHitBreakpoint(
            trace_name, trace_address, argument_register))
        print("AV1_RUNTIME_TRACE_ARMED", trace_name, hex(trace_address))
    except Exception as exc:
        print("AV1_RUNTIME_TRACE_ARM_FAILED", trace_name, repr(exc))

if errors or len(trace_breakpoints) != len(trace_points):
    print("RESOLVE_AV1_SESSION_PRE_RESUME_CHECK_FAILED; rolling back before allowing Resolve to run")
    rollback_result = int(gdb.parse_and_eval(
        '((int (*)(const char *, unsigned long, const void *, void *))' +
        hex(bootstrap_rollback_fn) + ')((const char *)' + hex(token_address) +
        ', ' + str(len(authorization)) + ', (const void *)' +
        hex(plan_address + 24) + ', (void *)' + hex(execution_address) + ')'))
    print("RESOLVE_AV1_SESSION_PRE_RESUME_ROLLBACK_RESULT", rollback_result)
    gdb.execute("kill")
    gdb.execute("quit 1")

print("RESOLVE_AV1_SESSION_PATCH_ACTIVE; map av01->226; ten CALL sites and callback detour committed; relay RX; provider pinned")
print("RESOLVE_AV1_SESSION_PID", inferior.pid)
print("RESOLVE_AV1_SESSION_TRACE_POINTS", len(trace_breakpoints), "of", len(trace_points))
print("RESOLVE_AV1_SESSION_READY; use Resolve normally, import an AV1 clip and play it; close Resolve normally to end this session")
gdb.execute("set scheduler-locking off")
while inferior.is_valid() and inferior.pid:
    try:
        gdb.execute("continue")
    except gdb.error as exc:
        print("RESOLVE_AV1_SESSION_CONTINUE_ERROR", repr(exc))
        raise
    if inferior.is_valid() and inferior.pid:
        stop_info = gdb.execute("info program", to_string=True)
        trace_log("RESOLVE_AV1_SESSION_STOPPED", stop_info)
        fatal_signal = None
        for signal_name in ("SIGSEGV", "SIGABRT"):
            if signal_name in stop_info:
                fatal_signal = signal_name
                break
        if fatal_signal:
            fatal_provider_events = list(TRACE_RECENT_EVENTS)
            trace_log("RESOLVE_FATAL_DIAGNOSTICS_BEGIN", fatal_signal)
            diagnostic_commands = (
                ("SIGINFO", "p $_siginfo"),
                ("FAULTING_THREAD", "info threads"),
                ("REGISTERS", "info registers"),
                ("INSTRUCTIONS", "x/24i $pc-32"),
                ("STRING_DESTINATION_OBJECT", "x/32gx $rbx-0x20"),
                ("BACKTRACES", "thread apply all bt full 40"),
                ("CALLER_FRAME", "frame 2"),
                ("CALLER_FRAME_INFO", "info frame"),
                ("CALLER_FRAME_REGISTERS", "info registers"),
                ("CALLER_STACK_WORDS", "x/96gx $rsp-0x80"),
                ("CALLER_STACK_BYTES", "x/256bx $rsp-0x80"),
                ("MAPPINGS", "info proc mappings"),
            )
            for label, command in diagnostic_commands:
                try:
                    trace_log("RESOLVE_FATAL_" + label + "_BEGIN")
                    trace_log(gdb.execute(command, to_string=True))
                except Exception as exc:
                    trace_log("RESOLVE_FATAL_" + label + "_ERROR", repr(exc))
                trace_log("RESOLVE_FATAL_" + label + "_END")
            try:
                with open("/proc/%d/maps" % inferior.pid, "r", encoding="utf-8") as maps_file:
                    trace_log("RESOLVE_FATAL_PROC_MAPS_BEGIN")
                    trace_log(maps_file.read())
                    trace_log("RESOLVE_FATAL_PROC_MAPS_END")
            except Exception as exc:
                trace_log("RESOLVE_FATAL_PROC_MAPS_ERROR", repr(exc))
            try:
                recent_events = fatal_provider_events
                with open(TRACE_DETAIL_FILE, "a", encoding="utf-8") as trace_file:
                    trace_file.write("RESOLVE_FATAL_RECENT_PROVIDER_EVENTS_BEGIN\n")
                    for event in recent_events:
                        trace_file.write(event + "\n")
                    trace_file.write("RESOLVE_FATAL_RECENT_PROVIDER_EVENTS_END\n")
                print("RESOLVE_FATAL_RECENT_PROVIDER_EVENTS_BEGIN")
                for event in recent_events:
                    print(event)
                print("RESOLVE_FATAL_RECENT_PROVIDER_EVENTS_END")
            except Exception as exc:
                trace_log("RESOLVE_FATAL_RECENT_PROVIDER_EVENTS_ERROR", repr(exc))
            trace_log("RESOLVE_FATAL_DIAGNOSTICS_END", fatal_signal)
    else:
        print("RESOLVE_AV1_SESSION_EXITED")
try:
    exit_code = int(gdb.parse_and_eval("$_exitcode"))
    print("RESOLVE_AV1_SESSION_EXIT_CODE", exit_code)
except Exception:
    pass
gdb.execute("quit 0")
end
