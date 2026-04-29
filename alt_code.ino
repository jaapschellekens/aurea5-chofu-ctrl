/* 
Alternative control for the Atlantic Aurea heat pump, which based on the Chofu AEYC-0643XU-AT unit.
This is a sketch to control the power setting of a heat pump with an Arduino ATMega2560 and to modify communication inside the controlbox using 3 of the 4 hardware serial ports of this board. 
The original code shows various useful information like temperatures etc. on a 480*320 TFT LCD. This has been removed here to keep the code more compact.
Version 0.1Beta, JHN 19-03-2025 */

#include <QuickPID.h>                                   // Library for PID controller, see for documentation: https://github.com/Dlloydev/QuickPID

//Define variables and constants
  // General
  String MessageString = "";

  // For heat pump
  const uint8_t SpeedStages = 7;                        // Number of speed stages to be used (7 --> ~1400 W max)
  uint8_t SpeedSetpoint = 0;                            // Speed setpoint sent to heatpump   // const uint8_t MinTempSetpoint = 25;
  uint8_t TargetTemperatureThermostat = 20;             // Target temperature as requested by the thermostat   
  const float MinTemp4HP = -2;                          // Outside temperatute below wich heat pump should stop working and CV should start
  uint16_t TelegramCount = 0;                           // Follow-up number of telegrams
  // Variables to track timings
  uint32_t Interval = 0;                                // Time since last telegram
  uint32_t currentMillis = 0;                           // Check timing for sending messages
  uint32_t LastSpeedSetpointChange = 0;                 // Keep track when last time speedsetpint has been changed in milliseconds
  uint32_t SpeedSetpointHysteresis = 420000;            // Allow a change in speedsetpoint only once per 7 minutes (when using slow PID parameter set)
  // Cycle time check
  uint32_t previousCycleMicros = 0;                     // To store the time of the last cycle start
  uint32_t currentCycleMicros = 0;                      // To store the time of the current cycle start
  uint32_t cycleTime = 0;                               // To store the calculated cycle time
  uint16_t cycleCounter = 0;                            // Counter to track cycles
  const uint16_t displayInterval = 10000;               // Display cycle time every 10000 cycles
  uint8_t lcdUpdateStep = 0;                            // Variable to spread writing to screen over multiple steps
  // Receive and send inter MCU data 
  const uint32_t McuMessageWaitTime = 200;              // Waiting time between receiving message on Pad 1 (Serial 1) and sending on pad 2 (serial 2) in ms
  uint32_t McuMessageOneReceived = 0;                   // Tie last message send in ms
  bool dataMcu2Send = false;
  uint32_t PreviousMessageSerial2Sent = 0;   
  uint32_t MessageSerial2Interval = 10000;              // Maximum interval for sending message on serial 2 (to avoid that communication stops)
  const uint8_t DataMcuLength = 12;                     // Length of each transmission between MCU's
  uint8_t dataArrayOT_IC1_IC2[DataMcuLength] = {0};     // Array to store the incoming 12 bytes for OT IC1 to IC2.
  uint8_t dataArrayOT_IC2_IC1[DataMcuLength] = {0};     // Array to store the incoming 12 bytes for OT IC2 to IC1.
  uint8_t dataArrayOT_IC1_IC2_Old[DataMcuLength] = {0}; // Array to store the previous incoming 12 bytes for OT IC1 to IC2.
  uint8_t dataArrayOT_IC2_IC1_Old[DataMcuLength] = {0}; // Array to store the previous incoming 12 bytes for OT IC2 to IC1.  
  uint16_t ChecksumMCU = 0;                             // Checksum for communication between MCU's
