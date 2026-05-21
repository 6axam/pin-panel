#include <BleKeyboard.h>
#include <Preferences.h>

#define MAX_BINDS 16 
#define MAX_PROFILES 3

struct KeyBind {
  uint8_t active;     // 1 - настроена, 0 - пустая
  uint8_t type;       // 1 - клавиатура, 0 - мышь (игнорируем, но структуру сохраняем для совместимости с сайтом)
  uint16_t modifier;  // Битовая маска: 1=CTRL, 2=SHIFT, 4=ALT, 8=WIN
  uint16_t key;       // Код клавиши (keyCode)
};

KeyBind binds[MAX_BINDS];
Preferences prefs;

int currentProfile = 0; 

// --- ТВОИ ПИНЫ ---
const int rowPins[4] = {2, 3, 10, 0}; 
const int colPins[4] = {4, 5, 6, 7}; 
const int battery_pin = 1;            

bool isLearnMode = false;
bool lastBtnState[MAX_BINDS] = {false};
unsigned long lastBtnTime[MAX_BINDS] = {0}; // Спиздили из перчатки для антидребезга
unsigned long lastBatteryCheck = 0;

// Инициализируем чистую BLE клавиатуру вместо Combo
BleKeyboard bleKeyboard("StreamPad Combo", "Maxim", 100);

void checkSerial();
int getBatteryPercent();
void executeBind(KeyBind b);
void sendAllBinds();
void loadConfig(int profileId);

void setup() {
  setCpuFrequencyMhz(80);
  delay(100);
  
  Serial.begin(115200);
  
  bleKeyboard.begin();
  Serial.println("BLE: Started Pure Keyboard Device");

  // Инициализация пинов матрицы
  for (int i = 0; i < 4; i++) {
    pinMode(rowPins[i], OUTPUT);
    digitalWrite(rowPins[i], HIGH);
    pinMode(colPins[i], INPUT_PULLUP);
  }
  pinMode(battery_pin, ANALOG);

  // Читаем сохраненный номер профиля
  prefs.begin("streampad", false);
  currentProfile = prefs.getInt("cur_prof", 0);
  if(currentProfile < 0 || currentProfile >= MAX_PROFILES) currentProfile = 0;
  prefs.end();

  loadConfig(currentProfile);
  Serial.println("SYSTEM: StreamPad initialized");
}

void loop() {
  checkSerial();
  unsigned long now = millis();

  // Раз в 10 секунд меряем батку
  if (now - lastBatteryCheck > 10000) {
    lastBatteryCheck = now;
    int batteryLevel = getBatteryPercent();
    if (bleKeyboard.isConnected()) {
      bleKeyboard.setBatteryLevel(batteryLevel); 
    }
    Serial.printf("BATTERY:%d\n", batteryLevel);
  }

  // Опрос матрицы кнопок (Быстрый, БЕЗ delay)
  for (int r = 0; r < 4; r++) {
    digitalWrite(rowPins[r], LOW);
    for (int c = 0; c < 4; c++) {
      int btnId = r * 4 + c;
      bool isPressed = (digitalRead(colPins[c]) == LOW);

      if (isPressed && !lastBtnState[btnId]) {
        // Неблокирующий антидребезг (копируем логику перчатки)
        if (now - lastBtnTime[btnId] > 50) { 
          lastBtnState[btnId] = true;
          lastBtnTime[btnId] = now;
          
          Serial.printf("CLICK_B=%d\n", btnId);
          
          if (!isLearnMode && bleKeyboard.isConnected()) {
             executeBind(binds[btnId]);
          }
        }
      }
      
      if (!isPressed && lastBtnState[btnId]) {
        if (now - lastBtnTime[btnId] > 50) {
          lastBtnState[btnId] = false;
          lastBtnTime[btnId] = now;
        }
      }
    }
    digitalWrite(rowPins[r], HIGH);
  }
}

int getBatteryPercent() {
  int raw = analogRead(battery_pin);
  float voltage = (raw / 4095.0) * 3.3 * 2.0;
  int percent = map((int)(voltage * 100), 340, 420, 0, 100);
  if (percent > 100) percent = 100;
  if (percent < 0) percent = 0;
  return percent;
}

