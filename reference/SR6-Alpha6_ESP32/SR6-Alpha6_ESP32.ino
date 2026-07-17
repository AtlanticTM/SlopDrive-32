// OSR-Alpha6_ESP32
// by TempestMAx 1-6-26
//
// This code is designed to drive the SR6 and OSR2 stroker robots.
// Have fun, play safe!
// History:
// Alpha3 - First ESP32 release, 9-7-21
// Alpha4 - Support for T-wist 4 added - standard servo now default, parallax is an option, 5-3-22
// Alpha5 - Fix for ESP32 LED library problems - Button support added 2-9-24
// Alpha6 - Experimental build with TCode v0.4 compatibility. No button or preference save support. 1-6-22
//
//

// ----------------------------
//   User Settings
// ----------------------------

// Device IDs, for external reference
#define TCODE_DEVICE_INFO "SR6-Alpha6-ESP32.ino"  // Device and firmware version

#define OSR2_MODE false // (true/false) Switch servo outputs to OSR2 mode

// Type of servo being used (160, 180, 270-degree)
#define ServoMaxRange 180  // Servo full range of movement (degrees)

// Pin assignments
// T-wist feedback goes on digital pin 2
#define LowerLeftServo_PIN 15    // Lower Left Servo (OSR2 Left Servo)
#define UpperLeftServo_PIN 2     // Upper Left Servo
#define LowerRightServo_PIN 13   // Lower Right Servo (OSR2 Right Servo)
#define UpperRightServo_PIN 12   // Upper Right Servo
#define LeftPitchServo_PIN 4     // Left Pitch Servo (OSR2 Pitch Servo)
#define RightPitchServo_PIN 14   // Right Pitch Servo
#define TwistServo_PIN 27        // Twist Servo
#define ValveServo_PIN 25        // Valve Servo
#define TwistFeedback_PIN 26     // Twist Servo Feedback
#define Vibe0_PIN 18             // Vibration motor 1
#define Vibe1_PIN 19             // Vibration motor 2

// Arm servo zeros
// Change these to adjust arm positions
// (1500 = centre, 1 complete step = 160)
#define LowerLeftServo_ZERO 1500         // Lower Left Servo (OSR2 Left Servo)
#define UpperLeftServo_ZERO 1500         // Upper Left Servo 
#define LowerRightServo_ZERO 1500        // Lower Right Servo (OSR2 Right Servo)
#define UpperRightServo_ZERO 1500        // Upper Right Servo 
#define LeftPitchServo_ZERO 1500         // Left Pitch Servo (OSR2 Pitch Servo)
#define RightPitchServo_ZERO 1500        // Right Pitch Servo
#define TwistServo_ZERO 1500
#define ValveServo_ZERO 1500

// Servo operating frequencies
#define PitchServo_Freq 330 // Pitch Servos
#define MainServo_Freq 330  // Main Servos
#define TwistServo_Freq 50  // Twist Servo
#define ValveServo_Freq 50  // Valve Servo
#define VibePWM_Freq 8000   // Vibe motor control PWM frequency

// Buttons
#define USE_BUTTONS true      // Use a Pre-Play-Next-Edge resistor ladder
#define Buttons_PIN 35        // Buttons on pin D35
#define USE_EDGEBUTTON false  // Use a digital edge button
#define EdgeButton_PIN 34     // Buttons on pin D34

// Other functions
#define TWIST_PARALLAX false      // (true/false) Parallax 360 feedback servo on twist (t-wist3)
#define REVERSE_TWIST_SERVO false // (true/false) Reverse twist servo direction 
#define VALVE_DEFAULT 5000        // Auto-valve default suction level (low-high, 0-9999) 
#define REVERSE_VALVE_SERVO true  // (true/false) Reverse T-Valve direction
#define VIBE_TIMEOUT 2000         // Timeout for vibration channels (milliseconds).
#define LUBE_V1 false             // (true/false) Lube pump installed instead of vibration channel 1
#define Lube_PIN 23               // Lube manual input button pin (Connect pin to +5V for ON)
#define Lube_SPEED 255            // Lube pump speed (0-255)
#define MIN_SMOOTH_INTERVAL 3     // Minimum auto-smooth ramp interval for live commands (ms)
#define MAX_SMOOTH_INTERVAL 100   // Maximum auto-smooth ramp interval for live commands (ms)


// ----------------------------
//  Auto Settings
// ----------------------------
// Do not change 

// Servo Pulse intervals
#define MainServo_Int 1000000/MainServo_Freq
#define PitchServo_Int 1000000/PitchServo_Freq
#define TwistServo_Int 1000000/TwistServo_Freq
#define ValveServo_Int 1000000/ValveServo_Freq

// Servo microseconds per radian
#define ms_per_rad 114592/ServoMaxRange  // (μs/rad)

