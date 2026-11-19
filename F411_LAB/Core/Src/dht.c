#include "dht.h"
#include "main.h"
#include "ultrasonic.h"

extern TIM_HandleTypeDef htim1;

/* ==========================================================================
 * 1. SWV 디버깅 추적 전역 변수 (DHT + Ultrasonic 통합 정의)
 * ========================================================================== */
volatile float    g_trace_temperature_c;   /* 예: 21.6 (°C) */
volatile float    g_trace_humidity_rh;     /* 예: 30.0 (%RH) */

volatile uint16_t g_trace_echo_ticks;
volatile uint8_t  g_trace_echo_edge;
volatile uint8_t  g_trace_echo_state;
volatile uint32_t g_trace_edge_count;

volatile uint16_t g_trace_trig_ticks;
volatile uint8_t  g_trace_trig_edge;
volatile uint8_t  g_trace_trig_state;
volatile uint32_t g_trace_trig_edge_count;
volatile uint16_t g_trace_trig_to_echo_ticks;

#define DHT_PORT GPIOA
#define DHT_PIN GPIO_PIN_4

#define DHT_TIMER_TICKS_PER_US 1U

#define DHT_READ_PERIOD_MS 2000U
#define DHT_START_LOW_US 20000U
#define DHT_RELEASE_US 30U
#define DHT_RESPONSE_TIMEOUT_US 200U
#define DHT_BIT_TIMEOUT_US 200U
#define DHT_ONE_THRESHOLD_US 50U

#define US_SAMPLE_PERIOD_MS          200U
#define US_PUBLISH_PERIOD_MS         1000U
#define US_ECHO_TIMEOUT_MS           35U
#define US_TRIG_PULSE_US             10U
#define US_TIMER_TICKS_PER_US        1U
#define US_MIN_ECHO_TICKS            (100U * US_TIMER_TICKS_PER_US)
#define US_MAX_ECHO_TICKS            (30000U * US_TIMER_TICKS_PER_US)
#define US_MIN_DISTANCE_MM           20U
#define US_MAX_DISTANCE_MM           4000U
#define US_FILTER_SIZE               5U
#define US_REFERENCE_SOUND_SPEED_X10 3430U
#define US_DEFAULT_SOUND_SPEED_X10   US_REFERENCE_SOUND_SPEED_X10
#define US_MIN_SOUND_SPEED_X10       2500U
#define US_MAX_SOUND_SPEED_X10       4000U

/* ==========================================================================
 * 3. 내부 상태 및 정적 변수
 * ========================================================================== */
/* DHT 정적 변수 */
static uint32_t s_lastReadTick;
static uint32_t s_usErrorCount;
static Dht_Result s_result;
static volatile uint8_t s_resultReady;

/* Ultrasonic 정적 변수 */
typedef enum {
    US_STATE_IDLE = 0,
    US_STATE_WAIT_ECHO
} Ultrasonic_State;

static TIM_HandleTypeDef *s_htim;
static volatile Ultrasonic_State s_usState;
static uint32_t s_usLastSampleTick;
static uint32_t s_usLastPublishTick;
static uint32_t s_usEchoStartTick;
static uint16_t s_usEchoStartCnt;
static volatile uint16_t s_usTrigEndCnt;
static volatile uint16_t s_usEchoWidthTicks;
static volatile uint8_t  s_usEchoRising;
static volatile uint8_t  s_usEchoDone;

static uint16_t s_usSamplesRef[US_FILTER_SIZE];
static uint16_t s_usSamplesCorr[US_FILTER_SIZE];
static uint8_t  s_usSampleCount;
static uint8_t  s_usSampleIndex;
static uint32_t s_usErrorCount;
static uint16_t s_usSoundSpeedX10;
static Ultrasonic_Result s_usResult;
static volatile uint8_t s_usResultReady;

/* ==========================================================================
 * 4. 내부 함수 선언
 * ========================================================================== */
static void DHT_SetPinOutput(void);
static void DHT_SetPinInput(void);
static uint32_t DHT_NowUs(void);
static uint32_t DHT_ElapsedUs(uint32_t start);
static void DHT_DelayUs(uint32_t us);
static uint8_t DHT_WaitLevel(GPIO_PinState level, uint32_t timeout_us, uint32_t *duration);
static uint8_t DHT_ReadRaw(uint8_t *data);
static uint8_t DHT_Decode(const uint8_t *data, int32_t *temperature_c_x10, uint32_t *humidity_rh_x10);

