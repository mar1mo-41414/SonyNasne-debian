import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class CallersDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Set<Address> seen = new HashSet<>();
        try (PrintWriter pw = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(args[i]);
                pw.println("##### callers of " + args[i]);
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f == null) { pw.println("// ref from " + r.getFromAddress() + " (no function)"); continue; }
                    pw.println("// ref from " + r.getFromAddress() + " in " + f.getName() + "@" + f.getEntryPoint());
                    if (!seen.add(f.getEntryPoint())) continue;
                    pw.println("// ===== " + f.getName() + " @ " + f.getEntryPoint() + " =====");
                    DecompileResults res = di.decompileFunction(f, 120, monitor);
                    pw.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed");
                }
            }
        }
        println("done");
    }
}
