#pragma once

namespace aadp::smoke {

// Name of the Highway target this build compiled for.
const char* highway_target_name();

// Sum of the lanes of a vector of ones, which is the lane count.
double sum_of_ones();

}  // namespace aadp::smoke
