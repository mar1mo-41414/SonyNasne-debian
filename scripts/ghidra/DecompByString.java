import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.util.DefinedStringIterator;
import java.io.*;
import java.util.*;

public class DecompByString extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String out = args[0];
        List<String> needles = new ArrayList<>();
        for (int i = 1; i < args.length; i++) needles.add(args[i]);
        Set<Function> funcs = new LinkedHashSet<>();
        for (Data d : DefinedStringIterator.forProgram(currentProgram)) {
            Object v = d.getValue();
            if (v == null) continue;
            String s = v.toString();
            for (String n : needles) {
                if (s.contains(n)) {
                    for (Reference r : getReferencesTo(d.getAddress())) {
                        Function f = getFunctionContaining(r.getFromAddress());
                        if (f != null) funcs.add(f);
                    }
                }
            }
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter pw = new PrintWriter(new FileWriter(out))) {
            for (Function f : funcs) {
                pw.println("// ===== " + f.getName() + " @ " + f.getEntryPoint() + " =====");
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                if (r.decompileCompleted()) pw.println(r.getDecompiledFunction().getC());
                else pw.println("// decompile failed: " + r.getErrorMessage());
            }
        }
        println("wrote " + funcs.size() + " functions to " + out);
    }
}
