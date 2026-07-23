#pragma once

#include "baseclient.h"
#include <basedevice.h>

class MyClient : public INDI::BaseClient
{
    public:
        MyClient();
        ~MyClient() = default;

    public:
        void setTemperature(double value);
        void takeExposure(double seconds);

    protected:
        void newMessage(INDI::BaseDevice baseDevice, int messageID) override;

    private:
        INDI::BaseDevice mSimpleCCD;
};