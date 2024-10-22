#include <Arduino.h>

#include "print3x5.h"

#define PART_AMOUNT 58 // общее число песчинок на поле
#define BTN1_PIN 2
#define BTN2_PIN 3
#define BTN3_PIN 8
#define CS_PIN 6
#define DT_PIN 4
#define CK_PIN 5
#define SOUND_PIN 9

#define MaxVolume 1 // максимальный уровень громкости эффектов
#define MaxMelody 6 // число имеющихся мелодий (для корректной работы меню)

#define battery_min 3000 // минимальный уровень заряда батареи для отображения 3000
#define battery_max 3800 // максимальный уровень заряда батареи для отображения  3800

// Структура конфигурации для хранения в EEPROM
struct Data // TODO: BUG NOT READING THIS VALUES AT ALL!
{
  int16_t sec = 60; // время
  int8_t bri = 1;   // яркость
  int8_t vol = 1;   // громкость эффектов
  int8_t mel = 3;   // мелодия окончания времени
  int8_t ani = 0;   // анимации окончания времени
  int8_t tmpo = 4;  // темп проигрывания мелодий
  int8_t bat = 1;
  int16_t standby_seconds = 5;
};
Data data;

#include <EEManager.h>
EEManager memory(data);

// button
#include <EncButton.h>
Button up(BTN1_PIN);
Button down(BTN2_PIN);
Button menu(BTN3_PIN);
VirtButton dbl;

// matrix
#include <GyverMAX7219.h>
MAX7219<2, 1, CS_PIN, DT_PIN, CK_PIN> mtrx;

// mpu
#include "mini6050.h"
Mini6050 mpu;

// sandbox
#define BOX_W 16
#define BOX_H 16

#include "sand.h"
Sand<BOX_W, BOX_H> box;

// timer
#include "Timer.h"
Timer fall_tmr, disp_tmr, menu_tmr;

uint8_t inMenu = 0;
uint8_t lastUsedMenu = 0;   // последнее использованное меню
unsigned long voltage;      // напряжение аккумулятора
float my_vcc_const = 1.080; // константа вольтметра

bool isDisplayOn = 1;
bool isTimerRunning = false;
unsigned long lastStandbyTimestamp = 0;
bool isStandbyTimerOn = 0;
int16_t defaultStandbyWatchInSeconds = 10;

// ============== ВАШ КОД ==============
// функция вызывается при каждом "проталкивании" песчинки
void onSandPush()
{
#ifdef SOUND_PIN
  StopMelody(); // чтобы мелодия не накладывалась на звук падающего песка
  if (data.vol > 0)
    PlaySandPushTone();
#endif
}

// todo
//    сложность в том, что событие onSandEnd должно срабатывать именно тогда, когда весь песок пересыпался
//    с учётом возможных пауз из-за наклонов
//    возможно, тут получится сделать таймер, который будет отсчитывать настроенные секунды, делать паузу когда нужно и автоматом обнуляться, когда весь песок пересыпется
//    либо нужно отследить число частиц на втором экране
// функция вызывается при завершении времени отведённого времени
void onSandEnd()
{
  if (isAllSandFallen())
  {
    
    showTime();
    isTimerRunning = false;
    startStandbyWatch();
  }
}

// функция вызывается, когда песок перестал сыпаться
// это происходит в том числе из-за крена часов, когда ещё не весь песок пересыпался
void onSandStop()
{
#ifdef SOUND_PIN
  if (data.mel > 0)
    PlayMelody(data.mel);
#endif
}

// функция вызывается, когда заряд батареи ниже порога
void onBatteryEmpty()
{
  mtrx.setBright(0);
  while (true)
  {
    mtrx.clear();
    printIcon(&mtrx, 0, 0, 6);
    mtrx.update();
    delay(1000);
    mtrx.clear();
    printIcon(&mtrx, 0, 0, 4);
    mtrx.update();
    delay(1000);
  }
}

bool runFlag = 1;
bool checkBound(int8_t x, int8_t y)
{
  if (y >= 8 && x < 8)
    return 0;
  if (y < 8 && x >= 8)
    return 0;
  if (mpu.getDir() > 0)
  {
    if (x == 8 && y == 8)
      return 0;
  }
  else
  {
    if (x == 7 && y == 7)
      return 0;
  }
  return (x >= 0 && y >= 0 && x < BOX_W && y < BOX_H);
}
void setXY(int8_t x, int8_t y, bool value)
{
  if (y >= 8)
    y -= 8;
  mtrx.dot(x, y, value);
}

