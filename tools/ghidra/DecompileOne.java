// Decompile one function with a long timeout.
// Script args: <hex address> <timeout seconds> <output file>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.Function;
import java.io.*;

public class DecompileOne extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Function f = getFunctionAt(toAddr(Long.parseLong(a[0], 16)));
        DecompInterface ifc = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.setMaxPayloadMBytes(512);       // huge functions exceed the default response buffer
        opts.setMaxInstructions(1000000);
        ifc.setOptions(opts);
        ifc.openProgram(currentProgram);
        DecompileResults r = ifc.decompileFunction(f, Integer.parseInt(a[1]), monitor);
        try (PrintWriter out = new PrintWriter(new FileWriter(a[2]))) {
            out.println("// ==== " + f.getName() + " @ " + f.getEntryPoint());
            out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
        }
    }
}
