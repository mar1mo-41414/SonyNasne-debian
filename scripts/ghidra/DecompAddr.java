import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.*;

public class DecompAddr extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = args[0];
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Set<Address> seen = new HashSet<>();
        try (PrintWriter pw = new PrintWriter(new FileWriter(out))) {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(args[i]);
                Function f = getFunctionAt(a);
                if (f == null) f = getFunctionContaining(a);
                if (f == null) { pw.println("// no function at " + args[i]); continue; }
                if (!seen.add(f.getEntryPoint())) continue;
                pw.println("// ===== " + f.getName() + " @ " + f.getEntryPoint() + " (" + f.getSignature() + ") =====");
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                pw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
        println("done");
    }
}