// Libraries used
#include <EEPROM.h>       // Permanent memory
#include "TCode.h"        // Tempest's TCode Library


// ----------------------------
//   SETUP
// ----------------------------
// This code runs once, on startup

// Declare classes
TCode tcode;
// Declare device axes
Axis stroke(5000);
Axis surge(5000);
Axis sway(5000);
Axis twist(5000);
Axis roll(5000);
Axis pitch(5000);
Axis vibration0(0);
Axis vibration1(0);
Axis valve(5000);
Axis suck(VALVE_DEFAULT);
Axis lube(0);

// Declare operating variables
// Position variables
int xLin,yLin,zLin;
// Rotation variables
int xRot,yRot,zRot;
// Vibration variables
int vibe0,vibe1;
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

  // Set SR6 arms to startup positions
  if (!OSR2_MODE) { tcode.stringInput("R2750"); }

  // #ESP32# Enable EEPROM
  EEPROM.begin(320);

  // Register device axes
  tcode.addAxis("L0", stroke);
  if (!OSR2_MODE) {
    tcode.addAxis("L1", surge);
    tcode.addAxis("L2", sway);
  }
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

  // Setup Servo PWM channels
  // Lower Left Servo
  ledcAttach(LowerLeftServo_PIN,MainServo_Freq,16);
  // Upper Left Servo
  ledcAttach(UpperLeftServo_PIN,MainServo_Freq,16);
  // Lower Right Servo
  ledcAttach(LowerRightServo_PIN,MainServo_Freq,16);
  // Upper Right Servo
  ledcAttach(UpperRightServo_PIN,MainServo_Freq,16);
  // Left Pitch Servo
  ledcAttach(LeftPitchServo_PIN,PitchServo_Freq,16);
  // Right Pitch Servo
  ledcAttach(RightPitchServo_PIN,PitchServo_Freq,16);
  // Twist Servo
  ledcAttach(TwistServo_PIN,TwistServo_Freq,16);
  // Valve Servo
  ledcAttach(ValveServo_PIN,ValveServo_Freq,16);

  // Set vibration PWM pins
  // Vibe0 Pin
  ledcAttach(Vibe0_PIN,VibePWM_Freq,8);
  // Vibe1 Pin
  ledcAttach(Vibe1_PIN,VibePWM_Freq,8);

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
  if (!OSR2_MODE) {
    yLin = surge.getPosition();
    zLin = sway.getPosition();
  }
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

  // OSR2 Kinematics
  if (OSR2_MODE) {
    // Calculate arm angles
    // Linear scale inputs to servo appropriate numbers
    int strokeCmd,rollCmd,pitchCmd;
    strokeCmd = map(xLin,0,9999,-350,350);
    rollCmd   = map(yRot,0,9999,-180,180);
    pitchCmd  = map(zRot,0,9999,-350,350);
    ledcWrite(LowerLeftServo_PIN, map(LowerLeftServo_ZERO + strokeCmd + rollCmd,0,MainServo_Int,0,65535));
    ledcWrite(LowerRightServo_PIN, map(LowerRightServo_ZERO - strokeCmd + rollCmd,0,MainServo_Int,0,65535));
    ledcWrite(LeftPitchServo_PIN, map(LeftPitchServo_ZERO - pitchCmd,0,PitchServo_Int,0,65535));
    // Unused servo pins.
    ledcWrite(UpperLeftServo_PIN, map(UpperLeftServo_ZERO,0,MainServo_Int,0,65535));
    ledcWrite(RightPitchServo_PIN, map(RightPitchServo_ZERO,0,PitchServo_Int,0,65535));
    ledcWrite(UpperRightServo_PIN, map(UpperRightServo_ZERO,0,MainServo_Int,0,65535));
  }
  
  // SR6 Kinematics
  else {
    // Calculate arm angles
    int rollCmd,pitchCmd,fwdCmd,thrustCmd,sideCmd;
    int out1,out2,out3,out4,out5,out6;
    rollCmd = map(yRot,0,9999,-3000,3000);
    pitchCmd = map(zRot,0,9999,-2500,2500);
    fwdCmd = map(yLin,0,9999,-3000,3000);
    thrustCmd = map(xLin,0,9999,-6000,6000);
    sideCmd = map(zLin,0,9999,-3000,3000);
    // Main arms
    out1 = SetMainServo(16248 - fwdCmd, 1500 + thrustCmd + rollCmd); // Lower left servo
    out2 = SetMainServo(16248 - fwdCmd, 1500 - thrustCmd - rollCmd); // Upper left servo
    out5 = SetMainServo(16248 - fwdCmd, 1500 - thrustCmd + rollCmd); // Upper right servo
    out6 = SetMainServo(16248 - fwdCmd, 1500 + thrustCmd - rollCmd); // Lower right servo
    // Pitchers
    out3 = SetPitchServo(16248 - fwdCmd, 4500 - thrustCmd,  sideCmd - 1.5*rollCmd, -pitchCmd);
    out4 = SetPitchServo(16248 - fwdCmd, 4500 - thrustCmd, -sideCmd + 1.5*rollCmd, -pitchCmd);
    // Set Servos
    ledcWrite(LowerLeftServo_PIN, map(LowerLeftServo_ZERO - out1,0,MainServo_Int,0,65535));
    ledcWrite(UpperLeftServo_PIN, map(UpperLeftServo_ZERO + out2,0,MainServo_Int,0,65535));
    ledcWrite(LeftPitchServo_PIN, map(constrain(LeftPitchServo_ZERO - out3,LeftPitchServo_ZERO-600,LeftPitchServo_ZERO+1000),0,PitchServo_Int,0,65535));
    ledcWrite(RightPitchServo_PIN, map(constrain(RightPitchServo_ZERO + out4,RightPitchServo_ZERO-1000,RightPitchServo_ZERO+600),0,PitchServo_Int,0,65535));
    ledcWrite(UpperRightServo_PIN, map(UpperRightServo_ZERO - out5,0,MainServo_Int,0,65535));
    ledcWrite(LowerRightServo_PIN, map(LowerRightServo_ZERO + out6,0,MainServo_Int,0,65535));
  }

  // Twist and valve
  int twistCmd,valveCmd;
  twistCmd  = map(xRot,0,9999,1000,-1000);
  if (REVERSE_TWIST_SERVO) { twistCmd = -twistCmd; }
  valveCmd  = valvePos - 500;
  valveCmd  = constrain(valveCmd, -500, 500);
  if (REVERSE_VALVE_SERVO) { valveCmd = -valveCmd; }
  // Set Servos
  ledcWrite(TwistServo_PIN, map(TwistServo_ZERO + twistCmd,0,TwistServo_Int,0,65535));
  ledcWrite(ValveServo_PIN, map(ValveServo_ZERO + valveCmd,0,ValveServo_Int,0,65535));

  // Done with servo channels


  // Output vibration channels
  // These should drive PWM pins connected to vibration motors via MOSFETs or H-bridges.
  if (vibe0 > 0 && vibe0 <= 9999) {
    ledcWrite(Vibe0_PIN, map(vibe0,1,9999,31,255));
  } else {
    ledcWrite(Vibe0_PIN, 0);
  }
  if (!LUBE_V1 && vibe1 > 0 && vibe1 <= 9999) {
    ledcWrite(Vibe1_PIN, map(vibe1,1,9999,31,255));
  } else {
    ledcWrite(Vibe1_PIN, 0);
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
      ledcWrite(Vibe1_PIN, map(lubeCmd,1,9999,127,255));
    } else if (digitalRead(Lube_PIN) == HIGH) {
      ledcWrite(Vibe1_PIN,Lube_SPEED);
    } else { 
      ledcWrite(Vibe1_PIN,0);
    }
    if (millis() - lube.getLast() > 500) {   // Auto cutoff
      lube.prepAxis(0,InputType::INTERVAL,100);
      lube. setAxis();
    }
  }
  // Done with lube

}


