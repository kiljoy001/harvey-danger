# WDEV + DMA descriptor spec (clean-room input)

Status: B4 spec, written from RE evidence only (B0-B4 reports under
`../re/`). This is the document the clean-room libpp replacement is
written against. Nothing here comes from Espressif source; SDK
material was used only to name functions (see `../re/sdk/
PROVENANCE.md`). Confidence is per item: **proven** (direct code
evidence), **inferred** (behaviour + one corroboration), **open**.

Target hardware: ESP8266EX MAC ("WDEV"), register bank
0x3ff20000-0x3ff20fff, DMA via linked descriptors in RAM.

## 1. DMA descriptor (lldesc)

**Proven** layout, 12 bytes, built in RAM by ROM
`lldesc_build_chain` (0x40004f40) called only from wdev init
(0x4021ff18), walked by `wDev_AppendRxBlocks`, `wDev_ProcessFiq`,
`wdev_frame_process`:

| offset | field | evidence |
| --- | --- | --- |
| +0x00 | control word (below) | all descriptor code |
| +0x04 | buffer pointer (`buf`) | init/append walks (word 1 untouched in the FIQ walk, set at build) - inferred |
| +0x08 | `next` descriptor pointer | `puVar5 = (uint*)puVar5[2]` list walks in init, AppendRxBlocks, ProcessFiq - proven |

Control word (+0x00), bit fields:

| bits | name | meaning | confidence |
| --- | --- | --- | --- |
| 11:0 | `size` | buffer capacity in bytes | proven (see below) |
| 23:12 | `len` | valid/available length in bytes | proven |
| 28:24 | reserved/offset | untouched by all analysed code | open |
| 29 | `sub_sof` | sub-start-of-frame flag | inferred |
| 30 | `eof` | end-of-frame / completed | inferred, strong |
| 31 | `owner` | 1 = hardware owns descriptor | proven |

How the order was confirmed (B4, `inventory/b4/decomp4/`):

- `wDev_AppendRxBlocks` (0x40105d4c), per descriptor, in order:
  `w = w & 0x7fffffff | 0x80000000` (set owner), `w &= 0xbfffffff`
  (clear bit 30), `w &= 0xdfffffff` (clear bit 29), then
  `w = w & 0xff000fff | (*w & 0xfff) << 0xc` - clear bits 23:12 and
  copy the low 12 bits into them. Appending *empty* RX blocks sets
  the high field equal to the low field: the low field is the
  pre-existing buffer capacity (`size`), the high field is how
  much of it is on offer (`len`). `wDev_AppendRxAmpduLensBlocks`
  writes per-descriptor lengths from an external array into the
  same high field, confirming `len` at 23:12.
- Wdev init (0x4021ff18) does the same copy after subtracting 4
  from the low field - RX usable length excludes the 4-byte FCS.
- `wDev_ProcessFiq` (0x4010584c) treats a descriptor as completed
  when `(w & 0x7fffffff) >> 0x1e != 0`, and one path asserts the
  value is exactly 1 - i.e. bit 30 set, bit 29 clear - for a valid
  completed head descriptor. Hardware sets bit 30 on completion;
  bit 29 is the remaining frame-boundary flag (`sub_sof` is the
  conventional name at this position; the analysed code only ever
  clears it).

Lifecycle (RX): software builds chains with `size` set, appends
them with owner=1, len=size (or size-4 at init), eof/sub_sof=0.
Hardware fills buffers, sets eof on the last descriptor of a
frame, and the FIQ path walks from the queue head checking
owner/eof before handing frames to `wdev_frame_process`.
Software must `memw()` barrier around descriptor word writes -
the blobs do, and the clean-room code must too.

Queues: the wdev control struct pointer is the DRAM global at
0x3ffe834c (reached via IRAM literal cell 0x401053f8); in the stock
image that global initially holds 0x3ffecca0 (BSS, zero at boot).
The same pointer is `*PTR_DAT_4021fe84` in wdev init. Field
evidence (u16-indexed decompiler offsets converted to bytes):