// Variables for communication with heat pump
  uint8_t CB_HPlength = 0;                              // Length of communcation CB to HP
  uint16_t ChecksumCBHP = 0;                            // Checksum for communication of heat pump with control box
  uint8_t ChecksumCBHPfalseCount = 0;                   // Counter for number of times that the checksum did not match. After certain number of mismatches, message will be send to the serial port
  uint8_t data0[] = { 0x19, 0x0, 0x8, 0x0, 0x0, 0x0, 0xd9, 0xb5 };                       // Array of hexadecimal bytes
  uint8_t data1[] = { 0x19, 0x1, 0x0c, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xaa, 0x35 };  // Array of hexadecimal bytes
  uint8_t data2[] = { 0x19, 0x2, 0x8, 0x1, 0x1, 0x0, 0x99, 0x37 };                       // Array of hexadecimal bytes. This is the one to change bytes 4 (on/off) and 5 (setpoint), and 7 and 8 (high and low bytes of the CRC CCITT checksum)
  uint8_t data3[] = { 0x19, 0x3, 0x8, 0xb2, 0x2, 0x0, 0xc1, 0x9a };                      // Array of hexadecimal bytes

// Variables for sending messages to heat pump
bool IncomingMessageEnded = 0;                          // Last message start time in milliseconds
volatile uint32_t PreviousMessageSent = 0;              // Time when prveious message was sent in milliseconds
volatile bool IsReceiving = false;                      // Tracks if message is being received from heat pump to block sending next message to heat pump
const uint32_t SendTimeout = 2000;                      // Time in milliseconds to wait before considering the communication ended
const uint32_t SendDelay = 99;                          // Delay in milliseconds before sending information to heatpump
const uint32_t MinMessageSendInterval  = 300;           // Minimum interval in millis between sending messages when no reply is received
volatile uint32_t previousMessageInEnded = 0;

// Definitions for reading and checking heat pump data
  // Prepare array for storing data from heatpump
    const uint8_t StartByte = 0x91;                     // First byte must be 145 (0x91)
    const uint8_t Rows = 4;                            // Number of rows in heat pump data array 
    const uint8_t Cols = 80;                            // Number of columns in heat pump data array 
    uint8_t numChars = 0;                               // Number of characters to be written to screen
    uint8_t dataHeatPumpTemp[Cols][Rows] = {0};         // Array to store of four messages from heatpump before checksum check
    uint8_t dataHeatPump[Cols][Rows] = {0};             // Array to store of four messages from heatpump after checksum is correct
    uint8_t dataHeatPumpOld[Cols][Rows] = {0};          // Array to store previous data of four messages so that screen is only updayted when needed
    uint8_t extractedRow[Cols];                         // 1D array to store the row  
    const uint8_t ValidLengths[] = {13, 14, 19, 21};    // Lengths 12, 13, 18 & 20   (12+1), (13+1), (18+1), (20+1)
    uint16_t value;                                     // Temporarily storage of datapoint from array
    float valuefloat;
    uint8_t Length = 0;
    float T_supply =0;                                  // Supply water temperature by heatpump
    float T_return =0;                                  // Return water temperature to heatpump
    float T_outside =0;                                 // Outside temperature measured on heatpump
  // State variables for non-blocking serial reading
    enum SerialState {  Wait_Start,                     // Waiting for the start of a serial message  
                        Read_Header,                    // Reading the header of the message  
                        Read_Payload,                   // Reading the main data (payload)  
                        Read_End                        // Reading the end of the message  
                        };                              // Enumeration type, which helps in defining different states of serial communication
    SerialState state = Wait_Start;                     // Make waiting to start the default
    uint8_t ID = 0;
    uint8_t msgLength = 0;
    uint8_t DataLength = 0;
    uint8_t buffer[Cols] = {0};                         // Temporary buffer for incoming data
    uint8_t index = 0;
    int8_t NegT = 0;                                    // Store if temperature is negative (value of 255 in MSB)
  // For PID controller 
    float TemperaturePIDsetpoint = 20;                  // Desired supply temperature
    const float TemperaturePIDsetpointMax = 37;         // Maximum temperature setpoint
    float TemperaturePIDinput = 0;                      // Current suppy temperature (feedback from the sensor in the heatpump)
    float TemperaturePIDoutput = 0;                     // Heat pump control signal
    float TemperaturePIDoutputRounded = 0;              // Heater control signal, rounded off
    // PID tuning parameter. Default values were Kp = 2.0, Ki = 5.0, Kd = 1.0
    uint8_t PIDParameterSet = 0;                        // Keep track of whcih PID parameters set is being used so that new parameter or only written when required
    float Sensitivity = 3;                              // Sensitivity (ratio) between the slow and fast PID settings. Optionally make this chnageable via rotary encoder
    float PIDParameterThreshold = 1;                    // Threshold for switch between fast (agressive) and slow (conservative) PID parameters
    float T_GapPIDParameterSwitch = 0;                  // Gap between the setpoint and actual temperature, used to determine which set PID parameters to use 
    const float Kp0 = 0.02, Ki0 = 0.008, Kd0 = 0.01;// 
    float Kp = Kp0, Ki = Ki0, Kd = Kd0;// 
