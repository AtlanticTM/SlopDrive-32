// OSR-Release v4.0
// by TempestMAx 1-6-26
//
// This code is designed to drive the OSR2 stroker robot, but is also intended to be
// used as a template to be adapted to run other t-code controlled arduino projects
// Have fun, play safe!
// History:
// v4.0 - TCode v0.4 compatible, no buttons, no T-wist3 support, 1-6-2026
//
//
// MIT License
//
// Copyright (c) 2021 Richard Unger
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.


// ----------------------------
//  User Settings
// ----------------------------
// These are the setup parameters for an OSR2 on a Romeo BLE mini v2

// Device IDs, for external reference
#define TCODE_DEVICE_INFO "OSR2-Arduino Release v4.0"  // Device and firmware version

// Pin assignments
// T-wist feedback goes on digital pin 2
#define LeftServo_PIN 8    // Left Servo (change to 7 for Romeo v1.1)
#define RightServo_PIN 3   // Right Servo (change to 4 for Romeo v1.1)
#define PitchServo_PIN 9   // Pitch Servo (change to 8 for Romeo v1.1)
#define TwistServo_PIN 10  // Twist Servo
#define ValveServo_PIN 12  // Valve Servo
#define Vibe0_PIN 5        // Vibration motor 1
#define Vibe1_PIN 6        // Vibration motor 2

// Arm servo zeros
// Change these to adjust servo centre positions
// (1500 = centre, 1 complete step = 160)
#define LeftServo_ZERO 1500   // Right Servo
#define RightServo_ZERO 1500  // Left Servo
#define PitchServo_ZERO 1500  // Pitch Servo
#define TwistServo_ZERO 1500  // Twist Servo
#define ValveServo_ZERO 1500  // Valve Servo

// Other functions
#define REVERSE_TWIST_SERVO false // (true/false) Reverse twist servo direction 
#define VALVE_DEFAULT 5000        // Auto-valve default suction level (low-high, 0-9999) 
#define REVERSE_VALVE_SERVO true  // (true/false) Reverse T-Valve direction
#define VIBE_TIMEOUT 2000         // Timeout for vibration channels (milliseconds).
#define LUBE_V1 false             // (true/false) Lube pump installed instead of vibration channel 1
#define Lube_PIN 13               // Lube manual input button pin (Connect pin to +5V for ON)
#define Lube_SPEED 255            // Lube pump speed (0-255)
#define MIN_SMOOTH_INTERVAL 3     // Minimum auto-smooth ramp interval for live commands (ms)
#define MAX_SMOOTH_INTERVAL 100   // Maximum auto-smooth ramp interval for live commands (ms)

// Libraries used
#include <Servo.h>     // Standard Arduino servo library
#include "TCode.h"     // Tempest's TCode library
#include "Axis.h"      // Axis extension to Tempest's TCode library

// ----------------------------
//   SETUP
// ----------------------------
// This code runs once, on startup

// Declare classes
// TCode handler
TCode tcode;
// Declare device axes
Axis stroke(5000);
Axis twist(5000);
Axis roll(5000);
Axis pitch(5000);
Axis vibration0(0);
Axis vibration1(0);
Axis valve(5000);
Axis suck(VALVE_DEFAULT);
Axis lube(0);


// Declare servos
Servo LeftServo;
Servo RightServo;
Servo PitchServo;
Servo TwistServo;
Servo ValveServo;

// Declare operating variables
// Position variables
int xLin;
// Rotation variables
int xRot,yRot,zRot;
// Vibration variables
int vibe0,vibe0set,vibe1,vibe1set;
// Lube variables
int lubeCmd;
// Valve variables
int valveCmd,suckCmd;
// Velocity tracker variables, for valve
int xLast;
unsigned long tLast;
int32_t strokeVel;
int valvePos;

// Setup function
// This is run once, when the arduino starts
void setup() {

  // Start serial connection and report status
  Serial.begin(115200);
  tcode.stringInput("D0\n");
  while (tcode.available() > 0) { char c = tcode.read(); Serial.write(c); }
  tcode.stringInput("D1\n");
  while (tcode.available() > 0) { char c = tcode.read(); Serial.write(c); }

  // Register device axes
  tcode.addAxis("L0", stroke);
  tcode.addAxis("R0", twist);
  tcode.addAxis("R1", roll);
  tcode.addAxis("R2", pitch);
  tcode.addAxis("V0", vibration0);
  if (!LUBE_V1) { tcode.addAxis("V1", vibration1); }
  tcode.addAxis("A0", valve);
  tcode.addAxis("A1", suck);
  if (LUBE_V1) {
    tcode.addAxis("A2", lube);
    pinMode(Lube_PIN,INPUT);
  }

  // Attach servos
  LeftServo.attach(LeftServo_PIN);
  RightServo.attach(RightServo_PIN);
  PitchServo.attach(PitchServo_PIN);
  TwistServo.attach(TwistServo_PIN);
  ValveServo.attach(ValveServo_PIN);

  // Set vibration PWM pins
  pinMode(Vibe0_PIN,OUTPUT);
  vibe0set = 0;
  pinMode(Vibe1_PIN,OUTPUT);
  vibe1set = 0;
  
  // Signal done
  Serial.println("Ready!");

}


