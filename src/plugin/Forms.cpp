#include "plugin/Forms.h"

#include "plugin/Frees.h"
#include "plugin/Memory.h"

namespace StaleParentCellFix::Forms
{
    namespace
    {
        std::uintptr_t cellVtable = 0;
        std::uintptr_t worldSpaceVtable = 0;

        bool IsReferenceType(std::uint8_t type) noexcept
        {
            return type >= std::to_underlying(RE::FormType::Reference) &&
                   type <= std::to_underlying(RE::FormType::PlacedHazard);
        }

        std::string TypeName(std::uint8_t type)
        {
            if (type < std::to_underlying(RE::FormType::Max)) {
                return std::string{ RE::FormTypeToString(static_cast<RE::FormType>(type)) };
            }
            return std::format("0x{:02X}", type);
        }

        // Name or editor ID, only for forms the form map still resolves to this exact address.
        std::string Identity(std::uintptr_t address, const Header& header)
        {
            if (header.formID == 0) {
                return {};
            }
            auto* form = RE::TESForm::LookupByID(header.formID);
            if (reinterpret_cast<std::uintptr_t>(form) != address) {
                return form ? " [form map has a different object for this ID]" : " [not in form map]";
            }
            const char* editorID = form->GetFormEditorID();
            const char* name = form->GetName();
            return std::format(" \"{}\" ({})", name ? name : "", editorID ? editorID : "");
        }

        std::string HeaderLine(std::uintptr_t address, const Header& header)
        {
            return std::format("vtable {} [{}] formID {:08X} type {} flags {:08X} inGameFlags {:04X}{}",
                Memory::Symbol(header.vtable), Memory::ClassName(header.vtable), header.formID,
                TypeName(header.formType), header.flags, header.inGameFlags, Identity(address, header));
        }

        std::string Brief(std::uintptr_t address)
        {
            if (address == 0) {
                return "null";
            }
            const auto header = ReadHeader(address);
            if (!header) {
                return std::format("0x{:X} (unreadable)", address);
            }
            return std::format("0x{:X} {}", address, HeaderLine(address, *header));
        }
    }

    void Init()
    {
        cellVtable = RE::VTABLE_TESObjectCELL[0].address();
        worldSpaceVtable = RE::VTABLE_TESWorldSpace[0].address();
    }

    std::optional<Header> ReadHeader(std::uintptr_t address) noexcept
    {
        return Memory::Read<Header>(address);
    }

    bool IsLiveCell(std::uintptr_t address) noexcept
    {
        const auto header = ReadHeader(address);
        return header && header->vtable == cellVtable && header->formType == std::to_underlying(RE::FormType::Cell);
    }

    bool IsLiveWorldSpace(std::uintptr_t address) noexcept
    {
        const auto header = ReadHeader(address);
        return header && header->vtable == worldSpaceVtable &&
               header->formType == std::to_underlying(RE::FormType::WorldSpace);
    }

    std::string WhyDead(std::uintptr_t reference)
    {
        const auto header = ReadHeader(reference);
        if (!header) {
            return "reference unreadable";
        }
        if (!Memory::InGameImage(header->vtable)) {
            return std::format("reference vtable 0x{:X} is outside SkyrimSE.exe", header->vtable);
        }
        if (!IsReferenceType(header->formType)) {
            return std::format("reference has form type {}", TypeName(header->formType));
        }
        const auto cell = Memory::Read<std::uintptr_t>(reference + Offset::RefParentCell);
        if (!cell) {
            return "reference parentCell unreadable";
        }
        if (*cell == 0) {
            return {};
        }
        if (!IsLiveCell(*cell)) {
            return std::format("parentCell 0x{:X} is not a live TESObjectCELL", *cell);
        }
        const auto flags = Memory::Read<std::uint16_t>(*cell + Offset::CellFlags);
        if (flags && (*flags & 1) == 0) {
            const auto world = Memory::Read<std::uintptr_t>(*cell + Offset::CellWorldSpace);
            if (!world || (*world != 0 && !IsLiveWorldSpace(*world))) {
                return std::format("parentCell 0x{:X} worldSpace 0x{:X} is not a live TESWorldSpace", *cell,
                    world.value_or(0));
            }
        }
        return {};
    }

    std::string Describe(std::string_view label, std::uintptr_t address, Kind kind)
    {
        return Describe(label, address, kind, 4);
    }

    std::string Describe(std::string_view label, std::uintptr_t address, Kind kind, std::size_t indent)
    {
        const std::string pad = "\n" + std::string(indent, ' ');
        const std::string inner = pad + "  ";
        std::string out = pad + std::format("{}: {}", label, Brief(address));
        if (address == 0) {
            return out;
        }
        if (const auto freed = Frees::Describe(address); !freed.empty()) {
            out += inner + freed;
        }
        std::size_t dump = 0x40;
        switch (kind) {
        case Kind::Reference:
            {
                dump = Offset::RefSize;
                const auto base = Memory::Read<std::uintptr_t>(address + Offset::RefBaseObject);
                const auto position = Memory::Read<std::array<float, 3>>(address + Offset::RefPosition);
                const auto cell = Memory::Read<std::uintptr_t>(address + Offset::RefParentCell);
                out += inner + "baseObject: " + (base ? Brief(*base) : "unreadable"s);
                if (position) {
                    out += inner + std::format("position: ({:.1f}, {:.1f}, {:.1f})", (*position)[0], (*position)[1],
                        (*position)[2]);
                }
                if (cell) {
                    out += Describe("parentCell", *cell, Kind::Cell, indent + 2);
                }
                break;
            }
        case Kind::Cell:
            {
                dump = Offset::CellSize;
                const auto flags = Memory::Read<std::uint16_t>(address + Offset::CellFlags);
                const auto state = Memory::Read<std::uint8_t>(address + Offset::CellState);
                const auto world = Memory::Read<std::uintptr_t>(address + Offset::CellWorldSpace);
                out += inner + std::format("cellFlags {:04X} cellState {}", flags.value_or(0xFFFF), state.value_or(0xFF));
                if (flags && (*flags & 1) == 0) {
                    if (const auto data = Memory::Read<std::uintptr_t>(address + Offset::CellData)) {
                        if (const auto grid = Memory::Read<std::array<std::int32_t, 2>>(*data)) {
                            out += std::format(" grid ({}, {})", (*grid)[0], (*grid)[1]);
                        }
                    }
                    out += inner + "worldSpace: " + (world ? Brief(*world) : "unreadable"s);
                }
                break;
            }
        default:
            break;
        }
        out += inner + std::format("raw 0x{:X} bytes:", dump) + Memory::HexDump(address, dump, indent + 4);
        return out;
    }
}