// Create PID object
  QuickPID myPID(&TemperaturePIDinput, &TemperaturePIDoutput, &TemperaturePIDsetpoint, Kp, Ki, Kd, /* OPTIONS */
                myPID.pMode::pOnError,                   /* pOnError, pOnMeas, pOnErrorMeas */
                myPID.dMode::dOnMeas,                    /* dOnError, dOnMeas */
                myPID.iAwMode::iAwCondition,             /* iAwCondition, iAwClamp, iAwOff */
                myPID.Action::direct);                   /* direct, reverse */

void setup() {
// Serial Setup
  Serial.begin(115200);                                   // Start the Serial Monitor for debugging on terminal (Serial port 0)
  Serial1.begin(9600);                                    // Start serial port 1 for receiving messages from OT MCU to HP MCU (RX1, 9600 bps)
  Serial2.begin(9600);                                    // Start serial port 2 to receive messages from HP MCU and send modified message to OT MCU (RX2 pin & TX2 9600 bps)
  Serial3.begin(666);                                     // Start serial port 3 for sending an receiving info to heat pump at 666 baud (RX3 and TX3)
  Serial1.setTimeout(100);                                // Changes the timeout for readBytes() and other blocking serial read functions (readString(), readBytesUntil(), etc.).
  Serial2.setTimeout(100);
// For QuickPID
  myPID.SetOutputLimits(0, 99);                           // Set to max 99% to have max. 4 digits ('99.9')
  myPID.SetSampleTimeUs(5000000);                         // Sample time of PID controller in microseconds. Made slower than default because of relatively slow temperature changes and heat pump response to save processing time.
  myPID.SetTunings(Kp, Ki, Kd);
  myPID.SetMode(myPID.Control::automatic);                // Set PID controller in automatic, i.e. input (PV) is perdiodically compared with setpoint (SP) and control variable (CV) is adjusted 

  Serial.println("Setup complete.");
}

