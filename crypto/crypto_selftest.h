/*
 * SecOS Kernel - Crypto known-answer self-tests
 * Copyright (c) 2025 iDev srl
 * SPDX-License-Identifier: MIT
 */
#ifndef SECOS_CRYPTO_SELFTEST_H
#define SECOS_CRYPTO_SELFTEST_H

/* Runs known-answer tests for the crypto primitives, logging [CRYPTO] lines to
 * debugcon. Returns the number of FAILED tests (0 = all pass). */
int crypto_selftest(void);

#endif
