// @category Analysis
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.util.*;

public class Decompile3 extends GhidraScript {
    private DecompInterface decomp;

    long[] seeds = {
        0x180017710L,0x1800178a0L,0x180017be0L,0x180016a30L,0x180011b20L,
        0x180012540L,0x1800129b0L,0x180011870L,0x180008150L,0x180008b80L,
        0x180015e80L,0x1800134b0L,0x180007c60L,0x180007cf0L,0x180007550L,
        0x180007a10L,0x180004c4cL,0x1800057f0L,0x180007130L,0x180013700L,
        0x1800099e0L,0x180011b20L,0x1800134b0L,0x180017710L
    };
    Function resolve(long a){ Function f=getFunctionContaining(toAddr(a)); if(f==null)f=getFunctionAt(toAddr(a)); return f;}
    public void run() throws Exception {
        decomp=new DecompInterface(); decomp.openProgram(currentProgram);
        DecompileOptions opts=new DecompileOptions(); decomp.setOptions(opts);
        Set<Long> done=new TreeSet<Long>();
        for(long s:seeds){
            Function f=resolve(s);
            if(f==null){ println("// none @"+Long.toHexString(s)); continue; }
            long fe=f.getEntryPoint().getOffset();
            if(!done.add(fe)) continue;
            println("\n########## "+f.getName()+" @ "+f.getEntryPoint()+" ##########");
            DecompileResults res=decomp.decompileFunction(f,90,monitor);
            if(res!=null&&res.decompileCompleted()) println(res.getDecompiledFunction().getC());
            else println("// fail");
            Set<Long> callers=new TreeSet<Long>();
            for(Reference r:getReferencesTo(f.getEntryPoint())){ Function cf=getFunctionContaining(r.getFromAddress()); if(cf!=null)callers.add(cf.getEntryPoint().getOffset()); }
            println("// CALLERS: "+callers);
        }
    }
}
