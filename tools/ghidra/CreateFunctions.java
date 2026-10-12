// Create functions at the addresses listed (one hex address per line) in the
// file given as the script argument, then let analysis fill them in.
// Used to fill gaps in golf.exe's auto-analysis (call targets it missed).
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.Address;
import java.nio.file.*;

public class CreateFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        int made = 0;
        for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            line = line.trim();
            if (line.isEmpty()) continue;
            Address a = toAddr(Long.parseLong(line, 16));
            if (getFunctionAt(a) != null) continue;
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            if (new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) made++;
        }
        println("created " + made + " functions");
    }
}
