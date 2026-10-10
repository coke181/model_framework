#pragma once

#if MF_ENABLE_TRACY
#include <tracy/Tracy.hpp>
#define MF_PROFILE_SCOPE(name) ZoneScopedN(name)
#else
#define MF_PROFILE_SCOPE(name) ((void)0)
#endif
