/*
 * SecOS Kernel - Block Device Registry
 * Lightweight registry for fixed small set of block devices.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "block.h"

#define BLOCK_MAX_DEVS 4
static block_dev_t* g_devs[BLOCK_MAX_DEVS];
static int str_eq(const char* a,const char* b){ while(*a && *b){ if(*a!=*b) return 0; a++; b++; } return *a==0 && *b==0; }

int block_register(block_dev_t* dev){ if(!dev||!dev->name||!dev->read||dev->sector_size==0) return -1; for(int i=0;i<BLOCK_MAX_DEVS;i++){ if(g_devs[i]==dev) return 0; if(!g_devs[i]){ g_devs[i]=dev; return 0; } } return -1; }
block_dev_t* block_find(const char* name){ if(!name) return NULL; for(int i=0;i<BLOCK_MAX_DEVS;i++){ if(g_devs[i] && str_eq(g_devs[i]->name,name)) return g_devs[i]; } return NULL; }