//////////////// Main loop ////////////
void loop() {
  currentMillis = millis();                                 // get curent millis (ms) for sending message to heat pump
  currentCycleMicros = micros();                            // Capture the current time (in us) to calculate the cycle time of the loop
  cycleCounter++;                                           // Increment the cycle counter
  
// Check for incoming data on Serial 1 (Pad 1 from MCU 2), 2 (Pad 2 from MCU 1) and Serial 3 (data from heat pump)
readSerialMessageHP();                                      // Call this function continuously to check for data on the incoming serial port from the heat pump
if (Serial1.available() >= DataMcuLength) {                 // Check whether the serial buffer connected to Pad 1 contains DataMcuLength (12) bytes or more  
  Serial.print("Serial1.available()"); Serial.println(Serial1.available()); 
  readSerialMessageMCU(1);                                  // When so, jump to subroutine to read the bytes or clear the buffer
  McuMessageOneReceived = currentMillis;                    // Set timestamp after receiving message on pad 1 (from MCU 2 to 1)
  dataMcu2Send = true;                                      // Set flag to send message on serial port 2
  TemperaturePIDsetpoint = 1 + 0.85 * dataArrayOT_IC2_IC1[0];// Scale the setpoint from the thermostat (0, and 30~40oC) to 25~37oC
  if (TemperaturePIDsetpoint > TemperaturePIDsetpointMax){
    TemperaturePIDsetpoint = TemperaturePIDsetpointMax;
    }
  }
if (Serial2.available() >= DataMcuLength) {                 // Check whether the serial buffer connected to Pad 2 contains DataMcuLength (12) bytes or more 
  Serial.print("Serial2.available()"); Serial.println(Serial2.available()); 
  readSerialMessageMCU(2);                                  // When so, jump to subrotuine to read the bytes or clear the buffer
  }

// Write modified message from MCU 1 to 2 on serial 2, McuMessageWaitTime (default 200 ms) after receiving message on serial 1 
if ((((currentMillis - McuMessageOneReceived) >= McuMessageWaitTime) && dataMcu2Send == true) || ((currentMillis-PreviousMessageSerial2Sent) > MessageSerial2Interval))  {
  Serial2.write(dataArrayOT_IC1_IC2, sizeof(dataArrayOT_IC1_IC2));    // Write array to serial port 2
  PreviousMessageSerial2Sent = currentMillis;
  dataMcu2Send = false;
  }

// Send the next message to the heat pump 100 ms after the previous incoming message has ended, or after 1000 ms without sending anything. Use the periodic sending also to update the screen
  currentMillis = millis();                                 // get curent millis (ms) for sending message to heat pump
    if ((( (currentMillis - previousMessageInEnded >= SendDelay)  &&  (!IsReceiving))  || ( currentMillis - previousMessageInEnded >= SendTimeout )) && (currentMillis - PreviousMessageSent >= ( MinMessageSendInterval ))) 
      {
      switch (TelegramCount) {
        case 0:
          Serial3.write(data0, sizeof(data0));
          break;
        case 1:
          Serial3.write(data1, sizeof(data1));
          break;
        case 2:
          SpeedSetpoint = (int)round( TemperaturePIDoutput / (100 / SpeedStages )) ;    // Scale the PID controller output from 0~100% to 0~8 speed setpoints, and convert from double to rounded integer
          if (SpeedSetpoint == 0) {
            data2[3] = 0x0;
            data2[4] = 0x0;
            } else {
              if ((PIDParameterSet == 0) && (currentMillis - LastSpeedSetpointChange > SpeedSetpointHysteresis)) { // Add hysteresis when slow PID parameter set is active
                data2[3] = 0x1;
                data2[4] = SpeedSetpoint;                 // Write calculate speed setpoint in array  
                LastSpeedSetpointChange = currentMillis;
                } else {
                  if ((PIDParameterSet > 0) && (currentMillis - LastSpeedSetpointChange > SpeedSetpointHysteresis/5)) 
                    { // Hysteresis 1/5 of default when fast PID parameter set is active to allow for fast respons
                    data2[3] = 0x1;
                    data2[4] = SpeedSetpoint;             // Write calculate speed setpoint in array  
                    }
                  }
              } 
          CB_HPlength = sizeof(data2);
          calculate_CRC_CCITT_Checksum(data2, CB_HPlength-2, &ChecksumCBHP);  
          data2[6] = (ChecksumCBHP >> 8) & 0xFF;          // Replace byte 6 with checksum High Byte
          data2[7] = ChecksumCBHP & 0xFF;                 // Replace byte 7 with checksum Low Byte
          Serial3.write(data2, sizeof(data2));
          break;
        case 3:
          Serial3.write(data3, sizeof(data3));
          break;
        default:
          Serial.print("Invalid TelegramCount value:  ");Serial.println(TelegramCount); 
          break;
        }
//        Serial.print("Message ");Serial.print(TelegramCount);Serial.println(" sent!");  // Confirm that message has bene sent
        TelegramCount = (TelegramCount + 1) % 4;          // Cycle through 0-3
        PreviousMessageSent = millis();
      }

   if (cycleCounter >= displayInterval) {
        if (previousCycleMicros != 0) {
          cycleTime = ( currentCycleMicros - previousCycleMicros ) / displayInterval;  // Time difference between cycles
          previousCycleMicros = currentCycleMicros;           // Update the previous cycle time
      cycleCounter = 0;                                   // Reset the cycle counter
      } else {
        previousCycleMicros = currentCycleMicros;
      }
    switch (lcdUpdateStep) {
      case 0:                                             // Print the cycle time etc. The timing for sneding message to the heatpump wil be oof when the cycle time is too long (> ~50 ms)
        Serial.println("Cycle time "+String(cycleTime)+" ms");
      break;
      case 1:                                             // Print PID settings
      break;
      case 2:                                             // Compresor speed 91-3,9
        if (dataHeatPump[9][3] != dataHeatPumpOld[9][3]) {  // Data has changed?
          Serial.print("Speed setpoint: "); Serial.println(dataHeatPump[9][3]);
          }
      break;
      case 3:                                             // Compresor power 91-3,10
        if (dataHeatPump[10][3] != dataHeatPumpOld[10][3]) {
          MessageString = String((25.6 * dataHeatPump[10][3]),0);
          Serial.print("Compressor power: "); Serial.println(MessageString);
          dataHeatPumpOld[10][3] = dataHeatPump[10][3];
          }
        // Compressor mode 91-1,3
        if (dataHeatPump[3][1] != dataHeatPumpOld[3][1]) {
          Serial.print("Compressor mode: "); Serial.println(dataHeatPump[3][1]);
          dataHeatPumpOld[3][1] = dataHeatPump[3][1];
          }
        // Defrost?   91-1,4
        if (dataHeatPump[4][1] != dataHeatPumpOld[4][1]) {
          Serial.print("Defrost ongoing? "); Serial.println(dataHeatPump[4][1]);
          dataHeatPumpOld[4][1] = dataHeatPump[4][1];
          }
      break;
      case 4:
        // ?Data? 91-3,5
        if (dataHeatPump[5][3] != dataHeatPumpOld[5][3]) {
          Serial.print("Generic info "); Serial.println(dataHeatPump[5][3]);
          dataHeatPumpOld[5][3] = dataHeatPump[5][3];
          } 
      break;
      case 5:                                             // Temperatures
        // Supply temperature   91-2,3~4
        if (dataHeatPump[3][2] != dataHeatPumpOld[3][2]) {
          value = dataHeatPump[4][2];                             // MSB of temperature; previous byte is the MSB
          T_supply = (((value*256+dataHeatPump[3][2])-(65536*((value == 255) ? 1 : 0)))/10.0);  // Do the conversion to temperature in such a way that also negative temperature can be shown
          TemperaturePIDinput = T_supply;                       // Make the input the PID controller equal to the current supply water temperature   
          MessageString = String(T_supply,1);
          Serial.print("Supply temperature "); Serial.println(MessageString);
          dataHeatPumpOld[3][2] = dataHeatPump[3][2];
        }
        // Return temperature 91-2,5~6
        if (dataHeatPump[5][2] != dataHeatPumpOld[5][2]) {
          value = dataHeatPump[6][2];               // MSB of temperature; previous byte is the MSB
          T_return = (((value*256+dataHeatPump[5][2])-(65536*((value == 255) ? 1 : 0)))/10.0);  // Do the conversion to temperature in such a way that also negative temperature can be shown
          MessageString = String(T_return,1);
          Serial.print("Return temperature "); Serial.println(MessageString);
          dataHeatPumpOld[5][2] = dataHeatPump[5][2];
        }
        break;
      case 6:
        // Outside temperature 282 91-2,7~8
        if (dataHeatPump[7][2] != dataHeatPumpOld[7][2]) {  
          value = dataHeatPump[8][2];               // MSB of temperature; previous byte is the MSB
          T_outside = (((value*256+dataHeatPump[7][2])-(65536*((value == 255) ? 1 : 0)))/10.0);  // Do the conversion to temperature in such a way that also negative temperature can be shown
          MessageString = String(T_outside,1);
          Serial.print("Outside temperature "); Serial.println(MessageString);
          }
 
      break;
      case 7:                                             // Do PID calculation
        // Evaluate PID calculation    
        T_GapPIDParameterSwitch = abs(TemperaturePIDsetpoint - TemperaturePIDinput);    // Check the gap between the temperature setpoint adn the actual temperature
        if ((T_GapPIDParameterSwitch < PIDParameterThreshold) && (PIDParameterSet = 1)) { // If the gap is smaller than the threshold and the PID controller is currently using the fast set, then
          Kp = Kp0; Ki = Ki0; Kd = Kd0;                       // Choose slow (conservative) settings
          myPID.SetTunings(Kp, Ki, Kd);                       // Write a new set of control parameter
          PIDParameterSet = 0;                                // Set parameter to default default (low) settings
          } else {
            if (PIDParameterSet = 1) {                          // Gap larger than threshold but slow parameters are used, then 
              Kp = Sensitivity*Kp0;                           // Choose fast or aggressive settings by larger gap
              Ki = Sensitivity*Ki0; 
              Kd = Sensitivity*Kd0;
              myPID.SetTunings(Kp, Ki, Kd);                   // Write a new set of control parameter
              PIDParameterSet = 1;                            // Set parameter to default default (low) settings
              }
            }
          break;
      case 8:                                             // Print inter MCU communication 
      break;
      case 9:
        TemperaturePIDoutputRounded = round(TemperaturePIDoutput*10.0)/10.0;
    }
    lcdUpdateStep = (lcdUpdateStep + 1) % 10;  // Cycle through screen updates
  }
}////////////// End of main loop  //////////////////////////

