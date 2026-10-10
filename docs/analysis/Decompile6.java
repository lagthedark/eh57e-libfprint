// @category Analysis
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.util.*;
public class Decompile6 extends GhidraScript {
    long[] seeds = {0x18000ace0L,0x180009f50L,0x18000a6a0L,0x18000a8b0L,0x18000abf0L};
    Function resolve(long a){Function f=getFunctionContaining(toAddr(a)); if(f==null)f=getFunctionAt(toAddr(a)); return f;}
    public void run() throws Exception {
        DecompInterface decomp=new DecompInterface(); decomp.openProgram(currentProgram);
        decomp.setOptions(new DecompileOptions());
        Set<Long> done=new TreeSet<Long>();
        for(long s:seeds){ Function f=resolve(s); if(f==null){println("// none "+Long.toHexString(s));continue;}
            long fe=f.getEntryPoint().getOffset(); if(!done.add(fe))continue;
            println("\n########## "+f.getName()+" @ "+f.getEntryPoint()+" ##########");
            DecompileResults r=decomp.decompileFunction(f,120,monitor);
            if(r!=null&&r.decompileCompleted()) println(r.getDecompiledFunction().getC()); else println("// fail");
            Set<Long> callers=new TreeSet<Long>();
            for(Reference rr:getReferencesTo(f.getEntryPoint())){Function cf=getFunctionContaining(rr.getFromAddress()); if(cf!=null)callers.add(cf.getEntryPoint().getOffset());}
            println("// CALLERS: "+callers);
        }
    }
}
