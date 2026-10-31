#include "makcu.h"
#include "../globals.h"
#include "../macros.h"
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

static std::unordered_map<makcu::MouseButton, bool> buttonStates;

namespace makcu {
    Device::Device()
        : m_connected(false),
          m_useUdp(false),
          m_udpSocket(INVALID_SOCKET),
          m_targetHost("pi.local"),
          m_targetPort(5005)
    {
        memset(&m_udpAddr, 0, sizeof(m_udpAddr));
    }

    Device::~Device() {
        disconnect();
    }

    bool Device::connect() {
        return connect(g_Index::makcu_ip, g_Index::makcu_port);
    }

    bool Device::connect(const std::string& host, int port) {
        disconnect();

        m_targetHost = host.empty() ? "pi.local" : host;
        m_targetPort = (port <= 0) ? 5005 : port;

        if (g_Index::makcu_use_udp) {
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
                m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (m_udpSocket != INVALID_SOCKET) {
                    struct addrinfo hints = {};
                    hints.ai_family = AF_INET; // 優先解析 IPv4
                    hints.ai_socktype = SOCK_DGRAM;
                    hints.ai_protocol = IPPROTO_UDP;

                    struct addrinfo* result = nullptr;
                    std::string portStr = std::to_string(m_targetPort);
                    // 執行 DNS / mDNS 解析 (支援 pi.local 與標準域名)
                    if (getaddrinfo(m_targetHost.c_str(), portStr.c_str(), &hints, &result) == 0 && result != nullptr) {
                        bool foundIpv4 = false;
                        for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
                            if (ptr->ai_family == AF_INET) {
                                memcpy(&m_udpAddr, ptr->ai_addr, sizeof(m_udpAddr));
                                foundIpv4 = true;
                                break;
                            }
                        }
                        freeaddrinfo(result);

                        if (foundIpv4) {
                            m_useUdp = true;
                            m_connected = true;

                            char resolvedIp[INET_ADDRSTRLEN] = { 0 };
                            inet_ntop(AF_INET, &m_udpAddr.sin_addr, resolvedIp, sizeof(resolvedIp));
                            timelog("Makcu DNS/mDNS resolved '%s' -> %s:%d (UDP Active)", m_targetHost.c_str(), resolvedIp, m_targetPort);

                            // 校準 Makcu 虛擬螢幕尺寸，確保 km.moveto 絕對座標 100% 像素對齊
                            int scrW = GetSystemMetrics(SM_CXSCREEN);
                            int scrH = GetSystemMetrics(SM_CYSCREEN);
                            char scrBuf[48];
                            int scrLen = std::snprintf(scrBuf, sizeof(scrBuf), "km.screen(%d,%d)\r\n", scrW, scrH);
                            if (scrLen > 0) {
                                sendCommand(scrBuf);
                            }

                            // 測試發送微小探測封包確認連線
                            sendCommand("km.version()\r\n");
                            return true;
                        }
                    }

                    timelog("Failed to resolve Makcu address '%s' via DNS/mDNS, falling back to local mouse API.", m_targetHost.c_str());
                    closesocket(m_udpSocket);
                    m_udpSocket = INVALID_SOCKET;
                }
            }
        }

        // 若未開啟 UDP 或解析失敗，啟用本機滑鼠模擬模式
        m_useUdp = false;
        m_connected = true;
        timelog("Makcu running in Local Windows Mouse API mode.");
        return true;
    }

    void Device::disconnect() {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (m_udpSocket != INVALID_SOCKET) {
            closesocket(m_udpSocket);
            m_udpSocket = INVALID_SOCKET;
            WSACleanup();
        }
        m_connected = false;
        m_useUdp = false;
    }

    bool Device::isConnected() const {
        return m_connected;
    }

    std::string Device::getTargetAddress() const {
        if (m_useUdp) {
            char ipStr[INET_ADDRSTRLEN] = { 0 };
            inet_ntop(AF_INET, &m_udpAddr.sin_addr, ipStr, sizeof(ipStr));
            if (ipStr[0] != '\0' && m_targetHost != ipStr) {
                return m_targetHost + " (" + ipStr + "):" + std::to_string(m_targetPort);
            }
            return m_targetHost + ":" + std::to_string(m_targetPort);
        }
        return "Local Windows Mouse API";
    }

    void Device::sendCommand(const std::string& cmd) {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (m_useUdp && m_udpSocket != INVALID_SOCKET) {
            std::string payload = cmd;
            if (payload.empty() || payload.back() != '\n') {
                payload += "\r\n";
            }
            sendto(m_udpSocket, payload.c_str(), static_cast<int>(payload.size()), 0,
                reinterpret_cast<const sockaddr*>(&m_udpAddr), sizeof(m_udpAddr));
        }
    }

    void Device::setMouseButtonCallback(std::function<void(MouseButton, bool)> callback) {
        m_buttonCallback = callback;
    }

    void Device::mouseDown(MouseButton button) {
        if (m_useUdp) {
            switch (button) {
            case MouseButton::LEFT:   sendCommand("km.left(1)"); break;
            case MouseButton::RIGHT:  sendCommand("km.right(1)"); break;
            case MouseButton::MIDDLE: sendCommand("km.middle(1)"); break;
            case MouseButton::SIDE1:  sendCommand("km.side1(1)"); break;
            case MouseButton::SIDE2:  sendCommand("km.side2(1)"); break;
            }
        }
        else {
            DWORD flags = 0;
            DWORD data = 0;
            switch (button) {
            case MouseButton::LEFT:   flags = MOUSEEVENTF_LEFTDOWN; break;
            case MouseButton::RIGHT:  flags = MOUSEEVENTF_RIGHTDOWN; break;
            case MouseButton::MIDDLE: flags = MOUSEEVENTF_MIDDLEDOWN; break;
            case MouseButton::SIDE1:  flags = MOUSEEVENTF_XDOWN; data = XBUTTON1; break;
            case MouseButton::SIDE2:  flags = MOUSEEVENTF_XDOWN; data = XBUTTON2; break;
            }
            if (flags) {
                mouse_event(flags, 0, 0, data, 0);
            }
        }
    }

    void Device::mouseUp(MouseButton button) {
        if (m_useUdp) {
            switch (button) {
            case MouseButton::LEFT:   sendCommand("km.left(0)"); break;
            case MouseButton::RIGHT:  sendCommand("km.right(0)"); break;
            case MouseButton::MIDDLE: sendCommand("km.middle(0)"); break;
            case MouseButton::SIDE1:  sendCommand("km.side1(0)"); break;
            case MouseButton::SIDE2:  sendCommand("km.side2(0)"); break;
            }
        }
        else {
            DWORD flags = 0;
            DWORD data = 0;
            switch (button) {
            case MouseButton::LEFT:   flags = MOUSEEVENTF_LEFTUP; break;
            case MouseButton::RIGHT:  flags = MOUSEEVENTF_RIGHTUP; break;
            case MouseButton::MIDDLE: flags = MOUSEEVENTF_MIDDLEUP; break;
            case MouseButton::SIDE1:  flags = MOUSEEVENTF_XUP; data = XBUTTON1; break;
            case MouseButton::SIDE2:  flags = MOUSEEVENTF_XUP; data = XBUTTON2; break;
            }
            if (flags) {
                mouse_event(flags, 0, 0, data, 0);
            }
        }
    }

    void Device::click(int button, int count, int hold_ms) {
        if (m_useUdp) {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            if (m_udpSocket != INVALID_SOCKET) {
                char buf[48];
                int len = std::snprintf(buf, sizeof(buf), "km.click(%d,%d,%d)\r\n", button, count, hold_ms);
                if (len > 0) {
                    sendto(m_udpSocket, buf, len, 0,
                        reinterpret_cast<const sockaddr*>(&m_udpAddr), sizeof(m_udpAddr));
                }
            }
        }
        else {
            std::thread([button, hold_ms]() {
                INPUT down = {};
                down.type = INPUT_MOUSE;
                down.mi.dwFlags = (button == 1) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
                SendInput(1, &down, sizeof(INPUT));

                std::this_thread::sleep_for(std::chrono::milliseconds((std::max)(1, hold_ms)));

                INPUT up = {};
                up.type = INPUT_MOUSE;
                up.mi.dwFlags = (button == 1) ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;
                SendInput(1, &up, sizeof(INPUT));
            }).detach();
        }
    }

    void Device::mouseMove(int dx, int dy) {
        if (dx == 0 && dy == 0) return;
        if (m_useUdp) {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            if (m_udpSocket != INVALID_SOCKET) {
                char buf[48];
                int len = std::snprintf(buf, sizeof(buf), "km.move_now(%d,%d)\r\n", dx, dy);
                if (len > 0) {
                    sendto(m_udpSocket, buf, len, 0,
                        reinterpret_cast<const sockaddr*>(&m_udpAddr), sizeof(m_udpAddr));
                }
            }
        }
        else {
            INPUT input = {};
            input.type = INPUT_MOUSE;
            input.mi.dx = dx;
            input.mi.dy = dy;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &input, sizeof(INPUT));
        }
    }

    void Device::mouseMoveTo(int absX, int absY) {
        if (m_useUdp) {
            std::lock_guard<std::mutex> lock(m_sendMutex);
            if (m_udpSocket != INVALID_SOCKET) {
                char buf[64];
                int len = std::snprintf(buf, sizeof(buf), "km.moveto(%d,%d)\r\n", absX, absY);
                if (len > 0) {
                    sendto(m_udpSocket, buf, len, 0,
                        reinterpret_cast<const sockaddr*>(&m_udpAddr), sizeof(m_udpAddr));
                }
            }
        }
        else {
            int sw = (std::max)(1, GetSystemMetrics(SM_CXSCREEN));
            int sh = (std::max)(1, GetSystemMetrics(SM_CYSCREEN));
            INPUT input = {};
            input.type = INPUT_MOUSE;
            input.mi.dx = (absX * 65536) / sw;
            input.mi.dy = (absY * 65536) / sh;
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            SendInput(1, &input, sizeof(INPUT));
        }
    }


}

namespace g_Makcu {
    void mouseButtonHandler(makcu::MouseButton button, bool pressed) {
        buttonStates[button] = pressed;
        if (pressed) {
            g_Index::g_makcuDevice.mouseDown(button);
        }
        else {
            g_Index::g_makcuDevice.mouseUp(button);
        }
    }

    bool isButtonHeld(makcu::MouseButton button) {
        switch (button) {
        case makcu::MouseButton::LEFT:
            return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        case makcu::MouseButton::RIGHT:
            return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        case makcu::MouseButton::MIDDLE:
            return (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        case makcu::MouseButton::SIDE1:
            return (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
        case makcu::MouseButton::SIDE2:
            return (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
        case makcu::MouseButton::ALT:
            return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        case makcu::MouseButton::SHIFT:
            return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        default:
            return false;
        }
    }
}
