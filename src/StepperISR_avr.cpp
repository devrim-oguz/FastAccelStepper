#include "FastAccelStepper.h"
#include "StepperISR.h"

#if defined(ARDUINO_ARCH_AVR)

#define Stepper_Toggle(X) TCCR1A = (TCCR1A | _BV(COM1##X##0)) & ~_BV(COM1##X##1)
#define Stepper_Zero(X) TCCR1A = (TCCR1A | _BV(COM1##X##1)) & ~_BV(COM1##X##0)
#define Stepper_Disconnect(X) \
  TCCR1A = (TCCR1A & ~(_BV(COM1##X##1) | _BV(COM1##X##0)))
#define Stepper_IsToggling(X) \
  ((TCCR1A & (_BV(COM1##X##0) | _BV(COM1##X##1))) == _BV(COM1##X##0))

// Here are the global variables to interface with the interrupts
StepperQueue fas_queue[NUM_QUEUES];

void StepperQueue::init(uint8_t queue_num, uint8_t step_pin) {
  _initVars();
  skip = 0;
  digitalWrite(step_pin, LOW);
  pinMode(step_pin, OUTPUT);
  if (step_pin == stepPinStepperA) {
    isChannelA = true;
    noInterrupts();
    Stepper_Disconnect(A);
    TCCR1C = _BV(FOC1A);     // force compare to ensure disconnect
    TIMSK1 &= ~_BV(OCIE1A);  // disable compare A interrupt
    interrupts();
  }
  if (step_pin == stepPinStepperB) {
    isChannelA = false;
    noInterrupts();
    Stepper_Disconnect(B);
    TCCR1C = _BV(FOC1B);     // force compare to ensure disconnect
    TIMSK1 &= ~_BV(OCIE1B);  // disable compare B interrupt
    interrupts();
  }
}

#define AVR_STEPPER_ISR(CHANNEL, queue, ocr, foc)                   \
  ISR(TIMER1_COMP##CHANNEL##_vect) {                                \
    if (queue.skip) {                                               \
      if ((--queue.skip) == 0) {                                    \
        Stepper_Toggle(CHANNEL);                                    \
      }                                                             \
      ocr += PERIOD_TICKS;                                          \
      return;                                                       \
    }                                                               \
    uint8_t rp = queue.read_idx;                                    \
    if (Stepper_IsToggling(CHANNEL)) {                              \
      TCCR1C = _BV(foc); /* clear bit */                            \
      struct queue_entry* e = &queue.entry[rp & QUEUE_LEN_MASK];    \
      if ((e->steps -= 2) > 1) {                                    \
        /* perform another step with this queue entry */            \
        ocr += queue.period;                                        \
        /* assign to skip and test for not zero */                  \
        if (0 != (queue.skip = e->n_periods)) {                     \
          Stepper_Zero(CHANNEL);                                    \
        }                                                           \
        return;                                                     \
      }                                                             \
      rp++;                                                         \
      queue.read_idx = rp;                                          \
    }                                                               \
    if (rp == queue.next_write_idx) {                               \
      /* queue is empty => set to disconnect */                     \
      Stepper_Disconnect(CHANNEL);                                  \
      /* disable compare interrupt */                               \
      TIMSK1 &= ~_BV(OCIE1##CHANNEL);                               \
      /* force compare to ensure disconnect */                      \
      TCCR1C = _BV(FOC1##CHANNEL);                                  \
      queue.isRunning = false;                                      \
      queue.ticks_at_queue_end = TICKS_FOR_STOPPED_MOTOR;           \
      return;                                                       \
    }                                                               \
    /* command in queue */                                          \
    struct queue_entry* e = &queue.entry[rp & QUEUE_LEN_MASK];      \
    ocr += (queue.period = e->period);                              \
    /* assign to skip and test for not zero */                      \
    if (0 != (queue.skip = e->n_periods)) {                         \
      Stepper_Zero(CHANNEL);                                        \
    } else {                                                        \
      Stepper_Toggle(CHANNEL);                                      \
    }                                                               \
    uint8_t steps = e->steps;                                       \
    if ((steps & 0x01) != 0) {                                      \
      digitalWrite(queue.dirPin,                                    \
                   digitalRead(queue.dirPin) == HIGH ? LOW : HIGH); \
    }                                                               \
  }
AVR_STEPPER_ISR(A, fas_queue_A, OCR1A, FOC1A)
AVR_STEPPER_ISR(B, fas_queue_B, OCR1B, FOC1B)

void StepperQueue::startQueue() {
  isRunning = true;
  if (isChannelA) {
    noInterrupts();
    TIFR1 = _BV(OCF1A);     // clear interrupt flag
    TIMSK1 |= _BV(OCIE1A);  // enable compare A interrupt
    OCR1A = TCNT1 + 40;     // definite start point
    interrupts();
  } else {
    noInterrupts();
    TIFR1 = _BV(OCF1B);     // clear interrupt flag
    TIMSK1 |= _BV(OCIE1B);  // enable compare A interrupt
    OCR1B = TCNT1 + 40;     // definite start point
    interrupts();
  }
}
void StepperQueue::forceStop() {
  if (isChannelA) {
    /* disable compare interrupt */
    TIMSK1 &= ~_BV(OCIE1A);
    /* set to disconnect */
    Stepper_Disconnect(A);
    /* force compare to ensure disconnect */
    TCCR1C = _BV(FOC1A);
  } else {
    /* disable compare interrupt */
    TIMSK1 &= ~_BV(OCIE1B);
    /* set to disconnect */
    Stepper_Disconnect(B);
    /* force compare to ensure disconnect */
    TCCR1C = _BV(FOC1B);
  }
  isRunning = false;
  ticks_at_queue_end = TICKS_FOR_STOPPED_MOTOR;

  // empty the queue
  read_idx = next_write_idx;
}
#endif
