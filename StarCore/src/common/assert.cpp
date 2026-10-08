#include <cstdlib>

#include "starcore/common/assert.hpp"
#include "starcore/common/log.hpp"

#if defined(_MSC_VER)
    #include <intrin.h>
    #define SC_DEBUG_BREAK() __debugbreak()
#elif defined(__GNUC__) || defined(__clang__)
    #define SC_DEBUG_BREAK() __builtin_trap()
#else
    #define SC_DEBUG_BREAK() std::abort()
#endif

namespace starcore::detail
{

void assertFailed(std::string_view expression, std::string_view message,
                  std::source_location location)
{
    SC_CORE_CRITICAL("Assertion `{}` failed at {}:{} ({}){}{}", expression,
                     location.file_name(), location.line(),
                     location.function_name(), message.empty() ? "" : ": ",
                     message);
    log::core().flush();
    SC_DEBUG_BREAK();
    std::abort();
}

}  // namespace starcore::detail
