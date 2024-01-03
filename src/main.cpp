#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoQueue.h>
#include "SoftwareSerial.h"
#include "DFRobotDFPlayerMini.h"
#include <EEPROM.h>

typedef enum Modes {off, on, fade_off, fade_on, stop_next, ready, fade, blink, none, force, alwayson} Modes;

#define BUTTON_PIN D1
#define BUTTON_OFF_TIME 1400
#define TASK_BUTTON_CHECK_INTERVAL 10

#define LED_PIN D6
#define LED_PWM_MAX 300
#define LED_PWM_MIN 0

#define STRIP_PIN D5
#define STRIP_LEN 26
#define TASK_STRIP_FADE_INTERVAL 10
#define STRIP_FADE_MAX_DURATION 10000

#define RELAY_PIN D2
#define TASK_RELAY_INTERVAL 30

#define TASK_PLAYER_INTERVAL 30
#define TASK_INIT_TIME 20000

//##################################################################################

void player_pause();

//##################################################################################

uint8_t led_blink_state = HIGH;
uint8_t led_blink_on_flag = false;
uint8_t led_blink_stop_next_flag = false;
uint16_t task_led_blink_interval = 50;
uint32_t task_last_led_blink = 0;

uint8_t led_owner = fade;

uint16_t led_fade_steps_per_task = 10;
uint16_t led_fade_value = LED_PWM_MAX;
uint8_t led_fade_dir = true;
uint8_t led_fade_on_flag = false;
uint8_t led_fade_fade_off_flag = false;
uint8_t led_fade_fade_on_flag = false;
uint8_t led_fade_stop_next_flag = false;
uint32_t task_last_led_fade = 0;
uint16_t task_led_fade_interval = 50;

//##################################################################################

Adafruit_NeoPixel strip(STRIP_LEN, STRIP_PIN, NEO_GRB + NEO_KHZ800);
ArduinoQueue<uint8_t> strip_fade_precalc_q_r(STRIP_FADE_MAX_DURATION/TASK_STRIP_FADE_INTERVAL);
ArduinoQueue<uint8_t> strip_fade_precalc_q_b(STRIP_FADE_MAX_DURATION/TASK_STRIP_FADE_INTERVAL);
ArduinoQueue<uint8_t> strip_fade_precalc_q_g(STRIP_FADE_MAX_DURATION/TASK_STRIP_FADE_INTERVAL);
uint8_t strip_fade_enable_flag = false;
uint32_t task_last_strip_fade = 0;
uint8_t strip_fade_current_r = 0;
uint8_t strip_fade_current_b = 0;
uint8_t strip_fade_current_g = 0;

//##################################################################################

uint32_t current_time = 0;
uint32_t s_end_time = 0;
uint32_t task_last_button_check = 0;
uint32_t task_last_conflict_resolver = 0;
uint16_t task_conflict_resolver_interval = 100;

uint8_t button_state = HIGH;
uint8_t button_pressed_flag = false;
uint32_t button_last_pressed_time = 0;

//##################################################################################

uint32_t task_last_relay = 0;
uint8_t relay_state = LOW;
uint8_t relay_change_flag = false;

//##################################################################################

SoftwareSerial mySoftwareSerial(D7, D0); // RX, TX
DFRobotDFPlayerMini player;
void printDetail(uint8_t type, int value);
uint32_t task_last_player = 0;
uint8_t player_current_song = 0;
uint8_t player_volume = 3;
uint8_t player_play_flag = false;
uint8_t player_song_finished_flag = false;
uint8_t player_change_flag = false;
uint8_t player_unconfirmed_action_flag = false;
uint8_t init_done_flag = false;

//##################################################################################

int32_t round_double(double x) {
  int32_t res = 0;
  if(x>0.0) {
    x = x + 0.5;
    res = (int)x;
  }
  else if(x<0.0) {
    x = x - 0.5;
    res = (int)x;
  }
  return res;
}

//##################################################################################

void task_button_check() {
  current_time = millis();
  if(current_time - button_last_pressed_time > BUTTON_OFF_TIME) {
    button_state = digitalRead(BUTTON_PIN);
    if (button_state == LOW) {
      button_pressed_flag = true;
      button_last_pressed_time = millis();
    }
  }
}