| offset | field | evidence |
| --- | --- | --- |
| +0x00 | u16, set to 6 at DMA init | 0x4021ff18 (`*ctrl = 6`) |
| +0x02 | u16, zeroed at init; two queue counters live at +0x00/+0x02 in AppendRxBlocks | init + AppendRxBlocks |
| +0x04 | byte, set to 7 at init | 0x4021ff18 |
| +0x08 | ptr, output of lldesc_build_chain #1; stored to WDEV 0x3ff20008 | init |
| +0x0c | ptr, second output of chain #1 | init |
| +0x18 | word with descriptor-style flag bits set/cleared at init (bit2 set, then owner-style bit31 set, bits 30/29 cleared) - an embedded control/descriptor word | init |
| +0x1c | ptr, set to ctrl+0x24 (self-referential list anchor) | init |
| +0x20 | word, zeroed at init | init |
| +0x28/+0x2c | chain #2 output slots (their addresses are passed to lldesc_build_chain: ushort offsets +0x14/+0x16) | init |
| +0x30/+0x34 | chain #3 output slots (addresses passed likewise); the +0x30 value is also stored to WDEV 0x3ff2000c | init |

AppendRxBlocks raises the WDEV doorbell registers at
0x3ff20a00+0x2a8/0x2ac (0x3ff20ca8/0x3ff20cac) once more than one
block is queued. Counters at +0x00/+0x02 are incremented by the
number of descriptors appended per queue.

## 2. WDEV registers (0x3ff20000 bank)

Direction from `inventory/b4/wdev-direction.csv` (load/store
mnemonic split across all instructions; 71 registers touched in
0x3ff20000-0x3ff20fff: 29 R/W, 17 read-only, 25 write-only).
Roles are by named referrer function. Only registers with a
defensible role are listed; the full per-register referrer table
is `inventory/b3/mmio-registers.csv`.

| address | dir | role | confidence |
| --- | --- | --- | --- |
| 0x3ff20000 | R/W | touched only by wdev init (0x4021ff18) - core enable/config | inferred |
| 0x3ff20004 | R/W | init + RX append paths | open |
| 0x3ff20008 | W | one-time setup (init, AppendRxBlocks) | inferred |
| 0x3ff2000c | W | one-time setup (init, sniffer go/exit) | inferred |
| 0x3ff20014/18/1c | R | FIQ status, read only by `wDev_ProcessFiq` | inferred, strong |
| 0x3ff20038/3c | R | RX status, read by `wdev_frame_process` path | inferred |
| 0x3ff2006c | R/W | central control, read-modify-write, 25 ld / 25 st across 12+ functions incl. sniffer and scan | open (hottest register) |
| 0x3ff20800/04 | R/W | RX mode/filter pair: sniffer go/exit, `wDev_Crypto_Conf`, key helpers at 0x40220664/6b8 | inferred, strong |
| 0x3ff20a00+0x220 (0x3ff20c20) | R | interrupt/FIQ status word decoded bit-by-bit by `wDev_ProcessFiq` (bits 2,3,7,8,19,23,26,27,28,31 handled) | inferred, strong |
| 0x3ff20ca8/0x3ff20cac | W | RX queue doorbells set by AppendRxBlocks when queue depth > 1 (masks 0x10000000 / 0x50000000 written) | inferred |
| 0x3ff20c00 | R | read 54 times by system-wide functions, never stored - free-running MAC timer is the natural reading | inferred |
| 0x3ff20c14 | R/W | `wDev_Option_Init` + neighbours at 0x4021ae6c/aff8 | open |
| 0x3ff20c3c/44 | R/W | BSSID registers, written by `wDev_SetBssid` | proven by referrer |
| 0x3ff20c5c area | R/W | own-MAC registers, `wDev_SetMacAddress`, Enable/DisableUcRx | inferred, strong |
| 0x3ff20c74 | R/W | 12 ld / 12 st, IRAM system + pm cluster (0x40103728, 0x4021ab40) | open |
| 0x3ff20de0 area | R/W | pm/init cluster (0x4021ab40, 0x4021af8c) | open |

### 2.1 Observed init writes (replay candidates, not a recipe)

From wdev init 0x4021ff18 and its orchestrator 0x4022013c
(SDK-size match for `wDev_Initialize`, 104 bytes; it calls
`wDev_Option_Init`, FUN_40220664, 0x4021ff18, FUN_402200bc,
FUN_402200dc in that order). Absolute BSS addresses below are
stock-build-specific - a clean build allocates its own; only the
register/value structure carries over.

