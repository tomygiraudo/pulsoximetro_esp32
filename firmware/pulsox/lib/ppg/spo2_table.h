// SpO2 lookup table, indexed by round(R * 100), R being the "ratio of ratios"
// (AC_red / DC_red) / (AC_ir / DC_ir).
//
// Derived from the table in algorithm.h of the Maxim Integrated MAXREFDES117# reference
// design, which is the quadratic
//     SpO2(R) = -45.060 R^2 + 30.354 R + 94.845
// rounded to integers (half up) and tabulated so the device does no floating-point math
// for this step. Maxim declares uch_spo2_table[184] but lists 183 values (the last element
// is the implicit 0); only the 183 listed ones are kept here (R up to 1.82, where the
// quadratic reaches 0). This copy differs from Maxim's in the two ways described below.
//
// UNCALIBRATED for this sensor and housing: the values are indicative only.
//
// Rule used to generate the table (r = index / 100, v = the quadratic at r):
//     index < 34   ->  0                      (invalid, see 1)
//     v >= 99.85   ->  100                    (see 2)
//     otherwise    ->  min(99, floor(v + 0.5))
// test/host/validate_ppg.py recomputes the table from this rule and fails if it differs.
//
// 1. R below the maximum of the curve is invalid.
//    The parabola peaks at R = 0.3368 (99.957 %) and falls on both sides, but the physical
//    relation is monotonic: SpO2 does not drop as R drops. Maxim's table keeps that left
//    branch (it falls from 100 % at R = 0.24 to 95 % at R = 0), which is only an artefact of
//    fitting a parabola. Here the table starts at its own maximum, index 34 (R = 0.34,
//    99.956 %): indices 0-33 hold 0 and SPO2_TABLE_IDX_MIN keeps them from being read, so
//    R < 0.335 gives no SpO2. (Anything below 80 % is also rejected, by SPO2_MIN_VALID in
//    config.h.)
//
// 2. 100 % only where the quadratic is within 0.15 of it (v >= 99.85).
//    The curve never reaches 100 (its maximum is 99.957): Maxim's 100s are just the rounding
//    of every v >= 99.5, indices 24-43. Here 100 is kept for indices 34-38 (R 0.34-0.38,
//    v from 99.96 down to 99.87) and indices 39-43 (v 99.83 down to 99.57) become 99.
//    Every other value is rounded to the nearest integer, as Maxim does, so from index 52
//    on the table is Maxim's. Side effect: 99 covers v in [98.5, 99.85), indices 39-51,
//    which is wider than the one-point range of the other values.
//
// Original notice:
//
// Copyright (C) 2015 Maxim Integrated Products, Inc., All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL MAXIM INTEGRATED BE LIABLE FOR ANY CLAIM, DAMAGES
// OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//
// Except as contained in this notice, the name of Maxim Integrated
// Products, Inc. shall not be used except as stated in the Maxim Integrated
// Products, Inc. Branding Policy.
//
// The mere transfer of this software does not imply any licenses
// of trade secrets, proprietary technology, copyrights, patents,
// trademarks, maskwork rights, or any other form of intellectual
// property whatsoever. Maxim Integrated Products, Inc. retains all
// ownership rights.
#pragma once
#include <stdint.h>

#define SPO2_TABLE_SIZE 183
#define SPO2_TABLE_IDX_MIN 34  // first valid index: the maximum of the curve (R = 0.34)

static const uint8_t SPO2_TABLE[SPO2_TABLE_SIZE] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   100, 100, 100, 100, 100, 99,
    99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  98,  98,  98,  98,  98,  98,  97,  97,
    97,  97,  96,  96,  96,  96,  95,  95,  95,  94,  94,  94,  93,  93,  93,  92,  92,  92,  91,  91,
    90,  90,  89,  89,  89,  88,  88,  87,  87,  86,  86,  85,  85,  84,  84,  83,  82,  82,  81,  81,
    80,  80,  79,  78,  78,  77,  76,  76,  75,  74,  74,  73,  72,  72,  71,  70,  69,  69,  68,  67,
    66,  66,  65,  64,  63,  62,  62,  61,  60,  59,  58,  57,  56,  56,  55,  54,  53,  52,  51,  50,
    49,  48,  47,  46,  45,  44,  43,  42,  41,  40,  39,  38,  37,  36,  35,  34,  33,  31,  30,  29,
    28,  27,  26,  25,  23,  22,  21,  20,  19,  17,  16,  15,  14,  12,  11,  10,  9,   7,   6,   5,
    3,   2,   1};