void task_led_fade() {
  if(led_fade_on_flag && (led_owner == none || led_owner == fade)) {
    if(led_owner == none) {led_owner = fade;}
    if(led_fade_dir) {
      if(led_fade_value + led_fade_steps_per_task > LED_PWM_MAX) {
        led_fade_value = LED_PWM_MAX;
        led_fade_dir = false;
        led_blink_state = HIGH;
        if(led_fade_fade_off_flag || led_fade_stop_next_flag) {
          led_fade_on_flag = false;
          led_fade_fade_off_flag = false;
          led_fade_stop_next_flag = false;
          led_owner = none;
        }
      }
      else {
        led_fade_value += led_fade_steps_per_task;
      }
    }
    else {
      if(led_fade_value - led_fade_steps_per_task < LED_PWM_MIN) {
        led_fade_value = LED_PWM_MIN;
        led_fade_dir = true;
        led_blink_state = LOW;
        if(led_fade_fade_on_flag || led_fade_stop_next_flag) {
          led_fade_on_flag = false;
          led_fade_fade_on_flag = false;
          led_fade_stop_next_flag = false;
          led_owner = none;
        }
      }
      else {
        led_fade_value -= led_fade_steps_per_task;
      }
    }
    analogWrite(LED_PIN, led_fade_value);
    //Serial.print("Fade value: ");
    //Serial.print(led_fade_value);
  }
}

void task_led_blink() {
  if(led_blink_on_flag && (led_owner == none || led_owner == blink)) {
    if(led_owner == none) {led_owner = blink;}
    if(led_blink_stop_next_flag) {
      led_blink_on_flag = false;
      led_blink_stop_next_flag = false;
      led_owner = none;
    }
    else {
      led_blink_state = !led_blink_state;
      digitalWrite(LED_PIN, led_blink_state);
    }
  }
}

void task_strip_fade() {
  if(strip_fade_enable_flag) {
    if(!strip_fade_precalc_q_r.isEmpty()) {
      strip_fade_current_r = strip_fade_precalc_q_r.dequeue();
    }
    if(!strip_fade_precalc_q_b.isEmpty()) {
      strip_fade_current_b = strip_fade_precalc_q_b.dequeue();
    }
    if(!strip_fade_precalc_q_g.isEmpty()) {
      strip_fade_current_g = strip_fade_precalc_q_g.dequeue();
    }
    else if(strip_fade_precalc_q_r.isEmpty() && strip_fade_precalc_q_b.isEmpty() && strip_fade_precalc_q_g.isEmpty()) {
      strip_fade_enable_flag = false;
      strip.show();
      return;
    }
    strip.fill(strip.Color(strip_fade_current_r, strip_fade_current_b, strip_fade_current_g));
    strip.show();
  }
}

void task_relay() {
  if(relay_change_flag) {
    relay_change_flag = false;
    digitalWrite(RELAY_PIN, relay_state);
  }
}

void task_player() {
  if(player_change_flag) {
    player_change_flag = false;
    if(player_play_flag) {
      player.play(player_current_song);
      player_unconfirmed_action_flag = true;
    }
    else {
      player.pause();
      delay(1);
      player.pause();
      delay(1);
      player.pause();
    }
  } 
  // else if(player_unconfirmed_action_flag) {
  //   uint16_t player_state = 513;//player.readState();
  //   Serial.println(player_state);
  //   if(player_state == 512) {
  //     player_unconfirmed_action_flag = false;
  //   }
  //   else if(player_state == 513) {
  //     // player.pause();
  //   }
  //   else {
  //     // player.pause();
  //   }
  // }
  // else {
  //   if(player.readType()) {
  //     player_song_finished_flag = true;
  //   }
  // }
}

void task_init() {
  // player_pause();
  // player_pause();
  // player.volume(20);
}

//##################################################################################

