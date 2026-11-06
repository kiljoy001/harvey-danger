// B4Rename: resolutions from B4-prep (see B4 report).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class B4Rename extends GhidraScript {
    private void name(long addr, String name, String comment) throws Exception {
        Address a = toAddr(addr);
        Function f = getFunctionContaining(a);
        if (f == null) { println("no function containing " + Long.toHexString(addr)); return; }
        f.setName(name, SourceType.USER_DEFINED);
        f.setComment(comment);
        println("named " + f.getEntryPoint() + " -> " + name);
    }

    @Override
    public void run() throws Exception {
        name(0x40221a74L, "hostap_input",
            "B4: SDK fingerprint start 0x40221a6c is an 8-byte literal pool; code starts 0x40221a74 (frame-control parsing).");
        name(0x40221810L, "hostap_teardown",
            "B4 RESOLVED over SDK ieee80211_hostap_attach: sole caller is the softap-stop path (0x40222af8), only callee is mem_free, body is pure frees. SDK size match judged coincidental.");
        name(0x40222af8L, "wifi_softap_stop",
            "B4 candidate (medium): deauths/disassociates all stations (mgmt 0xa0/0xc0), frees g_ic+0x1a0/+0x1a4, calls hostap_teardown. SDK size 447 vs 412 here.");
    }
}
