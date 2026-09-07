#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace base64
{
    static const char kEncodeTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    inline std::string encode(const uint8_t *data, size_t len)
    {
        std::string out;
        out.reserve(((len + 2) / 3) * 4);

        size_t i = 0;
        while (i < len)
        {
            uint32_t octet_a = i < len ? data[i++] : 0;
            uint32_t octet_b = i < len ? data[i++] : 0;
            uint32_t octet_c = i < len ? data[i++] : 0;

            uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

            out.push_back(kEncodeTable[(triple >> 18) & 0x3F]);
            out.push_back(kEncodeTable[(triple >> 12) & 0x3F]);
            out.push_back(i > len + 1 ? '=' : kEncodeTable[(triple >> 6) & 0x3F]);
            out.push_back(i > len ? '=' : kEncodeTable[triple & 0x3F]);
        }

        return out;
    }

    inline std::string encode(const std::vector<uint8_t> &data)
    {
        return encode(data.data(), data.size());
    }
}
