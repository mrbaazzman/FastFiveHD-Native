// Create functions at caller-supplied ELF addresses and dump only those functions.
//
// Usage: -postScript AddAndDumpFunctions.java <output.tsv> <elf-address>...
//
// Ghidra addresses are ELF + 0x10000. Output uses the same format as DumpPcode.java.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.pcode.Varnode;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class AddAndDumpFunctions extends GhidraScript {

    private static final long IMAGE_BASE = 0x10000L;

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
        if (args.length < 2) {
            throw new IllegalArgumentException("usage: AddAndDumpFunctions.java <out.tsv> <addr>...");
        }
        File outFile = new File(args[0]);
        File parent = outFile.getParentFile();
        if (parent != null) { parent.mkdirs(); }

        List<Long> addrs = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            String s = args[i].trim();
            if (s.startsWith("0x") || s.startsWith("0X")) { s = s.substring(2); }
            addrs.add(Long.parseLong(s, 16));
        }

        try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(outFile)))) {
            w.println("H\t" + currentProgram.getName() + "\t" + currentProgram.getImageBase());
            int count = 0;
            for (long elf : addrs) {
                Address a = toAddr(elf + IMAGE_BASE);
                Function f = getFunctionAt(a);
                if (f == null) {
                    disassemble(a);
                    f = createFunction(a, null);
                }
                if (f == null) {
                    println("FAILED createFunction at 0x" + Long.toHexString(elf));
                    continue;
                }
                long size = f.getBody().getNumAddresses();
                println("OK 0x" + Long.toHexString(elf) + " -> " + f.getName() + " size=" + size);
                w.println("F\t" + f.getEntryPoint() + "\t"
                        + f.getName().replace('\t', ' ').replace('\n', ' ') + "\t"
                        + size + "\t0\t0");
                InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
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
        println("AddAndDumpFunctions: wrote " + outFile.getAbsolutePath());
    }
}
