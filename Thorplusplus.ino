// I2C device class (I2Cdev) demonstration Arduino sketch for MPU6050 class using DMP (MotionApps v2.0)
// 6/21/2012 by Jeff Rowberg <jeff@rowberg.net>
// Updates should (hopefully) always be available at https://github.com/jrowberg/i2cdevlib
//
// Changelog:
//      2019-07-08 - Added Auto Calibration and offset generator
//       - and altered FIFO retrieval sequence to avoid using blocking code
//      2016-04-18 - Eliminated a potential infinite loop
//      2013-05-08 - added seamless Fastwire support
//                 - added note about gyro calibration
//      2012-06-21 - added note about Arduino 1.0.1 + Leonardo compatibility error
//      2012-06-20 - improved FIFO overflow handling and simplified read process
//      2012-06-19 - completely rearranged DMP initialization code and simplification
//      2012-06-13 - pull gyro and accel data from FIFO packet instead of reading directly
//      2012-06-09 - fix broken FIFO read sequence and change interrupt detection to RISING
//      2012-06-05 - add gravity-compensated initial reference frame acceleration output
//                 - add 3D math helper file to DMP6 example sketch
//                 - add Euler output and Yaw/Pitch/Roll output formats
//      2012-06-04 - remove accel offset clearing for better results (thanks Sungon Lee)
//      2012-06-01 - fixed gyro sensitivity to be 2000 deg/sec instead of 250
//      2012-05-30 - basic DMP initialization working

/* ============================================
I2Cdev device library code is placed under the MIT license
Copyright (c) 2012 Jeff Rowberg
Copyright (c) 2021 Vipin M

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
===============================================
*/

#include "PID_v1.h"
#include "I2Cdev.h"
#include "MPU6050_6Axis_MotionApps20.h"
#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
#include <Wire.h>
#endif
#include <SoftwareSerial.h>

// MPU
#define GYRO_OFFSET_X 33
#define GYRO_OFFSET_Y 12
#define GYRO_OFFSET_Z 30
#define ACCEL_OFFSET_X -2721
#define ACCEL_OFFSET_Y -1645
#define ACCEL_OFFSET_Z 1171

MPU6050 mpu;
uint8_t mpu_int_status; // holds actual interrupt status byte from MPU
uint16_t packet_size; // expected DMP packet size (default is 42 bytes)
uint16_t fifo_count; // count of all bytes currently in FIFO
uint8_t fifo_buffer[64]; // FIFO storage buffer
Quaternion q; // [w, x, y, z] quaternion container
VectorFloat gravity; // [x, y, z] gravity vector
float ypr[3]; // [yaw, pitch, roll] yaw/pitch/roll container and gravity vector
volatile bool mpu_interrupt = false; // indicates whether MPU interrupt pin has gone high

// PID
#define SET_POINT 0.95

double setpoint = SET_POINT;
double input, output;
double Kp = 90;
double Kd = 3.5;
double Ki = 600;
PID pid(&input, &output, &setpoint, Kp, Ki, Kd, DIRECT);

// Motor
#define MOTOR_SPEED_MAX 255
#define MOTOR_SPEED_MIN -255
#define MOTOR_DEAD_BAND_THRESH 70
#define ENA 10
#define IN1 11
#define IN2 12

int cur_speed;

// Bluetooth
SoftwareSerial BT(8, 9); // RX, TX

void dmpDataReady() {
  mpu_interrupt = true;
}

void motor_move(int speed) {
    int direction = 1;
    
    if (speed < 0) {
        direction = -1;
        speed = min(speed, -MOTOR_DEAD_BAND_THRESH);
        speed = max(speed, MOTOR_SPEED_MIN);
    } else {
        speed = max(speed, MOTOR_DEAD_BAND_THRESH);
        speed = min(speed, MOTOR_SPEED_MAX);
    }
    
    if (speed == cur_speed) return;
    
    int real_speed = max(MOTOR_DEAD_BAND_THRESH, abs(speed));
    
    digitalWrite(IN1, speed > 0 ? HIGH : LOW);
    digitalWrite(IN2, speed > 0 ? LOW : HIGH);
    analogWrite(ENA, real_speed);
    
    cur_speed = direction * real_speed;
}