void task_scheduler(uint32_t s_time) {
  current_time = millis();
  s_end_time = current_time + s_time;

  while(current_time <= s_end_time && !button_pressed_flag) {
    current_time = millis();

    if(current_time - task_last_button_check > TASK_BUTTON_CHECK_INTERVAL) {
      task_last_button_check = current_time;
      task_button_check();
      yield();
    }

    if(current_time - task_last_led_fade > task_led_fade_interval) {
      task_last_led_fade = current_time;
      task_led_fade();
      yield();
    }

    if(current_time - task_last_led_blink > task_led_blink_interval) {
      task_last_led_blink = current_time; 
      task_led_blink();
      yield();
    }

    if(current_time - task_last_strip_fade > TASK_STRIP_FADE_INTERVAL) {
      task_last_strip_fade = current_time;
      task_strip_fade();
      yield();
    }

    if(current_time - task_last_relay > TASK_RELAY_INTERVAL) {
      task_last_relay = current_time;
      task_relay();
      yield();
    }

    if(current_time - task_last_player > TASK_PLAYER_INTERVAL) {
      task_last_player = current_time;
      task_player();
      yield();
    }

    if(current_time > TASK_INIT_TIME && !init_done_flag) {
      task_init();
      init_done_flag = true;
      yield();
    }

    delay(1);
    yield();
  }

}

//##################################################################################

void led_fade(uint16_t t, uint16_t task_interval, uint8_t mode) {
  // t = halbe periodendauer in ms
  // Modes: off, on, fade_off, fade_on, stop_next
  task_led_fade_interval = task_interval;
  led_fade_steps_per_task = ((LED_PWM_MAX-LED_PWM_MIN) * task_led_fade_interval) / t;
  switch(mode) {
    case off: 
      led_fade_on_flag = false;
      led_fade_value = LED_PWM_MAX;
      led_blink_state = led_fade_value;
      digitalWrite(LED_PIN, led_blink_state);
      led_owner = none;
      break;
    case on: led_fade_on_flag = true; break;
    case fade_off: led_fade_on_flag = true; led_fade_fade_off_flag = true; break;
    case fade_on: led_fade_on_flag = true; led_fade_fade_on_flag = true; break;
    case stop_next: led_fade_on_flag = true; led_fade_stop_next_flag = true; break;
    default: led_fade_on_flag = false; led_fade_value = LED_PWM_MAX; led_owner = none; break;
  }
}

void led_blink(uint16_t t, uint8_t mode) {
  // t = halbe periodendauer in ms
  // Modes: off, on, stop_next
  task_led_blink_interval = t;
  switch(mode) {
    case off: led_blink_on_flag = false; led_owner = none; break;
    case on: led_blink_on_flag = true; break;
    case stop_next: led_blink_on_flag = true; led_blink_stop_next_flag = true; break;
    case alwayson: led_blink_on_flag = false; led_blink_state=LOW; digitalWrite(LED_PIN, led_blink_state); break;
    default: led_blink_on_flag = false; led_owner = none; break;
  }
}

void strip_fade(uint16_t d, uint8_t r_start, uint8_t b_start, uint8_t g_start, uint8_t r_end, uint8_t b_end, uint8_t g_end) {
  // d = dauer
  if(d<TASK_STRIP_FADE_INTERVAL) {d=TASK_STRIP_FADE_INTERVAL;}
  uint16_t intervals = round_double(d/TASK_STRIP_FADE_INTERVAL);
  double m_r = ((double)(r_end - r_start)) / ((double)d/TASK_STRIP_FADE_INTERVAL);
  double m_b = ((double)(b_end - b_start)) / ((double)d/TASK_STRIP_FADE_INTERVAL);
  double m_g = ((double)(g_end - g_start)) / ((double)d/TASK_STRIP_FADE_INTERVAL);

  uint16_t queue_size = strip_fade_precalc_q_r.itemCount() > strip_fade_precalc_q_b.itemCount() ? strip_fade_precalc_q_r.itemCount() : strip_fade_precalc_q_b.itemCount();
  queue_size = strip_fade_precalc_q_g.itemCount() > queue_size ? strip_fade_precalc_q_g.itemCount() : queue_size;
  for(int i=0; i<queue_size; i++) {
    strip_fade_precalc_q_r.dequeue();
    strip_fade_precalc_q_b.dequeue();
    strip_fade_precalc_q_g.dequeue();
  }
  for(int i=0; i<intervals; i++) {
    strip_fade_precalc_q_r.enqueue((uint8_t)round_double(m_r*(i)) + r_start);
    strip_fade_precalc_q_b.enqueue((uint8_t)round_double(m_b*(i)) + b_start);
    strip_fade_precalc_q_g.enqueue((uint8_t)round_double(m_g*(i)) + g_start);
  }
  strip_fade_precalc_q_r.enqueue((uint8_t)r_end);
  strip_fade_precalc_q_b.enqueue((uint8_t)b_end);
  strip_fade_precalc_q_g.enqueue((uint8_t)g_end);
  strip_fade_enable_flag = true;
}

