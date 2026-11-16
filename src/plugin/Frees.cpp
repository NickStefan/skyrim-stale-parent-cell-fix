#include "plugin/Frees.h"

#include "plugin/Forms.h"
#include "plugin/Memory.h"
#include "plugin/Report.h"
#include "plugin/Ring.h"

namespace StaleParentCellFix::Frees
{
    namespace
    {
        struct Record
        {
            std::uintptr_t address;
            std::uintptr_t link;  // a reference's parentCell, a cell's worldSpace
            std::uint64_t time;
            std::uint32_t formID;
            std::uint32_t thread;
            std::uint32_t flags;
        };

        struct Traced
        {
            Record record;
            Stack stack;
        };

        Ring<Traced, 4096> cells;
        Ring<Record, 65536> references;
        bool installed = false;

        Record Capture(std::uintptr_t address, std::uintptr_t linkOffset)
        {
            Record record{};
            record.address = address;
            const auto header = Forms::ReadHeader(address);
            record.formID = header ? header->formID : 0;
            record.link = Memory::Read<std::uintptr_t>(address + linkOffset).value_or(0);
            record.time = GetTickCount64();
            record.thread = GetCurrentThreadId();
            return record;
        }

        using Deleter = void* (*)(void*, std::uint32_t);
        Deleter cellDelete = nullptr;
        Deleter referenceDelete = nullptr;

        void* CellDelete(void* self, std::uint32_t flags)
        {
            Traced traced{ Capture(reinterpret_cast<std::uintptr_t>(self), Forms::Offset::CellWorldSpace) };
            traced.record.flags = flags;
            traced.stack.Capture();
            cells.Next() = traced;
            return cellDelete(self, flags);
        }

        void* ReferenceDelete(void* self, std::uint32_t flags)
        {
            auto record = Capture(reinterpret_cast<std::uintptr_t>(self), Forms::Offset::RefParentCell);
            record.flags = flags;
            references.Next() = record;
            return referenceDelete(self, flags);
        }

        struct Match
        {
            const char* what;
            Record record;
            Stack stack;
        };

        std::vector<Match> Find(std::uintptr_t address, std::size_t limit)
        {
            std::vector<Match> found;
            if (!installed || address == 0) {
                return found;
            }
            const auto contains = [&](const Record& record, std::size_t size) {
                return address >= record.address && address < record.address + size;
            };
            cells.Scan([&](const Traced& traced) {
                if (contains(traced.record, Forms::Offset::CellSize)) {
                    found.push_back({ "FREED cell", traced.record, traced.stack });
                }
                return found.size() < limit;
            });
            references.Scan([&](const Record& record) {
                if (contains(record, Forms::Offset::RefSize)) {
                    found.push_back({ "FREED reference", record, {} });
                }
                return found.size() < limit;
            });
            return found;
        }

        std::string Line(std::uintptr_t address, const Match& match, std::uint64_t now)
        {
            const auto& r = match.record;
            const auto age = now - r.time;
            const auto thread = Report::ThreadName(r.thread);
            const bool cell = std::string_view{ match.what } == "FREED cell";
            return std::format("FREED: +0x{:X} into {} 0x{:X} (formID {:08X}, {} 0x{:X}) destroyed {} ms ago on {}{}",
                address - r.address, cell ? "TESObjectCELL" : "TESObjectREFR", r.address, r.formID,
                cell ? "worldSpace" : "parentCell", r.link, age, thread, (r.flags & 1) ? "" : " (not freed)");
        }
    }

    void Install()
    {
        REL::Relocation<std::uintptr_t> cell{ RE::VTABLE_TESObjectCELL[0] };
        REL::Relocation<std::uintptr_t> reference{ RE::VTABLE_TESObjectREFR[0] };
        cellDelete = reinterpret_cast<Deleter>(cell.write_vfunc(0, &CellDelete));
        referenceDelete = reinterpret_cast<Deleter>(reference.write_vfunc(0, &ReferenceDelete));
        installed = true;
        spdlog::info("tracking destroyed cells and references");
    }

    std::string Describe(std::uintptr_t address)
    {
        const auto now = GetTickCount64();
        std::string out;
        for (const auto& match : Find(address, 3)) {
            out += (out.empty() ? "" : "\n      ") + Line(address, match, now);
            if (match.stack.depth) {
                out += "; stack then:" + Report::StackTrace(match.stack.View());
            }
        }
        return out;
    }

    std::string Brief(std::uintptr_t address)
    {
        const auto matches = Find(address, 1);
        return matches.empty() ? std::string{} : Line(address, matches.front(), GetTickCount64());
    }
}