// Function to calculate the angle for the main arm servos
// Inputs are target x,y coords of receiver pivot in 1/100 of a mm
int SetMainServo(float x, float y) {
  x /= 100; y /= 100;          // Convert to mm
  float gamma = atan2(x,y);    // Angle of line from servo pivot to receiver pivot
  float csq = sq(x) + sq(y);   // Square of distance between servo pivot and receiver pivot
  float c = sqrt(csq);         // Distance between servo pivot and receiver pivot
  float beta = acos((csq - 28125)/(100*c));  // Angle between c-line and servo arm
  int out = ms_per_rad*(gamma + beta - 3.14159); // Servo signal output, from neutral
  return out;
}


// Function to calculate the angle for the pitcher arm servos
// Inputs are target x,y,z coords of receiver upper pivot in 1/100 of a mm
// Also pitch in 1/100 of a degree
int SetPitchServo(float x, float y, float z, float pitch) {
  pitch *= 0.0001745; // Convert to radians
  x += 5500*sin(0.2618 + pitch);
  y -= 5500*cos(0.2618 + pitch);
  x /= 100; y /= 100; z /= 100;   // Convert to mm
  float bsq = 36250 - sq(75 + z); // Equivalent arm length
  float gamma = atan2(x,y);       // Angle of line from servo pivot to receiver pivot
  float csq = sq(x) + sq(y);      // Square of distance between servo pivot and receiver pivot
  float c = sqrt(csq);            // Distance between servo pivot and receiver pivot
  float beta = acos((csq + 5625 - bsq)/(150*c)); // Angle between c-line and servo arm
  int out = ms_per_rad*(gamma + beta - 3.14159); // Servo signal output, from neutral
  return out;
}
