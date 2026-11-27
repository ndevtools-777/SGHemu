// Reverse-engineering helper only; emulator runtime remains C17.
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
import java.io.*;
import java.math.BigInteger;
import java.util.*;
import java.util.regex.*;

public class C100Irom extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Set<String> wanted = new HashSet<>(Arrays.asList(
            "mmi_VersionInit", "mmi_BuildHWVersionScr", "UHGetIromVersion", "uhConvertI16toA",
            "IromBistResults", "GenerateCRC", "IrDATxDMA", "SendUAresp", "SendRR", "SendRNR", "SendData",
            "SendDiscoveryResponse", "DoCRCTest", "IrFrameSearch", "IrCreateFrame", "gprsComputePPP16bitFCS",
            "gprsPutHDLC", "gprsCheckPPP16bitFCS", "gprsCopyPPPRawCirc", "gprsDecodePPPHDLC", "memcpy"));
        Pattern listingEntry =
            Pattern.compile("^\\s+(\\S+)\\s+0x([0-9a-fA-F]+)\\s+Thumb Code\\s+(\\d+)\\s+.*$");
        List<Function> functions = new ArrayList<>();
        for (String line : java.nio.file.Files.readAllLines(java.nio.file.Path.of(args[0]))) {
            Matcher entry = listingEntry.matcher(line);
            if (!entry.matches() || !wanted.contains(entry.group(1)))
                continue;
            String name = entry.group(1);
            long address = Long.parseLong(entry.group(2), 16);
            int length = Integer.parseInt(entry.group(3));
            if (length == 0)
                length = 46;
            Address start = toAddr(address);
            Address end = toAddr(address + length - 1);
            if (!currentProgram.getMemory().contains(start))
                continue;
            clearListing(start, end);
            currentProgram.getProgramContext().setValue(currentProgram.getRegister("TMode"), start, end,
                                                        BigInteger.ONE);
            new DisassembleCommand(start, new AddressSet(start, end), true).applyTo(currentProgram, monitor);
            Function function = getFunctionAt(start);
            if (function == null)
                function = createFunction(start, name);
            if (function != null) {
                function.setName(name, SourceType.USER_DEFINED);
                functions.add(function);
            }
        }
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[1])) {
            for (Function function : functions) {
                out.println("/* " + function.getEntryPoint() + " " + function.getName() + " */");
                DecompileResults result = decompiler.decompileFunction(function, 30, monitor);
                out.println(result.decompileCompleted() ? result.getDecompiledFunction().getC()
                                                        : result.getErrorMessage());
            }
        } finally {
            decompiler.dispose();
        }
    }
}
