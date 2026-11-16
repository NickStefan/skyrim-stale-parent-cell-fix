#pragma once

// Liveness checks for the forms these crashes trip over, read through Memory so a dangling
// pointer is reported instead of followed. Offsets are Skyrim 1.6.629+ (AE) layouts.
namespace StaleParentCellFix::Forms
{
    namespace Offset
    {
        inline constexpr std::uintptr_t RefBaseObject = 0x40;
        inline constexpr std::uintptr_t RefPosition = 0x54;
        inline constexpr std::uintptr_t RefParentCell = 0x60;
        inline constexpr std::uintptr_t CellFlags = 0x40;
        inline constexpr std::uintptr_t CellState = 0x44;
        inline constexpr std::uintptr_t CellData = 0x68;
        inline constexpr std::uintptr_t CellWorldSpace = 0x128;
        inline constexpr std::uintptr_t CellLoadedData = 0x130;
        inline constexpr std::size_t RefSize = 0x98;
        inline constexpr std::size_t CellSize = 0x148;
    }

    struct Header
    {
        std::uintptr_t vtable;
        std::uintptr_t sourceFiles;
        std::uint32_t flags;
        std::uint32_t formID;
        std::uint16_t inGameFlags;
        std::uint8_t formType;
        std::uint8_t pad1B;
        std::uint32_t pad1C;
    };
    static_assert(sizeof(Header) == 0x20);

    void Init();

    std::optional<Header> ReadHeader(std::uintptr_t address) noexcept;
    bool IsLiveCell(std::uintptr_t address) noexcept;
    bool IsLiveWorldSpace(std::uintptr_t address) noexcept;

    // Empty when the reference, its parent cell and that cell's worldspace all look alive;
    // otherwise what is wrong with them.
    std::string WhyDead(std::uintptr_t reference);

    enum class Kind
    {
        Reference,
        Cell,
        WorldSpace,
        Location,
        Form
    };
    // Multi-line description for the log: header fields, RTTI class, free history, raw bytes.
    std::string Describe(std::string_view label, std::uintptr_t address, Kind kind);
    std::string Describe(std::string_view label, std::uintptr_t address, Kind kind, std::size_t indent);
}
