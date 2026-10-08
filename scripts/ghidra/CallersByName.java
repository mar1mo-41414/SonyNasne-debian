import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

/* 引数: 出力ファイル 関数名... — 指定名のシンボル(外部/PLT含む)を呼んでいる関数をデコンパイルして書き出す */
public class CallersByName extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Set<Address> seen = new HashSet<>();
        try (PrintWriter pw = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                pw.println("##### callers of " + args[i]);
                for (Function ff : currentProgram.getFunctionManager().getFunctions(true)) {
                    if (ff.getName().equals(args[i])) {
                        pw.println("// function " + ff.getName() + " @ " + ff.getEntryPoint() + (ff.isThunk() ? " (thunk)" : ""));
                        for (Reference r : getReferencesTo(ff.getEntryPoint())) {
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
                SymbolIterator it = currentProgram.getSymbolTable().getSymbols(args[i]);
                while (it.hasNext()) {
                    Symbol s = it.next();
                    pw.println("// symbol " + s.getName() + " @ " + s.getAddress());
                    for (Reference r : getReferencesTo(s.getAddress())) {
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
        }
        println("done");
    }
}
