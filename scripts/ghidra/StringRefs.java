import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.util.DefinedStringIterator;
import java.io.*;
import java.util.*;

public class StringRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter pw = new PrintWriter(new FileWriter(args[0]))) {
            for (Data d : DefinedStringIterator.forProgram(currentProgram)) {
                String s = String.valueOf(d.getValue());
                for (int i = 1; i < args.length; i++) {
                    if (s.contains(args[i])) {
                        pw.println("STRING @ " + d.getAddress() + " : " + s);
                        for (Reference r : getReferencesTo(d.getAddress())) {
                            Function f = getFunctionContaining(r.getFromAddress());
                            pw.println("   ref from " + r.getFromAddress() + " (" + r.getReferenceType() + ") func=" + (f == null ? "none" : f.getName() + "@" + f.getEntryPoint()));
                        }
                    }
                }
            }
        }
        println("done");
    }
}