void motor_init(void) {
    pinMode(ENA, OUTPUT);
    pinMode(IN1, OUTPUT);
    pinMode(IN2, OUTPUT);
}

uint8_t gyroaccel_init(void) {
#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
    Wire.begin();
    TWBR = 24; // 400kHz I2C clock (200kHz if CPU is 8MHz)
#elif I2CDEV_IMPLEMENTATION == I2CDEV_BUILTIN_FASTWIRE
    Fastwire::setup(400, true);
#endif

    mpu.initialize();

    mpu.setXGyroOffset(GYRO_OFFSET_X);
    mpu.setYGyroOffset(GYRO_OFFSET_Y);
    mpu.setZGyroOffset(GYRO_OFFSET_Z);
    mpu.setXAccelOffset(ACCEL_OFFSET_X);
    mpu.setYAccelOffset(ACCEL_OFFSET_Y);
    mpu.setZAccelOffset(ACCEL_OFFSET_Z);

    uint8_t status = mpu.dmpInitialize();
    if (!status) {
        // turn on the DMP, now that it's ready
        mpu.setDMPEnabled(true);

        // enable Arduino interrupt detection
        attachInterrupt(0, dmpDataReady, RISING);
        mpu_int_status = mpu.getIntStatus();

        // get expected DMP packet size for later comparison
        packet_size = mpu.dmpGetFIFOPacketSize();
    }

    return status;
}

void pid_init(void) {
    //setup PID
    pid.SetMode(AUTOMATIC);
    pid.SetSampleTime(10);
    pid.SetOutputLimits(MOTOR_SPEED_MIN, MOTOR_SPEED_MAX); 
}

void setup() {
    Serial.begin(115200);
    BT.begin(115200);

    int status = gyroaccel_init();
    if (status) {
      // ERROR!
      // 1 = initial memory load failed
      // 2 = DMP configuration updates failed
      // (if it's going to break, usually the code will be 1)
      Serial.print(F("DMP Initialization failed (code "));
      Serial.print(status);
      Serial.println(F(")"));
      while (1);
    }

    motor_init();
    pid_init();
}

void loop() {
    // wait for MPU interrupt or extra packet(s) available
    while (!mpu_interrupt && fifo_count < packet_size) {
      //no mpu data - performing PID calculations and output to motors 
      pid.Compute();
      motor_move(output);
    }
  
    // reset interrupt flag and get INT_STATUS byte
    mpu_interrupt = false;
    mpu_int_status = mpu.getIntStatus();

    // get current FIFO count
    fifo_count = mpu.getFIFOCount();

    // check for overflow (this should never happen unless our code is too inefficient)
    if ((mpu_int_status & 0x10) || fifo_count == 1024) {
        // reset so we can continue cleanly
        mpu.resetFIFO();
        Serial.println(F("FIFO overflow!"));
        
        // otherwise, check for DMP data ready interrupt (this should happen frequently)
    } else if (mpu_int_status & 0x02) {
        // wait for correct available data length, should be a VERY short wait
        while (fifo_count < packet_size) fifo_count = mpu.getFIFOCount();

        // read a packet from FIFO
        mpu.getFIFOBytes(fifo_buffer, packet_size);
 
        // track FIFO count here in case there is > 1 packet available
        // (this lets us immediately read more without waiting for an interrupt)
        fifo_count -= packet_size;

        mpu.dmpGetQuaternion(&q, fifo_buffer);
        mpu.dmpGetGravity(&gravity, &q);
        mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);

        input = ypr[1] * 180/M_PI;
    }
}
