#include "plugin/Probe.h"

#include "plugin/Events.h"
#include "plugin/Frees.h"
#include "plugin/Memory.h"
#include "plugin/Report.h"

namespace StaleParentCellFix::Probe
{
    namespace
    {
        std::atomic<int> remaining{ 0 };

        std::vector<void*> Unwind(CONTEXT context)
        {
            std::vector<void*> frames;
            for (int i = 0; i < 40 && context.Rip; ++i) {
                frames.push_back(reinterpret_cast<void*>(context.Rip));
                DWORD64 imageBase = 0;
                auto* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (function) {
                    void* handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                        &establisher, nullptr);
                } else {
                    const auto ret = Memory::Read<DWORD64>(context.Rsp);
                    if (!ret) {
                        break;
                    }
                    context.Rip = *ret;
                    context.Rsp += 8;
                }
            }
            return frames;
        }

        std::string Value(std::uintptr_t value)
        {
            std::string out = std::format("0x{:X}", value);
            if (const auto freed = Frees::Brief(value); !freed.empty()) {
                out += "  <-- " + freed;
            } else if (const auto vtable = Memory::Read<std::uintptr_t>(value); vtable && Memory::InGameImage(*vtable)) {
                if (const auto name = Memory::ClassName(*vtable); !name.empty()) {
                    out += " (" + name + ")";
                }
            }
            return out;
        }

        LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
        {
            const auto* record = info->ExceptionRecord;
            const auto* context = info->ContextRecord;
            if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || Memory::Probing() ||
                !Memory::InGameImage(context->Rip) || remaining.fetch_sub(1) <= 0) {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            std::string text = std::format("ACCESS VIOLATION (first chance) at {} on {}: {} 0x{:X}",
                Memory::Symbol(context->Rip), Report::ThreadName(GetCurrentThreadId()),
                record->ExceptionInformation[0] ? "writing" : "reading", record->ExceptionInformation[1]);
            const std::pair<const char*, DWORD64> registers[]{ { "rax", context->Rax }, { "rbx", context->Rbx },
                { "rcx", context->Rcx }, { "rdx", context->Rdx }, { "rsi", context->Rsi }, { "rdi", context->Rdi },
                { "rbp", context->Rbp }, { "r8", context->R8 }, { "r9", context->R9 }, { "r10", context->R10 },
                { "r11", context->R11 }, { "r12", context->R12 }, { "r13", context->R13 }, { "r14", context->R14 },
                { "r15", context->R15 } };
            text += "\n    registers:";
            for (const auto& [name, value] : registers) {
                text += std::format("\n      {:3} {}", name, Value(value));
            }
            text += "\n    stack slots pointing into destroyed objects:";
            for (std::uintptr_t slot = 0; slot < 0x600; slot += 8) {
                const auto value = Memory::Read<std::uintptr_t>(context->Rsp + slot);
                if (!value) {
                    break;
                }
                if (const auto freed = Frees::Brief(*value); !freed.empty()) {
                    text += std::format("\n      [rsp+{:X}] 0x{:X}  <-- {}", slot, *value, freed);
                }
            }
            const auto frames = Unwind(*context);
            text += "\n    call stack:" + Report::StackTrace(frames);
            if (const auto events = Events::Recent(); !events.empty()) {
                text += "\n    recent Change Location events:" + events;
            }
            spdlog::error("{}", text);
            spdlog::default_logger()->flush();
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void Install(int reports)
    {
        remaining = reports;
        if (AddVectoredExceptionHandler(1, Handler)) {
            spdlog::info("logging the first {} access violations in SkyrimSE.exe code", reports);
        }
    }
}
