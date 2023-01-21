#include "SonyCamera.h"

void scanEndedCB(NimBLEScanResults results);

using NotifyFn = std::function<void(NimBLERemoteCharacteristic *pRemoteCharacteristic, uint8_t *pData, size_t length, bool isNotify)>;

/**  None of these are required as they will be handled by the library with defaults. **
 **                       Remove as you see fit for your needs                        */
class ClientCallbacks : public NimBLEClientCallbacks
{
public:
    ClientCallbacks(SonyCamera *cameraRef)
    {
        this->cameraRef = cameraRef;
    }

private:
    SonyCamera *cameraRef;

    void onConnect(NimBLEClient *pClient)
    {
        Serial.println("Connected");
        /** After connection we should change the parameters if we don't need fast response times.
         *  These settings are 150ms interval, 0 latency, 450ms timout.
         *  Timeout should be a multiple of the interval, minimum is 100ms.
         *  I find a multiple of 3-5 * the interval works best for quick response/reconnect.
         *  Min interval: 120 * 1.25ms = 150, Max interval: 120 * 1.25ms = 150, 0 latency, 60 * 10ms = 600ms timeout
         */
        pClient->updateConnParams(120, 120, 0, 60);
    };

    void onDisconnect(NimBLEClient *pClient)
    {
        cameraRef->connected = false;
        Serial.print(pClient->getPeerAddress().toString().c_str());
        Serial.println(" Disconnected - Starting scan");
        NimBLEDevice::getScan()->start(cameraRef->scanTime, scanEndedCB);
    };

    /** Called when the peripheral requests a change to the connection parameters.
     *  Return true to accept and apply them or false to reject and keep
     *  the currently used parameters. Default will return true.
     */
    bool onConnParamsUpdateRequest(NimBLEClient *pClient, const ble_gap_upd_params *params)
    {
        if (params->itvl_min < 24)
        { /** 1.25ms units */
            return false;
        }
        else if (params->itvl_max > 40)
        { /** 1.25ms units */
            return false;
        }
        else if (params->latency > 2)
        { /** Number of intervals allowed to skip */
            return false;
        }
        else if (params->supervision_timeout > 100)
        { /** 10ms units */
            return false;
        }

        return true;
    };

    /********************* Security handled here **********************
    ****** Note: these are the same return values as defaults ********/
    uint32_t onPassKeyRequest()
    {
        Serial.println("Client Passkey Request");
        /** return the passkey to send to the server */
        return 123456;
    };

    bool onConfirmPIN(uint32_t pass_key)
    {
        Serial.print("The passkey YES/NO number: ");
        Serial.println(pass_key);
        /** Return false if passkeys don't match. */
        return true;
    };

    /** Pairing process complete, we can check the results in ble_gap_conn_desc */
    void onAuthenticationComplete(ble_gap_conn_desc *desc)
    {
        if (!desc->sec_state.encrypted)
        {
            Serial.println("Encrypt connection failed - disconnecting");
            /** Find the client with the connection handle provided in desc */
            NimBLEDevice::getClientByID(desc->conn_handle)->disconnect();
            return;
        }
    };
};

bool isSonyCamera(std::__cxx11::string manufacturerData)
{
    return std::equal(CAMERA_MANUFACTURER_LOOKUP.begin(), CAMERA_MANUFACTURER_LOOKUP.end(), manufacturerData.begin());
}

// returns true if camera is open for pairing, false otherwise
bool isCameraReadyToPair(std::__cxx11::string data)
{
    // We are certain this is a camera, lets check for pairing status
    auto it = std::find(data.begin(), data.end(), 0x22);
    if (it != data.end() && (it + 1) != data.end())
    {
        // Bitmasks:
        //
        // PairingSupported	0x80
        // PairingEnabled	0x40
        // LocationFunctionSupported	0x20
        // LocationFunctionEnabled	0x10
        // UnknownFunctionSupported	0x08
        // UnknownFunctionEnabled	0x04
        // RemoteFunctionEnabled	0x02
        // Unknown	0x01

        if ((*(it + 1) & 0x40) == 0x40 && (*(it + 1) & 0x02) == 0x02)
        {
            // Camera is ready to pair
            return true;
        }
    }

    // camera does not want to pair
    return false;
}

/** Define a class to handle the callbacks when advertisements are received */
class AdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks
{
public:
    AdvertisedDeviceCallbacks(SonyCamera *camera)
    {
        this->cameraRef = camera;
    }

private:
    SonyCamera *cameraRef;

