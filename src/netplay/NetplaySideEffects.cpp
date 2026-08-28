#include "NetplaySideEffects.hpp"

namespace Netplay::SideEffects
{
namespace
{
bool g_Speculative = false;
}

void SetSpeculative(bool speculative)
{
    g_Speculative = speculative;
}

bool IsSpeculative()
{
    return g_Speculative;
}
} // namespace Netplay::SideEffects
