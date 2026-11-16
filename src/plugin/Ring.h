#pragma once

namespace StaleParentCellFix
{
    // Fixed-size history written from any thread without locks. A slot can be overwritten while
    // a reader copies it; records are diagnostics, so a torn read is acceptable.
    template <class T, std::size_t N>
    class Ring
    {
    public:
        T& Next() { return records[next.fetch_add(1, std::memory_order_relaxed) % N]; }

        // Newest first; stops when `visit` returns false.
        template <class F>
        void Scan(F visit) const
        {
            const auto newest = next.load(std::memory_order_relaxed);
            const auto count = std::min<std::uint64_t>(newest, N);
            for (std::uint64_t i = 1; i <= count; ++i) {
                if (!visit(records[(newest - i) % N])) {
                    return;
                }
            }
        }

    private:
        std::array<T, N> records{};
        std::atomic<std::uint64_t> next{ 0 };
    };

    constexpr std::size_t StackDepth = 32;

    struct Stack
    {
        std::array<void*, StackDepth> frames;
        std::uint16_t depth;

        // Skips this function and its caller (the hook).
        __declspec(noinline) void Capture() { depth = RtlCaptureStackBackTrace(2, StackDepth, frames.data(), nullptr); }
        std::span<void* const> View() const { return { frames.data(), depth }; }
    };
}
