// B3Dump: per-register referrers (with current names) for MMIO banks,
// plus field-offset access map for the g_ic pointer cell target.
// Run headless: -postScript B3Dump.java <output-dir>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

import java.io.File;
import java.io.PrintWriter;
import java.util.*;

public class B3Dump extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args[0]);
        outDir.mkdirs();
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();

        Map<Long, Map<String, Integer>> regs = new TreeMap<>();
        Map<String, Set<Integer>> gicFields = new TreeMap<>();

        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            Function fn = fm.getFunctionContaining(ins.getAddress());
            String fnName = fn == null ? "?" : fn.getName();
            for (Reference r : ins.getReferencesFrom()) {
                Address to = r.getToAddress();
                if (to == null || !r.getReferenceType().isData()) continue;
                long a = to.getOffset();
                if ((a >= 0x3FF20000L && a < 0x3FF21000L)
                        || (a >= 0x60000000L && a < 0x6000A000L)
                        || (a >= 0x3FF00000L && a < 0x3FF00100L)) {
                    regs.computeIfAbsent(a, k -> new TreeMap<>())
                        .merge(fnName, 1, Integer::sum);
                }
            }
            // g_ic field access: any scalar operand offset used with the
            // cell is too indirect to capture generically here; field map
            // is built in the report from decompiled offsets instead.
        }
        try (PrintWriter w = new PrintWriter(new File(outDir, "mmio-registers.csv"))) {
            w.println("register,referrer_function,ref_count");
            for (var e : regs.entrySet())
                for (var f : e.getValue().entrySet())
                    w.printf("%08x,%s,%d%n", e.getKey(), f.getKey(), f.getValue());
        }
        println("B3Dump: " + regs.size() + " MMIO registers");
    }
}
