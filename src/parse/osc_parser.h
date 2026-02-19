#pragma once
#include <iostream>
#include "lsd.h"

namespace LSD::OscParser
{

    static void process_osc(const std::string &seq) {
        size_t sep = seq.find(';'); if (sep==std::string::npos) return;
        int cmd=-1; try{cmd=std::stoi(seq.substr(0,sep));}catch(...){return;}
        if (cmd==0||cmd==2) LSD::WINDOW_TITLE=seq.substr(sep+1);
    }

    
}