void relay(uint8_t set) {
  relay_change_flag = true;
  relay_state = !set;
}

void player_play(uint8_t song) {
  player_current_song = song;
  player_play_flag = true;
  player_change_flag = true;
}

void player_pause() {
  player_play_flag = false;
  player_change_flag = true;
}

//##################################################################################

void strip_off() {
  strip_fade(0,0,0,0,0,0,0);
  strip_fade(0,0,0,0,0,0,0);
  strip.clear();
}

void wait_for_button() {
  while(!button_pressed_flag) {
    task_scheduler(0); // ein durchlauf des schedulers -> button_check
  }
  button_pressed_flag = false;
}

void s_delay(uint16_t d) {
  task_scheduler(d);
}

void button_hook() {
  button_pressed_flag = false;
}

//##################################################################################

void test_disko() {
  uint16_t takt = 100;
  s_delay(1500);
  led_blink(150, on);
  for(int i=0; i<20; i++) {
    s_delay(takt);
    strip_fade(takt, 0,0,0, 255,0,255);
    s_delay(takt);
    strip_fade(takt, 255,0,255, 0,255,0);
    s_delay(takt);
  }
  strip_off();
  player_pause();
  led_blink(150, off);
}

void scooter_nessaja() {
  player.volume(14);
  // songdauer ca = 50200 
  relay(true);
  s_delay(1000);
  player_play(1);
  
  s_delay(1000);
  strip_fade(1800, 0,0,0, 100,0,180);
  s_delay(1800);
  led_blink(150, on);
  uint16_t takt = 150;
  for(int i=0; i<43; i++) {
    s_delay(takt);
    strip_fade(0,0,0,0,0,0,255);
    s_delay(takt);
    strip_fade(0, 0,0,0, 140,0,255);
  }
  strip_fade(0,0,0,0, 50,30,20); // AHHH
  s_delay(2700); 
  takt = 500;
  for(int i=0; i<3; i++) {// You ain't stoppin' us now!
    s_delay(takt);
    strip_fade(takt-100,0,0,0, 0,255,190);
    s_delay(takt);
    strip_fade(takt, 0,255,190, 0,0,0);
  }
  s_delay(200);
  strip_fade(0, 0,0,0, 0,0,0);
  s_delay(450);
  for(int i=0; i<4; i++) { // NA NA NA NA
    strip_fade(0,0,0,0, 0,255,128);
    strip_fade(500, 0,255,128, 0,0,0);
    s_delay(500);
  }

  takt = 450;
  for(int i=0; i<4; i++) { // I am the Junglist souljah.
    s_delay(takt);
    strip_fade(takt-100,0,0,0, 60,10,0);
    s_delay(takt);
    strip_fade(takt, 60,10,0, 0,0,0);
  }
  s_delay(takt*2);
  strip_fade(0, 0,0,0, 0,0,0);
  s_delay(200);

  for(int i=0; i<4; i++) { // NA NA NA NA
    strip_fade(0,0,0,0, 0,255,128);
    strip_fade(500, 0,255,128, 0,0,0);
    s_delay(500);
  }

  takt = 450;
  for(int i=0; i<4; i++) { // The rocket launcher stopped ya!
    s_delay(takt);
    strip_fade(takt-100,0,0,0, 60,10,0);
    s_delay(takt);
    strip_fade(takt, 60,10,0, 0,0,0);
  }
  s_delay(takt*2);
  strip_fade(0, 0,0,0, 0,0,0);
  s_delay(300);

  for(int i=0; i<7; i++) { // It's not a bird, it's not a plane ...
    strip_fade(0,0,0,0, 90,255,128);
    strip_fade(480, 90,255,128, 0,0,0);
    s_delay(480);
  }
  for(int i=0; i<6; i++) { // It must be Dave who's on the train...
    strip_fade(0,0,0,0, 40,255,0);
    strip_fade(480, 40,255,0, 0,0,0);
    s_delay(480);
  }
  s_delay(200);
  takt = 230;
  for(int i=0; i<57; i++) { // Wanna wanna get'cha, gonna gonna get'cha!
    strip_fade(takt, 90,255,128, 0,0,0);
    s_delay(takt);
    takt -= 4;
  }
  strip_fade(0, 0,0,0, 0,0,0);
  s_delay(250);

  strip_fade(4000, 20,40,255, 0,0,0);
  s_delay(4000);

  strip_fade(0, 0,0,0, 0,0,0);
  player_pause();
  led_blink(150, off);
  relay(false);
}

