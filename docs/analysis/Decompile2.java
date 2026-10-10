// @category Analysis
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.util.*;

public class Decompile2 extends GhidraScript {
    private DecompInterface decomp;

    // addresses; containing function will be resolved
    long[] seeds = {
        0x180016be0L,0x1800168b0L,0x180018380L,0x180010cd0L,0x180010f20L,
        0x180019c00L,0x1800199a0L,0x180024190L,0x180024920L,0x180028fc0L,
        0x180018c8fL,0x1800192dfL,0x1800147fdL,0x180018340L,0x180018de3L,
        0x180009ab0L,0x18000a1b0L,0x18000cb80L,0x18001f460L,
        0x180017f80L,0x180018960L,0x1800194e0L,0x180017eb0L
    };

    long[][] ranges = {
        {0x1800d6ed0L, 0x1800d7030L}, // FIR
        {0x1800d7030L, 0x1800d7130L}, // atan2
        {0x1800d7140L, 0x1800d7324L}, // pyramid
        {0x1800d7330L, 0x1800d7420L}, // extra after pyramid
        {0x1800d19a0L, 0x1800d1f40L}, // sin
        {0x1800ca0a0L, 0x1800ca640L}, // cos
        {0x180059bb0L, 0x180059c00L}, // length byte tables
        {0x180040a00L, 0x180040a80L}, // sensor cfg candidate
        {0x1800ca000L, 0x1800ca0a0L},
    };

    private String rangeName(long a){
        for (long[] r: ranges) if (a>=r[0]&&a<r[1]) return String.format("[%x]",r[0]);
        return "?";
    }
    private boolean inRanges(long a){ for(long[] r:ranges) if(a>=r[0]&&a<r[1]) return true; return false;}

    Function resolve(long a){
        Function f=getFunctionContaining(toAddr(a));
        if(f==null){ f=getFunctionAt(toAddr(a)); }
        return f;
    }

    public void run() throws Exception {
        decomp=new DecompInterface();
        decomp.openProgram(currentProgram);
        DecompileOptions opts=new DecompileOptions();
        decomp.setOptions(opts);

        // table reference scan
        ReferenceManager rm=currentProgram.getReferenceManager();
        ReferenceIterator it=rm.getReferenceIterator(currentProgram.getMinAddress());
        Map<Long,Set<Long>> hits=new TreeMap<Long,Set<Long>>();
        while(it.hasNext()){
            Reference ref=it.next();
            Address to=ref.getToAddress(), from=ref.getFromAddress();
            if(to==null||from==null) continue;
            if(!inRanges(to.getOffset())) continue;
            Function f=getFunctionContaining(from);
            long fa=(f!=null)?f.getEntryPoint().getOffset():-2L;
            hits.computeIfAbsent(fa,k->new TreeSet<Long>()).add(to.getOffset());
        }
        println("=== TABLE REF HITS ===");
        for(Long fa:hits.keySet()){
            StringBuilder sb=new StringBuilder();
            for(Long t:hits.get(fa)) sb.append(String.format(" %x%s",t,rangeName(t)));
            println(String.format("func %x ->%s",fa,sb));
        }

        // decompile seeds
        Set<Long> done=new TreeSet<Long>();
        FunctionManager fm=currentProgram.getFunctionManager();
        for(long s:seeds){
            Function f=resolve(s);
            if(f==null){ println("// no function containing "+Long.toHexString(s)); continue; }
            long fe=f.getEntryPoint().getOffset();
            if(!done.add(fe)) continue;
            println("\n########## "+f.getName()+" @ "+f.getEntryPoint()+" ##########");
            DecompileResults res=decomp.decompileFunction(f,90,monitor);
            if(res!=null&&res.decompileCompleted()) println(res.getDecompiledFunction().getC());
            else println("// fail "+(res!=null?res.getErrorMessage():"null"));
            // callers
            Set<Long> callers=new TreeSet<Long>();
            for(Reference r:getReferencesTo(f.getEntryPoint())){
                Function cf=getFunctionContaining(r.getFromAddress());
                if(cf!=null) callers.add(cf.getEntryPoint().getOffset());
            }
            println("// CALLERS: "+callers);
        }
    }
}
