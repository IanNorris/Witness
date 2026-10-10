#include "DvrConfiguration.h"
#include <iostream>

int main()
{
    int seconds = Witness::Dvr::DefaultSegmentSeconds;
    for (const auto value : {"", "0", "29", "901", "-60", "60.0", "60junk", " 60", "9999999999999999999"}) {
        if (Witness::Dvr::ParseSegmentSeconds(value, seconds) || seconds != 60) return 1;
    }
    for (const auto value : {"30", "60", "300", "900"}) {
        if (!Witness::Dvr::ParseSegmentSeconds(value, seconds)) return 2;
    }
    std::cout << "PASS DVR duration defaults, bounds and strict parsing\n";
    return 0;
}
