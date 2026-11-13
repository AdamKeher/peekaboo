#include "McpConsoleTools.h"

#include <algorithm>
#include <cctype>

#include <fmt/format.h>

namespace adm::core {

using nlohmann::json;

namespace {

std::string Upper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

const McpConsoleToolSpec* FindSpec(const std::string& name)
{
    const auto& specs = McpConsoleToolSpecs();
    auto it = std::find_if(specs.begin(), specs.end(),
                           [&](const McpConsoleToolSpec& spec) { return name == spec.name; });
    return it == specs.end() ? nullptr : &*it;
}

} // namespace

const std::vector<McpConsoleToolSpec>& McpConsoleToolSpecs()
{
    // `arguments` deliberately uses the console's documented syntax. The interpreter remains the
    // one source of truth for parsing, validation, state changes, and output. `input` (declared by
    // AppendMcpConsoleToolManifest) supplies follow-up lines for interactive edit/assemble modes.
    static const std::vector<McpConsoleToolSpec> specs = {
        {"inspect_memory_hex",
         {"M", "MM", "MMM"},
         "Hex memory view/editor. Variants emit 1, 8, or 16 lines. Arguments: <addr> (<end>)."},
        {"inspect_memory_ascii",
         {"N", "NN", "NNN"},
         "ASCII memory view/editor. Variants emit 1, 8, or 16 lines. Arguments: <addr> (<end>)."},
        {"set_ascii_offset",
         {"NO"},
         "Show or set the byte offset applied by ASCII memory views. Arguments: (<offset>)."},
        {"inspect_memory_binary",
         {"Y", "YY", "YYY"},
         "Binary memory view/editor. Variants emit 1, 8, or 16 lines. Arguments: <addr> (<end>)."},
        {"set_binary_dump_width",
         {"YS"},
         "Show or set binary memory-view width. Arguments: (<bytes>, 1-8)."},
        {"inspect_memory_scrolling",
         {"MQ", "NQ"},
         "Scrolling memory view. MQ is hexadecimal and NQ is ASCII. Arguments: <addr> (<end>)."},
        {"set_cpu_register",
         {"R"},
         "Set a 68k CPU register through the shared debugger. Arguments: <register> <value>. "
         "Beyond D0-D7/A0-A7/PC/SR it takes the control registers the configured CPU has: USP, "
         "ISP (SSP on a 68000/010), MSP, VBR, SFC, DFC, CACR, CAAR, BUSCR, PCR and the "
         "68040/060 MMU registers (TC, URP, SRP, ITTn, DTTn, MMUSR). A register the CPU lacks "
         "is refused with the CPUs that have it; FPU and 68030 PMOVE registers are read-only. "
         "With no arguments, returns the console register display."},
        {"browse_custom_registers",
         {"E"},
         "Browse/edit any custom-chip register. Arguments: (<offset 000-1FE>); use input for "
         "edited register lines."},
        {"run_from_address",
         {"G", "X"},
         "Start/resume execution, optionally from an address. G accepts (<addr>); X resumes."},
        {"set_turbo",
         {"TURBO", "WARP"},
         "Show, toggle, or set emulator warp mode. Arguments: (<ON|OFF>)."},
        {"trace_until", {"TA"}, "Trace until PC reaches an address or range: <addr> (<end>)."},
        {"manage_breakpoints",
         {"BS", "BD", "B", "BDA"},
         "Set, remove, list, or clear PC breakpoints with full conditional, mask, ignored-hit, "
         "and one-shot syntax."},
        {"manage_watchpoints",
         {"MS", "MD", "MW", "MDA"},
         "Set, remove, list, or clear memory write watchpoints."},
        {"find_memory",
         {"F", "FS", "FR"},
         "Search memory. F is exact, FS is case-insensitive, and FR is relative. Arguments use "
         "the console syntax: (-L) <string|bytes>(,<start> <end>)."},
        {"find_memory_accesses",
         {"FA", "FAQ"},
         "Find instructions accessing an address; FAQ is concise. Arguments: <addr> (<start> "
         "<end>)."},
        {"compare_memory", {"COMP"}, "Compare blocks: <start> <end> <destination>."},
        {"transfer_memory", {"TRANS"}, "Copy a memory block: <start> <end> <destination>."},
        {"exchange_memory", {"EX"}, "Exchange two memory blocks: <start> <end> <destination>."},
        {"fill_memory", {"O"}, "Fill memory: <string|bytes>, <start> <end>."},
        {"write_memory_string", {"WS"}, "Write bytes or a string: <string|bytes>, <address>."},
        {"xor_memory", {"MEMCODE"}, "XOR each byte in a range: <start> <end> <byte>."},
        {"add_memory", {"ADD"}, "Add a value to every byte: <start> <end> <value>."},
        {"load_memory_file", {"LM"}, "Load a host file into memory: <file>,<address>."},
        {"save_memory_file", {"SM"}, "Save memory to a host file: <file>,<start> <end>."},
        {"assemble",
         {"A"},
         "Assemble 68k code at <address>. Supply assembly source lines with input; the same "
         "interactive assembler and validation as the console are used."},
        {"set_bootblock_break", {"FB", "TRB"}, "Toggle break-on-bootblock entry."},
        {"set_exception_break", {"EB"}, "Configure CPU exception breaks: <vector...|ALL|OFF>."},
        {"set_beam_break",
         {"BL"},
         "Configure a raster-position break: <line> [hpos] or OFF. Raster lines are normally "
         "written in decimal (PAL has 313, DIWSTRT sits at 44), but this argument is hex: \"227\" "
         "arms line 551, and line 227 is \"E3\" or \"!227\". The reply always echoes the line it "
         "resolved to, in decimal — check it."},
        {"set_copper_break", {"BC"}, "Toggle a Copper-instruction breakpoint: <address>."},
        {"trainer_search",
         {"TS", "T", "TX", "TF", "TFD"},
         "Run classic trainer operations. Select start/continue/exit/find/find-and-remove with "
         "variant and pass the documented console arguments."},
        {"memory_protect",
         {"PR", "PROTECT"},
         "Block guest writes to an address or range, or force a value there - the step that "
         "turns an address found by trainer_search into a live cheat. Also lists, enables, "
         "removes and rescans locks, and loads/saves trainer files (PR LOAD|SAVE|IMPORT). "
         "Grammar: PR | PR <addr> BLOCK (LEN=<n>) | PR <addr> SET=<value> (SIZE=8|16|32) | "
         "PR ON|OFF <id> | PR DEL <addr|id> | PR CLEAR | PR RESCAN | PR LOAD|SAVE|IMPORT <path>."},
        {"deep_trainer",
         {"TDS", "TDC", "TD", "TDI", "TDD", "TDX"},
         "Run deep-trainer start/change/list/inspect/delete/exit operations; select variant and "
         "pass the documented console arguments."},
        {"load_via_console",
         {"LOAD", "L"},
         "Load a path, recent-file alias, or open-document alias with exact console semantics. "
         "Arguments: (<path|fN|dN|file:...|doc:...>) (FORCE|!)."},
        {"list_recent_files", {"LIST"}, "List recent files and their fN aliases."},
        {"reset_emulator",
         {"RESET"},
         "Reset the running emulator session (a warm reset of the machine). Unlike start(), "
         "this does NOT rebase the frame counter: frameCount keeps counting and get_status's "
         "frameSession is unchanged, so frame numbers and run_until_frame targets taken before "
         "the reset stay valid."},
        {"manage_hard_drives",
         {"HD"},
         "List or configure DH0/DH1, each backed by a host directory or an .hdf/.hdz hardfile. "
         "No arguments lists configured and effective drives. Grammar: HD MOUNT DH0|DH1 "
         "<directory|hardfile> [RO|RW] [LABEL <name>] [PRI <n>] [CTRL UAE|IDE|A590|A2091|A4091] "
         "[UNIT <n>] [GEOM AUTO|RDB|<sectors>/<surfaces>/<reserved>/<blocksize>] | HD SET DH0|DH1 "
         "<same options> | HD UNMOUNT DH0|DH1 | HD INFO DH0|DH1|<hardfile> (RDB partition table) "
         "| HD ROM [A590|A2091|A4091 <path>|CLEAR] (SCSI card boot ROM) | HD OPEN|REFRESH "
         "DH0|DH1 | HD APPLY (cold restart with the configured drives; changes are pending until "
         "then). A path ending .hdf/.hdz, or naming an existing file, mounts a hardfile. IDE needs "
         "an A600/A1200/A4000; A4091 needs an A4000. get_status reports hardDrives[]. PRI and UNIT "
         "are decimal."},
        {"select_model",
         {"MODEL"},
         "List the machine presets (the current one is marked *, presets whose ROMs are missing "
         "say why), or select one by id, e.g. A1200 or CD32. Selecting restarts a running "
         "machine. get_status reports the model."},
        {"launch_whdload",
         {"WHD"},
         "Boot a WHDLoad install with the core's own WHDLoad: arguments <archive.lha|lzx | game "
         "directory | .slave | .info> (a host path, quote it if it has spaces). The game goes on "
         "DH0 transiently, WHDLoad: boots it, saved games land in WHDSaves:; the selected machine "
         "is used (most games want MODEL A1200 or A500+). No arguments shows what is running and "
         "the WHDLoad.prefs settings. 'OFF' restarts with the saved hard drives. 'SET "
         "BUTTONWAIT|NOWRITECACHE ON|OFF', 'SET PREFS NONE|CONFIG|SPLASH|BOTH', 'SET THEME "
         "DEFAULT|NATIVE' change the settings for the next launch. get_status reports whdload."},
        {"manage_cd_drive",
         {"CD"},
         "Show or change the disc in CD0. No arguments: the chosen disc, the image the machine is "
         "given, what the running machine has, and whether an archive is still extracting. "
         "'INSERT <path>' takes a .cue/.ccd/.mds/.nrg/.chd/.iso, or a .7z/.zip holding one (TOSEC "
         "CD32 sets), which is extracted into a cache in the background first - poll CD until "
         "it stops saying extracting. A CD32/CDTV (select_model) or a machine started with a disc "
         "takes it at once; any other machine takes it at the next power-on. 'EJECT' empties "
         "the drive. get_status reports cd."},
        {"configure_machine",
         {"MACHINE"},
         "Show or change the chipset and MMU below the model preset. No arguments: each setting as "
         "configured and as the running machine has it, plus its CPU, FPU and MMU. '<setting> "
         "<value>' stores one for the next power-on: CHIPSET "
         "AUTO|OCS|ECS_AGNUS|ECS_DENISE|ECS|AGA, "
         "EXTRA (the motherboard: Gayle, RTC, ROM mirrors) AUTO|GENERIC|A500|A500+|A600|A1000|"
         "A1200|A2000|A3000|A3000T|A4000|A4000T|CDTV|CDTV-CR|CD32|VELVET, AGNUS AUTO|VELVET|A1000|"
         "OCS|ECS|AGA, AGNUSSIZE AUTO|512K|1M|2M, DENISE AUTO|VELVET|A1000_NOEHB|A1000|OCS|ECS|"
         "AGA, MMU AUTO|FULL|EC|NONE (68030 and up; AUTO is a full MMU, EC runs MMU instructions "
         "without translating, NONE leaves MMU emulation out). '<setting>' alone lists its values. "
         "'APPLY' cold-restarts a running machine with "
         "them; 'DEFAULTS' puts every setting back to AUTO. select_model resets them all. "
         "get_status reports machine."},
        {"manage_serial_port",
         {"SERIAL"},
         "Show or change where the Amiga's serial port goes; a running machine takes the change "
         "at its next frame. No arguments: the target, whether it opened, connection and byte "
         "counts, and the host's serial devices. 'LISTEN [host:]port' makes it a TCP server for "
         "one client (telnet, a remote debugger, another emulator); 'CONNECT host:port' dials a "
         "TCP server and redials when the link drops; a device name (COM3, /dev/ttyUSB0) opens "
         "a host serial port at the baud rate the Amiga programs; 'TCP://host:port(/connect)' is "
         "the same in WinUAE's spelling; 'OFF' disconnects. Everything the Amiga sends is still "
         "captured for KPrintF breakpoints either way. get_status reports serial."},
        {"manage_sound_card",
         {"SOUNDCARD"},
         "Show or change the expansion sound card, applied at the next power-on: OFF, TOCCATA, "
         "PRELUDE (Zorro II cards), PRELUDE1200 (A1200 clock port), UAESND or UAESNDZ3 (WinUAE's "
         "own card, driven by WinUAE's AHI driver; Zorro III needs a 32-bit CPU). The card mixes "
         "into the same audio output as Paula, so it shows in the core audio recording; the "
         "Amiga needs the card's driver or AHI to use it. Recording input is not connected. "
         "'APPLY' cold-restarts a running machine. get_status reports soundCard."},
        {"manage_network",
         {"NETWORK"},
         "Show or change networking; every setting applies at the next power-on. 'BSDSOCKET "
         "ON|OFF' installs bsdsocket.library: Amiga programs that open it (browsers, FTP/IRC "
         "clients, wget) use the host's TCP/IP stack directly, with no Amiga TCP/IP stack; the "
         "UAE Boot ROM comes with it. 'A2065 OFF|NAT|OPEN' fits a Commodore A2065 Ethernet card "
         "behind SLIRP, a NAT inside the emulator: the Amiga is 10.0.2.15, gateway 10.0.2.2 "
         "(which is also the host's loopback), DNS 10.0.2.3, DHCP answers; OPEN also forwards "
         "host ports 21-23 and 80, on all interfaces, to the Amiga; it needs a2065.device and a "
         "TCP/IP stack on the "
         "Amiga. 'UAENET ON|OFF' adds uaenet.device, a SANA-II driver on the same NAT (unit 0; "
         "unit 1 with open ports) - use it or the A2065, not both. The A2065 and uaenet.device "
         "exist only in Windows cores. 'APPLY' cold-restarts a running machine with the "
         "settings. get_status reports network."},
        {"manage_midi",
         {"MIDI"},
         "Show or choose the host MIDI devices the Amiga's serial port uses at MIDI speed "
         "(Windows). Amiga MIDI interfaces sit on the serial port: while software runs the UART "
         "at 31250 baud the bytes go to MIDI out instead of the serial connection, and MIDI in "
         "arrives as received serial data. No arguments: the chosen devices, whether MIDI is "
         "active with message counts, and the host's devices by number. 'OUT <n>|MAPPER|NONE' "
         "and 'IN <n>|NONE' choose; both may be given at once. get_status reports midi."},
        {"manage_parallel_port",
         {"PARALLEL"},
         "Show or change the parallel port's capture file. '<file>' appends every byte the "
         "Amiga strobes out of the parallel port (a printer driver, PRT:, PAR:) to that host "
         "file; 'OFF' disconnects it. get_status reports parallel."},
        {"console_help", {"HELP", "H"}, "Return the debugger-console command reference."},
        {"console_keys", {"KEYS"}, "Return console cursor and line-editing key bindings."},
        {"clear_console", {"CLS", "CLEAR"}, "Clear the debugger console and restore its banner."},
        {"get_versions",
         {"VERSION", "VER", "V"},
         "Return PEEKaboo, PuAE core, and debugger-console versions."},
        {"read_disk_track",
         {"RT"},
         "Read disk tracks into emulator memory: <track> (<count> <address>)."},
        {"read_disk_sectors",
         {"RS"},
         "Read disk sectors into emulator memory: <sector> (<count> <address>)."},
        {"read_disk_bytes",
         {"RB"},
         "Read disk bytes into emulator memory: <offset> (<count> <address>)."},
        {"write_disk_track",
         {"WT"},
         "Write disk tracks from emulator memory: <track> <count> <address>."},
        {"write_disk_sectors",
         {"WDS"},
         "Write disk sectors from emulator memory: <sector> <count> <address>."},
        {"write_disk_bytes",
         {"WDB"},
         "Write disk bytes from emulator memory: <offset> <count> <address>."},
        {"repair_disk_checksum",
         {"BOOTCHK", "DATACHK", "BAMCHK"},
         "Repair a bootblock, OFS data-block, or bitmap-block checksum at <address>."},
        {"calculate",
         {"?"},
         "Evaluate a 32-bit expression with +, -, *, /, and parentheses; returns hex, decimal, "
         "and binary forms."},
        {"checksum_memory",
         {"CRC16", "CRC32"},
         "Calculate CRC-16/CCITT-FALSE or CRC-32 over <start> <end>."},
        {"inspect_stack",
         {"STACK"},
         "Dump longwords from A7 with pointed-to memory-region hints. Arguments: (<count>)."},
        {"inspect_call_chain",
         {"CALL"},
         "Find a heuristic call chain from BSR/JSR return addresses. Arguments: (<count>)."},
        {"inspect_exec_base", {"EXECBASE"}, "Return key fields from the live ExecBase."},
        {"inspect_available_memory", {"AVAIL"}, "Return Chip/Fast RAM totals and free space."},
        {"list_exec_libraries", {"LIBRARIES", "LIBS"}, "List loaded AmigaOS libraries."},
        {"list_exec_resources", {"RESOURCES"}, "List AmigaOS system resources."},
        {"list_exec_devices", {"DEVICES"}, "List loaded AmigaOS devices."},
        {"list_exec_tasks", {"TASKS"}, "List running, ready, and waiting AmigaOS tasks."},
        {"list_exec_ports", {"PORTS"}, "List public AmigaOS message ports."},
        {"list_exec_interrupts", {"INTERRUPTS", "INTS"}, "List Exec interrupt vectors."},
        {"list_exception_vectors", {"EXCEPTIONS"}, "List the 68000 exception-vector table."},
        {"list_custom_registers",
         {"CHIPREGS"},
         "List every known custom-chip register with address, value, and description."},
        {"decode_custom_register",
         {"DCHIP"},
         "Decode one custom-chip register bit by bit. Arguments: <name|offset>."},
        {"get_kickstart_version",
         {"KICKVER"},
         "Return Kickstart version, date, and ROM address range."},
    };
    return specs;
}

// Stated on every console-backed tool. Bare numbers being hex is the AR console's convention and
// is not going to change, but it is silently wrong in the natural direction: raster lines, sector
// counts and frame numbers are all written in decimal elsewhere, so an unprefixed decimal figure
// lands on a plausible-looking wrong value instead of erroring.
const char* kNumberBaseNote =
    "Numbers are parsed in HEX by default, as in the AR console: 227 means 551. Prefix ! for "
    "decimal (!227), $ or 0x for explicit hex, % for binary.";

void AppendMcpConsoleToolManifest(json& tools)
{
    for (const auto& spec : McpConsoleToolSpecs()) {
        json properties = {
            {"arguments",
             {{"type", "string"},
              {"description",
               fmt::format("Arguments in the documented console syntax, without the command "
                           "token. Omit for commands or query forms that take no arguments. {}",
                           kNumberBaseNote)}}},
            {"input",
             {{"type", "array"},
              {"items", {{"type", "string"}}},
              {"description", "Optional follow-up lines submitted verbatim to an interactive "
                              "memory editor or assembler after the initial command."}}},
        };
        if (spec.commands.size() > 1) {
            json variants = json::array();
            for (const char* command : spec.commands)
                variants.push_back(command);
            properties["variant"] = {
                {"type", "string"},
                {"enum", std::move(variants)},
                {"default", spec.commands.front()},
                {"description", "Console command variant; defaults to the first value."},
            };
        }
        tools.push_back(
            {{"name", spec.name},
             {"description", fmt::format("{} Uses the same implementation as the AR console. {}",
                                         spec.description, kNumberBaseNote)},
             {"inputSchema", {{"type", "object"}, {"properties", properties}}}});
    }
}

std::optional<json>
DispatchMcpConsoleTool(const std::string& name, const json& args,
                       const std::function<std::string(const std::string&)>& execConsole)
{
    const McpConsoleToolSpec* spec = FindSpec(name);
    if (!spec)
        return std::nullopt;
    if (!execConsole)
        return json{{"error", "console unavailable"}};
    if (!args.is_object())
        return json{{"error", "arguments must be an object"}};

    std::string command = spec->commands.front();
    if (args.contains("variant")) {
        if (!args["variant"].is_string())
            return json{{"error", "variant must be a string"}};
        const std::string requested = Upper(args["variant"].get<std::string>());
        auto it = std::find_if(spec->commands.begin(), spec->commands.end(),
                               [&](const char* candidate) { return requested == candidate; });
        if (it == spec->commands.end())
            return json{{"error", fmt::format("unsupported variant: {}", requested)}};
        command = *it;
    }

    if (args.contains("arguments")) {
        if (!args["arguments"].is_string())
            return json{{"error", "arguments must be a string"}};
        const std::string commandArgs = args["arguments"].get<std::string>();
        if (!commandArgs.empty())
            command += " " + commandArgs;
    }

    std::vector<std::string> inputLines;
    if (args.contains("input")) {
        if (!args["input"].is_array())
            return json{{"error", "input must be an array of strings"}};
        for (const auto& lineNode : args["input"]) {
            if (!lineNode.is_string())
                return json{{"error", "input must contain only strings"}};
            inputLines.push_back(lineNode.get<std::string>());
        }
    }

    json transcript = json::array();
    std::string output = execConsole(command);
    transcript.push_back({{"line", command}, {"output", output}});

    if (!inputLines.empty()) {
        for (const std::string& line : inputLines) {
            const std::string lineOutput = execConsole(line);
            transcript.push_back({{"line", line}, {"output", lineOutput}});
            if (!output.empty() && !lineOutput.empty() && output.back() != '\n')
                output.push_back('\n');
            output += lineOutput;
        }
    }

    json result{{"command", command}, {"output", output}};
    if (transcript.size() > 1)
        result["transcript"] = std::move(transcript);
    return result;
}

} // namespace adm::core
