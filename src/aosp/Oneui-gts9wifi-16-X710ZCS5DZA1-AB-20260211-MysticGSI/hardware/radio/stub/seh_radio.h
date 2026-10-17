// Samsung OneUI radio extension HALs (vendor.samsung.hardware.radio.*).
#pragma once

struct SehSlot {
    const char* iccid;
    const char* eid;
    // Samsung status-bar signal levels (0-4) for the slot's LTE / NR cell.
    int lteLevel;
    int nrLevel;
};

// Registers ISehRadioSim/Network/Data/Messaging for "slot<slot>".
void publishSehSlot(int slot, const SehSlot& info);