| register | write | source |
| --- | --- | --- |
| 0x3ff2007c | = 0x3ffef438 (chain array, BSS) | init, via base 0x3ff1fe00+0x27c |
| 0x3ff20080 | = 0x3ffece50 (chain array, BSS) | init, +0x280 |
| 0x3ff20084 | = 0x3ffefc20 (chain array, BSS) | init, +0x284 |
| 0x3ff20088 | = 0x3ffef4c0 (chain array, BSS) | init, +0x288 |
| 0x3ff20000 | &= 0xffffff00 (low byte cleared, late in init) | init tail |
| 0x3ff20008 | = ctrl+0x08 value (chain #1 head) | init tail |
| 0x3ff2000c | = ctrl+0x30 value (chain #3 head) | init tail |
| 0x3ff20010 | = 0 | init tail |
| 0x3ff20c18 | = 0, later = 0x2c9f0300 (magic config word) | orchestrator, via base 0x3ff20a00+0x218 |
| 0x3ff20c24 | = 0xffffffff | orchestrator, +0x224 |
| 0x3ff20178 | \|= 2 | orchestrator, via base 0x3ff1fe00+0x378 |
| 0x3ff20004 | &= 0x7fffffff (bit 31 cleared, last) | orchestrator |

The three `lldesc_build_chain` calls in init, for the record:
(0x3ffece50, desc 0x48, stride 0x50, bufsz 0x2598, buflen 0x644,
flag 1), (0x3ffef440, 0x60, 0x60, 0x20, 4, 0),
(0x3ffef4c0, 0x54, 0x60, 0x700, 0x100, 1) - argument semantics
beyond array/stride/buffer-size are **open**; the ROM routine is
treated as hardware until its contract is needed.

Other banks touched by the blobs (for completeness, not part of
this spec): DPORT 0x3ff00000 (13 registers; +0x04 is
EDGE_INT_ENABLE_REG per eagle_soc.h, +0x14/+0x18 hot and unnamed),
peripherals at 0x60000000 (UART0 +0x00, GPIO 0x60000300, TIMER
0x60000600, RTC 0x60000700, IO_MUX 0x60000800 per eagle_soc.h;
unnamed banks at 0x60000200, 0x60000500, 0x60000d00, 0x60001100,
0x60009800-0x60009d00 remain open).

## 3. g_ic / softc fields (writing target for upper layers)

Base `g_ic` = 0x3fff06d4, 1704 bytes (SDK OBJECT size, B3).
Offset evidence only; types are pointer/byte/word by usage.

| offset | evidence | from |
| --- | --- | --- |
| +0x10 | ptr to 688-byte sub-struct; its +0xd0 = state byte compared to 2, +0xd2 = state byte tested 4 then set 1 | wifi_mode_set, sta path 0x402278f4 |
| +0x14 | softc ptr (see below) | wifi_mode_set, hostap_teardown |
| +0x1a0, +0x1a4 | ptrs freed on softap stop with hostap.c line refs | wifi_softap_stop |
| +0x1f0 | ptr to struct whose word 0 is a receive callback, called when non-null | sta_input |
| +0x1f4 (500) | ptr to struct whose +0x08 is a callback, called when non-null | sta_input |
| +0x220 | byte copied into frame construction | sta path 0x402278f4 |

softc (via g_ic+0x14): +0x90 = station/list head used as first
argument context by `ieee80211_send_mgmt` (returns failure when
zero) and saved/restored around management sends in softap stop;
per-station records reached from +0x98; +0xd0 = flags word, bit
0x1000000 gates the softap-stop path; +0xcc (from g_ic base
struct in teardown path) heads the AP struct chain freed by
`hostap_teardown` in the order recorded in B2/B4.

`hostap_input` and `ieee80211_send_mgmt` mostly work on structs
passed as arguments (node/descriptor/softc), not on `g_ic`
directly - that is why the callback fields at +0x1f0/+0x1f4 only
show up in `sta_input`.

## 4. Rules for the clean-room implementation

1. Descriptors: implement section 1 exactly; `size` is set at
   build time and never rewritten by the RX path except the
   init-time size-4 adjustment; `len` is the only field the RX
   append path derives from `size`.
2. `memw()` (or the Xtensa equivalent barrier) around every
   descriptor control-word write and queue doorbell write.
3. Never write a register marked read-only in section 2; never
   read a write-only one expecting state back.
4. Registers marked open are not to be touched by clean-room code
   until their role is proven. Observed init writes are recorded
   in section 2.1, but the first replacement still keeps blob init
   for WDEV bring-up and replaces queue/frame plumbing first
   (DESIGN 6.1 order); replaying section 2.1 is a later step and
   only with freshly allocated descriptor arrays, never the
   stock BSS addresses.
5. All of this is little-endian, 32-bit accesses unless a
   referrer proves otherwise; access width was not split in B4
   and must be checked per register before first hardware use.

## 5. esf_buf — VERIFIED from RTOS libpp.a esf_buf.o (B22, 2026-11-04)

Full disassembly of the RTOS-generation `esf_buf.o` (`esf_buf_setup`,
`esf_buf_alloc`, `esf_buf_recycle`, `esf_rx_buf_alloc`,
`esp_wifi_{get,set}_rx_pbuf_mem_type`). This supersedes the NONOS-era
esf inference for the RTOS blobs. Source line numbers below are the
blob's own, passed to the heap allocator (alloc lines 400-433,
recycle free lines 612-616).

### 5.1 struct esf_buf — 68 bytes (0x44)

| off | type | meaning (evidence) |
|-----|------|--------------------|
| +0  | desc* | 12-byte DMA descriptor; separately allocated for type 4, ring-assigned at setup for type 1; freed first in type-4 recycle |
| +4  | desc* | same descriptor pointer stored again (setup and type-4 alloc both write +0 and +4 identically) |
| +8  | u16 | class/flags halfword: setup writes 1; type-4 alloc writes 1 |
| +12 | u8* | data pointer: type 1 = caller pointer; type 4/5 = payload base (+20); setup type 7 leaves unset here |
| +20 | u8* | payload base: type 1 = data−36 (36-byte headroom); type 4 = fresh allocation; type 5 = sbuf+12 |
| +26 | u16 | length (type-1 alloc, from arg truncated to 16 bits) |
| +36 | esf_buf* | free-list next, all three lists |
| +40 | hdr* | per-buffer header: 28 bytes for types 1/4/5, 8 bytes for type 7 (see 5.2) |
| +44 | u8[12] | inline area; its address is the callback argument |
| +56 | fn ptr | completion callback, called as cb(eb+44) by type-1 recycle; may be NULL |
| +64 | u32 | DMA control word composed by type-1 recycle (see 5.3) |

Unmapped: +16, +24, +28..+35, +48..+55, +60.

### 5.2 ebCxt — three free lists (BSS)

| off | list | alloc pop | recycle push |
|-----|------|-----------|--------------|
| +0  | general | esf_buf_alloc type 1 | recycle types 1, 2 |
| +8  | mgmt | esf_buf_alloc type 5 | recycle type 5 |
| +16 | RX | esf_rx_buf_alloc(7) only | recycle type 7 |

All pops/pushes run under `vPortETSIntrLock`. `esf_rx_buf_alloc`
with any type other than 7 returns NULL, and each successful RX pop
decrements the BSS counter `g_free_rxblock_eb_cnt`. Archive-wide
scan (B22): that counter's section is defined in `esf_buf.o` and
the only code reference in all of libpp.a is this decrement — it
is never initialised or incremented, so it is write-only
telemetry, not a pool gauge. The true RX pool level is the
length of the +16 list, which no exported symbol exposes.

The +40 header is two different objects by class: a 28-byte header
for types 1/4/5 (zalloc'd per buffer in type-4 alloc; carved from a
28-stride pool at setup; memset(28) on type-1/2/5 recycle) and an
8-byte header for type 7 (carved from an 8-stride pool at setup;
memset(8) on type-7 recycle).

### 5.3 Function semantics

- `esf_buf_alloc(data, type, len)`:
  - type 1 (data != NULL): pop general list; +12=data, +20=data−36,
    +26=(u16)len; header word0 |= 0x2000; desc[+4]=data;
    header word1 (+4) &= 0xfc03ffff.
  - type 4: allocate fresh — 68-byte struct, 12-byte descriptor
    (both +0 and +4), 28-byte zalloc header (header word0 |= 0x4000),
    len-byte payload (+12=+20=payload); desc[+4]=payload;
    header +4 &= 0xfc03ffff. Each allocation is range-checked
    against 0x3fffffff/0x40000000 and freed on failure.
  - type 5: pop mgmt list; memset(header,0,28); +12=+20;
    header word0 |= 0x8000; desc[+4]=+12; header +4 &= 0xfc03ffff.
  - other types: trap (`break 1,15` after a NULL dereference guard).
- `esf_buf_recycle(eb, type)`:
  - type 1: compose +64 from the 28-byte header —
    bits from header+8 fields [13:8]<<8 and [19:14]<<14,
    header+4 bits [25:18] into bits [7:0], header byte +12 <<20 —
    store at +64; if +56 callback set, call it with (eb+44);
    memset(header,0,28); push onto general list.
  - type 2: memset(header,0,28); push onto general list.
    (No +64 compose, no callback.)
  - type 4: heap-free descriptor (+0), header (+40), payload (+20),
    then the struct (lines 612, 614, 615, 616).
  - type 5: memset(header,0,28); push onto mgmt list.
  - type 7: memset(header,0,8); push onto RX list.
- `esf_buf_setup()` (no args; reads `g_wifi_menuconfig`):
  allocates, all with caps=4 via heap_caps_zalloc —
  header pool 28×(cfg[17]+8), RX-header pool 8×(cfg[3]+1),
  esf_buf pool 68×(cfg[3]+cfg[17]+9) — then carves and recycles:
  cfg[17] buffers as type 1 (descriptor pointers taken from the
  ring at `wDevCtrl+24`, descending 12-byte strides; +20 loaded
  from ring word +4), 8 mgmt buffers as type 5 (payload space
  `eb_mgmt_sbuf_space`, 76-byte stride, +20=sbuf+12), and
  cfg[3]+1 buffers as type 7 (RX; +40 = 8-byte headers).
  cfg[3]/cfg[17] are byte fields of the Wi-Fi menuconfig struct;
  resolved in B23: wifi_menuconfig_init (ieee80211_api.o)
  copies wifi_init_config_t byte 26 to cfg[3] and byte 28 to
  cfg[17], i.e. cfg[3] = rx_pkt_num and cfg[17] = tx_buf_num
  (esp9 sdkconfig: 7 and 6, so the RX pool is 8 buffers and
  the TX type-1 pool 6). The blob's own log
  strings name the header pools "tdesc" (28-byte, cfg[17]) and
  "rdesc" (8-byte, cfg[3]), i.e. the type-1 pool is the TX-side
  pool and the type-7 pool the RX-side pool. Independently
  confirmed by Ghidra 12.2 decompilation of esp9.elf
  (re/ghidra/esf_decomp.txt, B22): every offset above matches,
  and `esp_wifi_internal_free_rx_buffer` is a thin wrapper on
  `ppRecycleRxPkt`.

### 5.4 RX-stall implication

RX buffers are a **fixed preallocated pool**; per-frame allocation
is a free-list pop, never a heap call. RX death with a live
scheduler (B21) is therefore consistent with pool exhaustion: any
path that takes an RX esf_buf without a type-7 recycle permanently
shrinks the pool until `esf_rx_buf_alloc` returns NULL and the MAC
drops every frame. It also eliminates per-frame heap allocation
from the FIQ/NMI path as a stall cause on the RTOS blobs.

## 6. Open items blocking blob-free WDEV bring-up

- `lldesc_build_chain` argument contract (section 2.1): until it
  is decoded, clean-room code builds descriptor chains itself
  (section 1 has enough to do so) rather than calling the ROM
  routine the way init does.
- Role of 0x3ff2006c and 0x3ff20c74, and the 0x60009800+ baseband
  bank (libphy territory, not this spec).
- Wdev control struct fields beyond +0x34, and the esf free-list
  table at 0x3ffefc30 (heads per buffer class, indexed in the
  decompiled esf code at words 0, 2, 3 - class mapping open).
  (cfg[3]/cfg[17] from the B22 list are resolved: rx_pkt_num /
  tx_buf_num, see section 5.3. The esf layer itself is now
  replaced by clean-room code, B24, so its remaining unmapped
  struct offsets no longer block anything.)
- Descriptor word-1 (+0x04) semantics beyond "buffer pointer":
  the FIQ walk never reads it, but TX paths may use it as a
  second address/offset field.

