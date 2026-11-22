#pragma once
#include "Model.h"
namespace clients
{
bool requested();
bool replaying();
void validate(const std::vector<std::string>& modules);
void run(SVF::BVDataPTAImpl* pta, Clock::time_point wholeStart,
         double irSeconds = 0, double ptaSeconds = 0);
} // namespace clients
