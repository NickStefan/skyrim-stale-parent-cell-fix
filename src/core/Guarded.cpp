#include "core/Guarded.h"

#include <cstring>
#include <string_view>
#include <vector>

#include <excpt.h>

namespace StaleParentCellFix::Core
{
    namespace
    {
        template <class T>
        bool Read(std::uintptr_t address, T& out) noexcept
        {
            return GuardedCopy(reinterpret_cast<const void*>(address), &out, sizeof(T));
        }

        // ".?AVTESObjectREFR@@" -> "TESObjectREFR"; ".?AVVirtualMachine@Internal@BSScript@@" ->
        // "BSScript::Internal::VirtualMachine". Templates and other manglings are left as they are.
        std::string Undecorate(std::string_view text)
        {
            if (!(text.starts_with(".?AV") || text.starts_with(".?AU")) || !text.ends_with("@@")) {
                return std::string{ text };
            }
            text.remove_prefix(4);
            text.remove_suffix(2);
            if (text.find_first_of("?$") != std::string_view::npos) {
                return std::string{ text };
            }
            std::vector<std::string_view> parts;
            for (std::size_t at; (at = text.find('@')) != std::string_view::npos; text.remove_prefix(at + 1)) {
                parts.push_back(text.substr(0, at));
            }
            parts.push_back(text);
            std::string out;
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                out += (out.empty() ? "" : "::") + std::string{ *it };
            }
            return out;
        }
    }

    bool GuardedCopy(const void* source, void* out, std::size_t size) noexcept
    {
        __try {
            std::memcpy(out, source, size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::string RttiClassName(std::uintptr_t vtable)
    {
        struct CompleteObjectLocator
        {
            std::uint32_t signature, offset, cdOffset, typeDescriptor, classDescriptor, self;
        };
        std::uintptr_t locator = 0;
        CompleteObjectLocator col{};
        if (!Read(vtable - sizeof(std::uintptr_t), locator) || !Read(locator, col) || col.signature != 1) {
            return {};
        }
        char name[160]{};
        if (!GuardedCopy(reinterpret_cast<const void*>(locator - col.self + col.typeDescriptor + 0x10), name,
                sizeof(name) - 1)) {
            return {};
        }
        return Undecorate(name);
    }
}
