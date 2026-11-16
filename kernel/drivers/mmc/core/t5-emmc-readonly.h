/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T5_EMMC_READONLY_H
#define T5_EMMC_READONLY_H
/* Pure transport policy. Initialization may change only volatile bus mode and
 * the partition-access bits, preserving every boot configuration bit. */
static inline int t5_emmc_command_allowed(unsigned int op, unsigned int arg,
 int data_out, int boot_valid, unsigned int boot_bits)
{
 unsigned int index, value;
 if (data_out) return 0;
 switch (op) {
 case 0: case 2: case 12: return arg == 0;
 case 1: return 1; /* Read operating conditions; volatile addressing mode. */
 case 3: case 9: case 10: case 13:
  return !(arg & 0xffffU) && (arg >> 16);
 case 5: return !(arg & 0x7fffU) && (arg >> 16);
 case 7: return !(arg & 0xffffU);
 case 8: return arg == 0; /* SEND_EXT_CSD, never SD CMD8 on this host. */
 case 14: case 21: return arg == 0; /* Read-only bus/tuning patterns. */
 case 16: return arg == 512;
 case 17: case 18: return 1;
 case 23: return arg && !(arg & 0xffff0000U);
 case 6:
  if ((arg >> 24) != 3 || (arg & 0xffU) != 1U) return 0;
  index = (arg >> 16) & 255; value = (arg >> 8) & 255;
  if (index == 183) return value <= 2 || value == 5 || value == 6; /* Volatile SDR/DDR bus width; no enhanced strobe. */
  if (index == 185) return value <= 3 || (value >= 0x40 && value <= 0x43); /* Volatile timing + factory driver type 4 only. */
  if (index == 179)
   return boot_valid && !(boot_bits & 7U) &&
    (value & ~7U) == boot_bits && (value & 7U) <= 2;
  return 0;
 default: return 0;
 }
}
#endif
