/*
 * Copyright (C) 2015 David Devecsery
 */

#include "llvm/Oha/InstPrinter.h"

#include <map>
#include <string>

std::map<const llvm::Instruction *, std::string> InstPrinter::strs_;

