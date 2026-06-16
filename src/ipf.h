#pragma once

#include <string>

#include "disk.h"

int ipf_load(FILE*, t_drive*);
int ipf_load(const std::string&, t_drive*);