static uint16_t US_GetCounter(void);
static void US_StoreSample(uint32_t width_ticks);
static uint16_t US_Median(const uint16_t *samples);
static void US_Publish(uint32_t now);

/* ==========================================================================
 * 5. 초기화 함수 (Dht_Init / Ultrasonic_Init 통합 관리)
 * ========================================================================== */
void Ultrasonic_Init(void)
{
    if (s_htim != NULL) {
        return; /* 이미 초기화되었으면 중복 실행 방지 */
    }

    s_htim = &htim1;
    HAL_GPIO_WritePin(TRIG_GPIO_Port, TRIG_Pin, GPIO_PIN_RESET);
    if (HAL_TIM_Base_Start(s_htim) != HAL_OK) {
        Error_Handler();
    }

    s_usState = US_STATE_IDLE;
    s_usLastSampleTick = HAL_GetTick();
    s_usLastPublishTick = s_usLastSampleTick;
    s_usEchoStartTick = 0U;
    s_usEchoStartCnt = 0U;
    s_usTrigEndCnt = 0U;
    s_usEchoWidthTicks = 0U;
    s_usEchoRising = 0U;
    s_usEchoDone = 0U;
    s_usSampleCount = 0U;
    s_usSampleIndex = 0U;
    s_usErrorCount = 0U;
    s_usSoundSpeedX10 = US_DEFAULT_SOUND_SPEED_X10;
    s_usResultReady = 0U;
    s_usResult.timestamp_ms = 0U;
    s_usResult.distance_ref_mm = 0U;
    s_usResult.distance_mm = 0U;
    s_usResult.valid = 0U;
    s_usResult.errors = 0U;

    g_trace_echo_ticks = 0U;
    g_trace_echo_edge = 0U;
    g_trace_echo_state = 0U;
    g_trace_edge_count = 0U;

    g_trace_trig_ticks = 0U;
    g_trace_trig_edge = 0U;
    g_trace_trig_state = 0U;
    g_trace_trig_edge_count = 0U;
    g_trace_trig_to_echo_ticks = 0U;
}

void Dht_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  /* 초음파 및 htim1 타이머가 아직 초기화되지 않았다면 함께 초기화 */
  Ultrasonic_Init();

  __HAL_RCC_GPIOA_CLK_ENABLE();

  gpio.Pin = DHT_PIN;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(DHT_PORT, &gpio);

  s_lastReadTick = HAL_GetTick();
  s_usErrorCount = 0U;
  s_resultReady = 0U;
  s_result.timestamp_ms = 0U;
  s_result.errors = 0U;
  s_result.temperature_c_x10 = 0;
  s_result.humidity_rh_x10 = 0U;
  s_result.valid = 0U;

  g_trace_temperature_c = 0.0f;
  g_trace_humidity_rh = 0.0f;

}

/* ==========================================================================
 * 6. 주기 실행 태스크 (Ultrasonic_Task + Dht_Task 통합 스케줄링)
 * ========================================================================== */