void romantic_song() {
  player.volume(14);
  led_blink(1, alwayson);
  strip_fade(0, 0,0,0, 255,255,255);
  s_delay(600);
  relay(true);
  s_delay(300);
  player_play(2);
  s_delay(2500);
  strip_fade(5000, 255,255,255, 255,0,0);
  s_delay(5000);
  uint16_t takt = 1500;
  for(int i=0; i<20; i++) {
    s_delay(takt);
    strip_fade(takt, 255,0,0, 40,0,0);
    s_delay(takt);
    strip_fade(takt, 40,0,0, 255,0,0);
  }

  s_delay(3000);
  strip_fade(0, 0,0,0, 0,0,0);
  player_pause();
  led_blink(1, off);
  relay(false);
}

void farts() {
  player.volume(13);
  led_blink(750, on);
  // strip_fade(300, 0,0,0, 0,255,0);
  strip_fade(0, 0,0,0, 0,0,0);
  s_delay(200);
  // relay(true);
  strip_fade(300, 0,0,0, 0,255,0);
  player_play(3);
  s_delay(500);

  for(int i=0; i<10; i++) {
    for(int j=0; j<4; j++) {
      // strip_fade(100, 0,255,0, 0,0,0);
      strip_fade(100, 0,255,0, 0,0,0);
      s_delay(350);
      // strip_fade(100, 0,0,0, 0,255,0);
      strip_fade(100, 0,0,0, 0,255,0);
      s_delay(650);
    }
    s_delay(5000);
  }
  // relay(false);
  s_delay(200);
  strip_fade(0, 0,0,0, 0,0,0);
  player_pause();
  led_blink(1, off);
}

//##################################################################################

void setup() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(STRIP_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  EEPROM.begin(24);
  uint8_t lock = false;
  // uint16_t on_counter = 0;
  // EEPROM.get(0x8,on_counter);
  EEPROM.get(0,lock);
  led_blink_state = lock;
  digitalWrite(LED_PIN, led_blink_state);

  uint16_t button_counter = 0;
  do {
    delay(1);
    button_state = digitalRead(BUTTON_PIN);
    button_counter ++;
    if(button_counter > 2500) {
      lock = !lock;
      led_blink_state = lock;
      digitalWrite(LED_PIN, led_blink_state);
      break;
    }
  } while(!button_state);

  EEPROM.put(0,lock);
  // EEPROM.put(0x8,on_counter);
  EEPROM.commit();

  while(lock) {
    delay(100);
  };

  do {
    delay(1);
    button_state = digitalRead(BUTTON_PIN);
  } while(!button_state);

  delay(25);

  strip.begin();           // INITIALIZE NeoPixel strip object (REQUIRED)
  strip.setBrightness(80); // Set BRIGHTNESS to about 1/5 (max = 255)
  
  strip.clear();
  strip.show();

  mySoftwareSerial.begin(9600);
  !player.begin(mySoftwareSerial);
  s_delay(20);
  player.volume(0);  //Set volume value. From 0 to 30
  player.EQ(DFPLAYER_EQ_BASS);
  s_delay(20);
  // player_play(4);
  // Serial.begin(115200);
}

void loop() {
  led_fade(1700, 20, on);
  wait_for_button();
  led_fade(1, 20, off);
  scooter_nessaja();
  button_hook();
  player_pause();
  relay(false);
  strip_off();
  led_blink(150, off);
  
  led_fade(1700, 20, on);
  wait_for_button();
  led_fade(1, 20, off);
  romantic_song();
  button_hook();
  player_pause();
  relay(false);
  strip_off();
  led_blink(150, off);

    
  led_fade(1700, 20, on);
  wait_for_button();
  led_fade(1, 20, off);
  farts();
  button_hook();
  player_pause();
  relay(false);
  strip_off();
  led_blink(150, off);
}