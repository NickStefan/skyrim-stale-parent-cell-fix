#include "plugin/Deletes.h"

#include "plugin/Forms.h"
#include "plugin/Memory.h"
#include "plugin/Report.h"
#include "plugin/Ring.h"

namespace StaleParentCellFix::Deletes
{
    namespace
    {
        struct Record
        {
            std::uintptr_t address;
            std::uint64_t time;
            std::uint32_t formID;
            std::uint32_t baseFormID;
            std::uint32_t flags;  // before the deleted flag was set
            std::uint32_t cellFormID;
            std::uint32_t thread;
            Stack stack;
        };

        Ring<Record, 8192> deletes;
        bool installed = false;

        std::uint32_t FormIDAt(std::uintptr_t pointerAddress)
        {
            const auto form = Memory::Read<std::uintptr_t>(pointerAddress);
            if (!form || !*form) {
                return 0;
            }
            const auto header = Forms::ReadHeader(*form);
            return header ? header->formID : 0;
        }

        void Remember(RE::TESObjectREFR* reference)
        {
            const auto address = reinterpret_cast<std::uintptr_t>(reference);
            auto& record = deletes.Next();
            record.address = address;
            record.time = GetTickCount64();
            const auto header = Forms::ReadHeader(address);
            record.formID = header ? header->formID : 0;
            record.flags = header ? header->flags : 0;
            record.baseFormID = FormIDAt(address + Forms::Offset::RefBaseObject);
            record.cellFormID = FormIDAt(address + Forms::Offset::RefParentCell);
            record.thread = GetCurrentThreadId();
            record.stack.Capture();
        }

        using SetDeleteFn = void (*)(RE::TESObjectREFR*, bool);

        // One per vtable: each keeps the function its own slot held.
        template <std::size_t Slot>
        struct Hook
        {
            static inline SetDeleteFn original = nullptr;

            static void SetDelete(RE::TESObjectREFR* self, bool set)
            {
                if (set && self) {
                    Remember(self);
                }
                original(self, set);
            }
        };

        template <std::size_t Slot>
        void Write(REL::VariantID vtable)
        {
            REL::Relocation<std::uintptr_t> table{ vtable };
            Hook<Slot>::original = reinterpret_cast<SetDeleteFn>(table.write_vfunc(0x23, &Hook<Slot>::SetDelete));
        }
    }

    void Install()
    {
        Write<0>(RE::VTABLE_TESObjectREFR[0]);
        Write<1>(RE::VTABLE_Actor[0]);
        Write<2>(RE::VTABLE_Character[0]);
        installed = true;
        spdlog::info("tracking who deletes references");
    }

    std::string Describe(std::uintptr_t reference)
    {
        if (!installed) {
            return {};
        }
        std::optional<Record> found;
        deletes.Scan([&](const Record& record) {
            if (record.address == reference) {
                found = record;
                return false;
            }
            return true;
        });
        if (!found) {
            return "no deletion recorded this session (deleted before this game was loaded, by a path that "
                   "bypasses SetDelete, or too long ago)";
        }
        return std::format("deleted {} ms ago on {} (formID {:08X}, base {:08X}, flags {:08X}, cell {:08X}); stack "
                           "then:{}",
            GetTickCount64() - found->time, Report::ThreadName(found->thread), found->formID, found->baseFormID,
            found->flags, found->cellFormID, Report::StackTrace(found->stack.View()));
    }
}