    void onResult(NimBLEAdvertisedDevice *advertisedDevice)
    {
        Serial.print("Advertised Device found: ");
        Serial.println(advertisedDevice->toString().c_str());

        std::__cxx11::string manufacturerData = advertisedDevice->getManufacturerData();
        if (isSonyCamera(manufacturerData))
        {
            Serial.print("Sony camera found!");

            if (isCameraReadyToPair(manufacturerData))
            {
                Serial.println("Camera is able to pair");

                /** stop scan before connecting */
                NimBLEDevice::getScan()->stop();

                /** Save the device reference in a global for the client to use */
                this->cameraRef->advDevice = advertisedDevice;

                // /** Ready to connect now */
                this->cameraRef->doConnect = true;
            }
            else
            {
                Serial.println("Camera not ready");
            }
        }
    };
};

// /** Notification / Indication receiving handler callback */
// void notifyCB(NimBLERemoteCharacteristic *pRemoteCharacteristic, uint8_t *pData, size_t length, bool isNotify)
// {
//     std::string str = (isNotify == true) ? "Notification" : "Indication";
//     str += " from ";
//     /** NimBLEAddress and NimBLEUUID have std::string operators */
//     str += std::string(pRemoteCharacteristic->getRemoteService()->getClient()->getPeerAddress());
//     str += ": Service = " + std::string(pRemoteCharacteristic->getRemoteService()->getUUID());
//     str += ", Characteristic = " + std::string(pRemoteCharacteristic->getUUID());
//     str += ", Value = " + std::string((char *)pData, length);
//     Serial.println(str.c_str());
// }

/** Callback to process the results of the last scan or restart it */
void scanEndedCB(NimBLEScanResults results)
{
    Serial.println("Scan Ended");
}

SonyCamera::SonyCamera()
{
}

