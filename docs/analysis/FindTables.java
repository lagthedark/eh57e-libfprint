// Find all references into target RVA ranges and decompile containing functions.
// @category Analysis
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.util.*;

public class FindTables extends GhidraScript {
    private DecompInterface decomp;

    long[][] ranges = {
        {0x1800d6ed0L, 0x1800d7200L}, // FIR
        {0x1800d7140L, 0x1800d7320L}, // pyramid
        {0x1800e8000L, 0x1800e8100L}, // atan2
        {0x180040a00L, 0x180040a80L}, // sensor cfg
        {0x1800d19a0L, 0x1800d1f60L}, // sin+cos
    };

    private boolean inRanges(long a) {
        for (long[] r : ranges) if (a >= r[0] && a < r[1]) return true;
        return false;
    }

    public void run() throws Exception {
        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        DecompileOptions opts = new DecompileOptions();
        decomp.setOptions(opts);

        ReferenceManager rm = currentProgram.getReferenceManager();
        ReferenceIterator it = rm.getReferenceIterator(currentProgram.getMinAddress());
        Set<Long> funcs = new TreeSet<Long>();
        Map<Long, Set<Long>> hits = new TreeMap<Long, Set<Long>>();
        while (it.hasNext()) {
            Reference ref = it.next();
            Address to = ref.getToAddress();
            Address from = ref.getFromAddress();
            if (to == null || from == null) continue;
            long ta = to.getOffset();
            if (!inRanges(ta)) continue;
            Function f = getFunctionContaining(from);
            long fa = (f != null) ? f.getEntryPoint().getOffset() : -1L;
            if (fa < 0) continue;
            funcs.add(fa);
            hits.computeIfAbsent(fa, k -> new TreeSet<Long>()).add(ta);
        }

        println("=== REFERENCING FUNCTIONS ===");
        for (Long fa : funcs) {
            println(String.format("func %x  -> targets %s", fa, hits.get(fa)));
        }

        println("=== DECOMPILATIONS ===");
        FunctionManager fm = currentProgram.getFunctionManager();
        for (Long fa : funcs) {
            Function f = fm.getFunctionAt(toAddr(fa));
            if (f == null) continue;
            println("\n########## FUNCTION " + f.getName() + " @ " + f.getEntryPoint() + " ##########");
            DecompileResults res = decomp.decompileFunction(f, 60, monitor);
            if (res != null && res.decompileCompleted()) {
                println(res.getDecompiledFunction().getC());
            } else {
                println("// decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
            }
        }
    }
}
