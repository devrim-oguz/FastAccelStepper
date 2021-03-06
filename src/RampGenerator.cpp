#include <stdint.h>

#include "FastAccelStepper.h"
#include "StepperISR.h"

#ifdef TEST
#include <assert.h>
#endif

#include "RampGenerator.h"

// This define in order to not shoot myself.
#ifndef TEST
#define printf DO_NOT_USE_PRINTF
#endif

//*************************************************************************************************
// fill_queue generates commands to the stepper for executing a ramp
//
// Plan is to fill the queue with commmands with approx. 10 ms ahead (or more).
// For low speeds, this results in single stepping
// For high speeds (40kSteps/s) approx. 400 Steps to be created using 3 commands
//
// Basis of the calculation is the relation between steps and time via
// acceleration a:
//
//		s = 1/2 * a * t²
//
// With v = a * t for the acceleration case, then v can be deducted:
//
//		s = 1/2 * v² / a
//
//	    v = sqrt(2 * s * a)
//
//*************************************************************************************************

void RampGenerator::init() {
  _config.min_travel_ticks = 0;
  _config.upm_inv_accel2 = 0;
  _ro.target_pos = 0;
  _rw.ramp_state = RAMP_STATE_IDLE;
#if (TICKS_PER_S != 16000000L)
  upm_timer_freq = upm_from((uint32_t)TICKS_PER_S);
#endif
}
void RampGenerator::update_ramp_steps() {
  _config.ramp_steps = upm_to_u32(upm_divide(
      _config.upm_inv_accel2, upm_square(upm_from(_config.min_travel_ticks))));
}
void RampGenerator::setSpeed(uint32_t min_step_us) {
  if (min_step_us == 0) {
    return;
  }
  uint32_t min_travel_ticks = US_TO_TICKS(min_step_us);
  if (min_travel_ticks < MIN_DELTA_TICKS) {
    min_travel_ticks = MIN_DELTA_TICKS; // set to lower limit
  }
  _config.min_travel_ticks = min_travel_ticks;
  update_ramp_steps();
}
void RampGenerator::setAcceleration(uint32_t accel) {
  if (accel == 0) {
    return;
  }
  upm_float upm_inv_accel = upm_divide(UPM_TICKS_PER_S, upm_from(2 * accel));
  _config.upm_inv_accel2 = upm_multiply(UPM_TICKS_PER_S, upm_inv_accel);
  update_ramp_steps();
}
void RampGenerator::_applySpeedAcceleration(uint32_t ticks_at_queue_end,
                                            int32_t target_pos) {
  uint32_t performed_ramp_up_steps = upm_to_u32(upm_divide(
      _config.upm_inv_accel2, upm_square(upm_from(ticks_at_queue_end))));

  noInterrupts();
  _ro.min_travel_ticks = _config.min_travel_ticks;
  _ro.upm_inv_accel2 = _config.upm_inv_accel2;
  _rw.performed_ramp_up_steps = performed_ramp_up_steps;
  _ro.target_pos = target_pos;
  interrupts();
}
void RampGenerator::applySpeedAcceleration(uint32_t ticks_at_queue_end) {
  _applySpeedAcceleration(ticks_at_queue_end, _ro.target_pos);
}
int RampGenerator::calculateMoveTo(int32_t target_pos,
                                   int32_t position_at_queue_end,
                                   uint32_t ticks_at_queue_end) {
  if (_config.min_travel_ticks == 0) {
    return MOVE_ERR_SPEED_IS_UNDEFINED;
  }
  if (_config.upm_inv_accel2 == 0) {
    return MOVE_ERR_ACCELERATION_IS_UNDEFINED;
  }

  _applySpeedAcceleration(ticks_at_queue_end, target_pos);

  if (_rw.ramp_state == RAMP_STATE_IDLE) {
    uint8_t start_state;
    // This can overflow, which is legal
    int32_t delta = target_pos - position_at_queue_end;
    if (delta > 0) {
      start_state = RAMP_STATE_ACCELERATE | RAMP_MOVE_UP;
    } else if (delta < 0) {
      start_state = RAMP_STATE_ACCELERATE | RAMP_MOVE_DOWN;
    } else {
      return MOVE_OK;
    }

    noInterrupts();
    _rw.keep_running = false;
    _ro.force_stop = false;
    _rw.ramp_state = start_state;
    interrupts();
  }

#ifdef TEST
  printf(
      "Ramp data: go to %d  curr_ticks = %u travel_ticks = %u "
      "Ramp steps = %u Performed ramp steps = %u\n",
      target_pos, ticks_at_queue_end, _config.min_travel_ticks,
      _config.ramp_steps, _rw.performed_ramp_up_steps);
#endif
#ifdef DEBUG
  char buf[256];
  sprintf(buf,
          "Ramp data: go to = %ld  curr_ticks = %lu travel_ticks = %lu "
          "Ramp steps = %lu Performed ramp steps = %lu\n",
          target_pos, ticks_at_queue_end, _min_travel_ticks, _config.ramp_steps,
          _rw.performed_ramp_up_steps);
  Serial.println(buf);
#endif
  return MOVE_OK;
}

