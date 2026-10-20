/*
 * irq.h - interrupts: the vertical blank, the root counters (timers) and
 * DMA completion, delivered to the game's callbacks (LIBETC replacement).
 */
#ifndef RUNTIME_IRQ_H
#define RUNTIME_IRQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the interrupt numbers of InterruptCallback() */
enum {
    IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3,
    IRQ_TIMER0 = 4, IRQ_TIMER1 = 5, IRQ_TIMER2 = 6, IRQ_PAD = 7, IRQ_SIO = 8, IRQ_SPU = 9,
    IRQ_COUNT = 11
};

void irq_reset(void);

/* microseconds since the program started */
uint64_t irq_time_us(void);

/* One vertical blank: the frame is shown (the frame hook), the
 * controllers answer, the callbacks run. */
void irq_vblank(void);

/* The game waits for the hardware (e.g. the CD): lets the sound run for a
 * moment and checks for interrupts at the next chance. */
void irq_idle(void);
int irq_vblank_count(void);

/* A DMA transfer has finished (its callback runs soon). */
void irq_dma_done(int channel);

/* the GPU has finished drawing (DrawSyncCallback) */
void irq_draw_done(void);
uint32_t irq_set_draw_callback(uint32_t func);

/* timer registers (io.c) */
uint32_t irq_timer_value(int timer);
void irq_timer_write(int timer, int reg, uint32_t value);

/* controllers (hle_pad.c), updated at every vertical blank */
void pad_reset(void);
void pad_update(void);

/* hardware interrupt status (io.c) */
void io_set_interrupt(uint32_t bits);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_IRQ_H */
