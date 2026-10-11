// Decompile every function to <program>.c in the directory given as the script argument.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class DecompileAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String dir = getScriptArgs().length > 0 ? getScriptArgs()[0] : ".";
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(new FileWriter(dir + "/" + currentProgram.getName() + ".c"))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                DecompileResults r = ifc.decompileFunction(f, 60, monitor);
                out.println("// ==== " + f.getName() + " @ " + f.getEntryPoint());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
            }
        }
    }
}
