#include "Temperature.h"
#include "includes.h"

const char * const heaterID[MAX_HEATER_COUNT]      = HEAT_SIGN_ID;
const char * const heatDisplayID[MAX_HEATER_COUNT] = HEAT_DISPLAY_ID;
const char * const heatShortID[MAX_HEATER_COUNT]   = HEAT_SHORT_ID;
const char * const heatCmd[MAX_HEATER_COUNT]       = HEAT_CMD;
const char * const heatWaitCmd[MAX_HEATER_COUNT]   = HEAT_WAIT_CMD;
const char * const extruderDisplayID[]             = EXTRUDER_ID;
const char * const toolChange[]                    = TOOL_CHANGE;

static HEATER   heater = {{}, NOZZLE0};
static uint8_t  heat_update_seconds = TEMPERATURE_QUERY_SLOW_SECONDS;
static uint32_t heat_next_update_time = 0;
static bool     heat_update_waiting = false;
static uint8_t  heat_send_waiting = 0;
static uint8_t  heat_feedback_waiting = 0;

#define AUTOREPORT_TIMEOUT (heat_next_update_time + 3000)  // update interval + 3 second grace period

// verify that the heater index is valid, and fix the index of multiple in and 1 out tool nozzles
static uint8_t heaterIndexFix(uint8_t index)
{
  if (index == BED && infoSettings.bed_en)  // bed
    return index;

  if (index == CHAMBER && infoSettings.chamber_en)  // chamber
    return index;

  if (index < infoSettings.hotend_count)  // vaild tool nozzle
    return index;

  if (index < infoSettings.ext_count && infoSettings.hotend_count == 1)  // "multi-extruder" that shares a single nozzle
    return NOZZLE0;

  return INVALID_HEATER;  // invalid heater
}

void heatSetTargetTemp(uint8_t index, const int16_t temp, const TEMP_SOURCE tempSource)
{
  index = heaterIndexFix(index);

  if (index == INVALID_HEATER)
    return;

  switch (tempSource)
  {
    case FROM_HOST:
      if (GET_BIT(heat_feedback_waiting, index))
        SET_BIT_OFF(heat_feedback_waiting, index);
      else if (!GET_BIT(heat_send_waiting, index))
        heater.T[index].target = temp;
      break;

    case FROM_GUI:
      heater.T[index].target = NOBEYOND(0, temp, infoSettings.max_temp[index]);
      SET_BIT_ON(heat_send_waiting, index);

      if (inRange(heater.T[index].current, heater.T[index].target, TEMPERATURE_RANGE))
        heater.T[index].status = SETTLED;
      else
        heater.T[index].status = heater.T[index].target > heater.T[index].current ? HEATING : COOLING;
      break;

    case FROM_CMD:
      if (GET_BIT(heat_feedback_waiting, index) == false)
      {
        heater.T[index].target = temp;
        SET_BIT_ON(heat_feedback_waiting, index);
      }
      break;
  }
}

uint16_t heatGetTargetTemp(uint8_t index)
{
  index = heaterIndexFix(index);

  if (index == INVALID_HEATER)
    return 0;

  return heater.T[index].target;
}

void heatSetCurrentTemp(uint8_t index, const int16_t temp)
{
  index = heaterIndexFix(index);

  if (index == INVALID_HEATER)
    return;

  heater.T[index].current = NOBEYOND(-99, temp, 999);

  if (infoMachineSettings.autoReportTemp)
    heatSetNextUpdateTime();  // set next timeout for temperature auto-report
}

int16_t heatGetCurrentTemp(uint8_t index)
{
  index = heaterIndexFix(index);

  if (index == INVALID_HEATER)
    return 0;

  return heater.T[index].current;
}

void heatCoolDown(void)
{
  for (uint8_t i = 0; i < MAX_HEATER_COUNT; i++)
  {
    heatSetTargetTemp(i, 0, FROM_GUI);
  }
}

bool heatGetIsWaiting(const uint8_t index)
{
  return (heater.T[index].waiting == true);
}

bool heatHasWaiting(void)
{
  for (uint8_t i = 0; i < MAX_HEATER_COUNT; i++)
  {
    if (heater.T[i].waiting == true)
      return true;
  }

  return false;
}

void heatSetIsWaiting(uint8_t index, const bool isWaiting)
{
  index = heaterIndexFix(index);

  if (index == INVALID_HEATER)
    return;

  heater.T[index].waiting = isWaiting;

  if (isWaiting == true)  // wait heating now, query more frequently
    heatSetUpdateSeconds(TEMPERATURE_QUERY_FAST_SECONDS);
  else if (heatHasWaiting() == false)
    heatSetUpdateSeconds(TEMPERATURE_QUERY_SLOW_SECONDS);
}

