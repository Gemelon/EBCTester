/* SPDX-FileCopyrightText: 2026 Thomas Stoll
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include <ctime>
struct timeval { long long tv_sec; long tv_usec; };
inline bool host_clock_synchronized = true;
inline int gettimeofday(timeval *value, void *)
{
    value->tv_sec = host_clock_synchronized ? static_cast<long long>(std::time(nullptr)) : 0;
    value->tv_usec = 123000;
    return 0;
}
