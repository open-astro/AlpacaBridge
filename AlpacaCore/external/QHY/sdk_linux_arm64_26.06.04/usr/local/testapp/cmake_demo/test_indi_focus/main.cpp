#include "main.h"
#include <basedevice.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>


int main(int, char *[])
{
    std::cout << "!!!! focuser may move (100 step),  Press Enter to start !!!!!.\n";
    std::cin.ignore();
    MyClient myClient;
    myClient.setServer("localhost", 7624);

    myClient.connectServer();


    std::cout << "Press Enter key to terminate the client.\n";
    std::cin.ignore();
}


MyClient::MyClient()
{
    // wait for the availability of the device
    watchDevice("QFocuser", [this](INDI::BaseDevice device)
    {
        focuser_tester = device;
        device.watchProperty("CONNECTION", [this](INDI::Property)
        {
            IDLog("Connecting to INDI Driver...\n");

            if (!focuser_tester.isConnected())
            {
                connectDevice("QFocuser");
            }
        }, INDI::BaseDevice::WATCH_NEW);


        device.watchProperty("ABS_FOCUS_POSITION", [this](INDI::PropertyNumber property)
        {
            IDLog("Focus control is available.\n");


            setFocusPosition(100);
        }, INDI::BaseDevice::WATCH_NEW);
    });
}





void MyClient::newMessage(INDI::BaseDevice baseDevice, int messageID)
{
    if (!baseDevice.isDeviceNameMatch("QFocuser"))
        return;

    IDLog("Receiving message from Server:\n"
          "    %s\n\n",
          baseDevice.messageQueue(messageID).c_str());
}
void MyClient::setFocusPosition(int position)
{
    INDI::PropertyNumber focusPosition = focuser_tester.getProperty("REL_FOCUS_POSITION");

    if (!focusPosition.isValid())
    {
        IDLog("Error: unable to find focus, FOCUS_ABSOLUTE_POSITION property...\n");
        return;
    }

    IDLog("Setting focus position to %d.\n", position);
    focusPosition[0].setValue(position);
    sendNewProperty(focusPosition);
}