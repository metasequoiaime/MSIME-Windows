#pragma once

#include <cstdint>

struct ClientFocusLease {
    uint64_t client = 0;
    uint64_t epoch = 0;
    uint64_t token = 0;
};

struct ClientKeyEvent {
    ClientFocusLease lease;
    uint32_t virtual_key = 0;
    uint32_t scan_code = 0;
    uint32_t modifiers = 0;
    char16_t character = 0;
    bool ui_less = false;
};

class IClientKeyRouter {
  public:
    virtual ~IClientKeyRouter() = default;
    virtual bool dispatch(const ClientKeyEvent &event) = 0;
    virtual bool cancel(const ClientFocusLease &lease) = 0;
};
