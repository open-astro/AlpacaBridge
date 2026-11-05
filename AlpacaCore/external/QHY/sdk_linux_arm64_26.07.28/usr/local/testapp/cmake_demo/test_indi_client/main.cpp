#include "main.h"
#include <basedevice.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>


int main(int, char *[])
{
    MyClient myClient;
    myClient.setServer("localhost", 7624);

    myClient.connectServer();

    myClient.setBLOBMode(B_ALSO, "Simple CCD", nullptr);

    myClient.enableDirectBlobAccess("Simple CCD", nullptr);

    std::cout << "Press Enter key to terminate the client.\n";
    std::cin.ignore();
}

/**************************************************************************************
**
***************************************************************************************/
MyClient::MyClient()
{
    // wait for the availability of the device
    watchDevice("Simple CCD", [this](INDI::BaseDevice device)
    {
        mSimpleCCD = device; // save device

        // wait for the availability of the "CONNECTION" property
        device.watchProperty("CONNECTION", [this](INDI::Property)
        {
            IDLog("Connecting to INDI Driver...\n");
            connectDevice("Simple CCD");
        }, INDI::BaseDevice::WATCH_NEW);

        // wait for the availability of the "CCD_TEMPERATURE" property
        device.watchProperty("CCD_TEMPERATURE", [this](INDI::PropertyNumber property)
        {
            if (mSimpleCCD.isConnected())
            {
                IDLog("CCD is connected.\n");
                setTemperature(-20);
            }

            // call lambda function if property changed
            property.onUpdate([property, this]()
            {
                IDLog("Receiving new CCD Temperature: %g C\n", property[0].getValue());
                if (property[0].getValue() == -20)
                {
                    IDLog("CCD temperature reached desired value!\n");
                    takeExposure(1);
                }
            });
        }, INDI::BaseDevice::WATCH_NEW);

        // call if updated of the "CCD1" property - simplified way
        device.watchProperty("CCD1", [](INDI::PropertyBlob property)
        {
            // Save FITS file to disk
            std::ofstream myfile;

            myfile.open("ccd_simulator.fits", std::ios::out | std::ios::binary);
            myfile.write(static_cast<char *>(property[0].getBlob()), property[0].getBlobLen());
            myfile.close();

            IDLog("Received image, saved as ccd_simulator.fits\n");
        }, INDI::BaseDevice::WATCH_UPDATE);
    });
}

/**************************************************************************************
**
***************************************************************************************/
void MyClient::setTemperature(double value)
{
    INDI::PropertyNumber ccdTemperature = mSimpleCCD.getProperty("CCD_TEMPERATURE");

    if (!ccdTemperature.isValid())
    {
        IDLog("Error: unable to find Simple CCD, CCD_TEMPERATURE property...\n");
        return;
    }

    IDLog("Setting temperature to %g C.\n", value);
    ccdTemperature[0].setValue(value);
    sendNewProperty(ccdTemperature);
}

/**************************************************************************************
**
***************************************************************************************/
void MyClient::takeExposure(double seconds)
{
    INDI::PropertyNumber ccdExposure = mSimpleCCD.getProperty("CCD_EXPOSURE");

    if (!ccdExposure.isValid())
    {
        IDLog("Error: unable to find CCD Simulator CCD_EXPOSURE property...\n");
        return;
    }

    // Take a 1 second exposure
    IDLog("Taking a %g second exposure.\n", seconds);
    ccdExposure[0].setValue(seconds);
    sendNewProperty(ccdExposure);
}

/**************************************************************************************
**
***************************************************************************************/
void MyClient::newMessage(INDI::BaseDevice baseDevice, int messageID)
{
    if (!baseDevice.isDeviceNameMatch("Simple CCD"))
        return;

    IDLog("Recveing message from Server:\n"
          "    %s\n\n", 
          baseDevice.messageQueue(messageID).c_str());
}