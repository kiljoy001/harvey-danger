// B4Inspect: instructions around an address, callers of a function, MMIO direction split.
// Args: <outdir> <inspectAddrHex> <conflictAddrHex>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

import java.io.File;
import java.io.PrintWriter;
import java.util.*;

public class B4Inspect extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args[0]);
        outDir.mkdirs();
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();

        // 1) Instructions around inspect address
        Address probe = toAddr(Long.parseLong(args[1], 16));
        try (PrintWriter w = new PrintWriter(new File(outDir, "probe-listing.txt"))) {
            Address a = probe.subtract(16);
            Instruction ins = listing.getInstructionAt(a);
            if (ins == null) ins = listing.getInstructionAfter(a);
            for (int i = 0; i < 40 && ins != null; i++) {
                Function f = fm.getFunctionContaining(ins.getAddress());
                w.printf("%s  %-28s fn=%s%n", ins.getAddress(), ins,
                    f == null ? "-" : f.getName() + "@" + f.getEntryPoint());
                ins = ins.getNext();
            }
            Function cont = fm.getFunctionContaining(probe);
            w.println("containing function: " + cont);
        }

        // 2) Callers and callees of conflict function
        Address caddr = toAddr(Long.parseLong(args[2], 16));
        Function cf = fm.getFunctionContaining(caddr);
        try (PrintWriter w = new PrintWriter(new File(outDir, "conflict-callers.txt"))) {
            w.println("function: " + cf);
            if (cf != null) {
                w.println("callers:");
                for (Function c : cf.getCallingFunctions(monitor))
                    w.printf("  %s @%s size=%d%n", c.getName(), c.getEntryPoint(),
                        c.getBody().getNumAddresses());
                w.println("callees:");
                for (Function c : cf.getCalledFunctions(monitor))
                    w.printf("  %s @%s%n", c.getName(), c.getEntryPoint());
                w.println("body: " + cf.getBody().getMinAddress() + "-"
                    + cf.getBody().getMaxAddress());
            }
        }

        // 3) MMIO direction/width for WDEV bank
        Map<Long, int[]> dir = new TreeMap<>(); // [loads, stores]
        Map<Long, Set<String>> fns = new TreeMap<>();
        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            String m = ins.getMnemonicString().toLowerCase();
            boolean isLoad = m.startsWith("l");
            boolean isStore = m.startsWith("s");
            if (!isLoad && !isStore) continue;
            for (Reference r : ins.getReferencesFrom()) {
                Address to = r.getToAddress();
                if (to == null) continue;
                long a = to.getOffset();
                if (a < 0x3FF20000L || a >= 0x3FF21000L) continue;
                int[] c = dir.computeIfAbsent(a, k -> new int[2]);
                if (isLoad) c[0]++; else c[1]++;
                Function f = fm.getFunctionContaining(ins.getAddress());
                fns.computeIfAbsent(a, k -> new TreeSet<>())
                   .add(f == null ? "?" : f.getName());
            }
        }
        try (PrintWriter w = new PrintWriter(new File(outDir, "wdev-direction.csv"))) {
            w.println("register,loads,stores,functions");
            for (var e : dir.entrySet())
                w.printf("%08x,%d,%d,%s%n", e.getKey(), e.getValue()[0],
                    e.getValue()[1], String.join(";", fns.get(e.getKey())));
        }
        println("B4Inspect done");
    }
}
