//
// XSL: time for Linux programs: clock_gettime, nanosleep and
// clock_nanosleep.
//
// there are two clocks:
//   - the cpu's time counter, r_time(), counting at TIMER_HZ (10 MHz)
//     since boot. used for CLOCK_MONOTONIC, cpu time and sleeping.
//   - QEMU's Goldfish real time clock device at RTC0, which knows the
//     real date and time. used for CLOCK_REALTIME (date, os.time()).
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "linux.h"

#define NSEC_PER_SEC   1000000000L
#define NSEC_PER_COUNT (NSEC_PER_SEC / TIMER_HZ) // 100 ns

// read the real time clock: nanoseconds since 1 January 1970.
// reading the low 32 bits makes the device latch the high 32 bits,
// so the low register has to be read first.
uint64
rtc_now(void)
{
  uint64 lo, hi;

  lo = *(volatile uint32 *)(RTC0 + 0); // TIME_LOW
  hi = *(volatile uint32 *)(RTC0 + 4); // TIME_HIGH
  return (hi << 32) | lo;
}

// clock_gettime(clock, ts)
uint64
linux_clock_gettime(void)
{
  int clk;
  uint64 addr, ns;
  struct linux_timespec ts;
  struct proc *p = myproc();

  argint(0, &clk);
  argaddr(1, &addr);

  switch (clk) {
  case CLOCK_REALTIME:
  case CLOCK_REALTIME_COARSE:
    ns = rtc_now();
    break;
  case CLOCK_MONOTONIC:
  case CLOCK_MONOTONIC_RAW:
  case CLOCK_MONOTONIC_COARSE:
  case CLOCK_BOOTTIME:
    ns = r_time() * NSEC_PER_COUNT;
    break;
  case CLOCK_PROCESS_CPUTIME_ID:
  case CLOCK_THREAD_CPUTIME_ID:
    // cpu time of the earlier time slices (counted by the scheduler)
    // plus the time slice we are in now. Lua's os.clock() uses this.
    ns = (p->cputime + (r_time() - p->runstart)) * NSEC_PER_COUNT;
    break;
  default:
    return -EINVAL;
  }

  ts.tv_sec = ns / NSEC_PER_SEC;
  ts.tv_nsec = ns % NSEC_PER_SEC;
  if (copyout(p->pagetable, p->sz, addr, (char *)&ts, sizeof(ts)) < 0)
    return -EFAULT;
  return 0;
}

// sleep until r_time() reaches deadline.
// xv6's timer interrupt comes only every 0.1 s (TICK_CYCLES), too
// coarse for e.g. a 10 ms sleep. so while more than a tick is left we
// sleep on the tick counter, like xv6's pause(); for the last part we
// keep giving the cpu away with yield() until the deadline has passed.
static int
sleepuntil(uint64 deadline)
{
  struct proc *p = myproc();

  while (r_time() < deadline) {
    if (killed(p))
      return -EINTR;
    if (deadline - r_time() > TICK_CYCLES) {
      acquire(&tickslock);
      sleep_prepare(&ticks);
      release(&tickslock);
      sleep(); // woken by the next timer tick
    } else {
      yield();
    }
  }
  return 0;
}

// read a struct timespec from user memory as nanoseconds
static int
gettimespec(uint64 addr, uint64 *ns)
{
  struct linux_timespec ts;
  struct proc *p = myproc();

  if (copyin(p->pagetable, p->sz, (char *)&ts, addr, sizeof(ts)) < 0)
    return -EFAULT;
  if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= NSEC_PER_SEC)
    return -EINVAL;
  *ns = ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
  return 0;
}

// nanosleep(req, rem): sleep for req.
// (rem is only filled in by Linux when the sleep is interrupted.)
uint64
linux_nanosleep(void)
{
  uint64 req, ns;
  int r;

  argaddr(0, &req);
  if ((r = gettimespec(req, &ns)) < 0)
    return r;
  return sleepuntil(r_time() + ns / NSEC_PER_COUNT);
}

// clock_nanosleep(clock, flags, req, rem). glibc's sleep() and
// usleep() end up here. with TIMER_ABSTIME, req is a time on that
// clock to sleep until; otherwise it is a length of time.
uint64
linux_clock_nanosleep(void)
{
  int clk, flags, r;
  uint64 req, ns, now;

  argint(0, &clk);
  argint(1, &flags);
  argaddr(2, &req);
  if ((r = gettimespec(req, &ns)) < 0)
    return r;

  if (flags & TIMER_ABSTIME) {
    // turn "until time ns on clock clk" into a length of time
    if (clk == CLOCK_REALTIME)
      now = rtc_now();
    else
      now = r_time() * NSEC_PER_COUNT;
    ns = (ns > now) ? ns - now : 0;
  }
  return sleepuntil(r_time() + ns / NSEC_PER_COUNT);
}