int8_t RampGenerator::moveTo(int32_t position, int32_t pos_at_queue_end,
                             uint32_t ticks_at_queue_end) {
  int32_t curr_pos;
  if (isStopping()) {
    return MOVE_ERR_STOP_ONGOING;
  }
  if (isRampGeneratorActive() && !_rw.keep_running) {
    curr_pos = _ro.target_pos;
  } else {
    curr_pos = pos_at_queue_end;
  }
  inject_fill_interrupt(1);
  int res = calculateMoveTo(position, curr_pos, ticks_at_queue_end);
  inject_fill_interrupt(2);
  return res;
}
int8_t RampGenerator::move(int32_t move, int32_t pos_at_queue_end,
                           uint32_t ticks_at_queue_end) {
  int32_t curr_pos;
  if (isRampGeneratorActive() && !_rw.keep_running) {
    curr_pos = _ro.target_pos;
  } else {
    curr_pos = pos_at_queue_end;
  }
  int32_t new_pos = curr_pos + move;
  return moveTo(new_pos, pos_at_queue_end, ticks_at_queue_end);
}

//*************************************************************************************************
static bool _getNextCommand(const struct ramp_ro_s *ro, struct ramp_rw_s *rw,
                            uint32_t ticks_at_queue_end,
                            int32_t position_at_queue_end,
                            struct ramp_command_s *command) {
  if (rw->ramp_state == RAMP_STATE_IDLE) {
    return false;
  }

  // This should never be true
  if (ticks_at_queue_end == 0) {
    ticks_at_queue_end = TICKS_FOR_STOPPED_MOTOR;
  }

  uint8_t next_state = rw->ramp_state;
  uint8_t move_state = next_state & RAMP_MOVE_MASK;
  bool count_up = (move_state == RAMP_MOVE_UP);

  // check state for acceleration/deceleration or deceleration to stop
  uint32_t remaining_steps;
  bool need_count_up;
  if (rw->keep_running) {
    need_count_up = count_up;
    remaining_steps = 0xfffffff;
  } else {
    int32_t delta = ro->target_pos -
                    position_at_queue_end;  // this can overflow, which is legal
    if (delta == 0) {  // This case should actually never happen
      rw->ramp_state = RAMP_STATE_IDLE;
      return false;
    }
    need_count_up = delta > 0;
    remaining_steps = abs(delta);
  }

  if (ro->force_stop) {
    next_state = RAMP_STATE_DECELERATE_TO_STOP | move_state;
    remaining_steps = rw->performed_ramp_up_steps;
    rw->keep_running = false;
  }
  // Detect change in direction and if so, initiate deceleration to stop
  else if (count_up != need_count_up) {
    next_state = RAMP_STATE_DECELERATE_TO_STOP | move_state;
    remaining_steps = rw->performed_ramp_up_steps;
  } else {
    // If come here, then direction is same as current movement
    if (remaining_steps <= rw->performed_ramp_up_steps) {
      next_state = RAMP_STATE_DECELERATE_TO_STOP;
    } else if (ro->min_travel_ticks < ticks_at_queue_end) {
      next_state = RAMP_STATE_ACCELERATE;
    } else if (ro->min_travel_ticks > ticks_at_queue_end) {
      next_state = RAMP_STATE_DECELERATE;
    } else {
      next_state = RAMP_STATE_COAST;
    }
    next_state |= move_state;
  }

  // Forward planning of 1ms or more on slow speed.
  uint32_t planning_steps = max((TICKS_PER_S / 1000) / ticks_at_queue_end, 1);
  uint32_t next_ticks;

  rw->ramp_state = next_state;

#ifdef TEST
  printf("pos@queue_end=%d remaining=%u ramp steps=%u planning steps=%d  ",
         position_at_queue_end, remaining_steps, rw->performed_ramp_up_steps,
         planning_steps);
  switch (next_state & RAMP_MOVE_MASK) {
    case RAMP_MOVE_UP:
      printf("+");
      break;
    case RAMP_MOVE_DOWN:
      printf("-");
      break;
    case 0:
      printf("=");
      break;
    default:
      printf("ERR");
      break;
  }
  switch (next_state & RAMP_STATE_MASK) {
    case RAMP_STATE_COAST:
      printf("COAST");
      break;
    case RAMP_STATE_ACCELERATE:
      printf("ACC");
      break;
    case RAMP_STATE_DECELERATE:
      printf("DEC");
      break;
    case RAMP_STATE_DECELERATE_TO_STOP:
      printf("STOP");
      break;
  }
  printf("\n");
#endif

  uint32_t curr_ticks = ticks_at_queue_end;
  switch (next_state & RAMP_STATE_MASK) {
    uint32_t d_ticks_new;
    uint32_t upm_rem_steps;
    upm_float upm_d_ticks_new;
    case RAMP_STATE_COAST:
      next_ticks = ro->min_travel_ticks;
      // do not overshoot ramp down start
      planning_steps =
          min(planning_steps, remaining_steps - rw->performed_ramp_up_steps);
      break;
    case RAMP_STATE_ACCELERATE:
      upm_rem_steps = upm_from(rw->performed_ramp_up_steps + planning_steps);
      upm_d_ticks_new = upm_sqrt(upm_divide(ro->upm_inv_accel2, upm_rem_steps));

      d_ticks_new = upm_to_u32(upm_d_ticks_new);

      // avoid overshoot
      next_ticks = max(d_ticks_new, ro->min_travel_ticks);
      if (rw->performed_ramp_up_steps == 0) {
        curr_ticks = d_ticks_new;
      } else {
        // CLIPPING: avoid increase
        next_ticks = min(next_ticks, curr_ticks);
      }

#ifdef TEST
      printf("accelerate ticks => %d  during %d steps (d_ticks_new = %u)",
             next_ticks, planning_steps, d_ticks_new);
      printf("... %u+%u steps\n", rw->performed_ramp_up_steps, planning_steps);
#endif
      break;
    case RAMP_STATE_DECELERATE:
      upm_rem_steps = upm_from(rw->performed_ramp_up_steps + planning_steps);
      upm_d_ticks_new = upm_sqrt(upm_divide(ro->upm_inv_accel2, upm_rem_steps));

      d_ticks_new = upm_to_u32(upm_d_ticks_new);

      // avoid undershoot
      next_ticks = min(d_ticks_new, ro->min_travel_ticks);

      // CLIPPING: avoid reduction
      next_ticks = max(next_ticks, curr_ticks);

#ifdef TEST
      printf("decelerate ticks => %d  during %d steps (d_ticks_new = %u)",
             next_ticks, planning_steps, d_ticks_new);
      printf("... %u+%u steps\n", rw->performed_ramp_up_steps, planning_steps);
#endif
      break;
    case RAMP_STATE_DECELERATE_TO_STOP:
      upm_rem_steps = upm_from(remaining_steps - planning_steps);
      upm_d_ticks_new = upm_sqrt(upm_divide(ro->upm_inv_accel2, upm_rem_steps));

      d_ticks_new = upm_to_u32(upm_d_ticks_new);

      // avoid undershoot
      next_ticks = max(d_ticks_new, ro->min_travel_ticks);

      // CLIPPING: avoid reduction
      next_ticks = max(next_ticks, curr_ticks);
#ifdef TEST
      printf("decelerate ticks => %d  during %d steps (d_ticks_new = %u)\n",
             next_ticks, planning_steps, d_ticks_new);
#endif
      break;
    default:
      // TODO: how to treat this (error) case ?
      next_ticks = curr_ticks;
#ifdef TEST
      assert(false);
#endif
  }

  // CLIPPING: avoid increase
  next_ticks = min(next_ticks, ABSOLUTE_MAX_TICKS);

  // Number of steps to execute with limitation to min 1 and max remaining steps
  uint16_t steps = planning_steps;
#ifdef TEST
  printf(
      "steps for the command = %d  with planning_steps = %u and "
      "next_ticks = %u\n",
      steps, planning_steps, next_ticks);
#endif
  steps = max(steps, 1);
  steps = min(steps, abs(remaining_steps));
  steps = min(127, steps);

  switch (next_state & RAMP_STATE_MASK) {
    case RAMP_STATE_COAST:
      break;
    case RAMP_STATE_ACCELERATE:
      rw->performed_ramp_up_steps += steps;
      break;
    case RAMP_STATE_DECELERATE:
    case RAMP_STATE_DECELERATE_TO_STOP:
      rw->performed_ramp_up_steps -= steps;
      break;
  }

#ifdef TEST
  assert(next_ticks > 0);
#endif

  command->ticks = next_ticks;
  command->steps = steps;
  command->count_up = count_up;

  if (steps == abs(remaining_steps)) {
    if (count_up != need_count_up) {
      rw->ramp_state = RAMP_STATE_ACCELERATE | (move_state ^ RAMP_MOVE_MASK);
#ifdef TEST
      puts("Stepper reverse");
#endif
    } else {
      rw->ramp_state = RAMP_STATE_IDLE;
#ifdef TEST
      puts("Stepper stop");
#endif
    }
  }

#ifdef TEST
  printf(
      "add command Steps = %d ticks = %d  Target pos = %d "
      "Remaining steps = %d\n",
      steps, next_ticks, ro->target_pos, remaining_steps);
#endif
  return true;
}
bool RampGenerator::getNextCommand(uint32_t ticks_at_queue_end,
                                   int32_t position_at_queue_end,
                                   struct ramp_command_s *command) {
  return _getNextCommand(&_ro, &_rw, ticks_at_queue_end, position_at_queue_end,
                         command);
}
void RampGenerator::abort() { _rw.ramp_state = RAMP_STATE_IDLE; }
bool RampGenerator::isRampGeneratorActive() {
  return _rw.ramp_state != RAMP_STATE_IDLE;
}
