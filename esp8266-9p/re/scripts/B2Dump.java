// B2Dump: external call surface, MMIO/ROM data refs, string xrefs in Wi-Fi regions.
// Run headless: -postScript B2Dump.java <output-dir>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.*;

import java.io.File;
import java.io.PrintWriter;
import java.util.*;

public class B2Dump extends GhidraScript {

    static String region(long a) {
        if (a >= 0x40000000L && a < 0x40100000L) return "ROM";
        if (a >= 0x40100000L && a < 0x40108000L) return "IRAM";
        if (a >= 0x40201010L && a < 0x4025EA40L) return "IROM";
        if (a >= 0x3FFE8000L && a < 0x40000000L) return "DRAM";
        if (a >= 0x3FF00000L && a < 0x3FF80000L) return "MMIO-DPORT";
        if (a >= 0x60000000L && a < 0x60010000L) return "MMIO-PERIPH";
        return "OTHER";
    }

    static class Stat {
        int refs = 0;
        Set<String> callers = new TreeSet<>();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args.length > 0 ? args[0] : "/tmp/b2");
        outDir.mkdirs();
        Memory mem = currentProgram.getMemory();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        Listing listing = currentProgram.getListing();

        // 1) Call targets outside IROM/IRAM (external surface)
        Map<Long, Stat> calls = new TreeMap<>();
        // 2) Data refs to ROM/MMIO
        Map<Long, Stat> dataRefs = new TreeMap<>();
        // 3) String xrefs for functions in Wi-Fi regions
        List<String[]> wifiStrings = new ArrayList<>();

        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            Function fn = fm.getFunctionContaining(ins.getAddress());
            String fnAddr = fn == null ? "?" : fn.getEntryPoint().toString();
            for (Reference r : ins.getReferencesFrom()) {
                Address to = r.getToAddress();
                if (to == null) continue;
                long a = to.getOffset();
                String reg = region(a);
                RefType t = r.getReferenceType();
                if (t.isCall()) {
                    if (!reg.equals("IROM") && !reg.equals("IRAM")) {
                        Stat s = calls.computeIfAbsent(a, k -> new Stat());
                        s.refs++; s.callers.add(fnAddr);
                    }
                } else if (t.isData()) {
                    if (reg.startsWith("MMIO") || reg.equals("ROM")) {
                        Stat s = dataRefs.computeIfAbsent(a, k -> new Stat());
                        s.refs++; s.callers.add(fnAddr);
                    }
                }
            }
        }

        try (PrintWriter w = new PrintWriter(new File(outDir, "external-calls.csv"))) {
            w.println("target,region,ref_count,caller_count,sample_callers");
            for (var e : calls.entrySet()) {
                Stat s = e.getValue();
                w.printf("%08x,%s,%d,%d,%s%n", e.getKey(), region(e.getKey()),
                    s.refs, s.callers.size(),
                    String.join(";", s.callers.stream().limit(8).toList()));
            }
        }
        try (PrintWriter w = new PrintWriter(new File(outDir, "data-refs.csv"))) {
            w.println("target,region,ref_count,referrer_count,sample_referrers");
            for (var e : dataRefs.entrySet()) {
                Stat s = e.getValue();
                w.printf("%08x,%s,%d,%d,%s%n", e.getKey(), region(e.getKey()),
                    s.refs, s.callers.size(),
                    String.join(";", s.callers.stream().limit(8).toList()));
            }
        }

        // String values referenced by functions in Wi-Fi regions
        long[][] regions = {
            {0x40105F00L, 0x40106400L},   // wdev IRAM
            {0x4021D000L, 0x4022B000L},   // pm/esf/ieee/hostap/input/mesh/output/phy/regdomain/scan/sta/wl_cnx
        };
        for (var dsIt = ghidra.program.util.DefinedStringIterator.forProgram(currentProgram);
             dsIt.hasNext();) {
            var ds = dsIt.next();
            Object raw = ds.getValue();
            if (raw == null) continue;
            String val = raw.toString().replaceAll("[\\r\\n,;]", " ");
            if (val.length() > 90) val = val.substring(0, 90);
            for (Reference r : rm.getReferencesTo(ds.getAddress())) {
                Function fn = fm.getFunctionContaining(r.getFromAddress());
                if (fn == null) continue;
                long fa = fn.getEntryPoint().getOffset();
                for (long[] rg : regions) {
                    if (fa >= rg[0] && fa < rg[1]) {
                        wifiStrings.add(new String[]{
                            fn.getEntryPoint().toString(),
                            ds.getAddress().toString(), val});
                        break;
                    }
                }
            }
        }
        wifiStrings.sort(Comparator.comparing(s -> s[0]));
        try (PrintWriter w = new PrintWriter(new File(outDir, "wifi-string-xrefs.csv"))) {
            w.println("function,string_addr,string");
            for (String[] s : wifiStrings)
                w.printf("%s,%s,%s%n", s[0], s[1], s[2]);
        }
        println("B2Dump: extCalls=" + calls.size() + " dataRefs=" + dataRefs.size()
            + " wifiStrings=" + wifiStrings.size());
    }
}
