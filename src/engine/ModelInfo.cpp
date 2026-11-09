#include "engine/ModelInfo.h"

namespace gas
{

bool ModelInfo::includesCabinet() const
{
    return gearType.find("cab") != std::string::npos;
}

std::string ModelInfo::displayName() const
{
    if (!name.empty())
        return name;

    if (!gearMake.empty() || !gearModel.empty())
    {
        std::string s = gearMake;
        if (!s.empty() && !gearModel.empty())
            s += " ";
        return s + gearModel;
    }

    return fileName;
}

} // namespace gas