////////  Below are the subroutines //////////////////////
void readSerialMessageMCU(uint8_t MCU) {
uint8_t TempMcuData[DataMcuLength] = {0};
uint8_t TemperatureCategory;  // Variable to categorize temperature
  ////// Inter MCU communication
    switch (MCU) {
      case 1:          // 1. Communication IC2 (OT) to IC1 (HP): This communication is via PAD 1 and therefore used at serial 1. Check if enough data has arrived in the serial buffer and read it
        // for (int i = 0; i < DataMcuLength; i++) {
        //   TempMcuData[i] = Serial1.read();                 // Read the bytes into the array
        // }
        Serial1.readBytes(TempMcuData, DataMcuLength);        // Clean way of reading fixed number of bytes into array. Beware: Blocks execution until all bytes arrive or timeout (default timeout is 1000ms).
        calculateMCUChecksum(TempMcuData, DataMcuLength, ChecksumMCU);// Calculate and check the checksum by determining the sum of the first 11 bytes and taken modules 256
        if (ChecksumMCU == TempMcuData[DataMcuLength-1]) { // Check if the checksum matches
          // for (int i = 0; i < DataMcuLength; i++) {
          //   dataArrayOT_IC2_IC1[i] = TempMcuData[i];          
          //   }
          memcpy(dataArrayOT_IC2_IC1, TempMcuData, DataMcuLength); // Checksum OK so copy temp array to permanent array, cleaner and faster code than a loop  
          ChecksumMCU = false;                                // Make checksum false as start value for next time  
          } else {                                            // Checksum incorrect because communication possibly halfway received. 
              clearSerialBuffer(Serial1);                     // Clears the read buffer of serial 1 if the checksu was wrong to make a fresh start
              Serial.println("Serial buffer 1 cleared");
            }
      break;
      case 2:         // 2. Communication IC1 (HP) to IC2 (OT): This communication is via PAD 2 and therefore used at serial 2. Check if enough data has arrived in the serial buffer, read it, modify when required and resend
        Serial2.readBytes(TempMcuData, DataMcuLength);      // Clean way of reading fixed number of bytes into array. Beware: Blocks execution until all bytes arrive or timeout (default timeout is 1000ms).
        calculateMCUChecksum(TempMcuData, DataMcuLength, ChecksumMCU);// Calculate and check the checksum by determining the sum of the first 11 bytes and taken modules 256
        if (ChecksumMCU == TempMcuData[DataMcuLength-1]) { // Check if the checksum matches
          memcpy(dataArrayOT_IC1_IC2, TempMcuData, DataMcuLength); // Cleaner and faster code than a loop
          ChecksumMCU = false;                            // Make checksum false as start value for next time
          if (T_outside < MinTemp4HP) {
            TemperatureCategory = 0;                             // Too cold for heat pump, switch on cental heating
            } else if (T_outside < 4) {
              TemperatureCategory = 1;  // Below 4°C             // Prevent that controlbox starts CV 
              } else if (SpeedSetpoint == 0) {
                TemperatureCategory = 2;                         // SpeedSetpoint is zero
                } else {
                  TemperatureCategory = 3;                       // In all other cases use the temperature setpoint for water as read from Plugwise Anna via MCU2
                  }                 
          switch (TemperatureCategory) {
            case 0:                                     // Heat pump is not running because it is too cold so central heating should be started
              dataArrayOT_IC1_IC2[0] = 0xf0;
              break;
            case 1:                                     // Below 4°C but heat pump can run, adjust values to prevent central heating activation
              dataArrayOT_IC1_IC2[0] = dataArrayOT_IC2_IC1[0];
              dataArrayOT_IC1_IC2[9] = 0x04;
              dataArrayOT_IC1_IC2[4] = 0;               // Write 0's to force MCU2 to transfer HP temperature to OpenTherm 
              dataArrayOT_IC1_IC2[5] = 0;
              dataArrayOT_IC1_IC2[6] = 0;
              dataArrayOT_IC1_IC2[10] = 0;
              break;
              case 2:                                     // Heat pump is not running (Speedsetpoint  is zero), then let central heating boiler temperatures go to OpenTherm
              break;
              case 3:                                     // In all other cases use the temperature setpoint for water as read from Plugwise Anna via MCU2
                dataArrayOT_IC1_IC2[0] = dataArrayOT_IC2_IC1[0];
                dataArrayOT_IC1_IC2[4] = 0;             // Write 0's to force MCU2 to transfer HP temperature to OpenTherm 
                dataArrayOT_IC1_IC2[5] = 0;
                dataArrayOT_IC1_IC2[6] = 0;
                dataArrayOT_IC1_IC2[10] = 0; 
              break;
            }
            dataArrayOT_IC1_IC2[7] = round(T_supply);     // Write the actual outside temperature in the array. Will not work for temperatures below zero because array is 8 bits unsigned!
            dataArrayOT_IC1_IC2[8] = round(T_return);     //
            dataArrayOT_IC1_IC2[9] = round(T_outside);    // Write the actual outside temperature in the array. Will not wok for temperatures below zero because array is 8 bits unsigned!
            // Serial.print("T_supply: ");Serial.print(T_supply)     
            // Serial.print("T_return: ");Serial.print(T_return)
            // Serial.print("T_outside: ");Serial.print(T_outside)       
            calculateMCUChecksum(dataArrayOT_IC1_IC2, DataMcuLength, ChecksumMCU);// Recalculate the checksum
            dataArrayOT_IC1_IC2[11] = round(ChecksumMCU); // Replace old checksum value with new calculated one
            } else {                                      // Checksum incorrect because communication possibly halfway received. 
              clearSerialBuffer(Serial2);                 // Clears the read buffer of serial 2 if the checksu was wrong to make a fresh start
              Serial.println("Serial buffer 2 cleared");
              }
      break; 
    }
  }

