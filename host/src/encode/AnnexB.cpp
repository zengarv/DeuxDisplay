#include "AnnexB.h"

namespace dd::annexb
{
namespace
{

// Returns the index of the next start code at or after `from`, and its length (3 or 4).
bool FindStartCode(std::span<const uint8_t> d, size_t from, size_t& pos, size_t& len)
{
    for (size_t i = from; i + 3 <= d.size(); ++i)
    {
        if (d[i] == 0 && d[i + 1] == 0)
        {
            if (d[i + 2] == 1)
            {
                pos = i;
                len = 3;
                if (i > from && d[i - 1] == 0)
                {
                    pos = i - 1;
                    len = 4;
                }
                return true;
            }
        }
    }
    return false;
}

} // namespace

std::vector<Nal> SplitH264(std::span<const uint8_t> data)
{
    std::vector<Nal> nals;
    size_t pos = 0;
    size_t len = 0;
    if (!FindStartCode(data, 0, pos, len))
    {
        return nals;
    }

    while (true)
    {
        const size_t payload = pos + len;
        size_t nextPos = data.size();
        size_t nextLen = 0;
        const bool more = FindStartCode(data, payload, nextPos, nextLen);
        if (!more)
        {
            nextPos = data.size();
        }

        Nal nal;
        nal.offset = pos;
        nal.size = nextPos - pos;
        nal.type = payload < data.size() ? static_cast<uint8_t>(data[payload] & 0x1F) : 0;
        nals.push_back(nal);

        if (!more)
        {
            break;
        }
        pos = nextPos;
        len = nextLen;
    }
    return nals;
}

SplitAccessUnit SeparateParameterSets(std::span<const uint8_t> accessUnit)
{
    SplitAccessUnit out;
    out.frame.reserve(accessUnit.size());
    const auto nals = SplitH264(accessUnit);

    // Some encoders (Intel QSV) repeat a lone PPS on ordinary frames. Only a full SPS+PPS set is
    // worth a CODEC_CONFIG message; a lone PPS stays inline, which decoders handle fine.
    bool hasSps = false;
    for (const Nal& nal : nals)
    {
        hasSps = hasSps || nal.type == kNalSps;
    }

    for (const Nal& nal : nals)
    {
        const auto bytes = accessUnit.subspan(nal.offset, nal.size);
        if (hasSps && (nal.type == kNalSps || nal.type == kNalPps))
        {
            out.codecConfig.insert(out.codecConfig.end(), bytes.begin(), bytes.end());
        }
        else
        {
            out.frame.insert(out.frame.end(), bytes.begin(), bytes.end());
            if (nal.type == kNalIdr)
            {
                out.keyframe = true;
            }
        }
    }
    return out;
}

} // namespace dd::annexb
