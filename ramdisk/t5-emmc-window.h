/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T5_EMMC_WINDOW_H
#define T5_EMMC_WINDOW_H
#define T5_EMMC_ARM_ROOT 0xd545U
#define T5_EMMC_QUERY_ROOT 0xd546U
struct t5_root_window_info {
 unsigned long long first, blocks;
 unsigned int armed, partno;
};
static inline int t5_root_geometry(unsigned long long first,
 unsigned long long blocks, unsigned long long capacity)
{
 return first >= 34 && blocks >= 4096 && first < capacity &&
  blocks <= capacity-first && first+blocks <= 0x100000000ULL;
}
static inline int t5_root_overlap(unsigned long long a,unsigned long long an,
 unsigned long long b,unsigned long long bn)
{
 if (!an || !bn) return 1;
 return a<=b ? b-a<an : a-b<bn;
}
static inline int t5_root_write_allowed(unsigned long long first,
 unsigned long long blocks, int armed, unsigned int access,
 unsigned int op, unsigned int lba, unsigned int blksz,
 unsigned int count, unsigned long long bytes)
{
 if (!armed || access || (op!=24 && op!=25) || blksz!=512 || !count ||
     (op==24 && count!=1) || !first || !blocks || count>blocks ||
     lba<first || (unsigned long long)lba-first>blocks-count) return 0;
 return bytes==(unsigned long long)count*512;
}
#endif
