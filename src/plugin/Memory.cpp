#include "plugin/Memory.h"

#include "core/Guarded.h"

#include <REL/Offset2ID.h>

namespace StaleParentCellFix::Memory
{
    namespace
    {
        thread_local bool probing = false;

        struct Image
        {
            std::uintptr_t base;
            std::uintptr_t end;
        };

        const Image& GameImage()
        {
            static const Image image = [] {
                const auto base = REL::Module::get().base();
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
                return Image{ base, base + nt->OptionalHeader.SizeOfImage };
            }();
            return image;
        }

        std::optional<std::pair<std::uint64_t, std::uint64_t>> AddressLibraryID(std::uintptr_t offset)
        {
            static const REL::Offset2ID table;
            const auto next = std::upper_bound(table.begin(), table.end(), offset,
                [](std::uintptr_t value, const auto& entry) { return value < entry.offset; });
            if (next == table.begin()) {
                return std::nullopt;
            }
            const auto& entry = *std::prev(next);
            return std::pair{ entry.id, offset - entry.offset };
        }
    }

    bool Plausible(std::uintptr_t address) noexcept
    {
        return address >= 0x10000 && address < 0x7FFF'FFFF'0000;
    }

    bool Read(std::uintptr_t address, void* out, std::size_t size) noexcept
    {
        if (!Plausible(address) || !Plausible(address + size)) {
            return false;
        }
        probing = true;
        const bool ok = Core::GuardedCopy(reinterpret_cast<const void*>(address), out, size);
        probing = false;
        return ok;
    }

    bool Probing() noexcept
    {
        return probing;
    }

    bool InGameImage(std::uintptr_t address) noexcept
    {
        const auto& image = GameImage();
        return address >= image.base && address < image.end;
    }

    std::string ClassName(std::uintptr_t vtable)
    {
        if (!Plausible(vtable)) {
            return {};
        }
        probing = true;
        auto name = Core::RttiClassName(vtable);
        probing = false;
        return name;
    }

    std::string Symbol(std::uintptr_t address)
    {
        if (InGameImage(address)) {
            const auto offset = address - GameImage().base;
            if (const auto id = AddressLibraryID(offset)) {
                return std::format("SkyrimSE.exe+{:07X} (ID {}+0x{:X})", offset, id->first, id->second);
            }
            return std::format("SkyrimSE.exe+{:07X}", offset);
        }
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address), &module)) {
            return std::format("0x{:X}", address);
        }
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        const std::filesystem::path file{ path };
        return std::format("{}+{:X}", file.filename().string(), address - reinterpret_cast<std::uintptr_t>(module));
    }

    std::string HexDump(std::uintptr_t address, std::size_t size, std::size_t indent)
    {
        std::string out;
        for (std::size_t row = 0; row < size; row += 16) {
            out += std::format("\n{}+{:03X}:", std::string(indent, ' '), row);
            for (std::size_t i = 0; i < 16; i += 8) {
                if (const auto value = Read<std::uint64_t>(address + row + i)) {
                    out += std::format(" {:016X}", *value);
                } else {
                    out += " ????????????????";
                }
            }
        }
        return out;
    }
}
