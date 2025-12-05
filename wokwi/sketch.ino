#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- KONFIGURASI PIN (SESUAI GAMBAR WOKWI) ---
#define SENSOR_PIN 7      // Potensiometer
#define LED_PIN 16        // LED
#define BUZZER_PIN 17     // Buzzer
#define BUTTON_PIN 10     // Tombol (Ground ke Pin 10)

Adafruit_SSD1306 display(128, 64, &Wire, -1);

// --- RTOS RESOURCES ---
// KITA BUTUH 2 QUEUE AGAR TIDAK REBUTAN DATA
QueueHandle_t displayQueue;        
QueueHandle_t alarmQueue;
          
SemaphoreHandle_t oledMutex;       
SemaphoreHandle_t emergencySemaphore; 

const int threshold = 2000; // Batas bahaya gas

// --- 1. INTERRUPT SERVICE ROUTINE (ISR) ---
void IRAM_ATTR emergencyISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  // Beri sinyal ke TaskAlarm bahwa tombol ditekan
  xSemaphoreGiveFromISR(emergencySemaphore, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// --- 2. TASK SENSOR (PRODUCER) ---
void TaskSensor(void *pvParameters) {
  while (1) {
    int gasValue = analogRead(SENSOR_PIN);
    
    // PERBAIKAN: Kirim data ke DUA antrian sekaligus (Broadcast)
    xQueueSend(displayQueue, &gasValue, portMAX_DELAY);
    xQueueSend(alarmQueue, &gasValue, portMAX_DELAY);

    // Serial monitor opsional (bisa dikomentari agar tidak spam)
    // Serial.printf("[Sensor] Gas: %d\n", gasValue);

    vTaskDelay(pdMS_TO_TICKS(500));  // Baca setiap 500ms
  }
}

// --- 3. TASK DISPLAY (CONSUMER 1) ---
void TaskDisplay(void *pvParameters) {
  int gasValue;
  while (1) {
    // Ambil data khusus untuk display
    if (xQueueReceive(displayQueue, &gasValue, portMAX_DELAY)) {
      
      // Amankan OLED dengan Mutex
      if (xSemaphoreTake(oledMutex, portMAX_DELAY)) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 10);
        display.println("GAS DETECTOR");
        display.setCursor(0, 30);
        display.print("Value: ");
        display.println(gasValue);
        
        display.setCursor(0, 50);
        if(gasValue > threshold) display.print("STATUS: DANGER!");
        else display.print("STATUS: SAFE");

        display.display();
        xSemaphoreGive(oledMutex); // Lepas Mutex
      }
    }
  }
}

// --- 4. TASK ALARM (CONSUMER 2 - LOGIKA UTAMA) ---
void TaskAlarm(void *pvParameters) {
  int gasValue;
  bool emergencyActive = false; // False = Normal, True = Alarm Dimatikan (Muted)
  unsigned long lastDebounceTime = 0; // Timer untuk anti-mantul tombol

  while (1) {
    // A. CEK TOMBOL (DENGAN DEBOUNCE)
    // Gunakan timeout 0 agar tidak memblokir proses pembacaan sensor
    if (xSemaphoreTake(emergencySemaphore, 0) == pdTRUE) {
      unsigned long now = millis();
      // Hanya terima input jika jarak antar penekanan > 200ms
      if (now - lastDebounceTime > 200) { 
        emergencyActive = !emergencyActive; // Ubah status (Toggle)
        lastDebounceTime = now;
        
        Serial.print("[BUTTON] Status Emergency Mute: ");
        Serial.println(emergencyActive ? "AKTIF (Alarm Mati)" : "NON-AKTIF (Alarm Nyala)");
      }
    }

    // B. LOGIKA KONTROL ALARM
    // Jika Mode Mute AKTIF, Paksa mati semua output
    if (emergencyActive) {
      digitalWrite(LED_PIN, LOW);
      digitalWrite(BUZZER_PIN, LOW);
      
      // Tetap harus mengosongkan queue agar tidak menumpuk, meski datanya dibuang
      xQueueReceive(alarmQueue, &gasValue, 0); 
    } 
    // Jika Mode Normal (Tidak di-Mute)
    else {
      // Cek apakah ada data baru di Queue Alarm
      if (xQueueReceive(alarmQueue, &gasValue, 10)) {
        if (gasValue > threshold) {
          // 1. Nyalakan Hardware
          digitalWrite(LED_PIN, HIGH);
          digitalWrite(BUZZER_PIN, HIGH);

          // 2. AKSES OLED (Disini Mutex Berfungsi!)
          // TaskAlarm minta izin kunci Mutex untuk nulis peringatan
          if (xSemaphoreTake(oledMutex, portMAX_DELAY)) {
             display.clearDisplay(); // Hapus tampilan angka biasa
             display.setTextSize(2); // Tulisan Besar
             display.setCursor(10, 20);
             display.println("BAHAYA!!");
             display.setTextSize(1);
             display.setCursor(10, 45);
             display.print("GAS: ");
             display.println(gasValue);
             display.display();
             
             // Kembalikan Kunci Mutex
             xSemaphoreGive(oledMutex);
          } }
          else {
          digitalWrite(LED_PIN, LOW);
          digitalWrite(BUZZER_PIN, LOW);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100)); // Delay kecil untuk stabilitas task
  }
}


void setup() {
  Serial.begin(115200);
  Wire.begin(5, 4); // SDA Pin 5, SCL Pin 4 (Sesuai gambar Wokwi ESP32-S3)

  pinMode(SENSOR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT); // Wajib INPUT_PULLUP karena kaki lain ke Ground

  // Pasang Interrupt
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), emergencyISR, FALLING);

  // Inisialisasi OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED Error!");
    for (;;);
  }
  display.clearDisplay();
  display.display();

  // --- INISIALISASI RTOS (PENTING: 2 QUEUE) ---
  displayQueue = xQueueCreate(10, sizeof(int));
  alarmQueue = xQueueCreate(10, sizeof(int)); // Queue terpisah untuk alarm
  
  oledMutex = xSemaphoreCreateMutex();
  emergencySemaphore = xSemaphoreCreateBinary();

  // Buat Task
  xTaskCreatePinnedToCore(TaskSensor, "TaskSensor", 2048, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(TaskDisplay, "TaskDisplay", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskAlarm, "TaskAlarm", 2048, NULL, 2, NULL, 1);
}

void loop() {
  // Biarkan kosong
}