//handle communications
#include "LoRaWAN.h"
#include "TimerMillis.h"
#include <STM32L0.h>

// TTN fair usage policy guideline
// An average of 30 seconds uplink time on air, per day, per device. 
// 20 messages per day at SF12, 500 messages per day at SF7, for 12byte payload
// At most 10 downlink messages per day, including the ACKs for confirmed uplinks.

// Configure the keys here, node the DevEUI is acquired from the module, but you can manually override
const char *appEui  = "enter here";
const char *appKey  = "enter here";

//const char *devEui  = "enter here"; //uncomment if manual
char devEui[32]; // comment if manual

TimerMillis transmitTimer; //timer for transmission events
int datarate_old = -1;

long lorawan_uplink_counter_old = 0;
int lorawan_failed_counter = 0;

#define PACKET_SIZE 18
typedef struct sensorData_t{
  uint8_t   stat;
  int8_t    t1;
  uint8_t   t01;
  int8_t    h1;
  uint8_t   h01;
  uint16_t  ap; //sth wrong with this
  int8_t    acc;
  uint16_t  vdd; //sth wrong with this
  int8_t    tc1;
  uint8_t   tc01;
  uint16_t  significant_wh;
  uint16_t  average_wh;
  uint16_t  average_period;
}__attribute__((packed));

typedef union lorawanPacket_t{
  sensorData_t sensor;
  byte bytes[sizeof(sensorData_t)];
};

lorawanPacket_t packet;

void comms_setup( void )
{
    //Get the device ID and print
    LoRaWAN.getDevEui(devEui, 18); //comment if manual override
    
    #ifdef debug
      serial_debug.print("STM32L0 Device EUI = "); 
      serial_debug.println(devEui); 
    #endif

    //Configure lora parameters
    LoRaWAN.begin(EU868);
    LoRaWAN.addChannel(1, 868300000, 0, 6);
    
    LoRaWAN.setDutyCycle(true); // must be true except for development in confined environments
    LoRaWAN.setAntennaGain(0.0); // must be equal to the installed antenna
    //LoRaWAN.setADR(true); // Kicks in after 64 received packets
    // Manual settings if ADR disabled
    LoRaWAN.setADR(false);
    LoRaWAN.setDataRate(5);
    LoRaWAN.setTxPower(14);
    //below three could be set automatically, but can not be set after join apparently
    LoRaWAN.setLinkCheckLimit(48); // number of uplinks link check is sent, 5 for experimenting, 48 otherwise
    LoRaWAN.setLinkCheckDelay(4); // number of uplinks waiting for an answer, 2 for experimenting, 4 otherwise
    LoRaWAN.setLinkCheckThreshold(4); // number of times link check fails to assert link failed, 1 for experimenting, 4 otherwise
    // see examples/LoRaWAN_Disconnect/LoRaWAN_Disconnect.ino
    
    LoRaWAN.onJoin(joinCallback);
    LoRaWAN.onLinkCheck(checkCallback);
    LoRaWAN.onTransmit(doneCallback);
    LoRaWAN.onReceive(receiveCallback);
    
    LoRaWAN.joinOTAA(appEui, appKey, devEui);
    transmitTimer.stop();
    transmitCallback();
    //transmitTimer.start(transmitCallback, 0, 180*1000); // schedule a transmission every 90s as a start
}

void transmitCallback(void)
{
  STM32L0.wakeup();
  #ifdef debug
    serial_debug.println("transmitCallback() timer");
  #endif
}