void heatClearIsWaiting(void)
{
  for (uint8_t i = 0; i < MAX_HEATER_COUNT; i++)
  {
    heater.T[i].waiting = false;
  }

  heatSetUpdateSeconds(TEMPERATURE_QUERY_SLOW_SECONDS);
}

bool heatSetTool(const uint8_t toolIndex)
{
  if (storeCmd("%s\n", toolChange[toolIndex]))
  {
    heater.toolIndex = toolIndex;
    return true;
  }

  return false;
}

void heatSetToolIndex(const uint8_t toolIndex)
{
  heater.toolIndex = toolIndex;
}

uint8_t heatGetToolIndex(void)
{
  return heater.toolIndex;
}

uint8_t heatGetCurrentHotend(void)
{
  return (infoSettings.hotend_count == 1) ? NOZZLE0 : heater.toolIndex;
}

bool heaterDisplayIsValid(const uint8_t index)
{
  if (index >= infoSettings.hotend_count && index < MAX_HOTEND_COUNT)
    return false;

  if (!infoSettings.bed_en && index == BED)
    return false;

  if (!infoSettings.chamber_en && index == CHAMBER)
    return false;

  return true;
}

void heatSetUpdateSeconds(const uint8_t seconds)
{
  if (heat_update_seconds == seconds)
    return;

  heat_update_seconds = seconds;

  if (infoMachineSettings.autoReportTemp && !heat_update_waiting)
    heat_update_waiting = storeCmd("M155 S%u\n", heat_update_seconds);
}

uint8_t heatGetUpdateSeconds(void)
{
  return heat_update_seconds;
}

void heatSyncUpdateSeconds(const uint8_t seconds)
{
  heat_update_seconds = seconds;
}

void heatSetNextUpdateTime(void)
{
  heat_next_update_time = OS_GetTimeMs() + SEC_TO_MS(heat_update_seconds);
}

void heatSetUpdateWaiting(const bool isWaiting)
{
  heat_update_waiting = isWaiting;
}

void loopCheckHeater(void)
{
  // send M105 to query the temperatures, if motherboard does not supports M155 (AUTO_REPORT_TEMPERATURES) feature
  // to automatically report the temperatures
  if (!infoMachineSettings.autoReportTemp)
  {
    do
    { // send M105 to query temperature continuously

      // if next check time not yet elapsed or pending command (to avoid collision in gcode response processing), do nothing
      if (OS_GetTimeMs() < heat_next_update_time || requestCommandInfoIsRunning())
        break;

      // if M105 previously sent and still waiting for a reply, extend next check time
      if (heat_update_waiting)
      {
        heatSetNextUpdateTime();
        break;
      }

      heat_update_waiting = storeCmd("M105\n");

      if (heat_update_waiting)
        heatSetNextUpdateTime();
    } while (0);
  }
  else  // check temperature auto-report timout and resend M155 command in case of timeout expired
  {
    if (OS_GetTimeMs() >= AUTOREPORT_TIMEOUT && !heat_update_waiting)
    {
      heat_update_waiting = storeCmd("M155 S%u\n", heat_update_seconds);

      if (heat_update_waiting)
        heatSetNextUpdateTime();  // set next timeout for temperature auto-report
    }
  }

  for (uint8_t i = 0; i < MAX_HEATER_COUNT; i++)
  {
    if (heater.T[i].waiting && inRange(heater.T[i].current, heater.T[i].target, TEMPERATURE_RANGE))
      heater.T[i].waiting = false;

    if (heater.T[i].status != SETTLED && inRange(heater.T[i].current, heater.T[i].target, TEMPERATURE_RANGE))
    {
      switch (heater.T[i].status)
      {
        case HEATING:
          BUZZER_PLAY(SOUND_HEATED);
          break;

        case COOLING:
          BUZZER_PLAY(SOUND_COOLED);
          break;

        default:
          break;
      }

      heater.T[i].status = SETTLED;
    }

    if (GET_BIT(heat_send_waiting, i) && !GET_BIT(heat_feedback_waiting, i))
    {
      if (storeCmd("%s S%u\n", heatCmd[i], heatGetTargetTemp(i)))
      {
        SET_BIT_OFF(heat_send_waiting, i);
        SET_BIT_ON(heat_feedback_waiting, i);
      }
    }
  }

  if (MENU_IS_NOT(menuHeat) && !heatHasWaiting())
    heatSetUpdateSeconds(TEMPERATURE_QUERY_SLOW_SECONDS);
}
