#include <linux/sched/clock.h>
#include <linux/panic_notifier.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/time64.h>
#include <linux/string.h>
#include <linux/time.h>
static inline void hi1102_real_timeval(struct __kernel_old_timeval *tv)
{
 struct timespec64 now;
 ktime_get_real_ts64(&now);
 tv->tv_sec=now.tv_sec;
 tv->tv_usec=now.tv_nsec/1000;
}

#include <linux/fs.h>
static inline ssize_t hi1102_read_at(struct file *f, loff_t pos, void *buf, size_t size)
{
 return kernel_read(f, buf, size, &pos);
}

static inline char *hi1102_copy_padded(char *dst,const char *src,size_t n)
{
 size_t used=strnlen(src,n);
 memcpy(dst,src,used);
 memset(dst+used,0,n-used);
 return dst;
}
