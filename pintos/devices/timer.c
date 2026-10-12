#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

static struct list sleep_list;

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	list_init (&sleep_list);
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

//TODO 기존 코드
/* Suspends execution for approximately TICKS timer ticks. */
// void
// timer_sleep (int64_t ticks) {
// 	int64_t start = timer_ticks ();

// 	ASSERT (intr_get_level () == INTR_ON);

// 	while (timer_elapsed (start) < ticks)
// 		thread_yield ();
// }

//! === 정승영 수정 코드 === //
/* Suspends execution for approximately TICKS timer ticks. */
void
timer_sleep (int64_t ticks) {
	// ? 맨 처음 코딩한 아래 계산식 자체는 맞지만 wake_tick을 지역변수에만 저장하면
	// 	 timer_interrupt()가 sleep_list에서 꺼낸 각 스레드의 기상 시각을
	//   확인할 수 없다. 따라서 struct thread의 wake_tick에 저장한다.
	// int64_t start = timer_ticks ();
	// 스레드가 깨어날 시간(현재 틱 + 기다릴 틱)을 wake_tick에 저장한다.
	// int64_t wake_tick = start + ticks;

	ASSERT (intr_get_level () == INTR_ON);

	/* 0 이하의 대기 시간은 스레드를 재우지 않고 즉시 반환한다. */
	if (ticks <= 0) {
		return;
	}
	/* 대기 목록 등록과 스레드 차단 사이에 타이머 인터럽트가
	   실행되는 경쟁 상태를 막기 위해 인터럽트를 끈다.
	   반환값에는 나중에 복원할 기존 인터럽트 상태가 저장된다. */
	enum intr_level old_level = intr_disable ();

	/* 기상 시각과 대기 목록 연결 정보를 저장할 현재 스레드의
	   주소를 가져온다. */
	struct thread *current = thread_current ();

	/* 현재 시각에 요청받은 대기 시간을 더해 기상 시각을 기록하고,
	   타이머 인터럽트가 찾을 수 있도록 대기 목록에 등록한다. */
	current->wake_tick = timer_ticks () + ticks;
	list_push_back (&sleep_list, &current->elem);

	/* 타이머 인터럽트가 기상 시각에 이 스레드를 깨울 때까지
	   준비 큐에서 제외한다. */
	thread_block ();

	/* 스레드가 다시 실행되면 timer_sleep() 호출 전의
	   인터럽트 상태로 복원한다. */
	intr_set_level (old_level);
}
//! === END === //

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}


//TODO 기존 코드
/* Timer interrupt handler. */
// static void
// timer_interrupt (struct intr_frame *args UNUSED) {
// 	ticks++;
// 	thread_tick ();
// }

//! === 정승영 수정 코드 === //
/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	/* sleep_list를 순회하면서 현재 검사 중인 목록 항목을 가리킨다. */
	struct list_elem *elem;

	/* 현재 elem을 멤버로 가지고 있는 실제 스레드의 주소를 저장한다. */
	struct thread *sleeping;

	/* 타이머 인터럽트가 발생했으므로 현재 시간을 1틱 증가시킨다. */
	ticks++;

	/* shortcut: 매 틱 전체 대기 목록을 검사하며, 대기 스레드가
	   많아지면 기상 시각 순으로 정렬하는 방식으로 변경한다. */
	elem = list_begin (&sleep_list);
	while (elem != list_end (&sleep_list)) {
		/* 현재 목록 항목을 포함하고 있는 실제 스레드를 찾는다. */
		sleeping = list_entry (elem, struct thread, elem);

		if (sleeping->wake_tick <= ticks) {
			/* 하나의 elem이 sleep_list와 ready_list에 동시에
			   들어가지 않도록 sleep_list에서 먼저 제거한다.
			   list_remove()가 반환한 다음 항목부터 순회를 계속한다. */
			elem = list_remove (elem);

			/* 기상 시각이 된 스레드를 BLOCKED에서 READY 상태로
			   변경하여 다시 스케줄링 대상에 포함한다.
			   즉시 RUNNING이 되는 것은 아니며, 스케줄러가 선택해야
			   실제 실행을 재개한다. */
			thread_unblock (sleeping);
		} else {
			/* 아직 기상 시각이 아니라면 다음 항목을 검사한다. */
			elem = list_next (elem);
		}
	}

	/* 현재 실행 중인 스레드의 CPU 사용 시간과 선점 여부를 처리한다. */
	thread_tick ();
}
//! === END === //

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */
	barrier ();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