void comms_transmit(void)
{
  if (!LoRaWAN.busy())
  {
    #ifdef debug
        serial_debug.println("comms_transmit() scheduling send");
    #endif
    
    if (!LoRaWAN.linkGateways())
    {
      transmitTimer.stop();
      #ifdef debug
        serial_debug.println("REJOIN( )");
      #endif
        LoRaWAN.rejoinOTAA();
    }
    
    if (LoRaWAN.joined())
    {
      #ifdef debug
        serial_debug.print("TRANSMIT( ");
        serial_debug.print("TimeOnAir: ");
        serial_debug.print(LoRaWAN.getTimeOnAir());
        serial_debug.print(", NextTxTime: ");
        serial_debug.print(LoRaWAN.getNextTxTime());
        serial_debug.print(", MaxPayloadSize: ");
        serial_debug.print(LoRaWAN.getMaxPayloadSize());
        serial_debug.print(", DR: ");
        serial_debug.print(LoRaWAN.getDataRate());
        serial_debug.print(", TxPower: ");
        serial_debug.print(LoRaWAN.getTxPower(), 1);
        serial_debug.print("dbm, UpLinkCounter: ");
        serial_debug.print(LoRaWAN.getUpLinkCounter());
        serial_debug.print(", DownLinkCounter: ");
        serial_debug.print(LoRaWAN.getDownLinkCounter());
        serial_debug.println(" )");
      #endif
      // int sendPacket(uint8_t port, const uint8_t *buffer, size_t size, bool confirmed = false);
      LoRaWAN.sendPacket(2, &packet.bytes[0], sizeof(sensorData_t), false);
    }
  }

  //watchdog for potential deadlocks and hangs, this should really never happen
  
  //check if the counter has not incremented and increase the failed flag
  if(lorawan_uplink_counter_old==LoRaWAN.getUpLinkCounter()){
    lorawan_failed_counter++;
  }
  else{
    lorawan_failed_counter=0; // reset to 0 on all successful transmissions
  }
  
  //update the counter value
  lorawan_uplink_counter_old = LoRaWAN.getUpLinkCounter();
  
  #ifdef debug
        serial_debug.print("comms_transmit() lorawan_failed_counter ");
        serial_debug.println(lorawan_failed_counter);
  #endif

  //finally issue a full system reset if 50 scheduled transmissions failed
  //first option is a deadlock on lorawan stack, second is callting the transmitt timer more often then dutycycle allows
  if(lorawan_failed_counter>50){
    #ifdef debug
        serial_debug.println("comms_transmit() lorawan_failed_counter, full reset");
    #endif
    // perform full system reset
    STM32L0.reset();
  }
  
}

// Callback on Join failed/success
void joinCallback(void)
{
    if (LoRaWAN.joined())
    {
      #ifdef debug
        serial_debug.println("JOINED");
      #endif
      //force trigger transmitt callback
      transmitCallback();
    }
    else
    {
      #ifdef debug
        serial_debug.println("REJOIN( )");
      #endif
      LoRaWAN.rejoinOTAA();
    }
}

// Link check callback, useful for ADR debugging
void checkCallback(void)
{
  #ifdef debug
    serial_debug.print("CHECK( ");
    serial_debug.print("RSSI: ");
    serial_debug.print(LoRaWAN.lastRSSI());
    serial_debug.print(", SNR: ");
    serial_debug.print(LoRaWAN.lastSNR());
    serial_debug.print(", Margin: ");
    serial_debug.print(LoRaWAN.linkMargin());
    serial_debug.print(", Gateways: ");
    serial_debug.print(LoRaWAN.linkGateways());
    serial_debug.println(" )");
  #endif
}

// callback upon receiving data
void receiveCallback(void)
{
  #ifdef debug
    serial_debug.print("RECEIVE( ");
    serial_debug.print("RSSI: ");
    serial_debug.print(LoRaWAN.lastRSSI());
    serial_debug.print(", SNR: ");
    serial_debug.print(LoRaWAN.lastSNR());
  #endif
  if (LoRaWAN.parsePacket())
  {
    uint32_t size;
    uint8_t data[256];

    size = LoRaWAN.read(&data[0], sizeof(data));

    if (size)
    {
      data[size] = '\0';
      #ifdef debug
        serial_debug.print(", PORT: ");
        serial_debug.print(LoRaWAN.remotePort());
        serial_debug.print(", DATA: \"");
        serial_debug.print((const char*)&data[0]);
        serial_debug.println("\"");
      #endif
      //remote trigger system reset, use with caution
      if((LoRaWAN.remotePort()==99) & (data[0]==0xab)){
        STM32L0.reset();
        }
    }
  }
  #ifdef debug
    serial_debug.println(" )");
  #endif
}

//callback on link lost, handle rejoining schedule here
void doneCallback(void)
{
  if (!LoRaWAN.linkGateways())
  {
    #ifdef debug
      serial_debug.println("DISCONNECTED");
    #endif
  }
}