void resetSand()
{
  isTimerRunning = true;
  Serial.println("Starting timer.");

  box.buf.clear();
  mtrx.clear();
  mtrx.update();
  for (uint8_t n = 0; n < PART_AMOUNT; n++)
  {
    box.buf.set(n % 8, n / 8, 1);
  }

  stopStandbyWatch();

  showTime();
}

void showTime()
{
  mtrx.clear();

  if (data.sec < 0)
  {
    data.sec = 0;
  }

  uint8_t min = data.sec / 60;
  uint8_t sec = data.sec % 60;

  printDig(&mtrx, 0, 1, min / 10);
  printDig(&mtrx, 4, 1, min % 10);
  printDig(&mtrx, 8 + 0, 1, sec / 10);
  printDig(&mtrx, 8 + 4, 1, sec % 10);

  mtrx.update();

  disp_tmr.setTimeout(3000);
}

void changeTime(int8_t dir)
{
  disp_tmr.setTimeout(3000);
  mtrx.clear();
  data.sec += dir;
  if (data.sec < 0)
    data.sec = 0;
  uint8_t min = data.sec / 60;
  uint8_t sec = data.sec % 60;

  printDig(&mtrx, 0, 1, min / 10);
  printDig(&mtrx, 4, 1, min % 10);
  printDig(&mtrx, 8 + 0, 1, sec / 10);
  printDig(&mtrx, 8 + 4, 1, sec % 10);

  fall_tmr.setInterval(data.sec * 1000ul / PART_AMOUNT);
  memory.update();
  mtrx.update();
}

// изменение темпа
void changeTempo(int8_t dir)
{
  data.tmpo += dir;
  data.tmpo = constrain(data.tmpo, 1, 7);
  SetTempo(data.tmpo);
  PlayMelody(data.mel);
  memory.update();
}

// изменение анимации
void changeAni(int8_t dir)
{
  data.ani += dir;
  data.ani = constrain(data.ani, 0, 1);
  memory.update();
}

// изменение яркости
void changeBri(int8_t dir)
{
  data.bri += dir;
  data.bri = constrain(data.bri, 0, 15);
  mtrx.setBright(data.bri);
  memory.update();
}

// изменение громкости
void changeVol(int8_t dir)
{
  data.vol += dir;
  data.vol = constrain(data.vol, 0, MaxVolume);
  memory.update();
}

// изменение мелодии
void changeMel(int8_t dir)
{
  data.mel += dir;
  data.mel = constrain(data.mel, 0, MaxMelody);
  if (data.mel)
    PlayMelody(data.mel);
  else
    StopMelody();
  memory.update();
}

// изменение стиля отображения заряда
void changeBat(int8_t dir)
{
  data.bat += dir;
  data.bat = constrain(data.bat, 0, 2);
  memory.update();
}

void enterMenu(int8_t menu)
{
  disp_tmr.setTimeout(returnFromMenu, 5000);
  mtrx.clear();

  // здесь настраивается число доступных пунктов меню
  if (menu > 5)
    menu = 1;
  if (menu < 1)
    menu = 5;

  inMenu = menu;
  menu_tmr.setTimeout(forgetLastMenu, 30000);

  showMenu(menu);
}

void forgetLastMenu()
{
  lastUsedMenu = 0;
}

void showMenu(int8_t menu)
{
  // выводим иконку меню на первый экран
  // и текущее значение параметра на втворой
  switch (menu)
  {
  case 1: // яркость
    printIcon(&mtrx, 0, 0, 2);
    printDig(&mtrx, 8 + 0, 1, data.bri / 10);
    printDig(&mtrx, 8 + 4, 1, data.bri % 10);
    break;

  case 2: // громкость звуков
    printIcon(&mtrx, 0, 0, 0);
    printDig(&mtrx, 8 + 0, 1, data.vol / 10);
    printDig(&mtrx, 8 + 4, 1, data.vol % 10);
    break;

  case 3: // мелодия окончания
    printIcon(&mtrx, 0, 0, 1);
    printDig(&mtrx, 8 + 0, 1, data.mel / 10);
    printDig(&mtrx, 8 + 4, 1, data.mel % 10);
    break;

  case 4: // темп мелодий
    printIcon(&mtrx, 0, 0, 5);
    printDig(&mtrx, 8 + 0, 1, data.tmpo / 10);
    printDig(&mtrx, 8 + 4, 1, data.tmpo % 10);
    break;

  case 5: // стиль отображения заряда при включении
    printIcon(&mtrx, 0, 0, 6);
    printDig(&mtrx, 8 + 0, 1, data.bat / 10);
    printDig(&mtrx, 8 + 4, 1, data.bat % 10);
    break;

  case 6: // анимации
    printIcon(&mtrx, 0, 0, 3);
    printDig(&mtrx, 8 + 0, 1, data.ani / 10);
    printDig(&mtrx, 8 + 4, 1, data.ani % 10);
    break;
  }
  mtrx.update();
}