/** Handles the provisioning of clients and connects / interfaces with the server */
bool SonyCamera::connectToServer()
{
    NimBLEClient *pClient = nullptr;

    /** Check if we have a client we should reuse first **/
    if (NimBLEDevice::getClientListSize())
    {
        /** Special case when we already know this device, we send false as the
         *  second argument in connect() to prevent refreshing the service database.
         *  This saves considerable time and power.
         */
        pClient = NimBLEDevice::getClientByPeerAddress(advDevice->getAddress());
        if (pClient)
        {
            if (!pClient->connect(advDevice, false))
            {
                Serial.println("Reconnect failed");
                return false;
            }
            Serial.println("Reconnected client");
        }
        /** We don't already have a client that knows this device,
         *  we will check for a client that is disconnected that we can use.
         */
        else
        {
            pClient = NimBLEDevice::getDisconnectedClient();
        }
    }

    /** No client to reuse? Create a new one. */
    if (!pClient)
    {
        if (NimBLEDevice::getClientListSize() >= NIMBLE_MAX_CONNECTIONS)
        {
            Serial.println("Max clients reached - no more connections available");
            return false;
        }

        pClient = NimBLEDevice::createClient();

        Serial.println("New client created");

        pClient->setClientCallbacks(new ClientCallbacks(this), false);
        /** Set initial connection parameters: These settings are 15ms interval, 0 latency, 120ms timout.
         *  These settings are safe for 3 clients to connect reliably, can go faster if you have less
         *  connections. Timeout should be a multiple of the interval, minimum is 100ms.
         *  Min interval: 12 * 1.25ms = 15, Max interval: 12 * 1.25ms = 15, 0 latency, 51 * 10ms = 510ms timeout
         */
        pClient->setConnectionParams(12, 12, 0, 51);
        /** Set how long we are willing to wait for the connection to complete (seconds), default is 30. */
        pClient->setConnectTimeout(5);

        if (!pClient->connect(advDevice))
        {
            /** Created a client but failed to connect, don't need to keep it as it has no data */
            NimBLEDevice::deleteClient(pClient);
            Serial.println("Failed to connect, deleted client");
            return false;
        }
    }

    if (!pClient->isConnected())
    {
        if (!pClient->connect(advDevice))
        {
            Serial.println("Failed to connect");
            return false;
        }
    }

    Serial.print("Connected to: ");
    Serial.println(pClient->getPeerAddress().toString().c_str());
    Serial.print("RSSI: ");
    Serial.println(pClient->getRssi());

    /** Now we can read/write/subscribe the characteristics of the services we are interested in */
    NimBLERemoteService *remoteControlService = nullptr;
    NimBLERemoteCharacteristic *remoteNotifyChr = nullptr;
    NimBLERemoteDescriptor *pDsc = nullptr;

    remoteControlService = pClient->getService(REMOTE_CONTROL_SERVICE_UUID);
    if (remoteControlService)
    { /** make sure it's not null */
        remoteCommandChr = remoteControlService->getCharacteristic(NimBLEUUID((uint16_t)0xFF01));
        remoteNotifyChr = remoteControlService->getCharacteristic(NimBLEUUID((uint16_t)0xFF02));

        if (remoteCommandChr && remoteNotifyChr)
        {

            if (remoteCommandChr->canWrite())
            {
                Serial.println("0xFF01 can write!");
            }
            else
            {
                /** Disconnect if cannot write */
                pClient->disconnect();
                return false;
            }

            if (remoteNotifyChr->canNotify())
            {
                Serial.println("0xFF02 can notify!");

                SonyCamera *cameraRef = this;
                NotifyFn notifyCb = [cameraRef](NimBLERemoteCharacteristic *pRemoteCharacteristic, uint8_t *pData, size_t length, bool isNotify) -> void
                {
                    std::string str = (isNotify == true) ? "Notification" : "Indication";
                    str += " from ";
                    /** NimBLEAddress and NimBLEUUID have std::string operators */
                    str += std::string(pRemoteCharacteristic->getRemoteService()->getClient()->getPeerAddress());
                    str += ": Service = " + std::string(pRemoteCharacteristic->getRemoteService()->getUUID());
                    str += ", Characteristic = " + std::string(pRemoteCharacteristic->getUUID());
                    str += ", Value = " + std::string((char *)pData, length);
                    Serial.println(str.c_str());

                    cameraRef->handleCameraNotification(pData, length);
                };

                if (!remoteNotifyChr->subscribe(true, notifyCb))
                {
                    /** Disconnect if subscribe failed */
                    pClient->disconnect();
                    return false;
                }
            }
            else
            {
                /** Disconnect if cannot notify */
                pClient->disconnect();
                return false;
            }

            delay(500);
            remoteCommandChr->writeValue(SHUTTER_RELEASED);

            connected = true;

            //  /** make sure it's not null */
            // if (pChr->canRead())
            // {
            //     Serial.print(pChr->getUUID().toString().c_str());
            //     Serial.print(" Value: ");
            //     Serial.println(pChr->readValue().c_str());
            // }

            // if (pChr->canWrite())
            // {
            //     if (pChr->writeValue("Tasty"))
            //     {
            //         Serial.print("Wrote new value to: ");
            //         Serial.println(pChr->getUUID().toString().c_str());
            //     }
            //     else
            //     {
            //         /** Disconnect if write failed */
            //         pClient->disconnect();
            //         return false;
            //     }

            //     if (pChr->canRead())
            //     {
            //         Serial.print("The value of: ");
            //         Serial.print(pChr->getUUID().toString().c_str());
            //         Serial.print(" is now: ");
            //         Serial.println(pChr->readValue().c_str());
            //     }
            // }

            // /** registerForNotify() has been deprecated and replaced with subscribe() / unsubscribe().
            //  *  Subscribe parameter defaults are: notifications=true, notifyCallback=nullptr, response=false.
            //  *  Unsubscribe parameter defaults are: response=false.
            //  */
            // if (pChr->canNotify())
            // {
            //     // if(!pChr->registerForNotify(notifyCB)) {
            //     if (!pChr->subscribe(true, notifyCB))
            //     {
            //         /** Disconnect if subscribe failed */
            //         pClient->disconnect();
            //         return false;
            //     }
            // }
            // else if (pChr->canIndicate())
            // {
            //     /** Send false as first argument to subscribe to indications instead of notifications */
            //     // if(!pChr->registerForNotify(notifyCB, false)) {
            //     if (!pChr->subscribe(false, notifyCB))
            //     {
            //         /** Disconnect if subscribe failed */
            //         pClient->disconnect();
            //         return false;
            //     }
            // }
        }
    }
    else
    {
        Serial.println("remoteControlService service not found.");
    }

    // pSvc = pClient->getService("BAAD");
    // if (pSvc)
    // { /** make sure it's not null */
    //     pChr = pSvc->getCharacteristic("F00D");

    //     if (pChr)
    //     { /** make sure it's not null */
    //         if (pChr->canRead())
    //         {
    //             Serial.print(pChr->getUUID().toString().c_str());
    //             Serial.print(" Value: ");
    //             Serial.println(pChr->readValue().c_str());
    //         }

    //         pDsc = pChr->getDescriptor(NimBLEUUID("C01D"));
    //         if (pDsc)
    //         { /** make sure it's not null */
    //             Serial.print("Descriptor: ");
    //             Serial.print(pDsc->getUUID().toString().c_str());
    //             Serial.print(" Value: ");
    //             Serial.println(pDsc->readValue().c_str());
    //         }

    //         if (pChr->canWrite())
    //         {
    //             if (pChr->writeValue("No tip!"))
    //             {
    //                 Serial.print("Wrote new value to: ");
    //                 Serial.println(pChr->getUUID().toString().c_str());
    //             }
    //             else
    //             {
    //                 /** Disconnect if write failed */
    //                 pClient->disconnect();
    //                 return false;
    //             }

    //             if (pChr->canRead())
    //             {
    //                 Serial.print("The value of: ");
    //                 Serial.print(pChr->getUUID().toString().c_str());
    //                 Serial.print(" is now: ");
    //                 Serial.println(pChr->readValue().c_str());
    //             }
    //         }

    //         /** registerForNotify() has been deprecated and replaced with subscribe() / unsubscribe().
    //          *  Subscribe parameter defaults are: notifications=true, notifyCallback=nullptr, response=false.
    //          *  Unsubscribe parameter defaults are: response=false.
    //          */
    //         if (pChr->canNotify())
    //         {
    //             // if(!pChr->registerForNotify(notifyCB)) {
    //             if (!pChr->subscribe(true, notifyCB))
    //             {
    //                 /** Disconnect if subscribe failed */
    //                 pClient->disconnect();
    //                 return false;
    //             }
    //         }
    //         else if (pChr->canIndicate())
    //         {
    //             /** Send false as first argument to subscribe to indications instead of notifications */
    //             // if(!pChr->registerForNotify(notifyCB, false)) {
    //             if (!pChr->subscribe(false, notifyCB))
    //             {
    //                 /** Disconnect if subscribe failed */
    //                 pClient->disconnect();
    //                 return false;
    //             }
    //         }
    //     }
    // }
    // else
    // {
    //     Serial.println("BAAD service not found.");
    // }

    Serial.println("Done with this device!");
    return true;
}