void Ultrasonic_Task(void)
{
    uint32_t now;
    uint16_t trigStartCnt;
    uint16_t trigEndCnt;

    if (s_htim == NULL) {
        return;
    }

    now = HAL_GetTick();

    switch (s_usState) {
        case US_STATE_IDLE:
            /* 1. 트리거 발생 후 100ms가 지나면 SWV 그래프용 변수를 0으로 내림 (뚜렷한 펄스 표시용) */
            if ((g_trace_trig_ticks != 0U) && ((now - s_usLastSampleTick) >= 100U)) {
                g_trace_trig_ticks = 0U;
            }

            /* 2. 200ms 주기가 되면 새로운 트리거 펄스 출력 */
            if ((now - s_usLastSampleTick) >= US_SAMPLE_PERIOD_MS) {
                s_usLastSampleTick = now;
                s_usEchoRising = 0U;
                s_usEchoDone = 0U;

                trigStartCnt = US_GetCounter();
                HAL_GPIO_WritePin(TRIG_GPIO_Port, TRIG_Pin, GPIO_PIN_SET);
                g_trace_trig_edge = 1U;
                g_trace_trig_state = 1U;
                g_trace_trig_ticks = 1U;
                g_trace_trig_edge_count++;

                while ((uint16_t)(US_GetCounter() - trigStartCnt) < US_TRIG_PULSE_US) {
                }

                HAL_GPIO_WritePin(TRIG_GPIO_Port, TRIG_Pin, GPIO_PIN_RESET);
                trigEndCnt = US_GetCounter();
                s_usTrigEndCnt = trigEndCnt;

                g_trace_trig_ticks = (uint16_t)(trigEndCnt - trigStartCnt);
         //       g_trace_trig_ticks = 0U;
                g_trace_trig_edge = 0U;
                g_trace_trig_state = 0U;
                g_trace_trig_edge_count++;

                s_usEchoStartTick = now;
                s_usState = US_STATE_WAIT_ECHO;
                g_trace_echo_state = 2U;
            }
            break;

        case US_STATE_WAIT_ECHO:
            if (s_usEchoDone != 0U) {
                if (s_usEchoDone == 1U) {
                    US_StoreSample(s_usEchoWidthTicks);
                } else {
                    s_usErrorCount++;
                }
                s_usEchoDone = 0U;
                s_usState = US_STATE_IDLE;
//              g_trace_trig_ticks = 0U;
            } else if ((now - s_usEchoStartTick) >= US_ECHO_TIMEOUT_MS) {
                s_usErrorCount++;
                s_usState = US_STATE_IDLE;
                g_trace_echo_edge = 0U;
                g_trace_echo_state = 0U;
//              g_trace_trig_ticks = 0U;
            }
            break;

        default:
            s_usState = US_STATE_IDLE;
            break;
    }

    if ((now - s_usLastPublishTick) >= US_PUBLISH_PERIOD_MS) {
        s_usLastPublishTick = now;
        US_Publish(now);
    }
}

void Dht_Task(void)
{
  uint32_t now = HAL_GetTick();
  uint8_t data[5];
  int32_t temperature_c_x10;
  uint32_t humidity_rh_x10;
  uint8_t ok = 0U;

  /* 1. 초음파 태스크를 먼저 실행하여 상태 갱신 */
  Ultrasonic_Task();

  /* 2. DHT 읽기 주기 확인 */
  if ((now - s_lastReadTick) < DHT_READ_PERIOD_MS) {
      return;
  }

  /* 3. 초음파가 에코 대기(WAIT_ECHO) 중이면 20ms 지연 충돌을 막기 위해 대기 */
  if (s_usState != US_STATE_IDLE) {
      return;
  }

  s_lastReadTick = now;

  DHT_SetPinOutput();
  HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_RESET);
  DHT_DelayUs(DHT_START_LOW_US);
  HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_SET);
  DHT_DelayUs(DHT_RELEASE_US);
  DHT_SetPinInput();

  if (DHT_ReadRaw(data) != 0U)
  {
    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);

    if (checksum == data[4])
    {
      if (DHT_Decode(data, &temperature_c_x10, &humidity_rh_x10) != 0U)
      {
        s_result.temperature_c_x10 = (int16_t)temperature_c_x10;
        s_result.humidity_rh_x10 = (uint16_t)humidity_rh_x10;

        /* 소수점 1자리까지 표시되는 실수(float) 값으로 저장 */
        g_trace_temperature_c = (float)temperature_c_x10 / 10.0f;
        g_trace_humidity_rh = (float)humidity_rh_x10 / 10.0f;

        /* 측정 온도를 기반으로 초음파 음속 자동 보정 (v = 331.3 + 0.606 * T) */
        uint16_t speed_x10 = (uint16_t)(3313 + ((temperature_c_x10 * 606) / 1000));
        Ultrasonic_SetSoundSpeedX10(speed_x10);

        s_result.valid = 1U;
        ok = 1U;
      }
    }
  }

  if (ok == 0U)
  {
    s_result.valid = 0U;
    s_usErrorCount++;
  }

  s_result.timestamp_ms = now;
  s_result.errors = s_usErrorCount;
  s_resultReady = 1U;
}

/* ==========================================================================

	7. 공용 API 및 EXTI 인터럽트 콜백

========================================================================== */

uint8_t Dht_GetResult(Dht_Result *result)
{
  if ((result == NULL) || (s_resultReady == 0U))
  {
    return 0U;
  }

  *result = s_result;
  s_resultReady = 0U;
  return 1U;
}