// Отображение уровня заряда в процентах или вольтах
void showBattLevel(long voltage)
{
  int8_t level = 0;
  switch (data.bat)
  {
  case 1:
    level = map(voltage, battery_min, battery_max, 0, 99);
    level = constrain(level, 0, 99);
    mtrx.clear();
    printIcon(&mtrx, 0, 0, 4);
    printDig(&mtrx, 8 + 0, 1, level / 10);
    printDig(&mtrx, 8 + 4, 1, level % 10);
    mtrx.update();
    break;
  case 2:
    mtrx.clear();
    printDig(&mtrx, 0, 1, voltage / 1000);
    printDig(&mtrx, 4, 1, (voltage % 1000) / 100);
    printDig(&mtrx, 8 + 0, 1, (voltage % 100) / 10);
    printDig(&mtrx, 8 + 4, 1, (voltage % 100) % 10);
    mtrx.update();
    delay(2000);
    mtrx.clear();
    break;
  }
}

void changeMenuParam(int8_t menu, int8_t val)
{
  disp_tmr.setTimeout(returnFromMenu, 5000); // обновляем таймер
  mtrx.clear();                              // очищаем экран

  switch (menu)
  {
  case 1:           // яркость
    changeBri(val); // сохраняем новое значение яркости
    break;
  case 2: // громкость звуков
    changeVol(val);
    break;
  case 3: // мелодия окончания
    changeMel(val);
    break;

  case 4:             // темп мелодий
    changeTempo(val); // сохраняем новое значение темпа
    break;

  case 5: // отображение заряда
    changeBat(val);
    break;

  case 6: // анимация окончания
    changeAni(val);
    // todo
    // Варианты:
    // - мигание песка (вкл/выкл)
    // - плавное мигание песка (игра с яркостью)
    break;
  }
  showMenu(menu);
}

// колбэк для выхода из меню по таймауту
void returnFromMenu()
{
  lastUsedMenu = inMenu; // запоминаем последнее использованное
  inMenu = 0;
}

// обработчик кнопок
void buttons()
{
  up.tick();
  down.tick();
  menu.tick();
  // dbl.tick(up, down);

  if (!inMenu)
  {
    if (menu.hold())
      enterMenu(lastUsedMenu ? lastUsedMenu : 1);
    if (menu.click())
      isMelodyPlaying() ? StopMelody() : resetSand();

    // останавливаем проигрывание мелодии при нажатии на любую кнопку
    if (up.click())
      isMelodyPlaying() ? StopMelody() : changeTime(1);
    if (up.step())
      changeTime(10);
    if (up.hold())
      changeTime(10);

    // останавливаем проигрывание мелодии при нажатии на любую кнопку
    if (down.click())
      isMelodyPlaying() ? StopMelody() : changeTime(-1);
    if (down.step())
      changeTime(-10);
    if (down.hold())
      changeTime(-10);
  }
  else
  {
    // if (menu.hold()) returnFromMenu();
    if (menu.click())
      enterMenu(inMenu + 1); // if (down.hold() && down.click()) enterMenu(inMenu - 1);
    if (up.click())
      changeMenuParam(inMenu, 1);
    if (down.click())
      changeMenuParam(inMenu, -1);
  }
}

void step()
{
  uint16_t prd = 255 - mpu.getMag();
  prd = constrain(prd, 15, 90);
  if (mpu.update(prd))
  {
    mtrx.clear();
    box.step(mpu.getAngle() + 45);
    mtrx.update();
  }
}

void fall()
{
  if (fall_tmr)
  {
    bool pushed = 0;
    if (mpu.getDir() > 0)
    {
      if (box.buf.get(7, 7) && !box.buf.get(8, 8))
      {
        box.buf.set(7, 7, 0);
        box.buf.set(8, 8, 1);
        box.setCallback(7, 7, 0);
        box.setCallback(8, 8, 1);
        pushed = 1;
      }
    }
    else
    {
      if (box.buf.get(8, 8) && !box.buf.get(7, 7))
      {
        box.buf.set(8, 8, 0);
        box.buf.set(7, 7, 1);
        box.setCallback(8, 8, 0);
        box.setCallback(7, 7, 1);
        pushed = 1;
      }
    }

    if (pushed)
    {
      runFlag = 0;
      mtrx.update();
      onSandPush();
    }
    else
    {
      if (!runFlag)
      {
        runFlag = 1;
        onSandStop();
        onSandEnd();
      }
    }
  }
}

