#include "NativeKeyMap.h"
#include <array>

int qemuKeyNumber(const QString &platform, quint32 scanCode, quint32 virtualKey)
{
    if (platform == "windows") {
        if (scanCode >= 1 && scanCode <= 0x58)
            return int(scanCode);
        if ((scanCode & 0xff00) == 0xe000 && (scanCode & 0xff) >= 1 && (scanCode & 0xff) <= 0x7f)
            return int(scanCode & 0x7f) | 0x80;
        return 0;
    }
    if (platform == "cocoa") {
        // Apple physical virtual-key positions mapped to XT/QEMU numbers. No
        // characters, input-method text or host clipboard enter this mapping.
        static constexpr std::array<int, 128> positions = [] {
            std::array<int, 128> result{};
            result[0] = 0x1e;  // A
            result[1] = 0x1f;  // S
            result[2] = 0x20;  // D
            result[3] = 0x21;  // F
            result[4] = 0x23;  // H
            result[5] = 0x22;  // G
            result[6] = 0x2c;  // Z
            result[7] = 0x2d;  // X
            result[8] = 0x2e;  // C
            result[9] = 0x2f;  // V
            result[11] = 0x30; // B
            result[12] = 0x10; // Q
            result[13] = 0x11; // W
            result[14] = 0x12; // E
            result[15] = 0x13; // R
            result[16] = 0x15; // Y
            result[17] = 0x14; // T
            result[18] = 0x02; // 1
            result[19] = 0x03;
            result[20] = 0x04;
            result[21] = 0x05;
            result[22] = 0x07;
            result[23] = 0x06;
            result[24] = 0x0d; // Equals
            result[25] = 0x0a;
            result[26] = 0x08;
            result[27] = 0x0c; // Minus
            result[28] = 0x09;
            result[29] = 0x0b;
            result[30] = 0x1b;
            result[31] = 0x18; // O
            result[32] = 0x16; // U
            result[33] = 0x1a;
            result[34] = 0x17; // I
            result[35] = 0x19; // P
            result[36] = 0x1c; // Return
            result[37] = 0x26; // L
            result[38] = 0x24; // J
            result[39] = 0x28;
            result[40] = 0x25; // K
            result[41] = 0x27;
            result[42] = 0x2b;
            result[43] = 0x33;
            result[44] = 0x35;
            result[45] = 0x31; // N
            result[46] = 0x32; // M
            result[47] = 0x34;
            result[48] = 0x0f; // Tab
            result[49] = 0x39; // Space
            result[50] = 0x29;
            result[51] = 0x0e; // Backspace
            result[53] = 0x01; // Escape
            result[54] = 0xdc; // Right Command / Super
            result[55] = 0xdb; // Left Command / Super
            result[56] = 0x2a;
            result[57] = 0x3a;
            result[58] = 0x38;
            result[59] = 0x1d;
            result[60] = 0x36;
            result[61] = 0xb8;
            result[62] = 0x9d;
            result[65] = 0x53; // Keypad decimal
            result[67] = 0x37;
            result[69] = 0x4e;
            result[71] = 0x45;
            result[75] = 0xb5;
            result[76] = 0x9c;
            result[78] = 0x4a;
            result[82] = 0x52;
            result[83] = 0x4f;
            result[84] = 0x50;
            result[85] = 0x51;
            result[86] = 0x4b;
            result[87] = 0x4c;
            result[88] = 0x4d;
            result[89] = 0x47;
            result[91] = 0x48;
            result[92] = 0x49;
            result[96] = 0x3f; // F5
            result[97] = 0x40;
            result[98] = 0x41;
            result[99] = 0x3d;
            result[100] = 0x42;
            result[101] = 0x43;
            result[103] = 0x57;
            result[109] = 0x44;
            result[111] = 0x58;
            result[115] = 0xc7; // Home
            result[116] = 0xc9;
            result[117] = 0xd3;
            result[118] = 0x3e;
            result[119] = 0xcf;
            result[120] = 0x3c;
            result[121] = 0xd1;
            result[122] = 0x3b;
            result[123] = 0xcb;
            result[124] = 0xcd;
            result[125] = 0xd0;
            result[126] = 0xc8;
            return result;
        }();
        return virtualKey < positions.size() ? positions[virtualKey] : 0;
    }
    if (platform != "xcb" || scanCode < 8)
        return 0;
    const quint32 evdev = scanCode - 8;
    if (evdev >= 1 && evdev <= 83)
        return int(evdev);
    switch (evdev) {
    case 87:
        return 0x57;
    case 88:
        return 0x58;
    case 96:
        return 0x9c;
    case 97:
        return 0x9d;
    case 98:
        return 0xb5;
    case 100:
        return 0xb8;
    case 102:
        return 0xc7;
    case 103:
        return 0xc8;
    case 104:
        return 0xc9;
    case 105:
        return 0xcb;
    case 106:
        return 0xcd;
    case 107:
        return 0xcf;
    case 108:
        return 0xd0;
    case 109:
        return 0xd1;
    case 110:
        return 0xd2;
    case 111:
        return 0xd3;
    case 125:
        return 0xdb;
    case 126:
        return 0xdc;
    default:
        return 0;
    }
}
