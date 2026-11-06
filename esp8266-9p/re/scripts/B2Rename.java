// B2Rename: apply evidence-based candidate names from B2 analysis.
// Names are candidates (see B2 report for evidence/confidence), applied
// only to the Ghidra project, never to the extracted images.
// Run headless: -postScript B2Rename.java
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class B2Rename extends GhidraScript {
    private void name(long addr, String name, String comment) throws Exception {
        Address a = toAddr(addr);
        Function f = getFunctionAt(a);
        if (f == null) { println("no function at " + Long.toHexString(addr)); return; }
        f.setName(name, SourceType.USER_DEFINED);
        f.setComment(comment);
        println("named " + Long.toHexString(addr) + " -> " + name);
    }

    @Override
    public void run() throws Exception {
        // 0x40201764/0x40201778 are pointer-table cells in IROM, not code:
        // they hold 0x40100e24 / 0x40100d9c (read from the image in B2).
        name(0x40100e24L, "mem_alloc_fileline",
            "B2 candidate: target of IROM pointer cell 0x40201764; called as (size, file-string, line).");
        name(0x40100d9cL, "mem_free_fileline",
            "B2 candidate: target of IROM pointer cell 0x40201778; called as (ptr, file-string, line).");
        name(0x401018a0L, "printf_like_401018a0",
            "B2 candidate: target of IROM pointer cell 0x40201448; called with format strings throughout.");
        name(0x40100cd4L, "mem_alloc_fileline_iram",
            "B2 candidate: IRAM-resident (size, file, line) allocation used by wdev.c paths.");
        name(0x40220facL, "ieee80211_opmode_setup",
            "B2 candidate: ieee80211.c anchor; switch on mode 0..3, prints mode/MAC, allocates 0x2a0 and 0x110 structs.");
        name(0x40221810L, "hostap_teardown",
            "B2 candidate: ieee80211_hostap.c anchor; frees linked hostap structures (lines 0x1ab-0x1c3).");
        name(0x40226680L, "regdomain_free_field130",
            "B2 candidate: ieee80211_regdomain.c anchor; frees struct field +0x130 (line 0x39).");
        name(0x40105fd8L, "wdev_frame_process",
            "B2 candidate: wdev.c IRAM anchor; frame-control checks, 12-byte header copy via esf/wdev path.");
    }
}