void SonyCamera::startScan()
{
    /** Initialize NimBLE, no device name needed as we are not advertising */
    NimBLEDevice::init("");

    /** Set the IO capabilities of the device, each option will trigger a different pairing method.
     *  BLE_HS_IO_KEYBOARD_ONLY    - Passkey pairing
     *  BLE_HS_IO_DISPLAY_YESNO   - Numeric comparison pairing
     *  BLE_HS_IO_NO_INPUT_OUTPUT - DEFAULT setting - just works pairing
     */
    // NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY); // use passkey
    // NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO); //use numeric comparison

    /** 2 different ways to set security - both calls achieve the same result.
     *  no bonding, no man in the middle protection, secure connections.
     *
     *  These are the default values, only shown here for demonstration.
     */
    // NimBLEDevice::setSecurityAuth(false, false, true);
    NimBLEDevice::setSecurityAuth(/*BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM |*/ BLE_SM_PAIR_AUTHREQ_SC);

    /** Optional: set the transmit power, default is 3db */
#ifdef ESP_PLATFORM
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); /** +9db */
#else
    NimBLEDevice::setPower(9); /** +9db */
#endif

    /** Optional: set any devices you don't want to get advertisements from */
    // NimBLEDevice::addIgnored(NimBLEAddress ("aa:bb:cc:dd:ee:ff"));

    /** create new scan */
    NimBLEScan *pScan = NimBLEDevice::getScan();

    /** create a callback that gets called when advertisers are found */
    pScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks(this));

    /** Set scan interval (how often) and window (how long) in milliseconds */
    pScan->setInterval(45);
    pScan->setWindow(15);

    /** Active scan will gather scan response data from advertisers
     *  but will use more energy from both devices
     */
    pScan->setActiveScan(true);
    /** Start scanning for advertisers for the scan time specified (in seconds) 0 = forever
     *  Optional callback for when scanning stops.
     */
    pScan->start(scanTime, scanEndedCB);
}

