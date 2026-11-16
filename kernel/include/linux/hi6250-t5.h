/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_HI6250_T5_H
#define _LINUX_HI6250_T5_H
#include <linux/types.h>
struct hi6250_t5_gpu_policy { bool hot; };
int hi6250_t5_read_max_temp(int *temp);
#endif