uint8_t Ultrasonic_GetResult(Ultrasonic_Result *result)
{
	if ((result == NULL) || (s_usResultReady == 0U)) {
	return 0U;
	}

	*result = s_usResult;
	s_usResultReady = 0U;
	return 1U;
}

uint8_t Ultrasonic_IsIdle(void)
{
	return (s_usState == US_STATE_IDLE) ? 1U : 0U;
}

void Ultrasonic_SetSoundSpeedX10(uint16_t speed_x10)
{
	if ((speed_x10 >= US_MIN_SOUND_SPEED_X10) && (speed_x10 <= US_MAX_SOUND_SPEED_X10)) {
		s_usSoundSpeedX10 = speed_x10;
	}
}

uint16_t Ultrasonic_GetLastEchoTicks(void)
{
	return s_usEchoWidthTicks;
}

void Ultrasonic_OnEchoEdge(void)
{
  uint16_t now;
  uint16_t width;

  if (s_htim == NULL)
  {
    return;
  }

  now = US_GetCounter();

  if (HAL_GPIO_ReadPin(ECHO_GPIO_Port, ECHO_Pin) == GPIO_PIN_SET)
  {
    if ((s_usState == US_STATE_WAIT_ECHO) && (s_usEchoRising == 0U))
    {
      s_usEchoStartCnt = now;
      s_usEchoRising = 1U;

      /* TRIG 종료 후 ECHO 상승까지 걸린 시간 계산 */
      g_trace_trig_to_echo_ticks = (uint16_t)(now - s_usTrigEndCnt);

      g_trace_echo_edge = 1U;
      g_trace_echo_state = 1U;
      g_trace_edge_count++;
    }
  }
  else if (s_usEchoRising != 0U)
  {
    width = (uint16_t)(now - s_usTrigEndCnt);
    s_usEchoRising = 0U;
    s_usEchoWidthTicks = width;
    g_trace_echo_ticks = width;
    g_trace_echo_edge = 0U;
    g_trace_echo_state = 0U;
    g_trace_edge_count++;

    if ((width >= US_MIN_ECHO_TICKS) && (width <= US_MAX_ECHO_TICKS))
    {
    	s_usEchoDone = 1U;
    }
    else
    {
    	s_usEchoDone = 2U;
    }
  }
}

/* ==========================================================================

	8. 내부 헬퍼 함수 (DHT & Ultrasonic)

========================================================================== */
static uint16_t US_GetCounter(void)
{
	return (uint16_t)__HAL_TIM_GET_COUNTER(s_htim);
}

static void US_StoreSample(uint32_t width_ticks)
{
  uint32_t width_us = width_ticks / US_TIMER_TICKS_PER_US;
  uint32_t distance_ref_mm = (width_us * US_REFERENCE_SOUND_SPEED_X10) / 20000U;
  uint32_t distance_mm = (width_us * s_usSoundSpeedX10) / 20000U;

  if ((distance_mm >= US_MIN_DISTANCE_MM) && (distance_mm <= US_MAX_DISTANCE_MM))
  {
	  s_usSamplesRef[s_usSampleIndex] = (uint16_t)distance_ref_mm;
	  s_usSamplesCorr[s_usSampleIndex] = (uint16_t)distance_mm;
	  s_usSampleIndex = (uint8_t)((s_usSampleIndex + 1U) % US_FILTER_SIZE);
    if (s_usSampleCount < US_FILTER_SIZE)
    {
    	s_usSampleCount++;
    }
  }
  else
  {
	  s_usErrorCount++;
  }
}

static uint16_t US_Median(const uint16_t *samples)
{
  uint16_t sorted[US_FILTER_SIZE];
  uint8_t i;
  uint8_t j;

  for (i = 0U; i < s_usSampleCount; i++)
  {
    sorted[i] = samples[i];
  }

  for (i = 0U; (uint8_t)(i + 1U) < s_usSampleCount; i++)
  {
    for (j = (uint8_t)(i + 1U); j < s_usSampleCount; j++)
    {
      if (sorted[j] < sorted[i])
      {
        uint16_t temp = sorted[i];
        sorted[i] = sorted[j];
        sorted[j] = temp;
      }
    }
  }

  return sorted[s_usSampleCount / 2U];
}