void SonyCamera::loop()
{
    /** Loop here until we find a device we want to connect to */
    if (!doConnect)
    {
        return;
    }

    doConnect = false;

    /** Found a device we want to connect to, do it now */
    if (connectToServer())
    {
        Serial.println("Success! we should now be getting notifications!");
    }
    else
    {
        Serial.println("Failed to connect. Restarting scan...");
        NimBLEDevice::getScan()->start(scanTime, scanEndedCB);
    }

    // NimBLEDevice::getScan()->start(scanTime, scanEndedCB);
}

uint16_t commandToValue(CameraCommand cmd)
{
    switch (cmd)
    {
    case PressToFocus:
        return PRESS_TO_FOCUS;

    case HoldFocus:
        return HOLD_FOCUS;

    case TakePicture:
        return TAKE_PICTURE;

    case ShutterReleased:
    default:
        return SHUTTER_RELEASED;
    }
};

bool SonyCamera::send(CameraCommand cmd)
{
    uint16_t value = commandToValue(cmd);

    if (connected && remoteCommandChr->canWrite())
    {
        if (remoteCommandChr->writeValue(value))
        {
            Serial.print("Wrote new value ");
            Serial.printf("%#06x", value);
            Serial.print(" to: ");
            Serial.println(remoteCommandChr->getUUID().toString().c_str());

            return true;
        }
        else
        {
            return false;
        }
    }
    else
    {
        return false;
    }
}

void SonyCamera::handleCameraNotification(uint8_t *data, uint16_t length)
{

    Serial.println("Camera data: ");
    Serial.print("LEN: ");
    Serial.print(length);
    Serial.print(" DATA: ");
    for (int n = 0; n < length; n++)
    {
        Serial.print(" ");
        Serial.print(data[n], HEX);
        Serial.print(" ");
    }
    Serial.write("\0\n");
    Serial.flush();

    if (length == 3)
    {
        if (data[0] == 0x02)
        {
            switch (data[1])
            {
            case 0x3F:
                focusStatus = data[2];

                if (focusStatus == 0x20)
                {
                    // rs->set(Status::FOCUS_ACQUIRED);
                }
                else
                {
                    // rs->set(Status::READY);
                }

                break;

            case 0xA0:
                shutterStatus = data[2];

                if (shutterStatus == 0x20)
                {
                    // rs->set(Status::SHUTTER);
                }
                else
                {
                    // rs->set(Status::READY);
                }

                break;

            case 0xD5:
                recordingStatus = data[2];
                break;
            }

            last_message = micros();
        }
    }
}

void SonyCamera::trigger()
{
    // // hack until I get this to work
    // uint32_t timeout = millis() + 3000;

    // if (!focusHeld)
    // {
    //     // Reset focus status
    //     focusStatus = 0x00;

    //     // Focus
    //     send(PressToFocus);

    //     if (mode == AUTO_FOCUS)
    //     {
    //         while (focusStatus != 0x20)
    //         {
    //             yield();

    //             if (timeout < millis())
    //             {
    //                 break;
    //             }
    //         }
    //     }
    // }

    // // Release back to focus
    // send(HoldFocus);

    // // Reset focus status
    // shutterStatus = 0x00;

    // // Shutter
    // send(TakePicture);

    // if (mode == AUTO_FOCUS)
    // {
    //     while (shutterStatus != 0x20)
    //     {
    //         yield();

    //         if (timeout < millis())
    //         {
    //             break;
    //         }
    //     }
    // }

    // // Release back to focus
    // send(HoldFocus);

    // delay(10);

    // // Let go?
    // send(ShutterReleased);

    uint32_t timeout = millis() + 3000;

    // send(PressToFocus);
    // delay(timeout);

    send(PressToFocus);
    send(HoldFocus);
    delay(timeout);

    send(TakePicture);
    delay(timeout);

    send(ShutterReleased);
    // delay(timeout);
}

void SonyCamera::focus()
{
    send(ShutterReleased);

    // if (mode == AUTO_FOCUS)
    // {
    //     if (focusHeld)
    //     {
    //         // Focus
    //         send(PressToFocus);
    //     }
    //     else
    //     {
    //         send(HoldFocus);

    //         delay(10);

    //         // Let go?
    //         send(ShutterReleased);
    //     }
    // }
}