// ----------------------------
//   MAIN
// ----------------------------
// This loop runs continuously
void loop() {

  // Read serial and send to tcode class
  while (Serial.available() > 0) {
    // Send the serial bytes to the t-code object
    tcode.byteInput(Serial.read());
  }

  // Read tcode buffer and send to serial
  while (tcode.available() > 0) {
    char c = tcode.read();
    Serial.write(c);
  }

  // Collect inputs
  // These functions query the t-code object for the position/level at a specified time
  // Number recieved will be an integer, 0-9999
  xLin = stroke.getPosition();
  xRot = twist.getPosition();
  yRot = roll.getPosition();
  zRot = pitch.getPosition();
  vibe0 = vibration0.getPosition();
  if (!LUBE_V1) { vibe1 = vibration1.getPosition(); }
  valveCmd = valve.getPosition();
  suckCmd = suck.getPosition();
  if (LUBE_V1) { lubeCmd = lube.getPosition(); }

  // If you want to mix your servos differently, enter your code below:

  // Calculate valve position
  // Use axis most recently received
  boolean suckMode;
  if (suck.getLast() >= valve.getLast()) {
    suckMode = true;
    valveCmd = suckCmd;
  } else {
    suckMode = false;
  }
  // if on suck mode calculate valve position
  if (suckMode) {
    // Get receiver velocity
    strokeVel = stroke.getVelocity();
    if (strokeVel < -5) {
      valveCmd = 0;  
    } else if ( strokeVel < 0 ) {
      valveCmd = map(100*strokeVel,0,-500,suckCmd,0);
      if (valveCmd > 9999) { valveCmd = 9999; }
      if (valveCmd < 0) { valveCmd = 0; }
    }
  }
  valvePos = (9*valvePos + map(valveCmd,0,9999,0,1000))/10;

  // Mix and send servo channels
  // Linear scale inputs to servo appropriate numbers
  int stroke,roll,pitch,valve,twist;
  stroke = map(xLin,0,9999,-350,350);
  roll   = map(yRot,0,9999,-180,180);
  pitch  = map(zRot,0,9999,-350,350);
  twist  = map(xRot,0,9999,1000,-1000);
  if (REVERSE_TWIST_SERVO) { twist = -twist; }
  valve  = valvePos -500;
  valve  = constrain(valve, -500, 500);
  if (REVERSE_VALVE_SERVO) { valve = -valve; }

  // Set servo output values
  // Note: 1000 = -45deg, 2000 = +45deg
  LeftServo.writeMicroseconds(LeftServo_ZERO + stroke + roll);
  RightServo.writeMicroseconds(RightServo_ZERO - stroke + roll);
  PitchServo.writeMicroseconds(PitchServo_ZERO - pitch);
  TwistServo.writeMicroseconds(TwistServo_ZERO + twist);
  ValveServo.writeMicroseconds(ValveServo_ZERO + valve);
  // Done with servo channels


  // Output vibration channels
  // These should drive PWM pins connected to vibration motors via MOSFETs or H-bridges.
  // Vibe 0 channel
  if (vibe0 <= 0) {
    vibe0set = 0;
  } else if (vibe0set == 0 && vibe0 > 0) {
    vibe0set = 5000;
  } else if (vibe0set > vibe0 + 10) {
    vibe0set -= 10;
  } else {
    vibe0set = vibe0;
  }
  if (vibe0set > 0 && vibe0set <= 9999) {
    analogWrite(Vibe0_PIN,map(vibe0set,1,9999,63,255));
  } else {
    analogWrite(Vibe0_PIN,0);
  }
  // Vibe 1 channel
  if (vibe1 <= 0) {
    vibe1set = 0;
  } else if (vibe1set == 0 && vibe1 > 0) {
    vibe1set = 5000;
  } else if (vibe1set > vibe1 + 10) {
    vibe1set -= 10;
  } else {
    vibe1set = vibe1;
  }
  if (!LUBE_V1 && vibe1 > 0 && vibe1 <= 9999) {
    analogWrite(Vibe1_PIN,map(vibe1,1,9999,63,255));
  } else {
    analogWrite(Vibe1_PIN,0);
  }
  // Vibe timeout functions - shuts the vibe channels down if not commanded for a specified interval
  if (millis() - vibration0.getLast() > VIBE_TIMEOUT) {
    vibration0.prepAxis(0,InputType::INTERVAL,500);
    vibration0.setAxis();
  }
  if (!LUBE_V1 && millis() - vibration1.getLast() > VIBE_TIMEOUT) {
    vibration1.prepAxis(0,InputType::INTERVAL,500);
    vibration1.setAxis();
  }  
  // Done with vibration channels

  // Lube functions
  if (LUBE_V1) {
    if (lubeCmd > 0 && lubeCmd <= 9999) {
      analogWrite(Vibe1_PIN,map(lubeCmd,1,9999,127,255));
    } else if (digitalRead(Lube_PIN) == HIGH) {
      analogWrite(Vibe1_PIN,Lube_SPEED);
    } else { 
      analogWrite(Vibe1_PIN,0);
    }
    if (millis() - lube.getLast() > 500) {   // Auto cutoff
      lube.prepAxis(0,InputType::INTERVAL,100);
      lube. setAxis();
    }
  }

  // Done with lube


}