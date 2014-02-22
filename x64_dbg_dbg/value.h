#ifndef _VALUE_H
#define _VALUE_H

#include "_global.h"

//functions
bool valuesignedcalc();
void valuesetsignedcalc(bool a);
bool valapifromstring(const char* name, uint* value, int* value_size, bool printall, bool silent, bool* hexonly);
bool valfromstring(const char* string, uint* value, bool silent, bool baseonly, int* value_size, bool* isvar, bool* hexonly);
bool valfromstring(const char* string, uint* value, bool silent, bool baseonly);
bool valfromstring(const char* string, uint* value, bool silent);
bool valfromstring(const char* string, uint* value);
bool valflagfromstring(unsigned int eflags, const char* string);
bool valtostring(const char* string, uint* value, bool silent);

#endif // _VALUE_H
