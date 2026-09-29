#pragma once

#if defined(__GNUC__) || defined(__clang__)
#define WORLDANALYSIS_API __attribute__((visibility("default")))
#else
#define WORLDANALYSIS_API
#endif
