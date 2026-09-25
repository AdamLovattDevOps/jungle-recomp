// Export decompiled C for every function in the current program, plus a
// compact function inventory, into notes/decomp/<PROGRAM>.c and .csv.
// @category Jungle
import java.io.File;
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportDecomp extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = args.length > 0 ? args[0] : "/tmp";
        new File(outDir).mkdirs();

        String name = currentProgram.getName();
        PrintWriter c = new PrintWriter(new File(outDir, name + ".c"));
        PrintWriter csv = new PrintWriter(new File(outDir, name + ".csv"));
        csv.println("addr,size,name,calls,called_by,signature");

        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);

        int total = 0, ok = 0;
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            total++;
            long size = f.getBody().getNumAddresses();
            csv.println(String.format("%s,%d,%s,%d,%d,\"%s\"",
                f.getEntryPoint(), size, f.getName(),
                f.getCalledFunctions(monitor).size(),
                f.getCallingFunctions(monitor).size(),
                f.getSignature().getPrototypeString().replace("\"", "'")));

            DecompileResults r = d.decompileFunction(f, 60, monitor);
            c.println("/* ===== " + f.getName() + " @ " + f.getEntryPoint()
                      + "  (" + size + " bytes) ===== */");
            if (r.decompileCompleted() && r.getDecompiledFunction() != null) {
                c.println(r.getDecompiledFunction().getC());
                ok++;
            } else {
                c.println("/* DECOMPILE FAILED: " + r.getErrorMessage() + " */");
            }
            c.println();
        }
        d.dispose();
        c.close();
        csv.close();
        println("EXPORTED " + name + ": " + ok + "/" + total + " functions -> " + outDir);
    }
}
