// Dump raw per-instruction P-code for functions of an imported program.
//
// Output is tab-separated and easy to stream:
//   H <program> <imageBase>
//   F <entry> <name> <size> <external> <thunk>
//   I <address> <length> <mnemonic>
//   P <opcode> <out> <in1> <in2> ...
//   X <functionCount>
//
// A varnode is rendered as <space>:<offset>:<size>; constants use space "const".
//
// Usage: -postScript DumpPcode.java <output-file> [maxFunctionSize]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DumpPcode extends GhidraScript {

    private String vn(Varnode v) {
        if (v == null) {
            return "-";
        }
        AddressSpace space = v.getAddress().getAddressSpace();
        if (space.isRegisterSpace()) {
            Register reg = currentProgram.getRegister(v);
            String name = reg != null ? reg.getName() : ("reg" + Long.toUnsignedString(v.getOffset()));
            return "reg:" + name + ":" + v.getSize();
        }
        if (space.isUniqueSpace()) {
            return "uniq:" + Long.toUnsignedString(v.getOffset()) + ":" + v.getSize();
        }
        return space.getName() + ":" + Long.toUnsignedString(v.getOffset()) + ":" + v.getSize();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: DumpPcode.java <output-file> [maxFunctionSize]");
        }
        File outFile = new File(args[0]);
        long maxSize = args.length > 1 ? Long.parseLong(args[1]) : 0L;
        File parent = outFile.getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }

        try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(outFile)))) {
            w.println("H\t" + currentProgram.getName() + "\t" + currentProgram.getImageBase());
            FunctionManager fm = currentProgram.getFunctionManager();
            int count = 0;
            for (Function f : fm.getFunctions(true)) {
                long size = f.getBody().getNumAddresses();
                if (maxSize > 0 && size > maxSize) {
                    continue;
                }
                w.println("F\t" + f.getEntryPoint() + "\t"
                        + f.getName().replace('\t', ' ').replace('\n', ' ') + "\t"
                        + size + "\t" + (f.isExternal() ? 1 : 0) + "\t" + (f.isThunk() ? 1 : 0));
                InstructionIterator it =
                        currentProgram.getListing().getInstructions(f.getBody(), true);
                while (it.hasNext()) {
                    Instruction insn = it.next();
                    w.println("I\t" + insn.getAddress() + "\t" + insn.getLength() + "\t"
                            + insn.getMnemonicString());
                    for (PcodeOp op : insn.getPcode()) {
                        StringBuilder sb = new StringBuilder("P\t");
                        if (op.getOpcode() == PcodeOp.CALLOTHER) {
                            int id = (int) op.getInput(0).getOffset();
                            String name = currentProgram.getLanguage().getUserDefinedOpName(id);
                            sb.append("CALLOTHER:").append(name != null ? name : ("userop" + id));
                        } else {
                            sb.append(op.getMnemonic());
                        }
                        sb.append('\t').append(vn(op.getOutput()));
                        for (Varnode v : op.getInputs()) {
                            sb.append('\t').append(vn(v));
                        }
                        w.println(sb.toString());
                    }
                }
                count++;
            }
            w.println("X\t" + count);
        }
        println("DumpPcode: wrote " + outFile.getAbsolutePath());
    }
}
