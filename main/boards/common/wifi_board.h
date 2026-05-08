#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"

class WifiBoard : public Board {
private:
    bool wifi_config_mode_ = false;

public:
    WifiBoard();
    virtual std::string GetBoardJson() override;

protected:
    void EnterWifiConfigMode();
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual Http* CreateHttp() override;
    virtual WebSocket* CreateWebSocket() override;
    virtual Mqtt* CreateMqtt() override;
    virtual Udp* CreateUdp() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual void ResetWifiConfiguration();
};

#endif // WIFI_BOARD_H
