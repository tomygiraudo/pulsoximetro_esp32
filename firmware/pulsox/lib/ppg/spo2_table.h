// SpO2 lookup table, indexed by round(R * 100), R being the "ratio of ratios"
// (AC_red / DC_red) / (AC_ir / DC_ir).
//
// Taken from algorithm.h of the Maxim Integrated MAXREFDES117# reference design.
// It is exactly the quadratic -45.060 R^2 + 30.354 R + 94.845 rounded to integers
// (tools/validate_ppg.py checks it), tabulated so the device does no floating-point
// math for this step. Maxim declares uch_spo2_table[184] but lists 183 values (the
// last element is the implicit 0); only the 183 listed ones are kept here.
//
// UNCALIBRATED for this sensor and housing: the values are indicative only.
//
// The table is not monotonic: it is 100 % for R between 0.24 and 0.43, falls from
// there as R grows, and also falls (towards 95 %) as R drops below 0.24. Only the
// part above R = 0.43 is physiological.
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
#define SPO2_TABLE_IDX_MIN 3  // Maxim only trusts the table for 2 < index < 184

static const uint8_t SPO2_TABLE[SPO2_TABLE_SIZE] = {
    95,  95,  95,  96,  96,  96,  97,  97,  97,  97,  97,  98,  98,  98,  98,  98,  99,  99,  99,  99,
    99,  99,  99,  99,  100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100,
    100, 100, 100, 100, 99,  99,  99,  99,  99,  99,  99,  99,  98,  98,  98,  98,  98,  98,  97,  97,
    97,  97,  96,  96,  96,  96,  95,  95,  95,  94,  94,  94,  93,  93,  93,  92,  92,  92,  91,  91,
    90,  90,  89,  89,  89,  88,  88,  87,  87,  86,  86,  85,  85,  84,  84,  83,  82,  82,  81,  81,
    80,  80,  79,  78,  78,  77,  76,  76,  75,  74,  74,  73,  72,  72,  71,  70,  69,  69,  68,  67,
    66,  66,  65,  64,  63,  62,  62,  61,  60,  59,  58,  57,  56,  56,  55,  54,  53,  52,  51,  50,
    49,  48,  47,  46,  45,  44,  43,  42,  41,  40,  39,  38,  37,  36,  35,  34,  33,  31,  30,  29,
    28,  27,  26,  25,  23,  22,  21,  20,  19,  17,  16,  15,  14,  12,  11,  10,  9,   7,   6,   5,
    3,   2,   1};
