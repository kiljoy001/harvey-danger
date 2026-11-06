// B1Dump: dump memory blocks, functions, and xrefs to Wi-Fi strings.
// Run headless: -postScript B1Dump.java <output-dir>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.util.DefinedStringIterator;

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class B1Dump extends GhidraScript {

    private static class Anchor {
        final String text;
        Address addr;
        Anchor(String t) { text = t; }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args.length > 0 ? args[0] : "/tmp/b1");
        outDir.mkdirs();

        try (PrintWriter w = new PrintWriter(new File(outDir, "program.txt"))) {
            w.println("program=" + currentProgram.getName());
            w.println("language=" + currentProgram.getLanguageID());
            w.println("entry=" + currentProgram.getImageBase());
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                w.printf("block %s %s-%s size=%d init=%b exec=%b%n",
                    b.getName(), b.getStart(), b.getEnd(), b.getSize(),
                    b.isInitialized(), b.isExecute());
            }
        }

        FunctionManager fm = currentProgram.getFunctionManager();
        List<Function> funcs = new ArrayList<>();
        fm.getFunctions(true).forEach(funcs::add);
        try (PrintWriter w = new PrintWriter(new File(outDir, "functions.csv"))) {
            w.println("entry,name,size,called_count,calling_count");
            for (Function f : funcs) {
                w.printf("%s,%s,%d,%d,%d%n", f.getEntryPoint(),
                    f.getName().replace(",", " "),
                    f.getBody().getNumAddresses(),
                    f.getCalledFunctions(monitor).size(),
                    f.getCallingFunctions(monitor).size());
            }
        }

        String[] wanted = {
            "ieee80211.c", "ieee80211_hostap.c", "ieee80211_input.c",
            "ieee80211_output.c", "ieee80211_phy.c", "ieee80211_scan.c",
            "ieee80211_sta.c", "ieee80211_regdomain.c",
            "ieee80211_mesh_quick.c", "phy ver: %d, pp ver: %d.%d",
            "SDK ver: %s", "app_main.c",
        };
        List<Anchor> anchors = new ArrayList<>();
        for (String s : wanted) anchors.add(new Anchor(s));
        for (var it = DefinedStringIterator.forProgram(currentProgram); it.hasNext();) {
            var ds = it.next();
            Object raw = ds.getValue();
            String val = raw == null ? null : raw.toString();
            if (val == null) continue;
            for (Anchor a : anchors) {
                if (val.equals(a.text)) a.addr = ds.getAddress();
            }
            // Dynamic anchors: any source-filename string (assert __FILE__).
            if (val.matches("[A-Za-z0-9_./-]+\\.c") && val.length() < 40) {
                Anchor dyn = new Anchor(val);
                dyn.addr = ds.getAddress();
                anchors.add(dyn);
            }
        }

        ReferenceManager rm = currentProgram.getReferenceManager();
        try (PrintWriter w = new PrintWriter(new File(outDir, "anchors.csv"))) {
            w.println("string,address,xref_count,referring_functions");
            for (Anchor a : anchors) {
                if (a.addr == null) {
                    w.printf("%s,NOT_FOUND,0,%n", a.text.replace(",", " "));
                    continue;
                }
                int count = 0;
                StringBuilder fns = new StringBuilder();
                for (Reference r : rm.getReferencesTo(a.addr)) {
                    count++;
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    fns.append(f == null ? "?" : f.getEntryPoint().toString());
                    fns.append(";");
                }
                w.printf("%s,%s,%d,%s%n", a.text.replace(",", " "),
                    a.addr, count, fns);
            }
        }
        println("B1Dump: " + funcs.size() + " functions -> " + outDir);
    }
}
