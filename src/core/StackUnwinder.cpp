#include "StackUnwinder.h"

#include "DebugFrame.h"
#include "DebugInline.h"
#include "DebugLines.h"
#include "DebugSymbols.h"

#include <fmt/format.h>

namespace adm::core {

namespace {

// Fill in name/file/line for one frame from whatever debug info is available.
void Annotate(UnwoundFrame& frame, uint32_t linkAddr, const UnwindInputs& in)
{
    if (in.symbols) {
        if (const FunctionSymbol* fn = in.symbols->ResolvePc(linkAddr)) {
            frame.name = fn->name;
        }
    }
    if (in.lines) {
        if (auto src = in.lines->ResolveLine(linkAddr)) {
            frame.file = src->file;
            frame.line = src->line;
        }
    }
}

/**
 * Append the frame at `linkAddr`, expanded through DWARF inline info.
 *
 * ResolveInlineStack returns outermost-first (the real function, then each function
 * inlined into it). A backtrace reads innermost-first, so the names go on in reverse:
 * the innermost inlined body is the frame you are actually standing in, and the real
 * function is its outermost caller. Only that last one carries the frame's pc/sp/cfa.
 */
void PushFrameWithInlines(std::vector<UnwoundFrame>& out, UnwoundFrame base, uint32_t linkAddr,
                          const UnwindInputs& in)
{
    std::vector<DebugInline::InlineLevel> levels;
    if (in.inlines) {
        levels = in.inlines->ResolveInlineLevels(linkAddr);
    }
    if (levels.size() < 2) {
        out.push_back(std::move(base));
        return;
    }

    // `base` carries the source location of the *innermost* body, which is only correct for
    // the deepest level. Every level above it should report its own call site -- where it
    // called the level below -- which is what DW_AT_call_file/call_line on the callee records.
    // Otherwise a whole inlined chain repeats one file:line and never shows the call.
    auto applyCallSite = [&in](UnwoundFrame& f, const DebugInline::InlineLevel& callee) {
        if (!callee.haveCallSite || !in.lines) {
            return;
        }
        if (auto file = in.lines->ResolveUnitFile(callee.stmtList, callee.callFile)) {
            f.file = *file;
            f.line = callee.callLine;
        }
    };

    // Innermost first: levels.back() is the body actually executing and keeps base's location.
    for (size_t i = levels.size(); i-- > 1;) {
        UnwoundFrame f = base;
        f.name = levels[i].name;
        f.inlined = true;
        if (i + 1 < levels.size()) {
            applyCallSite(f, levels[i + 1]);
        }
        out.push_back(std::move(f));
    }

    base.name = levels.front().name;
    applyCallSite(base, levels[1]);
    out.push_back(std::move(base));
}

} // namespace

UnwindResult StackUnwinder::Unwind(const UnwindInputs& in)
{
    UnwindResult result;
    result.stopPc = in.regs.pc;

    if (!in.frames || !in.frames->IsLoaded()) {
        result.stop = UnwindStop::NoDebugInfo;
        return result;
    }
    if (!in.readLong || !in.toLink) {
        result.stop = UnwindStop::NoDebugInfo;
        return result;
    }

    UnwindRegs regs = in.regs;
    uint32_t pc = in.regs.pc;
    bool isTopFrame = true;

    while (result.frames.size() < in.maxDepth) {
        result.stopPc = pc;

        // A return address points at the instruction *after* the call, which at a function
        // boundary belongs to the next function entirely. Look the rules and the source line
        // up one word earlier so the frame is attributed to the call site, but keep the real
        // return address for display -- this is the same adjustment gdb makes.
        const uint32_t lookupPc = isTopFrame ? pc : pc - 2;

        const auto link = in.toLink(lookupPc);
        if (!link) {
            result.stop = UnwindStop::NoCfi;
            return result;
        }

        CfiRow row;
        if (!in.frames->ResolveRow(*link, row)) {
            result.stop = UnwindStop::NoCfi;
            return result;
        }

        // CFA = <some register> + offset. For this toolchain the register is always a7,
        // but honouring cfaReg costs nothing and is what the format actually says.
        if (row.cfaReg >= regs.reg.size()) {
            result.stop = UnwindStop::NoCfi;
            return result;
        }
        const uint32_t cfa = regs.reg[row.cfaReg] + static_cast<uint32_t>(row.cfaOffset);

        UnwoundFrame frame;
        frame.pc = pc;
        frame.sp = regs.reg[kDwarfRegSp];
        frame.cfa = cfa;
        Annotate(frame, *link, in);
        PushFrameWithInlines(result.frames, std::move(frame), *link, in);

        // Recover the caller's registers before overwriting anything: a rule is expressed
        // against this frame's CFA, so they all have to be read from the old state.
        UnwindRegs caller = regs;
        bool memoryFault = false;
        for (uint32_t r = 0; r < static_cast<uint32_t>(caller.reg.size()); ++r) {
            const CfiRegRule& rule = row.reg[r];
            switch (rule.kind) {
                case CfiRegRuleKind::OffsetCfa: {
                    uint32_t v = 0;
                    if (!in.readLong(cfa + static_cast<uint32_t>(rule.offset), v)) {
                        memoryFault = true;
                        break;
                    }
                    caller.reg[r] = v;
                    break;
                }
                case CfiRegRuleKind::ValOffsetCfa:
                    caller.reg[r] = cfa + static_cast<uint32_t>(rule.offset);
                    break;
                case CfiRegRuleKind::Register:
                    if (rule.reg < regs.reg.size()) {
                        caller.reg[r] = regs.reg[rule.reg];
                    }
                    break;
                case CfiRegRuleKind::SameValue:
                case CfiRegRuleKind::Undefined:
                    break; // leave as-is; Undefined simply means "do not trust this"
            }
        }
        if (memoryFault) {
            result.stop = UnwindStop::UnmappedMemory;
            return result;
        }

        // The return address lives in the RA column (register 24 on m68k), which is a
        // pseudo-register: it is never a real register, only ever a memory rule.
        const CfiRegRule& raRule = row.reg[std::min<uint32_t>(row.raRegister, kCfiRegCount - 1)];
        if (raRule.kind != CfiRegRuleKind::OffsetCfa) {
            result.stop = UnwindStop::EndOfStack;
            return result;
        }
        uint32_t retAddr = 0;
        if (!in.readLong(cfa + static_cast<uint32_t>(raRule.offset), retAddr)) {
            result.stop = UnwindStop::UnmappedMemory;
            return result;
        }
        if (retAddr == 0 || (retAddr & 1) != 0) {
            // An odd address cannot be an m68k instruction, and 0 is the classic
            // end-of-chain sentinel a startup stub leaves behind.
            result.stop = (retAddr == 0) ? UnwindStop::EndOfStack : UnwindStop::UnmappedMemory;
            result.stopPc = retAddr;
            return result;
        }

        // By definition the CFA is the caller's stack pointer.
        caller.reg[kDwarfRegSp] = cfa;
        if (cfa <= regs.reg[kDwarfRegSp]) {
            // The stack grows down, so unwinding must move a7 up. Anything else means the
            // rules and the real stack have diverged, and continuing would invent frames.
            result.stop = UnwindStop::NoProgress;
            return result;
        }

        regs = caller;
        pc = retAddr;
        isTopFrame = false;
    }

    result.stop = UnwindStop::DepthLimit;
    result.stopPc = pc;
    return result;
}

std::string DescribeUnwindStop(const UnwindResult& result, const std::string& region)
{
    const std::string where = region.empty() ? fmt::format("${:08X}", result.stopPc)
                                             : fmt::format("${:08X} ({})", result.stopPc, region);

    switch (result.stop) {
        case UnwindStop::EndOfStack:
            return "end of stack";
        case UnwindStop::NoCfi:
            return fmt::format("no CFI beyond {}", where);
        case UnwindStop::NoDebugInfo:
            return "no debug info: load a program with a sibling .elf";
        case UnwindStop::UnmappedMemory:
            return fmt::format("stack trail lost at {}", where);
        case UnwindStop::DepthLimit:
            return fmt::format("frame limit reached at {}", where);
        case UnwindStop::NoProgress:
            return fmt::format("stack pointer stopped rising at {}", where);
    }
    return {};
}

} // namespace adm::core
