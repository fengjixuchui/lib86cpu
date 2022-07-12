/*
 * ergo720                Copyright (c) 2022
 */

#pragma once

#include "helpers.h"


template<bool is_iret> uint8_t lret_pe_helper(cpu_ctx_t *cpu_ctx, uint8_t size_mode, uint32_t eip);
void iret_real_helper(cpu_ctx_t *cpu_ctx, uint8_t size_mode, uint32_t eip);