bool isAllSandFallen()
{
  for (uint8_t y = 0; y < BOX_H; y++)
  {
    for (uint8_t x = 0; x < BOX_W; x++)
    {
      if (box.buf.get(x, y))
      {
        // If there are still particles in the upper part of the box, sand hasn't finished falling
        if (y < 8)
        {
          return false;
        }
      }
    }
  }

  Serial.println("All particles are in the lower part.");

  return true;
}

void startStandbyWatch()
{
  if (!isStandbyTimerOn)
  {
    lastStandbyTimestamp = millis();
    isStandbyTimerOn = true;
    Serial.println("Set last standby timestamp: " + String(lastStandbyTimestamp) + " Standby timer set to: " + String(isStandbyTimerOn) + ".");
  }
}

void stopStandbyWatch()
{
  if (isStandbyTimerOn)
  {
    lastStandbyTimestamp = 0;
    isStandbyTimerOn = false;
    Serial.println("Set last standby timestamp: " + String(lastStandbyTimestamp) + " Standby timer set to: " + String(isStandbyTimerOn) + ".");
  }
}

void watchStandby()
{
  if (isStandbyTimerOn && (millis() - lastStandbyTimestamp >= defaultStandbyWatchInSeconds * 1000))
  {
    Serial.println("Stepping in standby watch passed: " + String(defaultStandbyWatchInSeconds) + " seconds.");
    stopStandbyWatch();
    setDisplayOff();
  }

  if (!mpu.isStable())
  {
    if (!isTimerRunning)
    {
      startStandbyWatch();
    }

    setDisplayOn();
  }
}

void setDisplayOn()
{
  if (!isDisplayOn)
  {
    Serial.println("Display on.");
    mtrx.clear();
    mtrx.setPower(true);
    isDisplayOn = true;
  }
}

void setDisplayOff()
{
  if (isDisplayOn)
  {
    Serial.println("Display off.");
    mtrx.clear();
    mtrx.setPower(false);
    isDisplayOn = false;
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.print("Setup");

  Wire.begin();
  mpu.begin();
  memory.begin(0, 'a');
  mtrx.begin();
  mtrx.setBright(data.bri);

  mpu.setX({1, -1});
  mpu.setY({2, 1});
  mpu.setZ({0, 1});

  box.attachBound(checkBound);
  box.attachSet(setXY);

#ifdef SOUND_PIN
  pinMode(SOUND_PIN, OUTPUT);
  SetTempo(data.tmpo);
#endif

  voltage = readVcc(); // считать напряжение питания
  // Serial.println(voltage);
  if (voltage <= battery_min)
    onBatteryEmpty();
  showBattLevel(voltage);
  delay(2000); // время отображения заряда после включения

  resetSand();
  fall_tmr.setInterval(data.sec * 1000ul / PART_AMOUNT);
}

void loop()
{
  memory.tick();
  disp_tmr.tick();
#ifdef SOUND_PIN
  soundsTick();
#endif
  buttons();
  watchStandby();

  if (!disp_tmr.state())
  {
    step();
    fall();
  }
}

unsigned long readVcc()
{ // функция чтения внутреннего опорного напряжения, универсальная (для всех ардуин)
#if defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
  ADMUX = _BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
#elif defined(__AVR_ATtiny24__) || defined(__AVR_ATtiny44__) || defined(__AVR_ATtiny84__)
  ADMUX = _BV(MUX5) | _BV(MUX0);
#elif defined(__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__)
  ADMUX = _BV(MUX3) | _BV(MUX2);
#else
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
#endif
  delay(2);            // Wait for Vref to settle
  ADCSRA |= _BV(ADSC); // Start conversion
  while (bit_is_set(ADCSRA, ADSC))
    ;                  // measuring
  uint8_t low = ADCL;  // must read ADCL first - it then locks ADCH
  uint8_t high = ADCH; // unlocks both
  long result = (high << 8) | low;
  result = my_vcc_const * 1023 * 1000 / result; // расчёт реального VCC
  return result;                                // возвращает VCC
}
