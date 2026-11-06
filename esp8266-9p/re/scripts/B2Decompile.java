// B2Decompile: decompile + list given function addresses for spot-checks.
// Run headless: -postScript B2Decompile.java <output-dir> <hexaddr>...
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;

import java.io.File;
import java.io.PrintWriter;

public class B2Decompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args[0]);
        outDir.mkdirs();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        Listing listing = currentProgram.getListing();
        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(Long.parseLong(args[i], 16));
            Function f = getFunctionContaining(a);
            if (f == null) { println("no function at " + args[i]); continue; }
            String base = "FUN_" + f.getEntryPoint();
            try (PrintWriter w = new PrintWriter(new File(outDir, base + ".c"))) {
                DecompileResults res = ifc.decompileFunction(f, 60, monitor);
                if (res != null && res.getDecompiledFunction() != null)
                    w.print(res.getDecompiledFunction().getC());
                else w.println("/* decompile failed */");
            }
            try (PrintWriter w = new PrintWriter(new File(outDir, base + ".asm.txt"))) {
                InstructionIterator it = listing.getInstructions(f.getBody(), true);
                while (it.hasNext()) {
                    Instruction ins = it.next();
                    w.printf("%s  %s%n", ins.getAddress(), ins);
                }
            }
            println("decompiled " + base);
        }
        ifc.dispose();
    }
}