void clearSerialBuffer(HardwareSerial &serialPort) {  
  while (serialPort.available() > 0) {
    serialPort.read();                                  // Read and discard the data in the buffer
  }
}

void calculateMCUChecksum(uint8_t dataArray[], int Length, unsigned int &checksum) {  // Calculate the checksum by determining the sum of the first 11 bytes and then take modules 256
  checksum = 0; // Initialize the checksum
      if (Length <= 1) {    // Ensure length is valid
        checksum = 0;
        return;
    }
    // Calculate the checksum for the first (Length-1) bytes in the specified row
    for (int i = 0; i < Length - 1; i++) {
        checksum += dataArray[i];
    }
    checksum %= 256;                                        // Modulus 256 to keep it in a single byte
}

// Function to calculate the CRC-CCITT (0xFFFF) checksum for the communication to and from the heat pump
void calculate_CRC_CCITT_Checksum(uint8_t *data, uint8_t Length, uint16_t *checksum) {
    uint16_t crc = 0xFFFF; // Initial value
    for (uint8_t i = 0; i < Length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    *checksum = crc;
}

// Function that reads the serial data coming from the heat pump
void readSerialMessageHP() {
    while (Serial3.available()) {  // Check if data is available on Serial3
      IsReceiving = true;
      uint8_t byteReceived = Serial3.read();
       // Serial.print(String(byteReceived,DEC)+" ");
      IsReceiving = true;
        switch (state) {
            case Wait_Start:                            // Code to wait for the start character
                if (byteReceived == StartByte) {
                    state = Read_Header;
                    index = 0;
                    IncomingMessageEnded = false; // Reset if any other byte is received to indicate
                }
                break;
            case Read_Header:                           // Code to process header bytes
                if (index == 0) {
                    ID = byteReceived;
                    if (ID > 3) {  // ID must be 0-3
                        Serial.println("Invalid ID, resetting...");
                        state = Wait_Start;
                        return;
                    }
                } else if (index == 1) {
                    msgLength = byteReceived+1;
                    if (!IsValidLength(msgLength)) {
                        Serial.println("Invalid Length! Resetting...");
                        state = Wait_Start;  // Reset
                        return;
                    }
                    DataLength = msgLength - 3;  // Exclude first 3 bytes and last byte
                    index = 0;
                    state = Read_Payload;
                }
                index++;
                break;

            case Read_Payload:                          // Code to process data payload
                buffer[index++] = byteReceived;
                if (index >= DataLength) {
                    state = Read_End;
                }
                break;

            case Read_End:                              // Code to check that last byte of message corresponds to '0'
                if (byteReceived == 0) {  // Ensure last byte is 0
                    StoreMessage(ID, DataLength, buffer);
                    IncomingMessageEnded = true;                          // If the end of the incoming message is reached, set the infication boolean
                    previousMessageInEnded = millis();                    // Set timestamp after receiving end of incoming message             
                } else {
                    Serial.println("Warning: Last byte is not 0! Resetting...");
                }
                IsReceiving = false;
                state = Wait_Start;                                     // Reset for next message
                break;
                default:
                Serial.println("Error in readSerialMessageHP state"); 
                break;
        }
    }
}

// Function to check if the Length is valid
bool IsValidLength(uint8_t Length) {
    for (uint8_t i = 0; i < sizeof(ValidLengths) / sizeof(ValidLengths[0]); i++) {
        if (Length == ValidLengths[i]) return true;
    }
    return false;
}

// Function to store messages in separate arrays based on ID
void StoreMessage(uint8_t ID, uint8_t DataLength, uint8_t *message) {
    for (uint8_t i = 0; i < (DataLength); i++) {
      dataHeatPumpTemp[i+2][ID] = message[i];
      }
    dataHeatPumpTemp[0][ID] = StartByte;
    dataHeatPumpTemp[1][ID] = ID;
    dataHeatPumpTemp[2][ID] = DataLength+2; // 12, 13, 18, 20
     for (int j = 0; j < DataLength+2; j++) {                                  // Copy row 
       extractedRow[j] = dataHeatPumpTemp[j][ID];  
       } 
    calculate_CRC_CCITT_Checksum(extractedRow, DataLength+2, &ChecksumCBHP);   // Calculate checksum, including the two checksum bytes --> result will be zero if mesage has been sent error free
    if (ChecksumCBHP == 0) {                                                   // Check that result is indeed zero
      memcpy(dataHeatPump[ID], dataHeatPumpTemp[ID], Cols * sizeof(uint8_t));  // Transfer the temporarily array to the permanent array
      ChecksumCBHPfalseCount = 0; 
      Serial.println("Checksum heat pump data received not OK");               // Print a message if there is an error     
      } else {
        ChecksumCBHPfalseCount++;                                              // Increase counter for false checksums. Nog actie bedebken als threshold wordt bereikt
        Serial.print("Checksum error! ");                                      // Print a message if there is an error
        Serial.print("Now ");Serial.print(ChecksumCBHPfalseCount);Serial.print(" Checksum errors (of max 10) counted!");
        }
}