// ЭТА ФУНКЦИЯ ТЕПЕРЬ РАБОТАЕТ ИДЕАЛЬНО НА ЧИСТОМ BLE_KEYBOARD
void executeBind(KeyBind b) {
  if (b.active == 0) return;
  
  // Принудительно сбрасываем всё, что могло застрять в буфере
  bleKeyboard.releaseAll();
  delay(2);

  // Зажимаем строго то, что пришло с сайта, используя нативные коды BleKeyboard
  if (b.modifier & 0x01) bleKeyboard.press(KEY_LEFT_CTRL);
  if (b.modifier & 0x02) bleKeyboard.press(KEY_LEFT_SHIFT);
  if (b.modifier & 0x04) bleKeyboard.press(KEY_LEFT_ALT);
  if (b.modifier & 0x08) bleKeyboard.press(KEY_LEFT_GUI);
  
  // Короткая пауза, чтобы ОС поняла, что зажат модификатор
  delay(10); 

  // Преобразуем код клавиши (браузерный keyCode в ASCII/BleKeyboard код)
  uint8_t targetKey = b.key;
  
  // Фикс для букв верхнего регистра (чтобы эмулятор не слал Shift автоматически)
  if (targetKey >= 'A' && targetKey <= 'Z') {
    targetKey = targetKey + 32; // Переводим в нижний регистр 'a'-'z'
  }

  // Нажимаем основную клавишу
  bleKeyboard.press(targetKey);
  
  // Удержание для стабильного распознавания системой
  delay(50); 
  
  // Полностью отпускаем комбинацию
  bleKeyboard.releaseAll();
}

void checkSerial() {
  if (Serial.available() > 0) {
    String in = Serial.readStringUntil('\n');
    in.trim();
    if (in.length() == 0) return;
    
    if (in == "LEARN_ON")   { isLearnMode = true;  Serial.println("OK:LEARN_ON"); return; } 
    if (in == "LEARN_OFF")  { isLearnMode = false; Serial.println("OK:LEARN_OFF"); return; } 
    if (in == "GET_BINDS")  { sendAllBinds(); return; } 
    if (in == "PING_PAD")   { Serial.println("PONG_PAD"); return; }

    if (in.startsWith("SET_PROFILE=")) {
      int targetProf = in.substring(12).toInt();
      if(targetProf >= 0 && targetProf < MAX_PROFILES) {
        currentProfile = targetProf;
        prefs.begin("streampad", false);
        prefs.putInt("cur_prof", currentProfile);
        prefs.end();
        loadConfig(currentProfile);
        sendAllBinds();
      }
      return;
    }

    if (in.startsWith("SET_B=")) {
      String data = in.substring(6);
      int s1 = data.indexOf(',');
      int s2 = data.indexOf(',', s1 + 1);
      int s3 = data.indexOf(',', s2 + 1);
      if(s1 != -1 && s2 != -1 && s3 != -1) {
        int slot = data.substring(0, s1).toInt();
        if (slot >= 0 && slot < MAX_BINDS) {
          binds[slot].active = 1;
          binds[slot].type = data.substring(s1 + 1, s2).toInt();
          binds[slot].modifier = data.substring(s2 + 1, s3).toInt();
          binds[slot].key = data.substring(s3 + 1).toInt();

          prefs.begin("streampad", false);
          String k = "p" + String(currentProfile) + "_b" + String(slot);
          prefs.putBytes(k.c_str(), &binds[slot], sizeof(KeyBind));
          prefs.end();
          Serial.println("SAVE_OK");
        }
      }
      return;
    } 
    else if (in.startsWith("DEL_B=")) {
      int slot = in.substring(6).toInt();
      if (slot >= 0 && slot < MAX_BINDS) {
        binds[slot] = {0, 0, 0, 0};
        prefs.begin("streampad", false);
        String k = "p" + String(currentProfile) + "_b" + String(slot);
        if (prefs.isKey(k.c_str())) prefs.remove(k.c_str());
        prefs.end();
        Serial.printf("DEL_OK=%d\n", slot);
      }
      return;
    }
  }
}

void sendAllBinds() { 
  Serial.println("BINDS_START");
  Serial.printf("CURRENT_PROFILE=%d\n", currentProfile); 
  Serial.println("CURRENT_NAME:StreamPad OS"); 
  for (int i = 0; i < MAX_BINDS; i++) {
    Serial.printf("BIND=%d,%d,%d,%d,%d\n", i, binds[i].active, binds[i].type, binds[i].modifier, binds[i].key);
  }
  Serial.println("BINDS_END");
}

void loadConfig(int profileId) {
  prefs.begin("streampad", false);
  for (int i = 0; i < MAX_BINDS; i++) {
    String k = "p" + String(profileId) + "_b" + String(i);
    if (prefs.isKey(k.c_str())) {
      prefs.getBytes(k.c_str(), &binds[i], sizeof(KeyBind));
    } else {
      binds[i] = {0, 0, 0, 0};
    }
  }
  prefs.end();
}