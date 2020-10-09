// =================================================================================================
// ModbusClient: Copyright 2020 by Michael Harwerth, Bert Melis and the contributors to ModbusClient
//               MIT license - see license.md for details
// =================================================================================================
#include "ModbusServerTCP.h"

// #ifndef CLIENTTYPE
#ifdef CLIENTTYPE

// Constructor
CLASSNAME::CLASSNAME(uint8_t maxClients) :
  ModbusServer(),
  numClients(maxClients) {
    clients = new ClientData[numClients];
    memset(clients, 0, numClients * sizeof(ClientData));
   }

// Destructor: closes the connections
CLASSNAME::~CLASSNAME() {
  delete[] clients;
}

// activeClients: return number of clients currently employed
uint16_t CLASSNAME::activeClients() {
  uint8_t cnt = 0;
  for (uint8_t i = 0; i < numClients; ++i) {
    if (clients[i].task != nullptr) cnt++;
  }
  return cnt;
}

// accept: start a task to receive requests and respond to a given client
bool CLASSNAME::accept(CLIENTTYPE client, uint32_t timeout, int coreID) {
  // Look for an empty client slot
  for (uint8_t i = 0; i < numClients; ++i) {
    if (clients[i].task == nullptr) {
      clients[i].client = client;
      clients[i].timeout = timeout;
      clients[i].parent = this;

      // Create unique task name
      char taskName[12];
      snprintf(taskName, 12, "MBsrv%02XTCP", i);

      // Start task to handle the client
      xTaskCreatePinnedToCore((TaskFunction_t)&worker, taskName, 4096, &clients[i], 5, &clients[i].task, coreID >= 0 ? coreID : NULL);

      Serial.printf("Created task %d\n", (uint32_t)clients[i].task);
      return true;
    }
  }
  return false;
}

void CLASSNAME::worker(ClientData *myData) {
  // Get own reference data in handier form
  CLIENTTYPE myClient = myData->client;
  uint32_t myTimeOut = myData->timeout;
  TaskHandle_t myTask = myData->task;
  CLASSNAME *myParent = myData->parent;
  uint32_t myLastMessage = millis();
  ResponseType response;               // Data buffer to hold prepared response

  // loop forever, if timeout is 0, or until timeout was hit
  while (myClient.connected() && (!myTimeOut || (millis() - myLastMessage < myTimeOut))) {
    // Get a request
    if (myClient.available()) {
      response.clear();
      TCPMessage m = myParent->receive(myClient, 100);

      Serial.print(" Request: ");
      for (auto& byte : m) { Serial.printf(" %02X", byte); }
      Serial.println();

      // has it the minimal length (6 bytes TCP header plus serverID plus FC)?
      if (m.size() >= 8) {
        // Yes. Take over TCP header for later response
        for (uint8_t i = 0; i < 6; ++i) {
          response.push_back(m[i]);
        }
        // ServerID shall be at [6], FC at [7]. Check both
        if (myParent->isServerFor(m[6])) {
          // Server is correct - in principle. Do we serve the FC?
          MBSworker callBack = myParent->getWorker(m[6], m[7]);
          if (callBack) {
            // Yes, we do. Invoke the worker method to get a response
            ResponseType data = callBack(m[6], m[7], m.size() - 8, m.data() + 8);
            // Process Response
            // One of the predefined types?
            if (data[0] == 0xFF && (data[1] == 0xF0 || data[1] == 0xF1 || data[1] == 0xF2 || data[1] == 0xF3)) {
              // Yes. Check it
              switch (data[1]) {
              case 0xF0: // NIL
                response.clear();
                break;
              case 0xF1: // ECHO
                response = m;
                break;
              case 0xF2: // ERROR
                response[4] = 0;
                response[5] = 3;
                response.push_back(m[6]);
                response.push_back(m[7] | 0x80);
                response.push_back(data[2]);
                break;
              case 0xF3: // DATA
                response[4] = ((data.size() - 2) >> 8) & 0xFF;
                response[5] = (data.size() - 2) & 0xFF;
                for (auto byte = data.begin() + 2; byte < data.end(); ++byte) {
                  response.push_back(*byte);
                }
                break;
              default:   // Will not get here!
                break;
              }
            } else {
              // No. User provided data in free format
              response[4] = (data.size() >> 8) & 0xFF;
              response[5] = data.size() & 0xFF;
              for (auto& byte : data) {
                response.push_back(byte);
              }
            }
          } else {
            // No, function code is not served here
            response[4] = 0;
            response[5] = 3;
            response.push_back(m[6]);
            response.push_back(m[7] | 0x80);
            response.push_back(ILLEGAL_FUNCTION);
          }
        } else {
          // No, serverID is not served here
          response[4] = 0;
          response[5] = 3;
          response.push_back(m[6]);
          response.push_back(m[7] | 0x80);
          response.push_back(INVALID_SERVER);
        }
      }
      delay(1);
      // Do we have a response to send?

      Serial.print(" Response: ");
      for (auto& byte : response) { Serial.printf(" %02X", byte); }
      Serial.println();

      if (response.size() >= 8) {
        // Yes. Do it now.
        myClient.write(response.data(), response.size());
        myClient.flush();
      }
      // We did something communicationally - rewind timeout timer
      myLastMessage = millis();
    }

    delay(1);
  }

  // Timeout!
  // Read away all that may still hang in the buffer
  while (myClient.available()) { myClient.read(); }
  // Now stop the client
  myClient.stop();

  Serial.printf("Sent stop - task %d killing itself\n", (uint32_t)myTask);
  Serial.flush();

  myData->task = nullptr;
  delay(50);
  vTaskDelete(myTask);
}

// receive: get request via Client connection
TCPMessage CLASSNAME::receive(CLIENTTYPE client, uint32_t timeWait) {
  uint32_t lastMillis = millis();     // Timer to check for timeout
  TCPMessage m;                       // vector to take read data

  // wait for packet data or timeout
  while (millis() - lastMillis < timeWait) {
    // Is there data waiting?
    if (client.available()) {
      // Yes. catch as much as is there and fits into buffer
      while (client.available()) {
        m.push_back(client.read());
      }
      // Rewind EOT and timeout timers
      lastMillis = millis();
    }
    delay(1); // Give scheduler room to breathe
  }
  return m;
}

#endif