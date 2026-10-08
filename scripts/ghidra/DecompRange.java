import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;

/* 引数: 出力ファイル 開始アドレス 終了アドレス  — 範囲内の全関数をデコンパイルして書き出す */
public class DecompRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Address lo = toAddr(args[1]), hi = toAddr(args[2]);
        try (PrintWriter pw = new PrintWriter(new FileWriter(args[0]))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(lo, true)) {
                if (f.getEntryPoint().compareTo(hi) > 0) break;
                pw.println("// ===== " + f.getName() + " @ " + f.getEntryPoint() + " =====");
                DecompileResults res = di.decompileFunction(f, 120, monitor);
                pw.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed");
            }
        }
        println("done");
    }
}
