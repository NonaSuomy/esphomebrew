// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
// PAX keyboard values retain the existing service's ASCII/special-key contract.
static inline int pax_bluetooth_key(uint8_t usage,uint8_t modifiers){
 int shift=(modifiers&0x22)!=0;
 if(usage>=4 && usage<=29)return (shift?'A':'a')+usage-4;
 if(usage>=30 && usage<=39)return (shift?"!@#$%^&*()":"1234567890")[usage-30];
 if(usage>=58 && usage<=69)return 135+usage-58;
 switch(usage){
 case 40:return 13;case 41:return 27;case 42:return 127;case 43:return 9;case 44:return 32;
 case 45:return shift?'_':'-';case 46:return shift?'+':'=';case 47:return shift?'{':'[';case 48:return shift?'}':']';
 case 49:return shift?'|':'\\';case 51:return shift?':':';';case 52:return shift?'"':'\'';case 53:return shift?'~':'`';
 case 54:return shift?'<':',';case 55:return shift?'>':'.';case 56:return shift?'?':'/';
 case 73:return 147;case 74:return 151;case 75:return 150;case 76:return 148;case 77:return 152;case 78:return 149;
 case 79:return 154;case 80:return 156;case 81:return 155;case 82:return 153;
 case 224:case 228:return 133;case 225:case 229:return 134;case 226:case 230:return 132;
 default:return 0;
 }
}
