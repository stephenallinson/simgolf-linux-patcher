// Pre-script: mark the .text block executable so auto-analysis disassembles it
// (golf.exe's protection wrapper left .text flagged as data).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;

public class MarkTextExecutable extends GhidraScript {
    @Override
    public void run() throws Exception {
        MemoryBlock b = currentProgram.getMemory().getBlock(".text");
        if (b != null) { b.setExecute(true); println("marked .text executable"); }
    }
}