static void US_Publish(uint32_t now)
{
	s_usResult.timestamp_ms = now;

  if (s_usSampleCount > 0U)
  {
	  s_usResult.distance_ref_mm = US_Median(s_usSamplesRef);
	  s_usResult.distance_mm = US_Median(s_usSamplesCorr);
	  s_usResult.valid = 1U;
  }
  else
  {
	  s_usResult.distance_ref_mm = 0U;
	  s_usResult.distance_mm = 0U;
	  s_usResult.valid = 0U;
  }

  s_usResult.errors = s_usErrorCount;
  s_usResultReady = 1U;

  s_usSampleCount = 0U;
  s_usSampleIndex = 0U;
}

static void DHT_SetPinOutput(void)
{
  GPIO_InitTypeDef gpio = {0};

  gpio.Pin = DHT_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_OD;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(DHT_PORT, &gpio);
}

static void DHT_SetPinInput(void)
{
  GPIO_InitTypeDef gpio = {0};

  gpio.Pin = DHT_PIN;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(DHT_PORT, &gpio);
}

static uint32_t DHT_NowUs(void)
{
  return (uint32_t)(__HAL_TIM_GET_COUNTER(&htim1) / DHT_TIMER_TICKS_PER_US);
}

static uint32_t DHT_ElapsedUs(uint32_t start)
{
  return (uint16_t)(DHT_NowUs() - start);
}

static void DHT_DelayUs(uint32_t us)
{
  uint32_t start = DHT_NowUs();

  while (DHT_ElapsedUs(start) < us)
  {
  }
}

static uint8_t DHT_WaitLevel(GPIO_PinState level, uint32_t timeout_us, uint32_t *duration)
{
  uint32_t start = DHT_NowUs();

  while (HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN) != level)
  {
    if (DHT_ElapsedUs(start) >= timeout_us)
    {
      return 0U;
    }
  }

  if (duration != NULL)
  {
    *duration = DHT_ElapsedUs(start);
  }

  return 1U;
}

static uint8_t DHT_ReadRaw(uint8_t *data)
{
  uint32_t duration;
  uint8_t i;
  uint8_t bit;

  for (i = 0U; i < 5U; i++)
  {
    data[i] = 0U;
  }

  if (DHT_WaitLevel(GPIO_PIN_RESET, DHT_RESPONSE_TIMEOUT_US, NULL) == 0U)
  {
    return 0U;
  }
  if (DHT_WaitLevel(GPIO_PIN_SET, DHT_RESPONSE_TIMEOUT_US, NULL) == 0U)
  {
    return 0U;
  }

  for (i = 0U; i < 40U; i++)
  {
    if (DHT_WaitLevel(GPIO_PIN_RESET, DHT_BIT_TIMEOUT_US, NULL) == 0U)
    {
      return 0U;
    }
    if (DHT_WaitLevel(GPIO_PIN_SET, DHT_BIT_TIMEOUT_US, NULL) == 0U)
    {
      return 0U;
    }
    if (DHT_WaitLevel(GPIO_PIN_RESET, DHT_BIT_TIMEOUT_US, &duration) == 0U)
    {
      return 0U;
    }

    bit = (duration > DHT_ONE_THRESHOLD_US) ? 1U : 0U;
    data[i / 8U] = (uint8_t)((data[i / 8U] << 1U) | bit);
  }

  return 1U;
}

static uint8_t DHT_Decode(const uint8_t *data, int32_t *temperature_c_x10, uint32_t *humidity_rh_x10)
{
  uint32_t humidity;
  int32_t temperature;

  humidity = ((uint32_t)data[0] << 8) | data[1];
  temperature = (int32_t)(((uint16_t)(data[2] & 0x7FU) << 8) | data[3]);
  if ((data[2] & 0x80U) != 0U)
  {
    temperature = -temperature;
  }

  if ((humidity <= 1000U) && (temperature >= -400) && (temperature <= 800))
  {
    *humidity_rh_x10 = humidity;
    *temperature_c_x10 = temperature;
    return 1U;
  }

  if ((data[1] <= 9U) && (data[3] <= 9U))
  {
    humidity = ((uint32_t)data[0] * 10U) + data[1];
    temperature = ((int32_t)data[2] * 10) + (int32_t)data[3];

    if ((humidity <= 1000U) && (temperature >= 0) && (temperature <= 800))
    {
      *humidity_rh_x10 = humidity;
      *temperature_c_x10 = temperature;
      return 1U;
    }
  }

  return 0U;
}


